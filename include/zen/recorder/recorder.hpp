// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_RECORDER_RECORDER_HPP
#define ZEN_RECORDER_RECORDER_HPP

// THE HOST'S MEMORY OF WHAT THIS BUS DID (RTH-1).
//
// Before this, Zen remembered almost nothing about a message once the turn that
// carried it was over. The Switchboard's journal keeps a VERDICT — one
// disposition per delivery seq, no sender, no target, no shape, no payload — and
// everything richer belonged to a participant: a TerminalSession's transcript, a
// ConsoleEngine's tap window, the Builder tool's picture of one operation. So
// nothing could answer a question about a message the asker did not itself send
// or receive. RTH-0 measured that; this is the smallest owner of the gap.
//
// WHAT IT IS. A host-side tap consumer, bounded, structured, and
// presentation-neutral: it copies the facts an ordinary `BusEvent` already
// carries into records that outlive the delivery, and hands them back as data.
// A terminal, a Workshop panel, a debugger and a test read the SAME records and
// each formats them for itself — which is why nothing here returns a formatted
// line (see recorder/dump.hpp, which is deliberately somewhere else).
//
// WHAT IT IS NOT. Not event sourcing, not a replay engine, not a query language,
// not a database, not a telemetry exporter, and not an unbounded log. It is also
// NOT INSIDE THE SWITCHBOARD, and that is a decision rather than an accident:
// the bus is the component that runs for weeks, retention policy is a thing a
// maker will want to change, and a recorder outside the bus can be replaced
// without touching it.
//
// AUTHORITY. Exactly the ConsoleEngine's, and by exactly the same construction:
// it takes a `Switchboard&`, and holding one is already root authority. It is
// not a weave, it is not addressable, it accepts nothing, and it widens no
// ordinary participant's observation by one shape. A scoped observation law is
// still absent and still has no consumer; a host lens needs none.
//
// THE ONE SHARP EDGE, MEASURED. `BusEvent::payload` points into a Message that
// dies when `deliver_one` returns. Anything retaining that pointer is a
// use-after-free the ordinary lane calls green. This recorder therefore
// SERIALIZES the payload inside the callback and keeps bytes.

#include <zen/schema.hpp>
#include <zen/switchboard.hpp>
#include <zen/value.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

// ---------------------------------------------------------------------------
// What one record is
// ---------------------------------------------------------------------------

/// What kind of fact a record is about.
enum class RecordKind : std::uint8_t {
    Delivery,      ///< the bus attempted a delivery: Delivered / Refused / HandlerFailed
    Lifecycle,     ///< a participant died or was revived
    RecorderPolicy ///< the recorder's own retention behaviour changed (§ Policy, below)
};

/// WHAT BECAME OF THE DELIVERY — transport truth, and only that.
///
/// THERE IS DELIBERATELY NO `Pending`. A recorder learns of a delivery from the
/// tap, which fires only once the delivery has reached a terminal fact, so a
/// record is never in an unfinished state and nothing here can be mistaken for
/// "still queued". The Switchboard's `Disposition::Pending` means five different
/// things at once (never issued, evicted, queued, in flight, handler threw) and
/// is deliberately not mirrored: this recorder answers only about facts it
/// actually saw, and says so where it did not (see `Horizon`).
enum class RecordedOutcome : std::uint8_t {
    None,         ///< this record is not about a delivery at all
    Delivered,    ///< the handler was entered and returned normally
    Refused,      ///< Loom declined it, with a reason
    HandlerFailed ///< the handler was entered and did not complete (MSG-10)
};

const char* name_of(RecordKind k) noexcept;
const char* name_of(RecordedOutcome o) noexcept;

/// WHICH WINDOW A RECORD COMPETES IN. Retention importance is RECORDER POLICY —
/// a shape never declares itself important, and nothing on the wire can ask to
/// be remembered.
enum class RetentionClass : std::uint8_t {
    Shared,     ///< the ordinary window: every fact with no rule of its own
    Dedicated,  ///< this shape gets a window of its own, sized for it
    Protected,  ///< rare facts that must not compete with ordinary traffic
    NotRetained ///< observed, deliberately not kept — and counted, never silent
};

const char* name_of(RetentionClass c) noexcept;

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

