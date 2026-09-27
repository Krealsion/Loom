// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_BUS_HPP
#define ZEN_SWITCHBOARD_BUS_HPP

#include <zen/schema.hpp>
#include <zen/switchboard/grant.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/switchboard/sense.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace loom {

/// A queued attempt, from an ordinary directed or role-addressed send (office-authored ones
/// included, natively and across the ABI). It proves enqueue, not delivery; zero is invalid.
/// A host reads the outcome with Switchboard::outcome(); an opted-in sender learns some later
/// refusals by notice (docs/reference/messaging.md#sender-visible-dispatch-refusal). A loaded
/// weave's answer doors return success markers, not attempts (docs/reference/dynamic-abi.md).
struct Ticket {
    std::uint64_t seq = 0;
    bool valid() const noexcept { return seq != 0; }
};

/// The result of an office-authored publication (MSG-07): `authored` says whether the office
/// spoke at all, and `recipients` counts the fanout only when it did, so a refusal and an
/// authorized publication nobody accepts stay distinct.
struct OfficePublication {
    bool authored = false;
    std::size_t recipients = 0;

    explicit operator bool() const noexcept { return authored; }
};

/// The send and publish surface a Weave's handle() uses. The Switchboard implements it
/// directly; a host adapter for a library Weave forwards across the C ABI. A Weave sees only
/// this, so the same Weave works compiled in or loaded.
class Bus {
public:
    virtual ~Bus() = default;

    /// Enqueue a directed delivery to `target`.
    virtual Ticket send(WeaveId target, Message msg) = 0;

    /// Enqueue a delivery to every accepter of the payload's shape; returns the
    /// recipient count.
    virtual std::size_t publish(Message msg) = 0;

    /// Enqueue a directed delivery to whichever Weave holds `role`, resolved at delivery, so a
    /// "shape to role" grant survives the holder reloading. An unheld role is refused like an
    /// unknown target.
    virtual Ticket send_to_role(std::string_view role, Message msg) = 0;

    /// Answer the message being handled, once, to its sender, with Loom's word that this is the
    /// answer. A weave that asked a role cannot know which incarnation received it, so the right
    /// to answer comes with the delivery: Loom sets the recipient (the request's stamped sender)
    /// and the correlation (the request's own). It grants no reach: the answer is authorized
    /// against the answering weave's grant like any send. The default returns an invalid Ticket:
    /// a Bus that is not a live delivery has no answer to give.
    virtual Ticket answer(Message msg) {
        (void)msg;
        return Ticket{};
    }

    /// Take the answer right away with you (ANS-02): converts this delivery's answer
    /// opportunity into one that outlives the handler. Afterwards `answer()` provides nothing and
    /// a second deferral fails. Invalid when the delivery had no answer to convert. The default
    /// refuses.
    virtual DeferredAnswer make_deferred_answer() { return DeferredAnswer{}; }

    /// Spend a deferred answer. The bus checks it against the bound requester, respondent, both
    /// incarnations and correlation, and against the current speaker, which is why it is spent
    /// through the Bus a handler was handed. Consumed before queueing.
    virtual Ticket spend_deferred(const DeferredAnswer& answer, Message msg) {
        (void)answer;
        (void)msg;
        return Ticket{};
    }

    /// Abandon a deferred answer. The conversation ends, the requester is told nothing (there is
    /// no cancellation vocabulary), and the record is reclaimed at once.
    virtual void release_deferred(const DeferredAnswer& answer) { (void)answer; }

    /// Attach Loom's lifecycle attestation to a message about `target`'s freshly committed
    /// incarnation. Needs the capability (LifecycleAuthority) and is still authorized against the
    /// sender's grant. Loom records `sequence` from this call, not the payload, and binds the
    /// attestation to `target`.
    virtual Ticket announce_lifecycle(const LifecycleAuthority& authority, WeaveId target,
                                      Message msg, std::int64_t sequence) {
        (void)authority;
        (void)target;
        (void)msg;
        (void)sequence;
        return Ticket{};
    }

    // ---- deliberate office authorship (MSG-07) ------------------------------
    // A weave may author one statement as a role it holds; Loom verifies the membership then
    // and stamps the fact on the delivery. Each `office_*` verb takes the office first and the
    // ordinary verb's parameters after. The ordinary grant still authorizes the delivery. The
    // defaults refuse: an invalid Ticket or unauthored publication means nothing was queued,
    // and a refusal is never sent as personal speech instead.

    /// Author `msg` as `as_role`, to a direct target. An invalid Ticket means authorship was
    /// refused and nothing was queued.
    virtual Ticket office_send(std::string_view as_role, WeaveId target, Message msg) {
        (void)as_role;
        (void)target;
        (void)msg;
        return Ticket{};
    }

    /// Author `msg` as `as_role`, to whoever holds `to_role`: the office spoken for is verified
    /// now, the destination resolved at delivery, and the two are carried separately.
    virtual Ticket office_send_to_role(std::string_view as_role, std::string_view to_role,
                                       Message msg) {
        (void)as_role;
        (void)to_role;
        (void)msg;
        return Ticket{};
    }

    /// Author `msg` as `as_role`, published to every accepter. "Authorship refused" and
    /// "authorized, zero recipients" stay distinct.
    virtual OfficePublication office_publish(std::string_view as_role, Message msg) {
        (void)as_role;
        (void)msg;
        return OfficePublication{};
    }

    // ---- Senses (SENSE-01..05) -----------------------------------------------
    // Claiming sits beside send and publish: a participant deliberately makes something
    // available, personally or, deliberately, as an office it holds. Observing is the one verb
    // here that returns data rather than queueing; it is synchronous, and the reading owns its
    // value. The defaults refuse.

