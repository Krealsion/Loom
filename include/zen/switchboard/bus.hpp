#ifndef ZEN_SWITCHBOARD_BUS_HPP
#define ZEN_SWITCHBOARD_BUS_HPP

#include <zen/switchboard/message.hpp>

#include <cstddef>
#include <cstdint>

namespace zen::sb {

/// A handle to a queued delivery. After the delivery is pumped, the sender can
/// read its fate via Switchboard::outcome(). The zero ticket is invalid.
struct Ticket {
    std::uint64_t seq = 0;
    bool valid() const noexcept { return seq != 0; }
};

/// The abstract send/publish surface a Shard's handle() sends through. The
/// Switchboard implements it directly; a host adapter for a library Shard
/// implements it by forwarding serialized messages across the C ABI. Because a
/// Shard only ever sees this interface, the same Shard works whether it is
/// compiled in or loaded from a .so — and survives a future move to per-Shard
/// mailboxes unchanged.
class Bus {
public:
    virtual ~Bus() = default;

    /// Enqueue a directed delivery to `target`.
    virtual Ticket send(ShardId target, Message msg) = 0;

    /// Enqueue a delivery to every accepter of the payload's shape; returns the
    /// recipient count.
    virtual std::size_t publish(Message msg) = 0;

protected:
    Bus() = default;
    Bus(const Bus&) = default;
    Bus& operator=(const Bus&) = default;
};

} // namespace zen::sb

#endif // ZEN_SWITCHBOARD_BUS_HPP