const char* name_of(PayloadDisposition d) noexcept;
const char* name_of(PayloadState s) noexcept;

/// One structured history record. EVERY DELIVERY FIELD BELOW IS READ OFF A
/// `BusEvent` — nothing here is inferred, looked up later, or invented. Fields a
/// log conventionally carries and this bus cannot supply (a wall-clock stamp, a
/// user, a host name, a severity) are absent on purpose.
struct HistoryRecord {
    /// THE RECORDER'S OWN MONOTONIC NUMBER, and the identity of this record.
    /// Distinct from the bus `seq` because not every fact has one: a lifecycle
    /// transition carries seq 0, and a policy change is not a bus fact at all.
    std::uint64_t record_seq = 0;
    RecordKind kind = RecordKind::Delivery;
    RetentionClass retention = RetentionClass::Shared;

    // ---- a delivery, as the bus stated it ---------------------------------
    std::uint64_t seq = 0; ///< the delivery seq; 0 for lifecycle and policy records
    WeaveId sender{};
    WeaveId target{};        ///< the RESOLVED recipient
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

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

/// Published defaults. Bounded and stated, like every other retention surface in
/// this tree — a bounded store that pretended to be complete would trade a
/// memory lie for an observability lie.
inline constexpr std::size_t kDefaultSharedCapacity = 4096;
inline constexpr std::size_t kDefaultProtectedCapacity = 512;
inline constexpr std::size_t kDefaultDedicatedCapacity = 512;
inline constexpr std::size_t kDefaultPayloadByteBudget = 1u << 20; ///< 1 MiB of retained payloads
inline constexpr std::size_t kDefaultMaxPayloadBytes = 64u << 10;  ///< the per-payload ceiling
inline constexpr std::size_t kDefaultLogByteBudget = 8u << 20;     ///< 8 MiB of appended log

/// ONE RULE: what happens to facts of one shape.
struct RetentionRule {
    std::string shape;
    RetentionClass klass = RetentionClass::Shared;
    std::size_t capacity = kDefaultDedicatedCapacity; ///< Dedicated only
    bool retain_payload = true;
};

/// The recorder's retention behaviour, as a plain structured object. NOT a DSL,
/// not a text format, and not something a message can change: `apply_policy` is
/// a host call, exactly as constructing the recorder is.
struct RecorderPolicy {
    RetentionClass default_class = RetentionClass::Shared;
    bool default_retain_payload = true;
    std::size_t shared_capacity = kDefaultSharedCapacity;
    std::size_t protected_capacity = kDefaultProtectedCapacity;
    std::size_t payload_byte_budget = kDefaultPayloadByteBudget;
    std::size_t max_payload_bytes = kDefaultMaxPayloadBytes;
    std::size_t log_byte_budget = kDefaultLogByteBudget;
    /// STRUCTURAL PROTECTION, and it is keyed on what the BUS did rather than on
    /// what a shape means. These are the facts a maker goes looking for after
    /// something went wrong, they are rare, and a single shared window loses them
    /// to ordinary traffic in seconds. A shape never asks to be protected; the
    /// recorder decides, and a maker can decide otherwise by clearing a flag.
    ///
    /// They outrank a shape rule, including a `NotRetained` one — `TimerFired:
    /// NotRetained` means "the flood of beats is not worth remembering", and the
    /// ONE beat that was refused is not part of that flood. A maker who wants the
    /// silence anyway clears the flag rather than discovering the exception.
    bool protect_refusals = true;
    bool protect_handler_failures = true;
    bool protect_lifecycle = true;
    std::vector<RetentionRule> rules;