    /// Claim `value` personally. The shape must be in this weave's `Claims<...>`; an undeclared
    /// one is refused.
    virtual SenseClaimResult claim(Value value) {
        (void)value;
        return SenseClaimResult{};
    }

    /// Claim `value` as the office `as_role`, verified now; a refusal stores nothing and is never
    /// made a personal claim.
    virtual SenseClaimResult office_claim(std::string_view as_role, Value value) {
        (void)as_role;
        (void)value;
        return SenseClaimResult{};
    }

    /// The latest claim `author` made personally of `shape`. The schema is passed, not just its
    /// name and version, so a reading crossing the dynamic seam is re-admitted against the
    /// reader's own definition.
    virtual SenseReading observe(WeaveId author, std::shared_ptr<const Schema> shape) {
        (void)author;
        (void)shape;
        return SenseReading{};
    }

    /// The latest claim made as the office `role`. A predecessor's claim survives the role
    /// moving, marked stale, never relabelled as the successor's.
    virtual SenseReading observe_office(std::string_view role, std::shared_ptr<const Schema> shape) {
        (void)role;
        (void)shape;
        return SenseReading{};
    }

    // ---- administering another subject's live authority (GATE-05) ------------
    // Baseline authority enters at admission and never changes; delegated authority may be
    // replaced by a holder of a host-minted capability for one subject and one ceiling; the
    // bus checks their union at delivery. On the Bus, so an administrator is an ordinary weave
    // with a capability, never a holder of the Switchboard. Neither verb sends anything: the
    // subject retries its own action and the target sees the subject. The defaults refuse with
    // `NoLiveDelivery`.

    /// Replace, atomically, the delegated live authority of the subject this capability
    /// governs: grant, revoke, widen and narrow are this one call. Pass what the subject should
    /// hold from now on, or `LiveAuthority::nothing()`. A request outside the ceiling changes
    /// nothing. The admission grant and the containment fields are out of its reach.
    virtual GrantChange delegate_authority(const GrantAuthority& authority,
                                           LiveAuthority requested) {
        (void)authority;
        (void)requested;
        return GrantChange{};
    }

    /// Read the governed subject's baseline, delegated and effective message authority, through
    /// the predicates the bus itself uses. Only that subject: there is no argument for another.
    virtual AuthorityView describe_authority(const GrantAuthority& authority) {
        (void)authority;
        return AuthorityView{};
    }

    // ---- Joint publication of latest claims ------------------------------------
    // On the Bus, so an operator is an ordinary weave holding a capability and a claimant
    // offers from its own delivery (zen/switchboard/sense.hpp,
    // docs/reference/joint-publication.md). The defaults refuse with `NoLiveDelivery`. A loaded
    // weave's Bus overrides `offer_claim` only, so its operator verbs are refused by name.

    /// Claimant: offer the next value of my own key (me, the value's shape) for operation `op`:
    /// admitted against the declared claim-set, checked against the bound revision, and kept
    /// until commit or abort. Nothing is published here.
    virtual JointResult offer_claim(std::uint64_t op, Value value) {
        (void)op;
        (void)value;
        return JointResult{false, JointRefusal::NoLiveDelivery};
    }

    /// Operator: bind the exact claimants and current revisions of `keys` into one operation.
    /// Each key must hold a claim whose claimant holds a role in the authority's ceiling, and
    /// no other live operation may bind it. The authority must name this weave's current life
    /// and incarnation, or the call meets `NotOperator`.
    virtual JointBegin begin_joint(const JointAuthority& authority, std::vector<ClaimKey> keys) {
        (void)authority;
        (void)keys;
        return JointBegin{false, 0, JointRefusal::NoLiveDelivery};
    }

    /// Operator: publish. Revalidates everything, then exchanges every offered value into its
    /// claim record in one step. `ok` is the commitment: false means nothing was published. A
    /// failed revalidation of this operator's own Preparing operation aborts it with `why`; a
    /// request that was not its to make (another operator's operation, or a capability for a
    /// life or incarnation that is gone) is refused with no effect on that record.
    virtual JointResult commit_joint(const JointAuthority& authority, std::uint64_t op) {
        (void)authority;
        (void)op;
        return JointResult{false, JointRefusal::NoLiveDelivery};
    }

    /// Operator: end a Preparing operation without publishing; its offers are released.
    virtual JointResult cancel_joint(const JointAuthority& authority, std::uint64_t op) {
        (void)authority;
        (void)op;
        return JointResult{false, JointRefusal::NoLiveDelivery};
    }

    /// Operator: the operation's state and, when terminal, why.
    virtual JointStatus joint_status(const JointAuthority& authority, std::uint64_t op) {
        (void)authority;
        (void)op;
        return JointStatus{JointState::Missing, JointRefusal::NoLiveDelivery};
    }

    /// Operator: release a terminal record it has consumed, Committed or Aborted, so its slot
    /// may be reused (SENSE-07). The bus never reuses an unreleased record under its operator.
    /// Afterwards the operation reads Missing; the per-key facts stay on the claim records.
    /// Refused `WrongState` while Preparing (cancel it instead), `NoSuchOperation` once released.
    virtual JointResult release_joint(const JointAuthority& authority, std::uint64_t op) {
        (void)authority;
        (void)op;
        return JointResult{false, JointRefusal::NoLiveDelivery};
    }

protected:
    Bus() = default;
    Bus(const Bus&) = default;
    Bus& operator=(const Bus&) = default;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_BUS_HPP
