// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_WEAVE_HPP
#define ZEN_WEAVE_WEAVE_HPP

// The weave-authoring layer, over the raw contract in zen/switchboard/weave_contract.hpp
// (zen/weave.hpp is the umbrella). A maker writes a state struct, message structs (ZEN_SHAPEs)
// and a typed handler per message, `void on(const Ping&, Mail&)`. The accept-set comes from
// the Accept<...> list, snapshot and revive from the State struct, and dispatch converts a
// Value to its struct only after the gate has admitted it. docs/guides/writing-a-weave.md

#include <zen/weave/describe.hpp>
#include <zen/weave/dispatch_refusal.hpp>
#include <zen/weave/poke.hpp>
#include <zen/weave/shape.hpp>
#include <zen/kernel/schema_codec.hpp>
#include <zen/switchboard.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace loom {

/// The shapes a Weave accepts (its doors) and the shapes it declares it emits. Both register
/// their definitions at mount, with everything they nest, so two weaves' `Pong v1` are
/// compared when the second mounts, natively and across a library seam.
///
/// `Emit<...>` grants nothing (a trusted `mount<>` derives send rules from it; `mount_granted`
/// and a loaded artifact's host decide authority themselves), and it is not an exhaustive send
/// list: an undeclared shape is not refused for that reason.
/// docs/decisions/declared-vocabulary-is-agreed-at-admission.md
template <class... S>
struct Accept {};
template <class... S>
struct Emit {};

/// The Senses a weave declares it can claim: its claim-set (SENSE-04). `Accept<...>` is what
/// may be delivered here, `Emit<...>` what may be sent from here, and `Claims<...>` what this
/// weave may say it observes. Registered at mount like the others, and, unlike `Emit<...>`,
/// enforced: claiming an undeclared shape is refused `SenseRefusal::Undeclared`.
template <class... S>
struct Claims {};

/// The Switchboard's lifecycle-policy grammar, typed. Not a registered shape: it targets
/// lifecycle_policy_schema() directly.
struct LifecyclePolicy {
    std::int64_t max_reloads = 4;
    bool revive_from_last_good = true;
};

/// The typed context a handler receives: the inbound envelope, and replies, sends and publishes
/// of plain structs. It is the one outbound path a woven weave's own code takes. It does not
/// check a send against `Emit<...>`: a router may emit shapes chosen at runtime, and that seam
/// stays open (docs/decisions/declared-vocabulary-is-agreed-at-admission.md).
class Mail {
public:
    Mail(loom::Bus& bus, const loom::Message& in, loom::WeaveId self)
        : bus_(bus), in_(in), self_(self) {}

    loom::Bus& bus() const { return bus_; }
    loom::WeaveId sender() const { return in_.sender; }
    loom::WeaveId reply_to() const { return in_.reply_to; }
    std::uint64_t correlation() const { return in_.correlation; }
    bool dispatch_refused() const noexcept { return in_.provenance.dispatch_refused(); }

    /// Reply to the inbound sender's reply address, echoing the correlation.
    template <class T>
    loom::Ticket reply(const T& msg) {
        return bus_.send(in_.reply_to,
                         loom::Message(to_value(msg), self_, self_, in_.correlation));
    }
    template <class T>
    loom::Ticket send(loom::WeaveId target, const T& msg, std::uint64_t correlation = 0) {
        return bus_.send(target, loom::Message(to_value(msg), self_, loom::WeaveId{},
                                                  correlation));
    }
    template <class T>
    std::size_t publish(const T& msg, std::uint64_t correlation = 0) {
        return bus_.publish(loom::Message(to_value(msg), self_, loom::WeaveId{}, correlation));
    }
    /// Send `msg` to whichever Weave holds `role`, resolved at delivery. The grant must permit
    /// the shape to that role (Grant::allow_to_role).
    template <class T>
    loom::Ticket send_to_role(std::string_view role, const T& msg,
                                 std::uint64_t correlation = 0) {
        return bus_.send_to_role(
            role, loom::Message(to_value(msg), self_, loom::WeaveId{}, correlation));
    }