    /// The rule governing `shape`, or nullptr when the defaults govern it.
    const RetentionRule* rule_for(std::string_view shape) const noexcept;
};

/// THE SMALLEST DEFENSIBLE STARTING POLICY, and it is deliberately almost empty.
///
/// It protects four kinds of rare fact — a participant's lifecycle, a refusal, a
/// handler that failed, and the recorder's own policy changes — because each is
/// exactly what a maker looks for after the fact and each is drowned by ordinary
/// traffic in a single shared window (RTH-0 measured 300 deliveries/s from an
/// idle Zengine application, so a 1024-entry window covers 3.4 seconds). It
/// names NO application shape, because retention importance is this recorder's
/// business and inventing a permanent list of "important messages" is how a
/// diagnostic surface acquires opinions nobody can audit.
RecorderPolicy default_policy();

// ---------------------------------------------------------------------------
// The structural blacklist
// ---------------------------------------------------------------------------

/// WHAT MUST NOT ENTER THE RECORDABLE UNIVERSE AT ALL — a structural class,
/// checked BEFORE any retention rule, and categorically not a filter.
///
///   RETENTION POLICY   maker-controlled. Which real facts deserve memory.
///   THIS               architecture-controlled. What is not a fact about the
///                      system at all, because it is the recorder's own
///                      machinery observing itself.
///
/// It exists to make recursive history impossible rather than unlikely. Today
/// this recorder authors NO bus traffic — it is a tap consumer that writes its
/// log with an ordinary file handle — so a default-constructed blacklist is
/// empty and honest about it. The moment a recorder mechanic does speak (a
/// storage broker exchange, a query answered by message, a policy applied over
/// the wire), the host declares it here and it cannot appear in its own record.
///
/// It must NEVER be used to hide ordinary Loom facts for being noisy. A
/// `TimerFired` is a real fact and belongs to policy; a recorder incrementing
/// its own counter is not a fact at all.
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

// ---------------------------------------------------------------------------
// What a reader gets back
// ---------------------------------------------------------------------------

/// THE FOUR ANSWERS A LOOKUP CAN HONESTLY GIVE, and the reason this type exists
/// at all: "no record" and "I have forgotten" must never share a word. A history
/// that answered a forgotten fact with something shaped like "nothing happened"
/// would be the most convincing lie the system could tell.
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
    std::size_t retained = 0;                 ///< records held right now, all windows
    std::uint64_t forgotten = 0;              ///< records released to keep the windows bounded
    std::uint64_t forgotten_horizon_seq = 0;  ///< the highest bus seq a released record carried
    std::uint64_t oldest_retained_seq = 0;    ///< 0 when nothing retained carries a seq
    std::uint64_t newest_observed_seq = 0;    ///< the highest bus seq the tap has shown us
    std::uint64_t payload_bytes = 0;          ///< bytes of payload held right now
    std::size_t payloads_retained = 0;
    std::uint64_t payloads_forgotten = 0;
};

/// Everything the recorder counted. Counters, not events — the recorder does not
/// narrate its own accounting into history (RTH-1 § derived chatter).
struct RecorderCounters {
    std::uint64_t observed = 0;          ///< events the tap handed us
    std::uint64_t recorded = 0;          ///< events that became a record
    std::uint64_t declined_by_policy = 0;///< observed, NotRetained by a rule
    std::uint64_t declined_internal = 0; ///< refused entry by the structural blacklist
    std::uint64_t log_records = 0;       ///< records appended to the persistent log
    std::uint64_t log_bytes = 0;         ///< bytes appended
    std::uint64_t log_refused = 0;       ///< records the log budget declined to write
};

/// Per-shape traffic, derived from counters rather than recorded as events.
struct ShapeTally {
    std::string shape;
    std::uint64_t observed = 0;
    std::uint64_t recorded = 0;
    std::uint64_t declined = 0;
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
    /// policy changes in order to observe them would be manufacturing the
    /// traffic it exists to watch.
    ///
    /// PROSPECTIVE (RTH-1 § 18). A smaller window governs FUTURE retention;
    /// nothing already retained is destroyed by the act of applying the policy.
    /// Records over the new bound are released one at a time as new traffic
    /// arrives, and the policy record states how many were over it at the moment
    /// of the change, so the trim is never a surprise. An explicit purge, if one
    /// is ever wanted, stays a separate operation.
    void apply_policy(RecorderPolicy next);

    // ---- the structural blacklist ------------------------------------------
    RecorderBlacklist& blacklist() noexcept { return blacklist_; }
    const RecorderBlacklist& blacklist() const noexcept { return blacklist_; }

