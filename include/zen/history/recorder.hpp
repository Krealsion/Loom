// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HISTORY_RECORDER_HPP
#define ZEN_HISTORY_RECORDER_HPP

// WHAT ZEN KNOWS RIGHT NOW (RTH-1, corrected by RTH-1a).
//
// RTH-1 gave this one component two jobs: remember, and persist. They are not the
// same job and they wanted opposite things — a memory should be small, bounded and
// cheap to throw away; a record should be selective, durable and never quietly
// truncated by traffic that has nothing to do with it. RTH-1a separates them:
//
//   Recorder (here)      volatile working memory.  What does Zen know right now?
//   Logger (logger.hpp)  durable selected record.  What did Zen choose not to forget?
//
// THIS HALF OWNS NO FILE. `open_log`, `close_log` and `read_log` are gone from
// here; they belong to the Logger, which observes the bus for itself and never
// reads a Recorder. That independence is what makes a durable fact survive a
// window that has already released its live copy.
//
// TWO COMPLEMENTARY KINDS OF WORKING MEMORY, and they answer different questions:
//
//   LAST CALL   per shape, `last_n` deep, default 1. "What was the last
//               BuildFinished? Has a TimerFired ever actually occurred here?"
//               It is what makes every observed shape DISCOVERABLE — a fact can
//               leave recent context without becoming unrecordable.
//
//   RECENT      one shared FIFO. "What happened AROUND now?" A last-call slot can
//               say `HandlerFailed happened`; only this can say what surrounded it.
//               Heartbeat traffic is kept OUT of it by policy, and stays fully
//               recordable in its last-call slot — that is the whole correction.
//
//   PROTECTED   the third window, and RTH-1's, kept for RTH-1's measured reason:
//               a refusal, a failed handler and a death are rare, are exactly what
//               a maker goes looking for afterwards, and are lost to ordinary
//               traffic in seconds if they compete with it.
//
// ONE OWNING STORE, SEVERAL CLAIMS. A fact is stored once and the windows hold its
// identity; it is released when the last window lets go. That is why a shape can
// be muted in recent context without being made unrecordable, and it is also the
// seam a future bounded forensic capture needs: such a capture is one more
// claimant with its own budget and its own admission, not a redefinition of this.
//
// AUTHORITY. Exactly the ConsoleEngine's, and by exactly the same construction: it
// takes a `Switchboard&`, and holding one is already root authority. It is not a
// weave, it is not addressable, it accepts nothing, and it widens no ordinary
// participant's observation by one shape.
//
// THE ONE SHARP EDGE, MEASURED. `BusEvent::payload` points into a Message that
// dies when `deliver_one` returns. Anything retaining that pointer is a
// use-after-free the ordinary lane calls green. This recorder therefore SERIALIZES
// the payload inside the callback and keeps bytes.

#include <zen/history/record.hpp>
#include <zen/switchboard.hpp>
#include <zen/value.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

/// Published defaults. Bounded and stated, like every other retention surface in
/// this tree — a bounded store that pretended to be complete would trade a memory
/// lie for an observability lie.
inline constexpr std::size_t kDefaultLastN = 1; ///< the last-call guarantee, per shape
inline constexpr std::size_t kDefaultRecentCapacity = 4096;
inline constexpr std::size_t kDefaultProtectedCapacity = 512;
inline constexpr std::size_t kDefaultPayloadByteBudget = 1u << 20; ///< 1 MiB of payloads
inline constexpr std::size_t kDefaultMaxPayloadBytes = 64u << 10;  ///< the per-payload ceiling

/// ONE RULE: what happens to facts of one shape. THREE INDEPENDENT KNOBS, because
/// the three questions are independent — a heartbeat wants a last-call slot, no
/// share of recent context, and no payloads at all, and no single class can say
/// that.
///
/// Deliberately NOT a predicate, an expression, or anything that reads a payload
/// field: admission is a metadata decision keyed on a stable shape name, taken on
/// the dispatch that produced the event.
struct RetentionRule {
    std::string shape;
    /// How many of the most recent observations of this shape to keep. 0 means no
    /// last-call slot at all — the one way to make a shape genuinely undiscoverable,
    /// and it has to be written down.
    std::size_t last_n = kDefaultLastN;
    /// Whether this shape competes for the shared recent-context FIFO.
    bool in_recent = true;
    bool retain_payload = true;
};

