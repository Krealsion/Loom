// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss
#ifndef ZEN_SWITCHBOARD_TURN_HPP
#define ZEN_SWITCHBOARD_TURN_HPP

// The turn's time: what the host's wait read when it ended, carried by every delivery of the turn
// that wait began, as a fact beside the sender and the correlation. Never in a shape and never a
// sender's to write. A unit the bus consults: the host hands it the facts, a dispatch call takes
// them for its deliveries and lets them go when it returns, so a turn no wait began carries none.
// docs/decisions/the-turns-time-on-every-delivery.md

#include <cstdint>
#include <optional>

namespace loom {

/// The two readings, taken together when the wait ended: `ms` on the host's steady clock, for
/// durations and deadlines, and `date_ms` on the calendar (milliseconds since 1970-01-01 UTC),
/// for a person and a record. Nothing is scheduled on the date.
struct TurnFacts {
    std::int64_t ms = 0;
    std::int64_t date_ms = 0;
};

/// The facts the next dispatch call carries, and the ones the current one does.
class TurnTime {
public:
    /// The host's wait ended with these readings: the next turn carries them.
    void begin(TurnFacts facts) noexcept { next_ = facts; }
    /// A dispatch call starts: it takes what the wait handed over, if anything.
    void open() noexcept {
        now_ = next_;
        next_.reset();
    }
    /// It returns: its deliveries are done, and the facts go with them.
    void close() noexcept { now_.reset(); }
    /// What a delivery dispatched now carries.
    std::optional<TurnFacts> now() const noexcept { return now_; }

private:
    std::optional<TurnFacts> next_;
    std::optional<TurnFacts> now_;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_TURN_HPP
