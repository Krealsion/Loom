// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVER_WEAVER_HPP
#define ZEN_WEAVER_WEAVER_HPP

// The Weaver: an ordinary weave that puts one governed session's requests for more message
// authority in front of a person, and installs or revokes what that person decides through a
// `GrantAuthority` the host minted for it. docs/reference/weaver.md
//
//     USER          decides
//     WEAVER        delegates / revokes
//     SESSION       acts
//     SWITCHBOARD   enforces and attributes
//
// It holds no `Switchboard&` and no privileged send path. Its Grant is what it may say and its
// GrantAuthority what it may delegate; the host grants the two separately, and neither gives
// the other. It keeps workflow state only, at most one request awaiting a person, the answer
// right taken with it and the name of the prompt that showed it, and reads what a subject may
// do from `mail.describe_authority(...)` each time, never from a record of its own. One
// operator seat, one governed subject, one ceiling, at most one request in flight, and a
// decision acts only on the prompt it names.
//
// It governs Loom message authority and nothing else: it does not contain in-process native
// code (a loaded weave shares this address space), authenticate a remote person, hold
// accounts, persist anything across a restart, or grant "allow once", for which Loom has no
// word. docs/reference/weaver.md#what-this-does-not-govern

// Not <zen/host/grant_wiring.hpp>: a Weaver uses a capability and never mints one, and that
// header keeps its minting function out of every weave's reach. The host mints the capability
// and passes it to the constructor.
#include <zen/switchboard/grant.hpp>
#include <zen/weave.hpp>
#include <zen/weaver/vocabulary.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loom {

// ---- making text safe to put in front of a person --------------------------

/// The most bytes of requester-authored prose an operator prompt will carry.
/// Small on purpose: a purpose line is a sentence, and a decision surface that
/// can be flooded is a decision surface that can be used to push the trusted
/// facts off the top of somebody's screen.
inline constexpr std::size_t kMaxPurposeBytes = 200;

/// The most bytes a requested shape name or office name may have. Both become
/// rules in a `LiveAuthority` and both are shown to a person; neither is a
/// document.
inline constexpr std::size_t kMaxNameBytes = 128;

/// How many prompts one Weaver can name. A prompt's name is its Weaver's WeaveId times this, plus
/// the prompt's place in that Weaver's count, so no two Weavers on one bus put the same name: a
/// WeaveId is never reused, and a Weaver replacing another in its office has a WeaveId of its own.
/// docs/reference/weaver.md#a-decision-names-the-prompt-it-answers
inline constexpr std::int64_t kPromptNamesPerWeaver = 1000 * 1000 * 1000;

/// Render `raw` so it cannot drive the terminal it is printed on: every byte outside printable
/// ASCII becomes a visible `\xNN` and a literal backslash is doubled, so the escaping is
/// unambiguous. Only the first `max_bytes` of `raw` are rendered, and a cut says
/// `...[truncated]`. It lives at the Weaver rather than in a renderer, so every renderer of an
/// `AuthorityPrompt` inherits it. Non-ASCII arrives escaped, never translated.
/// docs/reference/weaver.md#untrusted-text-at-a-decision-surface
inline std::string safe_operator_text(std::string_view raw, std::size_t max_bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    const bool truncated = raw.size() > max_bytes;
    const std::string_view kept = truncated ? raw.substr(0, max_bytes) : raw;
    out.reserve(kept.size());
    for (const char c : kept) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte == '\\') {
            out += "\\\\"; // doubled, so a \xNN below is never ambiguous with authored text
        } else if (byte >= 0x20 && byte <= 0x7e) {
            out.push_back(c);
        } else {
            out += "\\x";
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0f]);
        }
    }
    if (truncated) {
        out += "...[truncated]"; // what is missing is stated, never merely absent
    }
    return out;
}