    // ---- deliberate office authorship (MSG-07) ------------------------------
    // Holding an office is never speaking for it: `mail.send(...)` from a holder is personal
    // speech. Speaking as the office is a deliberate act per statement:
    //
    //     mail.as_role("matchmaker").send(player, MatchCreated{server});
    //     mail.as_role("worker.a").send_to_role("dispatcher", JobDone{...});
    //
    // Loom verifies the office then, and the recipient reads it with `authored_from_role()`.

    /// This weave speaking as `role`, for the statements made through the view. It carries a
    /// name and no authority: every statement is verified at the bus. Not copyable or movable,
    /// with rvalue-qualified verbs, so it is used in one expression:
    /// `mail.as_role("worker.a").publish(WorkerOpen{...});`
    class Office {
    public:
        Office(const Office&) = delete;
        Office& operator=(const Office&) = delete;
        Office(Office&&) = delete;
        Office& operator=(Office&&) = delete;

        /// Office-authored direct send. An invalid Ticket means authorship was refused (this weave
        /// does not hold the office; `RoleAuthorshipDenied` on the tap) and nothing was queued.
        template <class T>
        loom::Ticket send(loom::WeaveId target, const T& msg,
                          std::uint64_t correlation = 0) && {
            return mail_.bus_.office_send(
                role_, target, loom::Message(to_value(msg), mail_.self_, loom::WeaveId{},
                                             correlation));
        }

        /// Office-authored send to whoever holds `to_role`: authorship is verified now, the
        /// destination resolves at delivery.
        template <class T>
        loom::Ticket send_to_role(std::string_view to_role, const T& msg,
                                  std::uint64_t correlation = 0) && {
            return mail_.bus_.office_send_to_role(
                role_, to_role, loom::Message(to_value(msg), mail_.self_, loom::WeaveId{},
                                              correlation));
        }

        /// Office-authored publication. `authored` says whether the office spoke; `recipients`
        /// counts the fanout only when it did.
        template <class T>
        loom::OfficePublication publish(const T& msg, std::uint64_t correlation = 0) && {
            return mail_.bus_.office_publish(
                role_, loom::Message(to_value(msg), mail_.self_, loom::WeaveId{}, correlation));
        }

        /// Claim `observation` as this office (SENSE-04). The claim-set must declare the shape
        /// and this weave must hold the office now. A refusal stores nothing, and is never made a
        /// personal claim instead.
        template <class T>
        loom::SenseClaimResult claim(const T& observation) && {
            return mail_.bus_.office_claim(role_, to_value(observation));
        }

    private:
        friend class Mail;
        Office(Mail& mail, std::string_view role) : mail_(mail), role_(role) {}

        Mail& mail_;
        std::string_view role_;
    };

    /// Speak as `role` for the statements chained onto the result. This call checks and grants
    /// nothing; each statement is verified at the bus.
    Office as_role(std::string_view role) { return Office(*this, role); }

    // ---- Senses (SENSE-01..05; docs/reference/senses.md) --------------------

    /// Claim `observation` personally: "this is what I most recently claim is so". `T` must be
    /// in this weave's `Claims<...>`. Visible from this call on, so a reader dispatched later
    /// sees it. A role holder's personal claim is not an office claim: use
    /// `as_role(R).claim(...)`.
    template <class T>
    loom::SenseClaimResult claim(const T& observation) {
        return bus_.claim(to_value(observation));
    }

    /// The latest claim `author` made personally of shape `T`, read synchronously: a copy the
    /// caller owns, with no reference into the claimant. The latest claim Loom accepted, which
    /// queued work may already make stale; nothing is applied speculatively.
    template <class T>
    loom::SenseReading latest(loom::WeaveId author) {
        return bus_.observe(author, schema_of<T>());
    }

