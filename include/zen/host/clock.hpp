// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_CLOCK_HPP
#define ZEN_HOST_CLOCK_HPP

// THE TIME A HOST'S ONE WAIT KEEPS, given by its host. The wait (zen/host/reactor.hpp) reads it to
// know which trusted source is due and how long to rest, and hands each source the reading that
// woke it, so no handler reads a clock of its own. A program gives the machine's; a suite gives
// one that moves only when told, and a wait that finds nothing ready moves it to the soonest
// deadline instead of sleeping until it. Its date is the calendar beside it, read with each
// reading the wait hands the bus, for a person and a record: nothing is scheduled on it.
// docs/guides/running-loom.md#a-source-that-is-due-at-a-time

#include <chrono>
#include <cstdint>
#include <limits>

namespace loom::host {

/// A clock a host gives its wait. Readings are milliseconds on the clock's own scale, never going
/// back; only differences between two readings of one clock mean anything.
class Clock {
public:
    virtual ~Clock() = default;

    /// The reading now.
    virtual std::int64_t now_ms() const = 0;

    /// Whether this clock moves while the thread waits. The machine's does, so a wait rests until
    /// its deadline comes. One that does not is moved by the wait instead: a wait that finds
    /// nothing ready calls `arrive` with its soonest deadline and returns at once.
    virtual bool moves_on_its_own() const noexcept = 0;

    /// The wait found nothing ready before `ms`, its soonest deadline, which is later than
    /// `now_ms()`: a clock that does not move on its own reads `ms` from now on. Called only on
    /// such a clock.
    virtual void arrive(std::int64_t ms) = 0;

    /// The calendar now, in milliseconds since 1970-01-01 UTC: for a person and a record, never a
    /// deadline, since it may move back or jump. The machine's calendar unless the clock keeps
    /// its own.
    virtual std::int64_t date_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
};

/// THE MACHINE'S MONOTONIC CLOCK, as `std::chrono::steady_clock` reads it. What a wait keeps when
/// its host names no other.
class MachineClock final : public Clock {
public:
    std::int64_t now_ms() const override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    bool moves_on_its_own() const noexcept override { return true; }
    void arrive(std::int64_t) override {}
};

/// A CLOCK THAT MOVES ONLY WHEN TOLD: by its case (`advance_to`, `advance_by`) or by a wait that
/// found nothing ready before its soonest deadline. A suite runs its host's real loop on it and
/// says "until this time"; nothing it waits on is counted as time. It keeps the date it is given,
/// which moves with its readings: `date_ms` is that date plus how far it has moved since.
class ManualClock final : public Clock {
public:
    explicit ManualClock(std::int64_t start_ms = 0, std::int64_t date = 0) noexcept
        : now_(start_ms), start_(start_ms), date_(date) {}

    std::int64_t now_ms() const override { return now_; }
    std::int64_t date_ms() const override { return date_ + (now_ - start_); }
    bool moves_on_its_own() const noexcept override { return false; }
    void arrive(std::int64_t ms) override { advance_to(ms); }

    /// Read `ms` from now on, or stay where it is if that is earlier: it never goes back.
    void advance_to(std::int64_t ms) noexcept {
        if (ms > now_) {
            now_ = ms;
        }
    }
    /// Move on `ms`; zero or less leaves it where it is. A move past the largest reading stops
    /// there.
    void advance_by(std::int64_t ms) noexcept {
        if (ms <= 0) {
            return;
        }
        constexpr std::int64_t top = std::numeric_limits<std::int64_t>::max();
        now_ = now_ > 0 && ms > top - now_ ? top : now_ + ms;
    }

private:
    std::int64_t now_;
    std::int64_t start_; ///< the reading the date was given at
    std::int64_t date_;  ///< the date at that reading
};

} // namespace loom::host

#endif // ZEN_HOST_CLOCK_HPP
