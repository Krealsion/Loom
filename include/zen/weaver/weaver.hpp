// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVER_WEAVER_HPP
#define ZEN_WEAVER_WEAVER_HPP

// THE WEAVER — the first message-driven delegate of a human being's authority
// decisions.
// docs/reference/weaver.md
//
//     USER          decides
//     WEAVER        delegates / revokes
//     SESSION       acts
//     SWITCHBOARD   enforces and attributes
//
// Or: the Kernel enforces, the Weaver decides, the session acts. GATE-05 built
// the mechanism by which a host may appoint an administrator for one live
// subject's speech; this is the first real policy actor to hold one, and its
// whole job is to put that mechanism in front of a person and then get out of
// the way.
//
// IT IS AN ORDINARY WEAVE. It has an ordinary WeaveId, an ordinary Grant, an
// ordinary Mail, ordinary handlers reached by ordinary messages, and it answers
// through Loom's own answer authority. It holds no `Switchboard&`, so it is not
// a host; it has no privileged send path, so it cannot speak as anybody else.
// Its ONE extraordinary power is the `GrantAuthority` the host minted for it —
// and that capability is not a way to send anything at all.
//
// THE TWO POWERS ARE SEPARATE, AND THE HOST GRANTS THEM SEPARATELY:
//
//     the ordinary Grant   what this Weaver may SAY
//     the GrantAuthority   what this Weaver may DELEGATE
//
// Conflating them would make every send rule a Weaver happens to own into a
// delegable right, and would mean a Weaver could not answer a policy question
// without thereby gaining the power to grant what the answer is about. A Weaver
// permitted to say `zen.AuthorityGranted` has gained no authority to grant
// anything; a Weaver holding a wide ceiling has gained no new speech.
//
// WHAT IT REMEMBERS, AND WHAT IT REFUSES TO REMEMBER. It keeps POLICY WORKFLOW
// state — at most one request awaiting a human, and the answer right it took
// away with it. It keeps NO authority state: no map of what it thinks it
// granted, no ledger beside `AuthorityView`, no cache of effective rules. Every
// time it needs to know what a subject may do, it asks
// `mail.describe_authority(...)`, which reads the very values `deliver_one`
// reads through the very predicates `deliver_one` applies. The Kernel's store
// wins because it is the only store.
//
// ONE OF EVERYTHING, DELIBERATELY: one operator seat, one governed subject, one
// ceiling, at most one request in flight. Every one of those is a place a V2
// could grow a map, a queue, an id space and an ordering policy — and every one
// of those maps is a place a decision can land on the wrong request. The terminal and
// multiple sessions can earn concurrency when there is something to concur.
//
// WHAT IT IS NOT, said plainly because the surrounding words are grand: this
// governs Loom MESSAGE AUTHORITY and nothing else. It does not contain hostile
// in-process native code (a loaded weave shares this address space), does not
// authenticate a remote human, holds no accounts, persists nothing across a
// restart, and cannot make an "allow once" grant because Loom has no such thing.

// DELIBERATELY ABSENT: <zen/host/grant_wiring.hpp>. A Weaver USES a capability;
// it does not mint one. Pulling the host-wiring header in here would make
// `host_grant_authority` visible to every weave that includes this file, which
// is exactly the reachability that header exists to deny — and the name would
// still be useless, since minting needs a `Switchboard&` no weave holds. The
// host includes it, mints, and passes the result to this constructor.
#include <zen/switchboard/grant.hpp>
#include <zen/weave.hpp>
#include <zen/weaver/vocabulary.hpp>

#include <cstddef>
#include <cstdint>
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

/// RENDER `raw` SO IT CANNOT DRIVE THE TERMINAL IT IS PRINTED ON.
///
/// A security prompt is the last place to print attacker-chosen bytes verbatim:
/// an escape sequence in a "purpose" string could reposition the cursor, erase
/// the line above it, or repaint the trusted facts of the request the operator
/// is deciding. So every byte outside printable ASCII becomes a visible `\xNN`,
/// a literal backslash is doubled so the escaping is unambiguous, and the result
/// is bounded — with the truncation SAID rather than performed silently.
///
/// IT LIVES HERE, AT THE WEAVER, RATHER THAN IN A SKIN, and that placement is
/// the guarantee: the terminal console, a future Workshop pane and anything else
/// that ever renders an `AuthorityPrompt` inherit the safety without knowing it
/// exists. A sanitizer in one renderer protects one renderer.
///
/// ASCII-ONLY IS A REAL V1 LIMITATION, not an oversight: a non-ASCII purpose
/// arrives escaped rather than translated, because deciding which non-ASCII
/// bytes a given terminal will treat as printable is a question this phase has
/// no way to answer honestly. An internationalized operator surface belongs to
/// whoever builds the real terminal.
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