/// One send rule, as a person reads it: `Work v1 -> role some.service`. Rendered from the
/// snapshot the Kernel handed back, never from a string the Weaver kept, so it cannot disagree
/// with the bus. A role is printed as a role, never as the weave holding it now: role authority
/// follows whoever holds the office at delivery.
inline std::string render_rule(const SendRule& rule) {
    std::string out =
        rule.any_shape ? std::string("any shape")
                       : safe_operator_text(rule.shape_name, kMaxNameBytes) + " v" +
                             std::to_string(rule.shape_version);
    out += " -> ";
    if (rule.any_target) {
        out += "any target";
    } else if (!rule.target_role.empty()) {
        out += "role " + safe_operator_text(rule.target_role, kMaxNameBytes);
    } else {
        out += "weave #" + std::to_string(rule.target.value);
    }
    return out;
}

/// One observe rule, as a person reads it: `observe Tick v1`. Spelled with its
/// own verb because reading a shape and being allowed to send one are different
/// permissions, and an operator scanning a list must not have to infer which
/// kind a line is from its punctuation.
inline std::string render_rule(const ObserveRule& rule) {
    if (rule.any_shape) {
        return "observe any shape";
    }
    return "observe " + safe_operator_text(rule.shape_name, kMaxNameBytes) + " v" +
           std::to_string(rule.shape_version);
}

/// Every rule of one authority, rendered in the order the authority holds them.
inline std::vector<std::string> render_authority(const LiveAuthority& authority) {
    std::vector<std::string> out;
    out.reserve(authority.rules().size() + authority.observe_rules().size());
    for (const SendRule& r : authority.rules()) {
        out.push_back(render_rule(r));
    }
    for (const ObserveRule& r : authority.observe_rules()) {
        out.push_back(render_rule(r));
    }
    return out;
}

// ---- the Weaver's own state ------------------------------------------------

/// Workflow counters, and nothing else: no rule, no subject, no record of what was granted.
/// Both count what happened; neither is consulted to decide anything.
struct WeaverState {
    std::int64_t prompts = 0;   ///< authority requests put to the operator
    std::int64_t installed = 0; ///< approvals that actually installed a rule

    ZEN_SHAPE(WeaverState, 1, ZEN_FIELD(prompts), ZEN_FIELD(installed));
};