    /// The latest claim made as the office `role`. After the role moves this is still the
    /// predecessor's, with `by.office_holder_is_current = false`; the successor has claimed
    /// nothing until it does. The strict reading is `if (r && r.by.office_holder_is_current)`.
    template <class T>
    loom::SenseReading latest_from_office(std::string_view role) {
        return bus_.observe_office(role, schema_of<T>());
    }

    /// Was this delivery deliberately authored as `role`, verified when it was authored? False
    /// for the same holder's personal speech, and for an empty `role`. A fact about the
    /// statement, not about now. Trust the office through this, the exact weave through
    /// `sender()`.
    bool authored_from_role(std::string_view role) const {
        return in_.provenance.authored_from_role(role);
    }

    /// The office this delivery was deliberately authored as; empty for personal speech.
    std::string_view authored_role() const { return in_.provenance.authored_role(); }

    // ---- authenticated lifecycle conversation (ANS-01, LIFE-04) -------------
    // A role says where an ask is delivered; an authenticated conversation says who received it
    // and who may answer.

    /// Answer the message being handled, once, to its sender, with Loom's word that this is the
    /// answer. Loom sets the recipient and correlation. Answering twice, or a root's delivery,
    /// is refused visibly, never sent as ordinary speech instead.
    template <class T>
    loom::Ticket answer(const T& msg) {
        return bus_.answer(loom::Message(to_value(msg), self_));
    }

    /// Take the answer right away with you (ANS-02), for a responder whose answer depends on
    /// messages not yet received. It converts the one opportunity: afterwards `answer()` provides
    /// nothing and a second `defer_answer()` fails.
    ///
    ///     void on(const Prepare&, loom::Mail& mail) {
    ///         pending_ = mail.defer_answer();
    ///         begin_preparation();
    ///     }
    ///     void on(const Complete& c, loom::Mail& mail) {
    ///         answer_deferred(pending_, mail, Prepared{c.result});
    ///     }
    ///
    /// Invalid when this delivery had no answer authority to convert.
    loom::DeferredAnswer defer_answer() { return bus_.make_deferred_answer(); }

    /// Is this delivery the authorized answer to a request this weave sent? It says the answer is
    /// genuine, not that it is the one awaited: match the correlation against your own ask.
    bool answers_ask() const { return in_.provenance.answers_ask(); }

    /// Does Loom attest a lifecycle commit for the incarnation receiving this? Bound to this
    /// target: an attestation for another incarnation never arrives with this flag.
    bool lifecycle_attested() const { return in_.provenance.lifecycle_activation(); }

    /// The sequence Loom attested, to compare with the payload's own, so an attestation for one
    /// activation cannot authenticate another.
    std::int64_t attested_sequence() const { return in_.provenance.attested_sequence(); }

    /// Announce a lifecycle commit for `target` with Loom's attestation. Needs the host-granted
    /// capability, and is still gated by the ordinary grant.
    template <class T>
    loom::Ticket announce_lifecycle(const loom::LifecycleAuthority& authority,
                                    loom::WeaveId target, const T& msg, std::int64_t sequence) {
        return bus_.announce_lifecycle(authority, target,
                                       loom::Message(to_value(msg), self_, loom::WeaveId{},
                                                     /*correlation=*/0),
                                       sequence);
    }

    /// Replace the delegated live authority of the subject this capability governs (GATE-05):
    /// grant, revoke, widen and narrow are this one call, made from an ordinary handler. Nothing
    /// is sent; the subject stays the sender of whatever it retries.
    loom::GrantChange delegate_authority(const loom::GrantAuthority& authority,
                                         loom::LiveAuthority requested) {
        return bus_.delegate_authority(authority, std::move(requested));
    }

    /// The subject's baseline, delegated and effective authority, as the bus itself reads them.
    loom::AuthorityView describe_authority(const loom::GrantAuthority& authority) {
        return bus_.describe_authority(authority);
    }

