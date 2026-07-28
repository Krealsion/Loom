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

protected:
    Bus() = default;
    Bus(const Bus&) = default;
    Bus& operator=(const Bus&) = default;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_BUS_HPP