    // ---- persistence --------------------------------------------------------
    /// Append retained records to `path` as they are recorded. Truthful and
    /// boring: an ordinary file handle, opened for append, written on the
    /// dispatch that produced the record, flushed and closed by `close_log()` or
    /// this recorder's destructor. No scheduler, no background thread, no
    /// compaction and no rotation.
    ///
    /// Returns false and sets `*error` if the file cannot be opened.
    bool open_log(const std::string& path, std::string* error = nullptr);
    void close_log();
    bool logging() const noexcept;
    const std::string& log_path() const noexcept { return log_path_; }

    // ---- the structured reader ---------------------------------------------
    //
    // Records, never strings. A presentation formats these; see recorder/dump.hpp.

    /// Every retained record, oldest first, across all windows.
    std::vector<HistoryRecord> snapshot() const;
    /// Retained records of one shape, oldest first.
    std::vector<HistoryRecord> snapshot_of(std::string_view shape) const;
    std::size_t retained() const noexcept;
    RecorderBounds bounds() const noexcept;
    RecorderCounters counters() const noexcept { return counters_; }
    std::vector<ShapeTally> tallies() const;

    /// What became of bus delivery `seq` — as one of the four honest answers.
    Lookup find(std::uint64_t bus_seq) const noexcept;
    /// One record by the recorder's own identity for it.
    const HistoryRecord* record(std::uint64_t record_seq) const noexcept;
    /// Whether this recorder still holds that record's payload, and the bytes if so.
    PayloadLookup payload(std::uint64_t record_seq) const;

    /// The schema the persistent log's records are written against — exposed so a
    /// reader can admit them through the one gate rather than parse them.
    static const std::shared_ptr<const Schema>& record_schema();

    /// Read a log written by `open_log` back into records. It re-admits every
    /// record through the gate, so a corrupt or forged log is refused rather than
    /// trusted (Arena's rule, and the reason the log is values and not text).
    /// Returns false and sets `*error` on an unreadable or malformed file.
    static bool read_log(const std::string& path, std::vector<HistoryRecord>* out,
                         std::string* error = nullptr);

private:
    /// A window with a runtime capacity. `BoundedHistory` is the same idea with a
    /// COMPILE-TIME one, which is exactly what a runtime-changeable policy cannot
    /// use; the semantics kept are its semantics — chronological, oldest
    /// released first, and the release is counted and never silent.
    struct Window {
        std::deque<HistoryRecord> records;
        std::size_t capacity = 0;
        std::uint64_t evicted = 0;
    };

    void observe(const BusEvent& e);
    RetentionClass classify(const BusEvent& e, bool* retain_payload) const;
    void admit(HistoryRecord rec, const BusEvent* e);
    void release(const HistoryRecord& rec);
    Window& window_for(const HistoryRecord& rec);
    void trim(Window& w);
    void store_payload(std::uint64_t record_seq, std::string bytes, std::string shape,
                       std::uint32_t version);
    void trim_payloads();
    void write_frame(const HistoryRecord& rec, const std::string& body);
    void append_to_log(const HistoryRecord& rec, const std::string& body);
    void note_log_stopped();

    Switchboard& bus_;
    ObserverId tap_ = 0;
    RecorderPolicy policy_;
    RecorderBlacklist blacklist_;

    Window shared_;
    Window protected_;
    std::map<std::string, Window> dedicated_;

    struct PayloadSlot {
        std::uint64_t record_seq = 0;
        std::string bytes;
        std::string shape;
        std::uint32_t version = 0;
    };
    std::deque<PayloadSlot> payloads_;
    std::uint64_t payload_bytes_ = 0;
    std::uint64_t payloads_forgotten_ = 0;
    /// The highest record_seq whose payload was released. Together with a
    /// record's own `PayloadDisposition` it separates "I had it and let it go"
    /// from "I never took it" exactly, with no per-record mutation.
    std::uint64_t payload_horizon_ = 0;

    std::uint64_t next_record_seq_ = 1;
    std::uint64_t newest_observed_seq_ = 0;
    std::uint64_t forgotten_ = 0;
    std::uint64_t forgotten_horizon_seq_ = 0;
    RecorderCounters counters_;
    std::map<std::string, ShapeTally> tallies_;

    std::string log_path_;
    struct LogFile;
    std::unique_ptr<LogFile> log_;
    bool log_stop_pending_ = false;
};

} // namespace loom

#endif // ZEN_RECORDER_RECORDER_HPP