    // ---- Joint publication -----------------------------------------------------
    // A claimant offers the next value of a claim it declared, for the operation an operator
    // named; an operator holding a host-minted `JointAuthority` binds, commits, cancels, reads
    // and releases. The bus authenticates each verb against this Mail's live delivery. A loaded
    // weave can offer; its operator verbs are refused `NoLiveDelivery`.
    // docs/reference/joint-publication.md

    /// Claimant: offer `next` as my key's value for `op`; nothing is published yet.
    template <class T>
    loom::JointResult offer(std::uint64_t op, const T& next) {
        return bus_.offer_claim(op, to_value(next));
    }

    /// Operator: bind the exact claimants and revisions of `keys`.
    loom::JointBegin begin_joint(const loom::JointAuthority& authority,
                                 std::vector<loom::ClaimKey> keys) {
        return bus_.begin_joint(authority, std::move(keys));
    }
    /// Operator: publish. `ok` is the commitment, and nothing else is.
    loom::JointResult commit_joint(const loom::JointAuthority& authority, std::uint64_t op) {
        return bus_.commit_joint(authority, op);
    }
    loom::JointResult cancel_joint(const loom::JointAuthority& authority, std::uint64_t op) {
        return bus_.cancel_joint(authority, op);
    }
    loom::JointStatus joint_status(const loom::JointAuthority& authority, std::uint64_t op) {
        return bus_.joint_status(authority, op);
    }
    /// Operator: retire a terminal record this weave has consumed; the operation then reads
    /// Missing and its slot is free.
    loom::JointResult release_joint(const loom::JointAuthority& authority, std::uint64_t op) {
        return bus_.release_joint(authority, op);
    }

private:
    loom::Bus& bus_;
    const loom::Message& in_;
    loom::WeaveId self_;
};

/// Name one latest-claim key as an operator binds it: by the exact weave, or by the office whose
/// holder the bus resolves at begin.
template <class T>
loom::ClaimKey claim_key(loom::WeaveId claimant) {
    const std::shared_ptr<const loom::Schema> s = schema_of<T>();
    return loom::ClaimKey{claimant, std::string(), s->name(), s->version()};
}
template <class T>
loom::ClaimKey claim_key(std::string_view role) {
    const std::shared_ptr<const loom::Schema> s = schema_of<T>();
    return loom::ClaimKey{loom::WeaveId{}, std::string(role), s->name(), s->version()};
}

/// Spend a deferred answer, or abandon it. Free functions because `DeferredAnswer` lives
/// below the authoring layer and knows nothing of `Mail`. The current `Mail` is half the
/// check: the bus refuses unless the weave speaking now is the exact incarnation that earned
/// the right. Unlike `answer()`, no delivery need be in progress, since outliving the delivery
/// is the point.
template <class T>
loom::Ticket answer_deferred(const loom::DeferredAnswer& pending, Mail& mail, const T& msg) {
    return mail.bus().spend_deferred(pending, loom::Message(to_value(msg)));
}

/// Abandon it. The requester is not told (there is no cancellation vocabulary); the bus
/// reclaims the slot at once.
inline void release_deferred(const loom::DeferredAnswer& pending, Mail& mail) {
    mail.bus().release_deferred(pending);
}

/// The CRTP base. A Weave is
///   class Node : public WeaveBase<Node, Counter, Accept<Ping>, Emit<Pong>> {
///       void on(const Ping&, loom::Mail&) { ... }   // one per accepted shape
///   };
/// with a ZEN_SHAPE State; the protected `state_` is the live state.
template <class Self, class State, class AcceptList, class EmitList = Emit<>,
          class ClaimList = Claims<>>
class WeaveBase;

