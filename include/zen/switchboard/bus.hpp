// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_BUS_HPP
#define ZEN_SWITCHBOARD_BUS_HPP

#include <zen/switchboard/message.hpp>

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

/// The result of an office-authored publication (R2D-0) — two facts that a bare
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

    /// TAKE THE ANSWER RIGHT AWAY WITH YOU (R2B-2). Converts this delivery's
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

    // ---- deliberate office authorship (R2D-0) -------------------------------
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

protected:
    Bus() = default;
    Bus(const Bus&) = default;
    Bus& operator=(const Bus&) = default;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_BUS_HPP
