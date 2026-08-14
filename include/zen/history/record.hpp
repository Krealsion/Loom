// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HISTORY_RECORD_HPP
#define ZEN_HISTORY_RECORD_HPP

// ONE STRUCTURED FACT, SPOKEN BY TWO OWNERS (RTH-1a).
//
// RTH-1 built a single Recorder that both remembered and persisted. RTH-1a splits
// that in two, and this header is what the halves have in common:
//
//   Recorder (recorder.hpp)  what does Zen know RIGHT NOW?   volatile, bounded
//   Logger   (logger.hpp)    what did Zen choose NOT TO FORGET?  durable, selective
//
// Both describe a fact with the same `HistoryRecord`, so a durable record and a
// live one are the same shape and a reader written for one reads the other. What
// differs is entirely who keeps it and for how long — which is the split, stated
// as a type rather than as a convention.
//
// EVERY DELIVERY FIELD BELOW IS READ OFF A `BusEvent`. Nothing here is inferred,
// looked up later, or invented, and fields a log conventionally carries that this
// bus cannot supply (a wall-clock stamp, a user, a host name, a severity) are
// absent on purpose.

#include <zen/switchboard.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace loom {

/// What kind of fact a record is about.
enum class RecordKind : std::uint8_t {
    Delivery,      ///< the bus attempted a delivery: Delivered / Refused / HandlerFailed
    Lifecycle,     ///< a participant died or was revived
    RecorderPolicy ///< the recorder's own retention behaviour changed (recorder.hpp)
};

/// WHAT BECAME OF THE DELIVERY — transport truth, and only that.
///
/// THERE IS DELIBERATELY NO `Pending`. A record is made from the tap, which fires
/// only once the delivery has reached a terminal fact, so a record is never in an
/// unfinished state and nothing here can be mistaken for "still queued". The
/// Switchboard's `Disposition::Pending` means five different things at once
/// (never issued, evicted, queued, in flight, handler threw) and is deliberately
/// not mirrored.
enum class RecordedOutcome : std::uint8_t {
    None,         ///< this record is not about a delivery at all
    Delivered,    ///< the handler was entered and returned normally
    Refused,      ///< Loom declined it, with a reason
    HandlerFailed ///< the handler was entered and did not complete (MSG-10)
};

/// WHICH OF THE RECORDER'S WINDOWS STILL CLAIM THIS RECORD — a SET, not a class.
///
/// RTH-1 had one `RetentionClass` per record because a record lived in exactly one
/// window. RTH-1a's recorder keeps two complementary kinds of working memory that
/// ask different questions of the same fact:
///
///   LastCall   "what was the last one of THESE?"      per shape, small
///   Recent     "what happened around now?"            one shared FIFO
///   Protected  "the rare thing I will come looking for"  refusals, failures, deaths
///
/// so one fact can be held by several at once and released by them separately. The
/// mask on a retained record is therefore CURRENT: a record that has fallen out of
/// recent context but is still the last of its shape says exactly that.
enum class Held : std::uint8_t {
    Nothing = 0,
    LastCall = 1u << 0,
    Recent = 1u << 1,
    Protected = 1u << 2
};

using HeldMask = std::uint8_t;