template <class Self, class State, class... A, class... E, class... C>
class WeaveBase<Self, State, Accept<A...>, Emit<E...>, Claims<C...>> : public loom::Weave {
    // The zen.Poke* shapes are the construction layer's doors, answered from the declared
    // access model so a woven Weave cannot misreport its structure; one listed in Accept<...>
    // could never fire. The is_base_of terms reject a derived alias sharing the content id.
    static_assert((!is_poke_protocol_shape<A> && ...) &&
                      (!std::is_base_of_v<PokeDescribe, A> && ...) &&
                      (!std::is_base_of_v<PokeRead, A> && ...) &&
                      (!std::is_base_of_v<PokeWrite, A> && ...) &&
                      (!std::is_base_of_v<PokeResetState, A> && ...),
                  "loom: the zen.Poke* protocol shapes are answered by the construction layer; "
                  "do not list them in Accept<...>");

    // zen.DescribeAccepted is answered from this class's accepted_schemas(); a maker who could
    // intercept it could describe a vocabulary the gate does not enforce.
    static_assert((!std::is_same_v<DescribeAccepted, A> && ...) &&
                      (!std::is_base_of_v<DescribeAccepted, A> && ...),
                  "loom: zen.DescribeAccepted is answered by the construction layer from your "
                  "declared accept-set; do not list it in Accept<...>");

public:
    /// The maker's declared doors plus five substrate doors: four poke doors (every woven Weave
    /// can be inspected) and the self-description door. `final`, so the substrate doors are
    /// always truthfully advertised. The Switchboard matches deliveries against this vector and
    /// the self-description door answers from it, so the two cannot differ.
    std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const final {
        std::vector<std::shared_ptr<const loom::Schema>> out{schema_of<A>()...};
        for (auto& s : poke_door_schemas()) {
            out.push_back(std::move(s));
        }
        for (auto& s : describe_door_schemas()) {
            out.push_back(std::move(s));
        }
        return out;
    }

    /// The shapes this Weave declares it emits: the maker's `Emit<...>`, never the construction
    /// layer's answers. Registered at mount with the accept-set, so a divergent definition
    /// refuses registration. `final`, like the accept-set; not a send gate.
    std::vector<std::shared_ptr<const loom::Schema>> emitted_schemas() const final {
        return {schema_of<E>()...};
    }

    /// The Senses this Weave declares it can claim (SENSE-04): registered at mount, answerable
    /// as discovery, and checked by the claim doors. `final`, like the accept-set.
    std::vector<std::shared_ptr<const loom::Schema>> claimed_schemas() const final {
        return {schema_of<C>()...};
    }

    loom::Value snapshot() const override { return to_value(state_); }
    void revive(const loom::Value& v) override { state_ = from_value<State>(v); }

    loom::Value policy() const override {
        const LifecyclePolicy p = static_cast<const Self*>(this)->policy_config();
        loom::Value v(loom::lifecycle_policy_schema());
        v.set("max_reloads", loom::Cell::integer(p.max_reloads));
        v.set("revive_from_last_good", loom::Cell::boolean(p.revive_from_last_good));
        return v;
    }

