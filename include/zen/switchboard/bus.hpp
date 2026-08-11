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

namespace loom {

/// A handle to a queued delivery. After the delivery is pumped, the sender can
/// read its fate via Switchboard::outcome(). The zero ticket is invalid.
struct Ticket {
    std::uint64_t seq = 0;
    bool valid() const noexcept { return seq != 0; }
};

/// The result of an office-authored publication (MSG-07) — two facts that a bare
/// count would collapse and that are different problems:
///
///   authorship refused          the sender does not hold the office it asked
///                               to speak for; NOTHING fanned out
///   authorized, zero listeners  the office spoke; nobody currently accepts
///                               the shape — the same honest 0 an ordinary
///                               publish can return
///
/// `authored` answers the authorship question; `recipients` is meaningful only
/// when it is true. Deliberately small: a publication result, not a Result
/// framework.
struct OfficePublication {
    bool authored = false;
    std::size_t recipients = 0;

    explicit operator bool() const noexcept { return authored; }
};

/// The abstract send/publish surface a Weave's handle() sends through. The
/// Switchboard implements it directly; a host adapter for a library Weave
/// implements it by forwarding serialized messages across the C ABI. Because a
/// Weave only ever sees this interface, the same Weave works whether it is
/// compiled in or loaded from a .so — and survives a future move to per-Weave
/// mailboxes unchanged.
class Bus {
public:
    virtual ~Bus() = default;

    /// Enqueue a directed delivery to `target`.
    virtual Ticket send(WeaveId target, Message msg) = 0;

    /// Enqueue a delivery to every accepter of the payload's shape; returns the
    /// recipient count.
    virtual std::size_t publish(Message msg) = 0;

    /// Enqueue a directed delivery to whichever Weave currently holds `role`. The
    /// role is resolved to its holder at delivery (singleton in this phase), so a
    /// grant of "shape -> role" survives the holder reloading. An unheld role
    /// degrades like an unknown target: the delivery is refused, never gated.
    virtual Ticket send_to_role(std::string_view role, Message msg) = 0;

    /// ANSWER THE MESSAGE BEING HANDLED — once, to whoever sent it, carrying
    /// Loom's word that this is that answer.
    ///
    /// THE LAW IT IMPLEMENTS: *a role tells Loom where to deliver an ask; an
    /// authenticated conversation tells the asker who actually received it and
    /// who may answer.* A weave that addresses a role cannot know which
    /// incarnation the routing decision picked, so it cannot pre-bind an answer's
    /// sender — which is precisely why a correlation and a shape were never
    /// enough. This closes that gap from the other end: the authority to answer
    /// is not something a weave *has*, it is something a DELIVERY confers, on
    /// exactly the weave that received it, for exactly one reply.
    ///
    /// What Loom binds, and the sender therefore cannot choose: the recipient is
    /// the request's stamped sender, and the correlation is the request's own.
    /// The one-shot is consumed on the first call.
    ///
    /// It grants NO new reach: the answer is authorized against the answering
    /// weave's ordinary grant at delivery, exactly like any other send. Holding
    /// the grant for a shape has never been, and still is not, authority to
    /// answer someone else's conversation.
    ///
    /// The default is "no authority here": a Bus that is not a live delivery
    /// (a library-side shim, a future mailbox) truthfully answers nothing and
    /// returns an invalid Ticket rather than pretending.
    virtual Ticket answer(Message msg) {
        (void)msg;
        return Ticket{};
    }

    /// TAKE THE ANSWER RIGHT AWAY WITH YOU (ANS-02). Converts this delivery's
    /// immediate answer opportunity into one that survives the handler's return.
    ///
    /// It CONSUMES the immediate opportunity rather than sitting beside it: after
    /// a successful deferral `answer()` provides nothing and a second
    /// `defer_answer()` fails, so a request still grants exactly one answer.
    /// Returns an invalid capability when this delivery has no answer authority to
    /// convert — an ordinary path that never earned one cannot be deferred into an
    /// authenticated answer.
    ///
    /// The default is "no authority here", the same truthful answer a Bus that is
    /// not a live delivery gives to `answer()`.
    virtual DeferredAnswer make_deferred_answer() { return DeferredAnswer{}; }

