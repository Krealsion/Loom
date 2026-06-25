#ifndef ZEN_SWITCHBOARD_MESSAGE_HPP
#define ZEN_SWITCHBOARD_MESSAGE_HPP

#include <zen/value.hpp>

#include <cstdint>
#include <utility>

namespace loom {

/// A stable handle to a registered Weave, assigned by the Switchboard. The
/// zero id is the null/invalid handle (e.g. "no reply_to", "from outside").
struct WeaveId {
    std::uint64_t value = 0;

    bool valid() const noexcept { return value != 0; }
    friend bool operator==(WeaveId a, WeaveId b) noexcept { return a.value == b.value; }
    friend bool operator!=(WeaveId a, WeaveId b) noexcept { return a.value != b.value; }
};

/// The routed envelope. The payload is a self-describing Value — its own schema
/// is its routing shape. Routing metadata rides alongside it: who sent it, an
/// optional reply address, and an optional opaque correlation token so a handler
/// can reply by sending (synchronous await is a deliberate seam, not built here).
struct Message {
    Value payload;
    WeaveId sender{};
    WeaveId reply_to{};
    std::uint64_t correlation = 0;

    explicit Message(Value payload_, WeaveId sender_ = {}, WeaveId reply_to_ = {},
                     std::uint64_t correlation_ = 0)
        : payload(std::move(payload_)), sender(sender_), reply_to(reply_to_),
          correlation(correlation_) {}
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_MESSAGE_HPP