    void handle(const loom::Message& in, loom::Bus& bus) final {
        // `final`: dispatch goes through the loom::Weave vtable, so an override would bypass
        // the poke doors. A maker wanting its own dispatch implements loom::Weave directly and
        // advertises no poke doors. A bare ZEN_EXPOSE(); or ZEN_HIDE(); in the weave class
        // instead of the State struct would do nothing, and failing open for HIDE is refused.
        static_assert(!(has_whole_state_tag<Self>() && !has_whole_state_tag<State>()),
                      "loom: a bare ZEN_EXPOSE();/ZEN_HIDE(); belongs inside the State struct "
                      "(the ZEN_SHAPE type), not the weave class — it is read from the state type");
        // The poke doors are answered before maker dispatch, from the state's access model.
        Self* self = static_cast<Self*>(this);
        if (const PokeOutcome poked = try_poke(in, bus); poked != PokeOutcome::NotAPoke) {
            // A poke that wrote the state changed it like a delivery, so the maker's
            // end-of-delivery hook runs; a read, a describe or a refused write does not
            // (docs/reference/senses.md#authority).
            if (poked == PokeOutcome::Mutated) {
                if constexpr (requires(Self* s, Mail& m) { s->after_delivery(m); }) {
                    Mail mail(bus, in, self_);
                    self->after_delivery(mail);
                }
            }
            return;
        }
        // The self-description door, also before maker dispatch; it changes nothing.
        if (try_describe(in, bus)) {
            return;
        }
        Mail mail(bus, in, self_);
        // A delivered message passed the gate against one accepted schema, and the handler is
        // chosen by the same identity, so exactly one matches. No match means the accept-set and
        // the handlers drifted apart, which cannot happen since both are A...; say so loudly.
        const bool routed = (dispatch_to<A>(self, in, mail) || ...);
        if (!routed) {
            throw std::logic_error(
                "loom::WeaveBase: delivered message of shape '" +
                in.payload.schema().name() + " v" +
                std::to_string(in.payload.schema().version()) +
                "' matched no handler — accept-set and handler set are out of sync");
        }
        // The maker's end-of-delivery hook, run after the handler inside the same delivery, so a
        // weave deriving a mirror or claim from its state does it once; also after a poke that
        // wrote the state.
        if constexpr (requires(Self* s, Mail& m) { s->after_delivery(m); }) {
            self->after_delivery(mail);
        }
    }

    /// The bus published one of this weave's declared claims by a joint operation. Routed to
    /// `Self::on_claim_published(const T&)` for the `T` in `Claims<...>` the value carries; a
    /// shape with no handler is applied as it stands. `final`, like `handle`.
    ///
    /// The handler's return type is its answer: `void` applied it; `bool` is true for applied
    /// and false for declined (the owner keeps state of its own and re-claims it at its next
    /// delivery); `loom::Weave::PublishedClaim` says any of the three. A throw is a failure that
    /// holds the weave, recorded by the bus before it is rethrown; expected non-application is
    /// a decline, never a throw. The hook runs outside any delivery, with no Mail: apply or
    /// decline, and begin no work from it.
    /// docs/reference/joint-publication.md#the-showing-and-its-three-answers
    loom::Weave::PublishedClaim claim_published(const loom::Value& v) final {
        [[maybe_unused]] Self* self = static_cast<Self*>(this); // unused with an empty claim-set
        loom::Weave::PublishedClaim answer = loom::Weave::PublishedClaim::Applied;
        (void)(published_as<C>(self, v, answer) || ...);
        return answer;
    }

    /// Set by mount(); used as the sender id of emitted messages.
    void zen_set_self(loom::WeaveId id) { self_ = id; }

    /// Default lifecycle policy; a Self may declare its own policy_config().
    LifecyclePolicy policy_config() const { return LifecyclePolicy{}; }

    /// The capabilities this Weave asks of the host: advice, never authority. Empty by default;
    /// a Self declares its own with ZEN_ASK.
    loom::CapabilityAsk ask_config() const { return {}; }

    /// The manifest hook the export layer calls: the ask, or nothing when it is empty.
    std::optional<loom::CapabilityAsk> zen_requested_capabilities() const {
        loom::CapabilityAsk a = static_cast<const Self*>(this)->ask_config();
        if (!a.network && a.filesystem.empty() && a.roles.empty()) {
            return std::nullopt;
        }
        return a;
    }

protected:
    State state_{};
    loom::WeaveId self_{};

private:
    // Select the handler as the bus selected the door: by same_identity, which compares name,
    // version and content id. Matching on content id alone could, on a hash collision within
    // one accept-set, convert a value the gate checked against another schema; the name and
    // version rule that out, so from_value<S>'s precondition holds.
    template <class S>
    bool dispatch_to(Self* self, const loom::Message& in, Mail& mail) {
        if (!loom::same_identity(*schema_of<S>(), in.payload.schema())) {
            return false;
        }
        self->on(from_value<S>(in.payload), mail);
        return true;
    }