    /// Spend a deferred answer. `token` names bus-private state; the bus checks it
    /// against the bound requester, respondent, both incarnations and the original
    /// correlation, and against the CURRENT speaker — which is why this lives on
    /// the Bus a handler was handed rather than anywhere a capability could be
    /// carried to. Consumed before queueing, so reentrancy cannot double it.
    virtual Ticket spend_deferred(const DeferredAnswer& answer, Message msg) {
        (void)answer;
        (void)msg;
        return Ticket{};
    }

    /// Abandon a deferred answer without answering. The conversation ends; the
    /// requester is told nothing (V1 has no cancellation vocabulary) and the
    /// bus-side record is reclaimed immediately rather than waiting for death.
    virtual void release_deferred(const DeferredAnswer& answer) { (void)answer; }

    /// Attach Loom's lifecycle attestation to a message about `target`'s freshly
    /// committed incarnation. Requires the capability object — see
    /// LifecycleAuthority — and is still authorized against the sender's grant.
    ///
    /// `sequence` is recorded by Loom from THIS call, not read out of the
    /// payload, so an attestation issued for one sequence cannot authenticate
    /// another. Loom also binds the attestation to `target`: a proof minted for
    /// one incarnation is not a proof for a different one.
    virtual Ticket announce_lifecycle(const LifecycleAuthority& authority, WeaveId target,
                                      Message msg, std::int64_t sequence) {
        (void)authority;
        (void)target;
        (void)msg;
        (void)sequence;
        return Ticket{};
    }

    // ---- deliberate office authorship (MSG-07) ------------------------------
    //
    // THE LAW: *a weave may deliberately author one statement in the capacity of
    // a role it currently holds; Loom verifies that membership at the authorship
    // moment and carries the resulting office fact as immutable delivery
    // provenance. Merely holding the role attaches nothing.*
    //
    // Every `office_*` verb's FIRST parameter is the office being spoken for;
    // the remaining parameters are exactly the ordinary verb's. Authorship
    // changes why a receiver may trust WHO spoke — it never widens where the
    // sender may speak or what it may emit, so the ordinary grant still
    // authorizes the delivery afterwards, unchanged.
    //
    // The defaults refuse, truthfully: a Bus that is not a live speaking context
    // has no membership to verify and no standing to stamp an office fact. An
    // invalid Ticket / unauthored publication means NOTHING WAS QUEUED — a
    // refused authorship is never silently downgraded to personal speech.

    /// Author `msg` deliberately as `as_role`, delivered to a direct target.
    /// An invalid Ticket means authorship was refused and nothing was queued.
    virtual Ticket office_send(std::string_view as_role, WeaveId target, Message msg) {
        (void)as_role;
        (void)target;
        (void)msg;
        return Ticket{};
    }

    /// Author `msg` deliberately as `as_role`, delivered to whoever holds
    /// `to_role`. The two roles are DIFFERENT FACTS carried separately: the
    /// first is the office spoken for (verified at authorship), the second is
    /// the destination slot (resolved at delivery, exactly as send_to_role).
    virtual Ticket office_send_to_role(std::string_view as_role, std::string_view to_role,
                                       Message msg) {
        (void)as_role;
        (void)to_role;
        (void)msg;
        return Ticket{};
    }

    /// Author `msg` deliberately as `as_role`, published to every accepter.
    /// The result keeps "authorship refused" and "authorized, zero recipients"
    /// distinct — a publication's honest 0 must not be confusable with a
    /// refusal, and vice versa.
    virtual OfficePublication office_publish(std::string_view as_role, Message msg) {
        (void)as_role;
        (void)msg;
        return OfficePublication{};
    }

    // ---- Senses (SENSE-01..05) -----------------------------------------------
    //
    // THE LAW: *a Sense is a deliberate immutable claim of the latest observation
    // a participant has made available; reading one is synchronous, authorized,
    // and shares no memory with the claimant.*
    //
    // Claiming sits beside send/publish because it is the same kind of act — a
    // participant deliberately making something available — and the office form
    // reuses the `as_role` grammar because it is the SAME law (MSG-07): holding
    // an office is not speaking, or claiming, as one.
    //
    // Observing is the one verb on this surface that RETURNS DATA rather than
    // queueing anything. It is synchronous by design: that is the whole point of
    // the category. It confers nothing else — a reader cannot mutate the
    // claimant through a reading, because a reading owns its value outright.
    //
    // The defaults refuse, truthfully, exactly as the answer and office doors do:
    // a Bus that is not a live participating context has no identity to claim as
    // and no standing to read on anyone's behalf.

