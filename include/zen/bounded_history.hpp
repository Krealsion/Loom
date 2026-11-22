// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_BOUNDED_HISTORY_HPP
#define ZEN_BOUNDED_HISTORY_HPP

// A bounded window on the past: the retention primitive every long-running observer surface
// needs, written once (the console's tap and replies, a terminal participant's transcript and
// received messages). Everything stored in one is history, past observations on which nothing is
// owed, which is what makes discarding the oldest legitimate here and illegitimate for a
// backlog: a queue of work must not drop work, and this must not grow without bound. Never
// silently: `evicted()` is monotonic and answers "is this the complete history?".

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace loom {

/// A bounded history window: the most recent `Capacity` observations in
/// chronological order, the oldest discarded — and COUNTED — when a new one
/// arrives at capacity.
///
/// A window may also hold its observations to a WEIGHT budget (`WeightBudget`, 0 for none):
/// each observation is pushed with its weight, and the oldest are discarded, and counted the
/// same way, until the new one fits beside the rest. The value windows weigh a kept value by the
/// host memory it holds (`loom::ValueWeight::held_bytes`), so what they keep is bounded as a
/// whole and not at one value's worst times their count. An observation heavier than the whole
/// budget is kept alone.
///
/// A ring over one vector, the same shape as the Switchboard's journal
/// (`journal_[seq % cap]`): the slots are claimed once at construction and never
/// reallocated, an insert allocates nothing and is O(1) for the life of the
/// window (a weighed one discards as many as it must), and no front-erasure ever shifts a tail.
/// Storage is a function of the capacity alone — never of how many observations have passed
/// through it. A discarded slot is emptied, so what it held is released at once.
///
/// The slots are reserved UP FRONT rather than grown into deliberately. Leaving
/// it to `push_back` would also be bounded, but only because the capacities in
/// use happen to be powers of two; reserving makes "exactly `Capacity` slots,
/// always" a property of this class instead of a property of the standard
/// library's growth policy, and lets the storage assertions in the tests state
/// that directly.
///
/// Deliberately not a general retention framework: no policies, no
/// configuration, no persistence.
template <class T, std::size_t Capacity, std::uint64_t WeightBudget = 0>
class BoundedHistory {
    static_assert(Capacity > 0, "a history window must retain at least one observation");

public:
    BoundedHistory() {
        ring_.reserve(Capacity);
        weights_.reserve(Capacity);
    }

    /// Record one observation. At capacity this discards the oldest, in place, and counts it;
    /// under a weight budget, it first discards the oldest until `weight` fits beside the rest.
    void push(T v, std::uint64_t weight = 0) {
        if constexpr (WeightBudget > 0) {
            while (count_ > 0 && weight > WeightBudget - held_) {
                drop_oldest();
            }
            if (weight > WeightBudget) {
                weight = WeightBudget; // kept alone: nothing else fits beside it
            }
        } else {
            weight = 0;
        }
        if (count_ == Capacity) {
            drop_oldest();
        }
        const std::size_t at = head_ + count_;
        if (ring_.size() < Capacity) {
            // Still filling: the retained run is [head_, size()), so the next slot is the end.
            ring_.emplace_back(std::move(v));
            weights_.push_back(weight);
        } else {
            ring_[at % Capacity].emplace(std::move(v)); // the emptied slot after the newest
            weights_[at % Capacity] = weight;
        }
        held_ += weight;
        ++count_;
    }

    /// How many are retained right now (<= Capacity).
    std::size_t size() const noexcept { return count_; }

    /// How many observations were discarded to keep the window bounded. Monotonic for the life of
    /// this window; it is the reader's answer to "is this the complete history?".
    std::uint64_t evicted() const noexcept { return evicted_; }

    /// The weight held now, at most `WeightBudget` (0 for a window with none).
    std::uint64_t weight() const noexcept { return held_; }

    /// The i-th RETAINED entry, 0 = oldest retained. A position within the window, never a stable
    /// identity — the caller owns whatever identity it publishes (see ConsoleEngine::buffer_at and
    /// TerminalSession::received).
    const T& at(std::size_t i) const { return *ring_[(head_ + i) % Capacity]; }

    /// A chronological snapshot, oldest retained first.
    std::vector<T> snapshot() const {
        std::vector<T> out;
        out.reserve(count_);
        for (std::size_t i = 0; i < count_; ++i) {
            out.push_back(at(i));
        }
        return out;
    }

private:
    void drop_oldest() {
        const std::size_t at = head_ % Capacity;
        ring_[at].reset(); // what it held goes now, not when its slot is next written
        held_ -= weights_[at];
        weights_[at] = 0;
        head_ = (head_ + 1) % Capacity;
        --count_;
        ++evicted_;
    }

    /// The LIFE-07 instrument, reused: "size() stays at the capacity" and "the backing storage stops
    /// growing" are DIFFERENT claims, and only the second is what a process running for weeks needs.
    /// Reading the window's own slot count states the second directly instead of inferring it from
    /// process RSS, which is allocator- and OS-sensitive. Adds no member and no code path.
    friend struct ConsoleHistoryProbe;
    /// ...and the same instrument for the terminal transcript, for the same reason. Two probes
    /// rather than one shared name because the two suites assert about different owners, and a
    /// probe that reached into both would make a failure ambiguous about which window grew.
    friend struct TerminalHistoryProbe;

    std::vector<std::optional<T>> ring_;
    std::vector<std::uint64_t> weights_; ///< each slot's weight, beside it
    std::size_t head_ = 0;               ///< index of the oldest retained entry
    std::size_t count_ = 0;              ///< entries retained
    std::uint64_t held_ = 0;             ///< their weight
    std::uint64_t evicted_ = 0;
};

} // namespace loom

#endif // ZEN_BOUNDED_HISTORY_HPP