    /// `dispatch_to`'s twin for a joint-published claim, matched by the same identity. Reads
    /// three return types, `PublishedClaim`, `bool` and `void`; any other fails to compile, so an
    /// unreadable answer is never recorded as Applied.
    template <class S>
    bool published_as(Self* self, const loom::Value& v, loom::Weave::PublishedClaim& answer) {
        if (!loom::same_identity(*schema_of<S>(), v.schema())) {
            return false;
        }
        if constexpr (requires(Self* s, const S& x) { s->on_claim_published(x); }) {
            using Answer = decltype(self->on_claim_published(std::declval<const S&>()));
            if constexpr (std::is_same_v<Answer, loom::Weave::PublishedClaim>) {
                answer = self->on_claim_published(from_value<S>(v));
            } else if constexpr (std::is_same_v<Answer, bool>) {
                answer = self->on_claim_published(from_value<S>(v))
                             ? loom::Weave::PublishedClaim::Applied
                             : loom::Weave::PublishedClaim::Declined;
            } else if constexpr (std::is_void_v<Answer>) {
                self->on_claim_published(from_value<S>(v));
                answer = loom::Weave::PublishedClaim::Applied;
            } else {
                static_assert(std::is_void_v<Answer>,
                              "on_claim_published must return void (applied), bool (true "
                              "applied / false declined) or loom::Weave::PublishedClaim; any "
                              "other return type cannot be read as an answer and is refused "
                              "rather than recorded as Applied");
            }
        }
        return true;
    }

    // ---- the poke doors (answered by the substrate; see poke.hpp) ------------

    /// Substrate answers use the same authority as a maker's: a reply to the stamped requester
    /// is an authenticated answer, one redirected elsewhere is ordinary grant-checked speech, and
    /// a root request with no reply address gets none.
    template <class Answer>
    void answer_poke(const loom::Message& in, loom::Bus& bus, const Answer& answer) {
        answer_substrate(in, bus, to_value(answer));
    }

    /// The same send, for a substrate answer built as a Value (zen.AcceptedShapes): one
    /// reply-addressing rule for every substrate answer.
    void answer_substrate(const loom::Message& in, loom::Bus& bus, loom::Value answer) {
        const loom::WeaveId to = in.reply_to.valid() ? in.reply_to : in.sender;
        if (!to.valid()) {
            return;
        }
        // A reply to the requester spends this delivery's answer right; a redirected reply is
        // ordinary speech, which cannot attest a conversation with somebody who did not ask. An
        // answer seeds no answer (ANS-01), and a refused answer is never resent as ordinary speech.
        if (to == in.sender) {
            (void)bus.answer(loom::Message(std::move(answer), self_, self_, in.correlation));
            return;
        }
        bus.send(to, loom::Message(std::move(answer), self_, self_, in.correlation));
    }

    /// What a poke door came to: not a poke; answered without touching the state; or an Ack
    /// that wrote the state, the only one that changes it.
    enum class PokeOutcome : std::uint8_t { NotAPoke, Answered, Mutated };