/// Bootstrapped by a host with the capability to administer one subject and the WeaveId of the
/// one weave whose decisions it obeys. The governed subject is read from the capability
/// (`GrantAuthority::subject()`), so the subject it checks requests against and the one it can
/// administer are one value.
class Weaver final
    : public WeaveBase<Weaver, WeaverState,
                       Accept<RequestAuthority, ApproveAuthority, RefuseAuthority, RevokeAuthority,
                              DescribeAuthority, ApproveAuthorityV1, RefuseAuthorityV1>,
                       Emit<AuthorityPrompt, AuthorityGranted, AuthorityDescription, Refused, Ack>> {
public:
    /// `authority` names the governed subject and the ceiling; `operator_seat` is the weave
    /// whose word counts as the user's. Both come from the host at bootstrap, and no message
    /// changes either. Throws `std::invalid_argument` when the seat is the governed subject: a
    /// session approving its own requests could widen itself up to the ceiling (GATE-05). A
    /// capability with no subject is accepted and governs nobody.
    Weaver(GrantAuthority authority, WeaveId operator_seat)
        : authority_(std::move(authority)), operator_seat_(operator_seat) {
        if (authority_.subject().valid() && operator_seat == authority_.subject()) {
            throw std::invalid_argument(
                "loom::Weaver: the operator seat must not be the governed session — a subject "
                "that decides its own authority requests can widen itself up to the ceiling");
        }
    }

    /// The one subject this Weaver governs — from the capability, so there is no
    /// second opinion available.
    WeaveId governed_session() const noexcept { return authority_.subject(); }

    /// The weave whose decisions this Weaver treats as the user's.
    WeaveId operator_seat() const noexcept { return operator_seat_; }

    /// Is a human decision outstanding? Answered by the answer right itself
    /// rather than by a bool beside it: one thing is pending exactly when there
    /// is one conversation waiting to be answered, so the flag and the capability
    /// cannot disagree about whether a request exists.
    bool has_pending_request() const noexcept { return answer_.valid(); }

    /// The name of the prompt awaiting a decision, or 0 when none is.
    std::int64_t pending_prompt() const noexcept {
        return has_pending_request() ? request_.prompt : 0;
    }

    /// A Weaver is never reloaded: a new incarnation would have dropped the request a person
    /// was deciding and the answer right with it, so a failure ends it visibly. Authority it
    /// installed outlives it, since a grant is not a lease (docs/reference/weaver.md).
    LifecyclePolicy policy_config() const { return LifecyclePolicy{0, true}; }

    // ---- the governed session speaks ---------------------------------------

    void on(const RequestAuthority& ask, Mail& mail) {
        // WHO ASKED IS A DELIVERY FACT. Not a payload field — there is none —
        // and not the reply address. A weave that merely manages to REACH this
        // Weaver does not thereby become the session it governs.
        if (mail.sender() != governed_session()) {
            (void)mail.answer(Refused{"authority requests here are only accepted from the one "
                                      "session this Weaver governs"});
            return;
        }
        std::string why;
        if (!well_formed(ask, &why)) {
            (void)mail.answer(Refused{std::move(why)});
            return;
        }
        // ONE PENDING REQUEST. A second ask does not displace the one a person is
        // deciding — it is refused, visibly, and the first stays exactly what the
        // operator was shown. Overwriting would silently retarget the next
        // approval onto a request the operator never saw.
        if (has_pending_request()) {
            (void)mail.answer(
                Refused{"another authority request from this session is already awaiting the "
                        "operator; this Weaver decides one at a time"});
            return;
        }
        const AuthorityView view = mail.describe_authority(authority_);
        if (!view.available) {
            (void)mail.answer(Refused{"this Weaver cannot administer that session right now"});
            return;
        }
        // ALREADY EFFECTIVE? Then nothing is being requested. Answering here
        // rather than prompting is not a shortcut: waking a person to approve
        // something that is already permitted teaches them to approve without
        // reading, and installing it again would grow a duplicate rule for no
        // authority gained. Asked through the bus's own predicate, over the bus's
        // own values, so "already permitted" means what delivery will mean.
        if (view.permits_role(ask.shape, static_cast<std::uint32_t>(ask.version), ask.to_role)) {
            (void)mail.answer(AuthorityGranted{"already-permitted"});
            return;
        }
        // A prompt's name is never reused, by this Weaver or another on its bus, so a Weaver
        // that cannot name one more puts no more prompts rather than repeat a name a decision
        // may still carry.
        const std::int64_t name = next_prompt_name();
        if (name == 0) {
            (void)mail.answer(Refused{"this Weaver cannot name another prompt; nothing was put "
                                      "to the operator"});
            return;
        }
        // TAKE THE ANSWER AWAY WITH US. The human is not in this call stack, and
        // may not be at the keyboard for minutes. Loom's deferral converts this
        // delivery's one answer right into one that outlives the handler — so the
        // eventual yes or no is still THE authenticated answer to THIS ask, bound
        // by Loom to the incarnation that asked. No local request map, no reply
        // token, no correlation of our own invention.
        DeferredAnswer taken = mail.defer_answer();
        if (!taken.valid()) {
            // Deferral refused (no answer authority to convert, or the bus is at
            // capacity). The immediate opportunity was not consumed, so say so
            // now rather than accepting a request nobody can ever be answered
            // about.
            (void)mail.answer(Refused{"this Weaver cannot hold an authority request open right "
                                      "now; try again"});
            return;
        }
        answer_ = std::move(taken);
        ++prompts_named_;
        request_ = Requested{name, ask.shape, static_cast<std::uint32_t>(ask.version), ask.to_role};
        ++state_.prompts;
        // The operator sees the prompt's name, the rule AS PARSED, the requester's
        // identity AS STAMPED, and the requester's prose AS ESCAPED — and nothing
        // that claims the destination office currently exists.
        (void)mail.send(operator_seat_,
                        AuthorityPrompt{request_.prompt,
                                        static_cast<std::int64_t>(mail.sender().value),
                                        request_.shape, static_cast<std::int64_t>(request_.version),
                                        request_.to_role, kGrantLifetime,
                                        safe_operator_text(ask.purpose, kMaxPurposeBytes)});
    }

    // ---- the human decides --------------------------------------------------

    void on(const ApproveAuthority& decision, Mail& mail) {
        if (!from_operator(mail) || !names_pending(decision.prompt, mail)) {
            return;
        }
        const AuthorityView view = mail.describe_authority(authority_);
        if (!view.available) {
            // The session died while the operator was deciding. WeaveIds are
            // never reused, so this authority can never be inherited by whatever
            // mounts next: abandon the conversation and install nothing.
            release_deferred(answer_, mail);
            clear_pending();
            (void)mail.answer(
                Refused{"the session that asked is gone; no authority was installed"});
            return;
        }
        // ADDITIVE, over the Kernel's own snapshot. Delegation is one atomic
        // replacement, so "approve one more thing" is spelled as "install
        // everything that is already delegated, plus this" — read fresh from the
        // enforcement store each time, never from a copy this Weaver kept.
        LiveAuthority next = view.delegated;
        next.allow_to_role(request_.shape, request_.version, request_.to_role);
        const GrantChange change = mail.delegate_authority(authority_, std::move(next));
        if (!change) {
            // THE HUMAN DOES NOT OUTRANK THE CAPABILITY. A yes that the ceiling
            // does not cover installs nothing, and both the session and the
            // operator are told which no it was.
            (void)answer_deferred(answer_, mail,
                                  Refused{"the operator approved, but this Weaver does not hold "
                                          "the authority to delegate that"});
            clear_pending();
            (void)mail.answer(Refused{refusal_for(change.outcome)});
            return;
        }
        ++state_.installed;
        // THE AUTHENTICATED ANSWER TO THE ORIGINAL ASK. Not a fresh message that
        // happens to be the right shape — Loom's own answer, which the session
        // reads back as `mail.answers_ask()`.
        (void)answer_deferred(answer_, mail, AuthorityGranted{"delegated"});
        clear_pending();
        (void)mail.answer(Ack{});
        // AND NOTHING ELSE HAPPENS. The Weaver does not send the session's
        // message for it, does not replay what was refused, and does not tell
        // anyone to retry. It changed authority; it does not resurrect intent.
    }

    void on(const RefuseAuthority& decision, Mail& mail) {
        if (!from_operator(mail) || !names_pending(decision.prompt, mail)) {
            return;
        }
        // The user said no, so the session hears no — as the authenticated answer
        // to the request it actually sent. A refusal is an answer, never silence:
        // a session left waiting cannot tell a decision from a lost message.
        (void)answer_deferred(answer_, mail, Refused{"the operator refused this request"});
        clear_pending();
        (void)mail.answer(Ack{});
    }

    /// A version 1 decision names no prompt, so it cannot say which request it decides: it
    /// changes nothing, whatever is pending, and the seat is told why.
    void on(const ApproveAuthorityV1&, Mail& mail) {
        if (from_operator(mail)) {
            refuse_unnamed(mail);
        }
    }
    void on(const RefuseAuthorityV1&, Mail& mail) {
        if (from_operator(mail)) {
            refuse_unnamed(mail);
        }
    }

    void on(const RevokeAuthority&, Mail& mail) {
        if (!from_operator(mail)) {
            return;
        }
        // THE WHOLE OVERLAY, AT ONCE. Installing the empty authority is
        // revocation, and the empty authority is contained by every ceiling, so
        // this can never itself be refused for being too wide. It takes back only
        // what was DELEGATED: the admission baseline is a union term the
        // delegation door cannot subtract from, so the session keeps its right to
        // come back and ask again.
        //
        // A request still awaiting the operator is deliberately NOT touched:
        // revoking acts on authority that is installed, and the pending question
        // is a separate thing the operator can still answer either way.
        const GrantChange change = mail.delegate_authority(authority_, LiveAuthority::nothing());
        if (!change) {
            (void)mail.answer(Refused{refusal_for(change.outcome)});
            return;
        }
        (void)mail.answer(Ack{});
    }

    void on(const DescribeAuthority&, Mail& mail) {
        // Two readers, one subject. The operator inspects what it governs; the
        // session inspects ITSELF, which tells it only what it could already
        // discover by trying things. Nobody else is answered, and nobody at all
        // can ask about another subject — the shape has no field for one.
        if (mail.sender() != operator_seat_ && mail.sender() != governed_session()) {
            (void)mail.answer(Refused{"this Weaver describes its governed session only to that "
                                      "session and to the operator seat"});
            return;
        }
        const AuthorityView view = mail.describe_authority(authority_);
        if (!view.available) {
            (void)mail.answer(Refused{"this Weaver has no administrable session to describe"});
            return;
        }
        // Rendered from the snapshot, at the moment of the ask. EFFECTIVE
        // authority is the union of the two lists and is therefore read, not
        // sent: a third list would be a materialized answer that could fall out
        // of date between here and delivery.
        (void)mail.answer(AuthorityDescription{static_cast<std::int64_t>(view.subject.value),
                                               render_authority(view.base),
                                               render_authority(view.delegated)});
    }

private:
    /// What a yes lasts for. Stated to the operator on every prompt because Loom
    /// has no one-shot grant and a decision surface that let a person assume
    /// otherwise would be describing a weaker grant than the one it is making.
    static constexpr const char* kGrantLifetime =
        "until the operator revokes, or the session dies (this is NOT a one-time allow)";

    /// The pending request, parsed, and the name of the prompt that showed it. It
    /// is the RULE, not the message: the requester's prose never reaches here, so
    /// there is nothing kept that could influence what gets installed.
    struct Requested {
        std::int64_t prompt = 0;
        std::string shape;
        std::uint32_t version = 0;
        std::string to_role;
    };

    /// Is this decision the user's? Possession of a send grant that happens to
    /// reach this Weaver is reachability, not identity — a distinct question from
    /// the one the bus answered by delivering the message at all. Refusing is
    /// speech and reveals nothing: the sender already knows it reached us, and
    /// the refusal names no seat.
    bool from_operator(Mail& mail) {
        if (mail.sender() == operator_seat_) {
            return true;
        }
        (void)mail.answer(Refused{"authority decisions here are only accepted from the configured "
                                  "operator seat"});
        return false;
    }

    /// Does this decision name the prompt pending now? A decision cannot be
    /// banked, and it cannot be moved: one naming no prompt, a prompt already
    /// decided, or one never put, changes nothing, leaves the pending prompt as it
    /// was, and does not wait around to be applied to whatever is asked next.
    bool names_pending(std::int64_t prompt, Mail& mail) {
        if (prompt <= 0) {
            refuse_unnamed(mail);
            return false;
        }
        if (!has_pending_request()) {
            (void)mail.answer(Refused{"this decision names prompt " + std::to_string(prompt) +
                                      ", and no authority request is pending; nothing was "
                                      "changed"});
            return false;
        }
        if (prompt != request_.prompt) {
            (void)mail.answer(Refused{
                "this decision names prompt " + std::to_string(prompt) +
                ", which is not the prompt awaiting a decision; nothing was changed, and prompt " +
                std::to_string(request_.prompt) + " still awaits one"});
            return false;
        }
        return true;
    }

    /// The name the next prompt takes, or 0 when this Weaver can name none: it was never told its
    /// own WeaveId, its WeaveId leaves no room for a name in an `int64`, or it has named every
    /// prompt `kPromptNamesPerWeaver` allows.
    std::int64_t next_prompt_name() const {
        constexpr std::int64_t kHighestNamingWeave =
            (std::numeric_limits<std::int64_t>::max() - (kPromptNamesPerWeaver - 1)) /
            kPromptNamesPerWeaver;
        if (!self_.valid() || self_.value > static_cast<std::uint64_t>(kHighestNamingWeave) ||
            prompts_named_ >= kPromptNamesPerWeaver - 1) {
            return 0;
        }
        return static_cast<std::int64_t>(self_.value) * kPromptNamesPerWeaver + prompts_named_ + 1;
    }

    void refuse_unnamed(Mail& mail) {
        (void)mail.answer(Refused{"this decision names no prompt, so it cannot say which request "
                                  "it decides; nothing was changed"});
    }

    void clear_pending() {
        answer_ = DeferredAnswer{}; // the right is spent or abandoned; hold nothing stale
        request_ = Requested{};
    }

    /// STRUCTURE ONLY. A request must name one shape, one version and one office,
    /// in bytes that are safe to print and short enough to read. It is
    /// deliberately NOT checked against the world: whether that role is currently
    /// held, and whether anything currently accepts that shape, are questions
    /// about configuration, not about authority — and answering them to an
    /// untrusted requester would turn a policy door into a service directory.
    static bool well_formed(const RequestAuthority& ask, std::string* why) {
        if (!plain_name(ask.shape)) {
            *why = "the requested shape name must be 1.." + std::to_string(kMaxNameBytes) +
                   " printable non-space characters";
            return false;
        }
        if (ask.version < 1 || ask.version > static_cast<std::int64_t>(UINT32_MAX)) {
            *why = "the requested shape version must be between 1 and 4294967295";
            return false;
        }
        if (!plain_name(ask.to_role)) {
            *why = "the requested destination role must be 1.." + std::to_string(kMaxNameBytes) +
                   " printable non-space characters";
            return false;
        }
        return true;
    }

    static bool plain_name(std::string_view s) {
        if (s.empty() || s.size() > kMaxNameBytes) {
            return false;
        }
        for (const char c : s) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte <= 0x20 || byte >= 0x7f) {
                return false; // control bytes, spaces and non-ASCII are not names
            }
        }
        return true;
    }

    /// Which no it was — written for a person, and each one sending that person
    /// somewhere different. A single "failed" would send all four to the same
    /// wrong place.
    ///
    /// None of these says whether the requested office exists. Loom checks
    /// authority BEFORE it resolves a role, precisely so a refusal cannot be used
    /// to enumerate what is running, and a policy refusal must not become the
    /// oracle the delivery path refuses to be.
    static std::string refusal_for(GrantOutcome outcome) {
        switch (outcome) {
        case GrantOutcome::ExceedsCeiling:
            return "that is outside the authority this Weaver may delegate; nothing was changed";
        case GrantOutcome::NoSuchSubject:
            return "the session that asked is gone; no authority was installed";
        case GrantOutcome::NoAuthority:
            return "this Weaver holds no administration capability; nothing was changed";
        case GrantOutcome::ForeignBoard:
            return "this Weaver's capability was issued by a different Loom; nothing was changed";
        case GrantOutcome::NoLiveDelivery:
            return "this Weaver has no live standing to administer; nothing was changed";
        case GrantOutcome::Installed:
            break;
        }
        return "the authority change did not take effect";
    }

    /// THE ONLY EXTRAORDINARY THING THIS WEAVE HOLDS. Not a bus, not a host, not
    /// a switchboard — a capability naming one subject and one ceiling.
    GrantAuthority authority_;
    /// Who the user is, decided by the host and unchangeable by any message.
    WeaveId operator_seat_;
    // ---- policy workflow state (NEVER authority state) ----------------------
    Requested request_{};
    DeferredAnswer answer_{};
    /// How many prompts this Weaver has named; the next takes the name after the last.
    std::int64_t prompts_named_ = 0;
};

} // namespace loom

#endif // ZEN_WEAVER_WEAVER_HPP