/// The recorder's retention behaviour, as a plain structured object. NOT a DSL,
/// not a text format, and not something a message can change: `apply_policy` is a
/// host call, exactly as constructing the recorder is.
struct RecorderPolicy {
    /// THE LAST-CALL DEFAULT, and the reason an unknown shape stays discoverable:
    /// a shape nobody has written a rule for still gets a slot the first time it
    /// is actually observed.
    std::size_t default_last_n = kDefaultLastN;
    bool default_in_recent = true;
    bool default_retain_payload = true;
    std::size_t recent_capacity = kDefaultRecentCapacity;
    std::size_t protected_capacity = kDefaultProtectedCapacity;
    std::size_t payload_byte_budget = kDefaultPayloadByteBudget;
    std::size_t max_payload_bytes = kDefaultMaxPayloadBytes;
    /// STRUCTURAL PROTECTION, keyed on what the BUS did rather than on what a shape
    /// means. These are the facts a maker goes looking for after something went
    /// wrong, they are rare, and a single shared window loses them to ordinary
    /// traffic in seconds. A shape never asks to be protected; the recorder
    /// decides, and a maker can decide otherwise by clearing a flag.
    ///
    /// PROTECTION DECIDES WHETHER A FACT IS KEPT. The shape's `in_recent` still
    /// decides whether it competes for RECENT CONTEXT — so `TimerFired: last_n=1,
    /// in_recent=false` means "the flood of beats is not worth recent context",
    /// the ONE beat that was refused is still kept (here), and a storm of refused
    /// beats still cannot drown the build a maker is looking for.
    bool protect_refusals = true;
    bool protect_handler_failures = true;
    bool protect_lifecycle = true;
    std::vector<RetentionRule> rules;

    /// The rule governing `shape`, or nullptr when the defaults govern it.
    const RetentionRule* rule_for(std::string_view shape) const noexcept;
};

/// THE SMALLEST DEFENSIBLE STARTING POLICY, and it is deliberately almost empty.
///
/// It protects three kinds of rare fact — a participant's lifecycle, a refusal and
/// a handler that failed — and gives every other shape a one-deep last-call slot
/// and a share of recent context. It names NO application shape, because retention
/// importance is this recorder's business and inventing a permanent list of
/// "important messages" is how a diagnostic surface acquires opinions nobody can
/// audit. A host that knows its own traffic writes rules for it.
RecorderPolicy default_policy();

// ---------------------------------------------------------------------------
// What a reader gets back
// ---------------------------------------------------------------------------

/// THE FOUR ANSWERS A LOOKUP CAN HONESTLY GIVE, and the reason this type exists at
/// all: "no record" and "I have forgotten" must never share a word. A history that
/// answered a forgotten fact with something shaped like "nothing happened" would
/// be the most convincing lie the system could tell.
enum class Horizon : std::uint8_t {
    Retained,    ///< here it is
    Forgotten,   ///< it was inside a window once; the window has moved past it
    NotRecorded, ///< the recorder was watching then, and this one it did not keep
    Unobserved   ///< beyond anything this recorder has seen at all
};

const char* name_of(Horizon h) noexcept;

struct Lookup {
    Horizon horizon = Horizon::Unobserved;
    const HistoryRecord* record = nullptr; ///< non-null iff horizon == Retained
};

struct PayloadLookup {
    PayloadState state = PayloadState::Absent;
    std::string bytes; ///< canonical native bytes; non-empty only when state == Retained
    std::string shape;
    std::uint32_t shape_version = 0;
};