/// One send rule, as a person reads it: `Work v1 -> role some.service`.
///
/// Generated from the snapshot the Kernel handed back — never from a display
/// string kept alongside what the Weaver believes it granted. If this line and
/// the bus ever disagreed, this line would be the lie, so it is not allowed to
/// have an independent existence.
///
/// A ROLE IS PRINTED AS A ROLE. "role some.service" is not "weave #19", however
/// certain the Weaver may be about who holds the office right now: role authority
/// follows whoever holds it at delivery, and saying otherwise would describe a
/// narrower, more permanent grant than the one being made.
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

/// WORKFLOW COUNTERS, AND DELIBERATELY NOTHING ELSE.
///
/// A Weaver's persistable state is the one place a shadow permission database
/// would grow, so it is worth saying what is absent: there is no rule here, no
/// subject here, and no record of what was granted. Both fields count things
/// that HAPPENED; neither is consulted to decide anything.
struct WeaverState {
    std::int64_t prompts = 0;   ///< authority requests put to the operator
    std::int64_t installed = 0; ///< approvals that actually installed a rule

    ZEN_SHAPE(WeaverState, 1, ZEN_FIELD(prompts), ZEN_FIELD(installed));
};

/// THE FIRST USER-POLICY DELEGATE.
///
/// Bootstrapped by a host with two things and no others: the capability to
/// administer ONE subject, and the WeaveId of the ONE weave whose decisions it
/// will obey. It learns which session it governs FROM THE CAPABILITY
/// (`GrantAuthority::subject()`) rather than from a second constructor
/// parameter — so "the subject I check requests against" and "the subject I can
/// actually administer" are one value and cannot drift apart.
class Weaver final
    : public WeaveBase<Weaver, WeaverState,
                       Accept<RequestAuthority, ApproveAuthority, RefuseAuthority, RevokeAuthority,
                              DescribeAuthority>,
                       Emit<AuthorityPrompt, AuthorityGranted, AuthorityDescription, Refused, Ack>> {
public:
    /// `authority` names the governed subject and the ceiling; `operator_seat` is
    /// the exact weave whose word counts as the user's. Both come from the host,
    /// out of band, at bootstrap — there is no message that can change either.
    ///
    /// THE DECIDER MAY NOT BE THE SUBJECT, and this refuses at the boot that
    /// wires it rather than at the decision that abuses it. A Weaver whose
    /// operator seat IS its governed session is a session that approves its own
    /// requests: "no weave can widen its own authority" (GATE-05) would then be
    /// false, defeated not by a bug but by one line of host wiring, silently.
    /// It is a host misconfiguration, so it fails the way `register_weave`
    /// fails a null weave or a doubly-held role — loudly, before anything runs.
    ///
    /// A wholly inert capability (no subject at all) is deliberately still
    /// constructible: it governs nobody, so there is nobody for the seat to be.
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

    /// A Weaver does not reload. A revived one would come back with no pending
    /// request and no answer right, having silently dropped a decision a person
    /// was in the middle of making — so it fails visibly instead. That a dead
    /// Weaver leaves previously-installed authority standing is separate, real,
    /// and documented (an installed grant is not a lease).
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
        request_ = Requested{ask.shape, static_cast<std::uint32_t>(ask.version), ask.to_role};
        ++state_.prompts;
        // The operator sees the rule AS PARSED, the requester's identity AS
        // STAMPED, and the requester's prose AS ESCAPED — and nothing that claims
        // the destination office currently exists.
        (void)mail.send(operator_seat_,
                        AuthorityPrompt{static_cast<std::int64_t>(mail.sender().value),
                                        request_.shape, static_cast<std::int64_t>(request_.version),
                                        request_.to_role, kGrantLifetime,
                                        safe_operator_text(ask.purpose, kMaxPurposeBytes)});
    }

    // ---- the human decides --------------------------------------------------

    void on(const ApproveAuthority&, Mail& mail) {
        if (!from_operator(mail)) {
            return;
        }
        if (!has_pending_request()) {
            // A decision cannot be banked. An approval with nothing pending
            // changes nothing and, crucially, does not wait around to be applied
            // to whatever is requested next.
            (void)mail.answer(Refused{"no authority request is pending; nothing was changed"});
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

    void on(const RefuseAuthority&, Mail& mail) {
        if (!from_operator(mail)) {
            return;
        }
        if (!has_pending_request()) {
            (void)mail.answer(Refused{"no authority request is pending; nothing was changed"});
            return;
        }
        // The user said no, so the session hears no — as the authenticated answer
        // to the request it actually sent. A refusal is an answer, never silence:
        // a session left waiting cannot tell a decision from a lost message.
        (void)answer_deferred(answer_, mail, Refused{"the operator refused this request"});
        clear_pending();
        (void)mail.answer(Ack{});
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

    /// The pending request, parsed. It is the RULE, not the message: the
    /// requester's prose never reaches here, so there is nothing kept that could
    /// influence what gets installed.
    struct Requested {
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
};

} // namespace loom

#endif // ZEN_WEAVER_WEAVER_HPP