    /// Answer the four protocol shapes from the declared access model, matched as dispatch_to
    /// matches.
    PokeOutcome try_poke(const loom::Message& in, loom::Bus& bus) {
        const loom::Schema& shape = in.payload.schema();
        if (loom::same_identity(*schema_of<PokeDescribe>(), shape)) {
            answer_poke(in, bus, poke_structure<State>());
            return PokeOutcome::Answered;
        }
        if (loom::same_identity(*schema_of<PokeRead>(), shape)) {
            const PokeRead req = from_value<PokeRead>(in.payload);
            std::visit([&](const auto& a) { answer_poke(in, bus, a); },
                       poke_read(state_, req.field));
            return PokeOutcome::Answered;
        }
        if (loom::same_identity(*schema_of<PokeWrite>(), shape)) {
            const PokeWrite req = from_value<PokeWrite>(in.payload);
            const std::variant<Ack, Refused> answer = poke_write(state_, req.field, req.value);
            std::visit([&](const auto& a) { answer_poke(in, bus, a); }, answer);
            return std::holds_alternative<Ack>(answer) ? PokeOutcome::Mutated
                                                       : PokeOutcome::Answered;
        }
        if (loom::same_identity(*schema_of<PokeResetState>(), shape)) {
            const std::variant<Ack, Refused> answer = poke_reset(state_);
            std::visit([&](const auto& a) { answer_poke(in, bus, a); }, answer);
            return std::holds_alternative<Ack>(answer) ? PokeOutcome::Mutated
                                                       : PokeOutcome::Answered;
        }
        return PokeOutcome::NotAPoke;
    }

/// Answer zen.DescribeAccepted from this weave's accepted_schemas(), the vector the
/// Switchboard holds as its doors. The answer includes the request's own shape, which is
/// accepted; being fieldless, it adds nothing to the closure. Sent like the poke answers; a
/// weave without allow_describe_answers is `CapabilityDenied` at delivery.
    bool try_describe(const loom::Message& in, loom::Bus& bus) {
        if (!loom::same_identity(*schema_of<DescribeAccepted>(), in.payload.schema())) {
            return false;
        }
        answer_substrate(in, bus, encode_accepted_shapes(accepted_schemas()));
        return true;
    }
};

/// The grant a Weave's declared Emit<...> implies: each emitted shape to any accepter. The
/// trusted in-process default, where the Weave's declaration is taken as its authority; an
/// untrusted Weave is given an explicit grant instead (mount_granted).
template <class Weave>
loom::Grant emit_default_grant(const Weave& weave) {
    loom::Grant grant;
    for (const auto& schema : weave.emitted_schemas()) {
        grant.allow_to_any(schema->name(), schema->version());
    }
    return grant;
}

/// Construct a trusted Weave, grant it its declared Emit set and the substrate answers (poke
/// and self-description), register it and set its self-id; returns its WeaveId.
template <class Self, class... Args>
loom::WeaveId mount(loom::Switchboard& bus, Args&&... args) {
    auto weave = std::make_unique<Self>(std::forward<Args>(args)...);
    Self* raw = weave.get();
    loom::Grant grant = emit_default_grant(*raw);
    loom::allow_poke_answers(grant);
    loom::allow_describe_answers(grant);
    loom::WeaveId id = bus.register_weave(std::move(weave), std::move(grant));
    raw->zen_set_self(id);
    return id;
}

/// As mount(), with a host-supplied grant used as given, for a Weave whose declared Emit is not
/// trusted as authority. Without allow_poke_answers(grant) or allow_describe_answers(grant),
/// poke and describe requests are still answered but the answers are `CapabilityDenied` at
/// delivery, on the tap; the two are separate so a host can allow either alone.
template <class Self, class... Args>
loom::WeaveId mount_granted(loom::Switchboard& bus, loom::Grant grant, Args&&... args) {
    auto weave = std::make_unique<Self>(std::forward<Args>(args)...);
    Self* raw = weave.get();
    loom::WeaveId id = bus.register_weave(std::move(weave), std::move(grant));
    raw->zen_set_self(id);
    return id;
}

} // namespace loom

/// Declare the capabilities a Weave asks the host for, shadowing WeaveBase::ask_config:
///   ZEN_ASK(.network = true, .filesystem = "write-scoped", .roles = {"storage"});
/// Advice only: the grant stays the host's decision. Omit it to ask for nothing.
#define ZEN_ASK(...)                                                                                \
    ::loom::CapabilityAsk ask_config() const {                                               \
        return ::loom::CapabilityAsk{__VA_ARGS__};                                           \
    }                                                                                               \
    static_assert(true, "") /* swallow the trailing semicolon */

#endif // ZEN_WEAVE_WEAVE_HPP