/// What the recorder can say about its own horizon, in numbers.
struct RecorderBounds {
    std::size_t retained = 0;                ///< records held right now, all windows
    std::uint64_t forgotten = 0;             ///< records released to keep the windows bounded
    std::uint64_t forgotten_horizon_seq = 0; ///< the highest bus seq a released record carried
    std::uint64_t oldest_retained_seq = 0;   ///< 0 when nothing retained carries a seq
    std::uint64_t newest_observed_seq = 0;   ///< the highest bus seq the tap has shown us
    std::uint64_t payload_bytes = 0;         ///< bytes of payload held right now
    std::size_t payloads_retained = 0;
    std::uint64_t payloads_forgotten = 0;
    std::size_t recent_held = 0;    ///< records the shared recent FIFO claims
    std::size_t protected_held = 0; ///< records the protected window claims
    std::size_t last_call_held = 0; ///< records claimed by some shape's last-call slot
    std::size_t shapes_observed = 0;
};

/// Everything the recorder counted. Counters, not events — the recorder does not
/// narrate its own accounting into history.
///
/// ONE ARITHMETIC A READER CAN CHECK BY HAND, and it is meant to be checked:
///
///     observed == recorded + declined_by_policy + declined_internal
///
/// The recorder's own policy notes are on neither side of it. They are not events
/// — nothing was published and nothing observed them — so counting them as
/// `recorded` would make a history appear to hold more than it was ever shown.
struct RecorderCounters {
    std::uint64_t observed = 0;           ///< events the tap handed us
    std::uint64_t recorded = 0;           ///< events that became a record
    std::uint64_t declined_by_policy = 0; ///< observed, claimed by no window
    std::uint64_t declined_internal = 0;  ///< refused entry by the structural blacklist
};

/// Per-shape traffic, derived from counters rather than recorded as events. This
/// is also the discoverability answer: a shape with `observed > 0` HAS happened in
/// this process, whatever became of the records.
struct ShapeTally {
    std::string shape;
    std::uint64_t observed = 0;
    std::uint64_t recorded = 0;
    std::uint64_t declined = 0;
    std::size_t last_call_held = 0; ///< how many of this shape's last-call slots are filled
};

// ---------------------------------------------------------------------------
// The recorder
// ---------------------------------------------------------------------------

class Recorder {
public:
    /// Attaches a tap for its whole lifetime. The bus must outlive the recorder,
    /// exactly as it must outlive a ConsoleEngine.
    explicit Recorder(Switchboard& bus, RecorderPolicy policy = default_policy());
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // ---- policy -----------------------------------------------------------
    const RecorderPolicy& policy() const noexcept { return policy_; }

    /// CHANGE WHAT ZEN REMEMBERS, and remember that it changed.
    ///
    /// It writes ONE `RecorderPolicy` record describing the transition, into the
    /// protected window, and sends NOTHING: a recorder that published its own
    /// policy changes in order to observe them would be manufacturing the traffic
    /// it exists to watch. This is a RECORDER-LOCAL structured fact; a Logger that
    /// wants durable evidence of it selects it like any other.
    ///
    /// PROSPECTIVE. A smaller window governs FUTURE retention; nothing already
    /// retained is destroyed by the act of applying the policy. Records over the
    /// new bound are released one at a time as new traffic arrives, and the policy
    /// record states how many were over it at the moment of the change, so the
    /// trim is never a surprise.
    void apply_policy(RecorderPolicy next);

    // ---- the structural blacklist ------------------------------------------
    RecorderBlacklist& blacklist() noexcept { return blacklist_; }
    const RecorderBlacklist& blacklist() const noexcept { return blacklist_; }

    // ---- the structured reader ---------------------------------------------
    //
    // Records, never strings. A presentation formats these; see history/dump.hpp.

    /// Every retained record, oldest first, across all windows. A record claimed by
    /// several windows appears ONCE — there is one of it.
    std::vector<HistoryRecord> snapshot() const;
    /// Retained records of one shape, oldest first.
    std::vector<HistoryRecord> snapshot_of(std::string_view shape) const;
    /// Just the shared recent-context FIFO, oldest first — "what happened around
    /// now", without the last-call slots of shapes that left it long ago.
    std::vector<HistoryRecord> recent() const;
    std::size_t retained() const noexcept;
    RecorderBounds bounds() const noexcept;
    RecorderCounters counters() const noexcept { return counters_; }

