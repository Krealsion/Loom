// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_PENDING_SPEECH_HPP
#define ZEN_SWITCHBOARD_PENDING_SPEECH_HPP

// What each sender's own waiting speech holds, and the bounds it is held to: the unit the bus
// consults at every enqueue, and a host door consults before it decodes a sender's bytes.
// docs/reference/bounds.md#what-one-senders-waiting-messages-hold

#include <zen/admission.hpp>
#include <zen/gate.hpp>      // ValueWeight: what a waiting message makes the host keep
#include <zen/serialize.hpp> // kMaxDecodedCells: a whole value's worth, the cells' unit
#include <zen/switchboard/bus.hpp>
#include <zen/switchboard/message.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace loom {

/// One sender's waiting speech, bounded in the three units it is held in: the messages waiting,
/// the cells they hold together, and the payload bytes they carry. A count of messages is no
/// bound on what they hold -- a message waits decoded, and a compact value may command the whole
/// decode allowance -- and a bound on structure is no bound on bytes, since a one-cell value may
/// carry megabytes. Together they say what one sender can make the host keep, and owe it.
/// MSG-01; docs/reference/bounds.md
class PendingSpeech {
public:
    /// How many of one sender's own messages may wait at once. A send past it is refused
    /// `PendingPerSender` and queues nothing; every other sender is left untouched. The same
    /// number as the journal's capacity: one sender's waiting speech is one journal window and
    /// cannot by itself roll that record. Not a turn bound (MSG-09).
    static constexpr std::size_t kMaxMessages = 1024;

    /// How many cells those same waiting messages may hold together, each counted as a decode
    /// counts it (`decoded_cells`). Four whole values of the decoder's bound, so one value of
    /// any admissible size always fits a sender with nothing waiting, and reaches four accepters
    /// at once; a sender of ordinary values meets `kMaxMessages` first and never this.
    static constexpr std::uint64_t kMaxCells = 4 * kMaxDecodedCells;
    static_assert(kMaxCells >= kMaxDecodedCells,
                  "one value of any admissible size fits a sender with nothing waiting");

    /// How many payload bytes those waiting messages may carry together: the `Text` and `Bytes`
    /// contents the cell count never charged for, since a 1 MiB field is one cell. 64 MiB, what
    /// the framed channels already publish as the bytes one peer may have in flight
    /// (`kMaxFrameLen`, `kMaxBacklog`), so a sender may have one transport's worth waiting on
    /// the bus and no more -- and so one message of any size a transport can deliver fits a
    /// sender with nothing waiting.
    static constexpr std::uint64_t kMaxBytes = 64u << 20;

    /// What one sender may still have waiting.
    struct Room {
        std::size_t messages = kMaxMessages;
        std::uint64_t cells = kMaxCells;
        std::uint64_t bytes = kMaxBytes;
        /// No send of this sender's own can be queued now, whatever it holds.
        bool spent() const noexcept { return messages == 0 || cells == 0 || bytes == 0; }
    };

    /// The room `sender`'s own speech has left.
    Room room(WeaveId sender) const noexcept {
        Room left;
        const Waiting w = waiting(sender);
        left.messages = w.messages >= kMaxMessages ? 0 : kMaxMessages - w.messages;
        left.cells = w.cells >= kMaxCells ? 0 : kMaxCells - w.cells;
        left.bytes = w.bytes >= kMaxBytes ? 0 : kMaxBytes - w.bytes;
        return left;
    }