constexpr HeldMask operator|(Held a, Held b) noexcept {
    return static_cast<HeldMask>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool held_in(HeldMask m, Held w) noexcept {
    return (m & static_cast<std::uint8_t>(w)) != 0;
}

/// WHAT THE RECORDER DECIDED ABOUT THIS RECORD'S PAYLOAD, at the moment it saw
/// it. A decision, taken once, that never changes afterwards.
enum class PayloadDisposition : std::uint8_t {
    None,       ///< the event carried no payload (every refusal, every lifecycle event)
    Retained,   ///< admitted to the payload store
    TooLarge,   ///< over the per-payload ceiling; the metadata is here, the bytes never were
    NotRetained ///< policy declined payloads for this record
};

/// ...AND WHAT THE RECORDER STILL HAS. A different question with a different
/// answer, which is the whole reason the two enums exist: metadata and payload
/// are separate budgets and are forgotten separately.
enum class PayloadState : std::uint8_t {
    Absent,   ///< there never was one
    Retained, ///< still here
    Evicted,  ///< it WAS retained; the byte budget released it
    Declined  ///< it was seen and deliberately not kept (TooLarge / NotRetained)
};

const char* name_of(RecordKind k) noexcept;
const char* name_of(RecordedOutcome o) noexcept;
const char* name_of(PayloadDisposition d) noexcept;
const char* name_of(PayloadState s) noexcept;
/// "LastCall|Recent", or "none". A mask has no single name, so it is spelled out.
std::string describe_held(HeldMask m);

/// One structured history record.
struct HistoryRecord {
    /// THE RECORDER'S OWN MONOTONIC NUMBER, and the identity of this record.
    /// Distinct from the bus `seq` because not every fact has one: a lifecycle
    /// transition carries seq 0, and a policy change is not a bus fact at all.
    std::uint64_t record_seq = 0;
    RecordKind kind = RecordKind::Delivery;
    /// Which windows hold it right now. Meaningful on a record read out of a live
    /// Recorder; on one read back from a Logger it is the mask as it stood when
    /// the Logger wrote it, which is a fact about a moment and not a live claim.
    HeldMask held = 0;

    // ---- a delivery, as the bus stated it ---------------------------------
    std::uint64_t seq = 0; ///< the delivery seq; 0 for lifecycle and policy records
    WeaveId sender{};
    WeaveId target{};           ///< the RESOLVED recipient
    std::string addressed_role; ///< the office the sender named, empty if none
    std::string authored_role;  ///< the office the sender SPOKE AS, empty for personal speech
    std::string shape;
    std::uint32_t shape_version = 0;
    std::uint64_t correlation = 0;     ///< 0 == none stated (and a legal choice; ANS-05)
    std::uint64_t dispatch_parent = 0; ///< SYNCHRONOUS ancestry only — see BusEvent
    std::uint64_t handler_elapsed_ns = 0;
    RecordedOutcome outcome = RecordedOutcome::None;
    RefusalReason refusal = RefusalReason::None;
    std::string refusal_detail; ///< the gate's field path / expected / actual, where there is one

    // ---- a lifecycle transition -------------------------------------------
    bool from_last_known_good = false;

    // ---- the recorder's own note (kind == RecorderPolicy) ------------------
    std::string note;

    // ---- payload -----------------------------------------------------------
    PayloadDisposition payload = PayloadDisposition::None;
    std::uint32_t payload_bytes = 0; ///< the serialized size, whether or not it was kept
};

/// Fill the delivery/lifecycle fields of a record from a bus event. Shared because
/// the Recorder and the Logger observe the SAME tap and must not drift into two
/// slightly different readings of one event; neither owns this translation.
///
/// It does NOT touch `record_seq`, `held` or the payload fields: those are each
/// owner's own business, and a Logger's copy of a fact is not the Recorder's copy
/// of it.
void fill_from_event(HistoryRecord& rec, const BusEvent& e);

/// WHAT MUST NOT ENTER THE RECORDABLE UNIVERSE AT ALL — a structural class,
/// checked BEFORE any retention rule, and categorically not a filter.
///
///   RETENTION POLICY   maker-controlled. Which real facts deserve memory.
///   THIS               architecture-controlled. What is not a fact about the
///                      system at all, because it is the recorder's own
///                      machinery observing itself.
///
/// It exists to make recursive history impossible rather than unlikely. Today the
/// Recorder authors NO bus traffic — it is a tap consumer, and the Logger writes
/// with an ordinary file handle — so a default-constructed blacklist is empty and
/// honest about it. The moment a mechanic here does speak (a storage broker
/// exchange, a query answered by message, a policy applied over the wire), the
/// host declares it here and it cannot appear in its own record.
///
/// It must NEVER be used to hide ordinary Loom facts for being noisy. A
/// `TimerFired` is a real fact and belongs to policy; a recorder incrementing its
/// own counter is not a fact at all.
///
/// THE LOGGER HAS NO BLACKLIST AND NEEDS NONE: its selection is a whitelist, so
/// machinery nobody named is already excluded by construction. The Recorder's
/// default is admit, which is why it is the half that carries this.
class RecorderBlacklist {
public:
    void declare_participant(WeaveId id);
    void declare_shape(std::string name);

    bool empty() const noexcept { return participants_.empty() && shapes_.empty(); }
    bool excludes(const BusEvent& e) const noexcept;

    const std::vector<WeaveId>& participants() const noexcept { return participants_; }
    const std::vector<std::string>& shapes() const noexcept { return shapes_; }

private:
    std::vector<WeaveId> participants_;
    std::vector<std::string> shapes_;
};

} // namespace loom

#endif // ZEN_HISTORY_RECORD_HPP