    /// Every BUS shape this process has actually OBSERVED, with its traffic. The
    /// difference between a shape that is registered and a shape that has happened
    /// — and the only one of the two this recorder can honestly answer, because a
    /// tap sees deliveries and not registrations. The recorder's own policy notes
    /// are not bus traffic and are deliberately absent from this roll.
    std::vector<ShapeTally> tallies() const;
    /// Whether this shape has been observed at least once by this recorder.
    bool observed(std::string_view shape) const noexcept;

    /// THE LAST ONE OF THESE — the last-call answer, and the one that survives a
    /// shape falling out of recent context. `Horizon::Unobserved` when the shape has
    /// never been seen; `Horizon::NotRecorded` when it has been seen and its rule
    /// gives it no last-call slot.
    Lookup last_of(std::string_view shape) const noexcept;
    /// This shape's last-call slots, oldest first (up to its `last_n`).
    std::vector<HistoryRecord> last_calls_of(std::string_view shape) const;

    /// What became of bus delivery `seq` — as one of the four honest answers.
    Lookup find(std::uint64_t bus_seq) const noexcept;
    /// One record by the recorder's own identity for it.
    const HistoryRecord* record(std::uint64_t record_seq) const noexcept;
    /// Whether this recorder still holds that record's payload, and the bytes if so.
    PayloadLookup payload(std::uint64_t record_seq) const;

private:
    /// One window: a bounded FIFO of record identities. It holds `record_seq`
    /// values rather than records because a fact can be in several windows and
    /// there is only ever one of it.
    struct Ring {
        std::deque<std::uint64_t> seqs;
        std::size_t capacity = 0;
        std::uint64_t evicted = 0;
    };

    /// The owning slot. `claims` is the number of windows currently holding this
    /// record's seq; at zero the fact is forgotten and its payload with it.
    struct Slot {
        HistoryRecord rec;
        unsigned claims = 0;
    };

    /// EVERYTHING KEYED ON A SHAPE, IN ONE PLACE, so the hot path performs ONE
    /// string-keyed lookup per observation rather than one per question. The
    /// resolved rule is cached here on first sight and invalidated wholesale by
    /// `apply_policy`, which takes the linear rule scan off the hot path entirely.
    struct ShapeState {
        ShapeTally tally;
        Ring last_call;
        bool in_recent = true;
        bool retain_payload = true;
    };

    void observe(const BusEvent& e);
    ShapeState& state_for(const std::string& shape);
    void admit(HistoryRecord rec, const BusEvent* e, ShapeState& st, bool structural);
    /// The recorder's own note about itself: stored, and claimed by the protected
    /// and recent windows. Deliberately given NO `ShapeState` — a policy change is
    /// a fact about this recorder, not traffic on the bus, and letting it into the
    /// shape tallies would make `tallies()` answer a question nobody asked.
    void admit_local(HistoryRecord rec);
    void claim(Ring& w, std::uint64_t record_seq, Held which);
    void forget(std::uint64_t record_seq);
    void store_payload(std::uint64_t record_seq, std::string bytes, std::string shape,
                       std::uint32_t version);
    void trim_payloads();
    void reseat_policy();

    Switchboard& bus_;
    ObserverId tap_ = 0;
    RecorderPolicy policy_;
    RecorderBlacklist blacklist_;

    std::map<std::uint64_t, Slot> records_; ///< the one owning store, keyed by record_seq
    Ring recent_;
    Ring protected_;
    std::map<std::string, ShapeState, std::less<>> shapes_;

    struct PayloadSlot {
        std::uint64_t record_seq = 0;
        std::string bytes;
        std::string shape;
        std::uint32_t version = 0;
    };
    std::deque<PayloadSlot> payloads_;
    std::uint64_t payload_bytes_ = 0;
    std::uint64_t payloads_forgotten_ = 0;
    /// The highest record_seq whose payload was released. Together with a record's
    /// own `PayloadDisposition` it separates "I had it and let it go" from "I never
    /// took it" exactly, with no per-record mutation.
    std::uint64_t payload_horizon_ = 0;

    std::uint64_t next_record_seq_ = 1;
    std::uint64_t newest_observed_seq_ = 0;
    std::uint64_t forgotten_ = 0;
    std::uint64_t forgotten_horizon_seq_ = 0;
    RecorderCounters counters_;
};

} // namespace loom

#endif // ZEN_HISTORY_RECORDER_HPP
