// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_BOUNDED_HISTORY_HPP
#define ZEN_BOUNDED_HISTORY_HPP

// A BOUNDED WINDOW ON THE PAST — the retention primitive every long-running
// observer surface in this tree needs, written once.
//
// It arrived with the console (COLD-2 C-1), whose own comment already named the
// reason it should live somewhere shared: "four console history surfaces need
// exactly these semantics, and writing the ring index arithmetic and the
// eviction counter four times is how one of them ends up wrong." TERM-0 made
// that a fifth and a sixth (a terminal participant's transcript and its received
// messages), so the class moved down here rather than being copied — this header
// is a RELOCATION, not a new design: the code, the invariants and the friend
// probe are the ones the console suite has been proving all along.
//
// WHAT IT IS FOR, precisely. Everything stored in one of these is HISTORY: past
// observations kept for inspection, on which NOTHING IS OWED. That is what makes
// discarding the oldest entry legitimate here and illegitimate for a backlog — a
// queue of work must not silently drop work, and this must not silently grow
// without bound. Neither is a substitute for the other.
//
// AND NEVER SILENTLY: `evicted()` is monotonic and is the caller's answer to "is
// this the complete history?". A bounded diagnostic surface that pretended to be
// complete would trade a memory lie for an observability lie.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace loom {

/// A bounded history window: the most recent `Capacity` observations in
/// chronological order, the oldest discarded — and COUNTED — when a new one
/// arrives at capacity.
///
/// A ring over one vector, the same shape as the Switchboard's journal
/// (`journal_[seq % cap]`): the slots are claimed once at construction and never
/// reallocated, an insert allocates nothing and is O(1) for the life of the
/// window, and no front-erasure ever shifts a tail. Storage is a function of the
/// capacity alone — never of how many observations have passed through it.
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
template <class T, std::size_t Capacity>
class BoundedHistory {
    static_assert(Capacity > 0, "a history window must retain at least one observation");

public:
    BoundedHistory() { ring_.reserve(Capacity); }

    /// Record one observation. At capacity this discards the oldest, in place, and counts it.
    void push(T v) {
        if (ring_.size() < Capacity) {
            ring_.push_back(std::move(v)); // still filling: chronological order is insertion order
            return;
        }
        ring_[oldest_] = std::move(v);       // overwrite the oldest slot — no allocation, no shift
        oldest_ = (oldest_ + 1) % Capacity;  // ... and its successor is the new oldest
        ++evicted_;
    }

    /// How many are retained right now (<= Capacity).
    std::size_t size() const noexcept { return ring_.size(); }

    /// How many observations were discarded to keep the window bounded. Monotonic for the life of
    /// this window; it is the reader's answer to "is this the complete history?".
    std::uint64_t evicted() const noexcept { return evicted_; }

    /// The i-th RETAINED entry, 0 = oldest retained. A position within the window, never a stable
    /// identity — the caller owns whatever identity it publishes (see ConsoleEngine::buffer_at and
    /// TerminalSession::received).
    const T& at(std::size_t i) const { return ring_[(oldest_ + i) % Capacity]; }

    /// A chronological snapshot, oldest retained first.
    std::vector<T> snapshot() const {
        std::vector<T> out;
        out.reserve(ring_.size());
        for (std::size_t i = 0; i < ring_.size(); ++i) {
            out.push_back(at(i));
        }
        return out;
    }

private:
    /// The R2F-C instrument, reused: "size() stays at the capacity" and "the backing storage stops
    /// growing" are DIFFERENT claims, and only the second is what a process running for weeks needs.
    /// Reading the window's own slot count states the second directly instead of inferring it from
    /// process RSS, which is allocator- and OS-sensitive. Adds no member and no code path.
    friend struct ConsoleHistoryProbe;
    /// ...and the same instrument for the terminal transcript, for the same reason. Two probes
    /// rather than one shared name because the two suites assert about different owners, and a
    /// probe that reached into both would make a failure ambiguous about which window grew.
    friend struct TerminalHistoryProbe;

    std::vector<T> ring_;
    std::size_t oldest_ = 0;    ///< index of the oldest retained entry (0 until the ring wraps)
    std::uint64_t evicted_ = 0;
};

} // namespace loom

#endif // ZEN_BOUNDED_HISTORY_HPP
