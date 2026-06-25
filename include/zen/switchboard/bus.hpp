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

protected:
    Bus() = default;
    Bus(const Bus&) = default;
    Bus& operator=(const Bus&) = default;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_BUS_HPP