    /// Claim `value` personally. The shape must be in this weave's declared
    /// `Claims<...>` set; an undeclared shape is refused, never stored.
    virtual SenseClaimResult claim(Value value) {
        (void)value;
        return SenseClaimResult{};
    }

    /// Claim `value` deliberately AS the office `as_role`. Membership is verified
    /// at the claim moment; a refusal stores nothing and is never downgraded to a
    /// personal claim.
    virtual SenseClaimResult office_claim(std::string_view as_role, Value value) {
        (void)as_role;
        (void)value;
        return SenseClaimResult{};
    }

    /// The latest claim `author` made personally of `shape`.
    ///
    /// The SHAPE is passed, not just its (name, version), because a reading
    /// crossing the dynamic seam must be re-admitted against the reader's own
    /// definition of the shape on its own side. Handing back a value that passed
    /// only the *other* side's gate would be the one place in Loom a value
    /// arrived un-admitted.
    virtual SenseReading observe(WeaveId author, std::shared_ptr<const Schema> shape) {
        (void)author;
        (void)shape;
        return SenseReading{};
    }

    /// The latest claim made AS the office `role`. A predecessor's claim survives
    /// role movement, stamped stale — never relabelled as the successor's.
    virtual SenseReading observe_office(std::string_view role, std::shared_ptr<const Schema> shape) {
        (void)role;
        (void)shape;
        return SenseReading{};
    }

    // ---- administering another subject's live authority (GATE-05) ------------
    //
    // THE LAW: *baseline authority enters at admission and never changes;
    // delegated live authority may be replaced at any time by a holder of a
    // host-minted capability scoped to one subject and one ceiling; and EFFECTIVE
    // authority — baseline ∪ delegated — is what the bus checks at delivery.*
    //
    // These sit on the Bus, not on the Switchboard, because that is the whole
    // point: an administrator must be an ORDINARY WEAVE. Holding a `Switchboard&`
    // is being the host, and a Weaver that had to hold one to do its job would be
    // host root wearing a weave's name. What it holds instead is a capability
    // object, presented here, through the same handle every other weave has.
    //
    // NEITHER IS A SEND. No message is queued, no sender is stamped, and nothing
    // about the administrator appears in what the governed subject later says: the
    // subject retries its own action, under its own identity, and the target sees
    // the subject. That separation is the reason the administrator shape was
    // chosen over a broker in the first place, so it is protected here by there
    // being no message at all to carry an administrator's name.
    //
    // The defaults refuse, truthfully, exactly as the answer and office doors do:
    // a Bus that is not a live participating context has no board to check the
    // capability against, and says so (`NoLiveDelivery`) rather than pretending.

    /// Replace, atomically, the delegated live authority of the subject this
    /// capability governs. Grant, revoke, widen and narrow are all this one call:
    /// pass what the subject should hold from now on, or `LiveAuthority::nothing()`
    /// to take it all back. The request must be a semantic subset of the
    /// capability's ceiling; if it is not, NOTHING changes.
    ///
    /// It never touches the admission grant, so a revocation cannot cost a subject
    /// authority the host gave it — and it never reaches the containment fields,
    /// which `LiveAuthority` has no words for.
    virtual GrantChange delegate_authority(const GrantAuthority& authority,
                                           LiveAuthority requested) {
        (void)authority;
        (void)requested;
        return GrantChange{};
    }

    /// Read the governed subject's baseline, delegated and effective message
    /// authority — the same values, through the same predicates, that the bus
    /// itself will use. Scoped to the one subject the capability names; there is
    /// no argument by which to ask about another.
    virtual AuthorityView describe_authority(const GrantAuthority& authority) {
        (void)authority;
        return AuthorityView{};
    }

protected:
    Bus() = default;
    Bus(const Bus&) = default;
    Bus& operator=(const Bus&) = default;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_BUS_HPP