    /// The refusal `envelopes` more of `sender`'s messages, each of `weight`, meet: nothing when
    /// they fit under every bound, else `PendingPerSender` with words naming the bound, what
    /// waits and what was asked. `where` is said after the bound, for a door that refuses
    /// somewhere the bus is not.
    std::optional<Refusal> refuses(WeaveId sender, ValueWeight weight, std::size_t envelopes = 1,
                                   std::string_view where = {}) const {
        const std::uint64_t cells = weight.cells;
        if (envelopes == 0) {
            return std::nullopt;
        }
        const Waiting held = waiting(sender);
        const Room left = room(sender);
        if (left.messages == 0) {
            return refused("this sender has its bound of " + std::to_string(kMaxMessages) +
                               " messages waiting (kMaxPendingPerSender)",
                           where);
        }
        if (envelopes > left.messages) {
            return refused("this sender has " + std::to_string(held.messages) +
                               " messages waiting, and " + std::to_string(envelopes) +
                               " more would pass its bound of " + std::to_string(kMaxMessages) +
                               " (kMaxPendingPerSender)",
                           where);
        }
        if (left.cells == 0) {
            return refused("this sender's waiting messages hold all " + std::to_string(kMaxCells) +
                               " cells of its bound (kMaxPendingCellsPerSender)",
                           where);
        }
        if (left.bytes == 0) {
            return refused("this sender's waiting messages carry all " +
                               std::to_string(kMaxBytes) +
                               " payload bytes of its bound (kMaxPendingBytesPerSender)",
                           where);
        }
        // What is asked for, without overflow: a message's cells are at most one past the
        // decoder's bound and its bytes at most what a field may carry, and `envelopes` is at
        // most what may wait.
        const std::uint64_t n = envelopes;
        if (cells * n > left.cells) {
            return refused("this sender's waiting messages hold " + std::to_string(held.cells) +
                               " cells, and " + each(cells, envelopes) +
                               " would pass its bound of " + std::to_string(kMaxCells) +
                               " (kMaxPendingCellsPerSender)",
                           where);
        }
        if (weight.payload_bytes > left.bytes / n) {
            return refused("this sender's waiting messages carry " + std::to_string(held.bytes) +
                               " payload bytes, and " + each(weight.payload_bytes, envelopes) +
                               " would pass its bound of " + std::to_string(kMaxBytes) +
                               " (kMaxPendingBytesPerSender)",
                           where);
        }
        return std::nullopt;
    }

    /// The refusal a send from `sender` meets now, whatever it would hold: the question a host
    /// door asks before it decodes that sender's bytes, and the words it answers with. Nothing
    /// while room remains, since only the value itself can settle the rest.
    std::optional<Refusal> refuses_now(WeaveId sender) const {
        return room(sender).spent()
                   ? refuses(sender, {}, 1, "; refused before its bytes were decoded")
                   : std::nullopt;
    }

    /// One more of `sender`'s messages waits, of this weight.
    void charge(WeaveId sender, ValueWeight weight) {
        Waiting& w = waiting_[sender.value];
        ++w.messages;
        w.cells += weight.cells;
        w.bytes += weight.payload_bytes;
    }

    /// One of `sender`'s messages waits no longer: it has been taken for dispatch.
    void give_back(WeaveId sender, ValueWeight weight) {
        const auto it = waiting_.find(sender.value);
        if (it == waiting_.end()) {
            return;
        }
        Waiting& w = it->second;
        w.cells -= weight.cells < w.cells ? weight.cells : w.cells;
        w.bytes -= weight.payload_bytes < w.bytes ? weight.payload_bytes : w.bytes;
        if (--w.messages == 0) {
            waiting_.erase(it);
        }
    }

private:
    struct Waiting {
        std::size_t messages = 0;
        std::uint64_t cells = 0;
        std::uint64_t bytes = 0;
    };

    /// "this one's <n>", or what each of several accepters' envelopes would hold.
    static std::string each(std::uint64_t n, std::size_t envelopes) {
        return envelopes == 1 ? "this one's " + std::to_string(n)
                              : std::to_string(envelopes) + " accepters' " + std::to_string(n) +
                                    " each";
    }

    Waiting waiting(WeaveId sender) const noexcept {
        const auto it = waiting_.find(sender.value);
        return it == waiting_.end() ? Waiting{} : it->second;
    }

    static Refusal refused(std::string what, std::string_view where) {
        what += where;
        what += "; nothing was queued, and it may be sent again once some of them are dispatched";
        return Refusal{RefusalReason::PendingPerSender,
                       Error{ErrorKind::TooLarge, "", "", "", std::move(what)}};
    }

    /// By `WeaveId::value`. A sender with nothing waiting holds no entry. Bus acts are absent,
    /// and so are answers: one delivery creates one answer right.
    std::map<std::uint64_t, Waiting> waiting_;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_PENDING_SPEECH_HPP
