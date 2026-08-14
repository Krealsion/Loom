// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HISTORY_LOGGER_HPP
#define ZEN_HISTORY_LOGGER_HPP

// WHAT ZEN CHOSE NOT TO FORGET (RTH-1a).
//
// The other half of the split RTH-1a makes. The Recorder answers "what do I know
// right now?" out of bounded, volatile windows that traffic pushes through in
// seconds. This answers a different question, and it must not be a bigger version
// of the first one:
//
//   RECORDER   admits by default, keeps a little of everything, forgets fast.
//   LOGGER     REFUSES by default, keeps almost nothing, and keeps it for good.
//
// A WHITELIST, NOT A BUDGET. RTH-1's persistence was the recorder's window written
// to a file behind a global 8 MiB ceiling — which meant an idle application's
// heartbeat could consume the horizon in about three minutes and a weave
// replacement an hour later would be silently unwritable. That is the exact
// failure this half exists to prevent: nothing is durable unless it was NAMED, and
// what was named is not capped by traffic that was not.
//
// IT DOES NOT READ THE RECORDER. It takes its own tap on the same Switchboard and
// captures a selected fact from the OBSERVATION, on the dispatch that produced it.
// Hoping a Recorder entry still exists at write time would make durability depend
// on a volatile window's mood — so there is no `Recorder&` here, on purpose, and a
// Logger works in a process that has no Recorder at all.
//
// NOT IN THE DELIVERY PATH. Like the Recorder, it is a tap consumer: a message
// reaches its recipient without passing through this, and turning logging on
// changes what is remembered and nothing about what is delivered.
//
// THREE ORIGINS, AND THE RULE THAT MATTERS. A durable record may come from
// somewhere other than the bus — the host says something directly, or this
// Logger's own selection changes — but it must NEVER pretend to have been a Loom
// message. Nothing here manufactures bus traffic to get a fact written down.
//
// DELIBERATELY NOT HERE, and each is a later bounded phase, not an omission:
// rotation, compaction, batching, a background writer, a scheduler, a database, a
// query grammar, display filters, crash bundles, stack walking.

#include <zen/history/record.hpp>
#include <zen/schema.hpp>
#include <zen/switchboard.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

// ---------------------------------------------------------------------------
// What a durable record is
// ---------------------------------------------------------------------------

/// WHERE A DURABLE RECORD CAME FROM. The discriminant, and the one field a reader
/// must consult before believing anything else in the record.
enum class LogOrigin : std::uint8_t {
    /// A fact the BUS stated, observed at the tap. `observation` is meaningful.
    BusObservation,
    /// The HOST said this directly. No message existed; `observation` is empty and
    /// must not be read as one. This is how a diagnostic reaches durable history
    /// without a fake `Loom` message being invented to carry it.
    Diagnostic,
    /// This Logger's own selection or retention changed. Written by the Logger, to
    /// its own stream, with no traffic on the bus.
    PolicyChange
};

/// How loud a direct diagnostic is. Deliberately three levels and not a taxonomy:
/// the bus supplies no severity for an observation, so this exists only for the
/// records the host authors itself.
enum class Severity : std::uint8_t { Info, Warning, Error };

const char* name_of(LogOrigin o) noexcept;
const char* name_of(Severity s) noexcept;

/// One durable record.
struct LogRecord {
    /// The LOGGER's own monotonic number, and this record's identity in the stream.
    /// Distinct from `observation.record_seq`, which is a Recorder's number for a
    /// Recorder's copy of the same fact and is 0 here — the two halves number their
    /// own memories and neither is a key into the other.
    std::uint64_t log_seq = 0;
    LogOrigin origin = LogOrigin::BusObservation;

    /// Meaningful IFF `origin == BusObservation`. Its `record_seq` is 0 and its
    /// `held` mask is empty, and both are honest: a Recorder's number for a
    /// Recorder's copy is not a key into this stream, and a window claim is a
    /// live fact about a different object.
    HistoryRecord observation;

    /// THE MESSAGE ITSELF, canonical bytes, for a `BusObservation` that carried
    /// one. A durable record of a rare fact WITHOUT its content is half a record —
    /// and the whole design is that a Logger selects almost nothing, so paying for
    /// the bytes is affordable exactly where they matter. `observation.payload`
    /// says which: `Retained` when these bytes are here, `None` when the event
    /// carried none.
    std::string payload_body;

    /// Meaningful for `Diagnostic` and `PolicyChange`.
    Severity severity = Severity::Info;
    std::string source; ///< Diagnostic: which host subsystem said it
    std::string text;
};

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

/// ONE WHITELIST ENTRY: a shape that is worth keeping for good.
struct LogRule {
    std::string shape;
    /// How many records of this shape to append before stopping, or 0 for
    /// UNCAPPED — the default, and the point of §11: a rare durable fact should not
    /// need a number, and a global ceiling shared with traffic nobody selected is
    /// exactly the horizon RTH-1a removes. A cap that is reached is stated once, as
    /// a `PolicyChange` record, so a reader can never mistake a cap for an ending.
    std::size_t cap = 0;
};

/// WHAT THIS LOGGER KEEPS. Small, structured, and not a language: shape names plus
/// two structural switches keyed on what the BUS did, mirroring the Recorder's
/// structural protection because the same argument applies — no shape can declare
/// itself the one whose handler threw.
struct LoggerSelection {
    /// ANY shape, when its handler was entered and did not complete (MSG-10). The
    /// single most valuable durable fact this system produces, and unselectable by
    /// shape because failure is not a property of a shape.
    bool log_handler_failures = true;
    /// Died / Revived: a participant left or came back. Rare by construction.
    bool log_lifecycle = true;
    /// Refusals are OFF by default and that is measured, not assumed: RTH-1's live
    /// Workshop run found every `KeyReleased` reaching nobody, so ordinary refusals
    /// are neither rare nor severe. A host that wants them says so.
    bool log_refusals = false;
    std::vector<LogRule> shapes;

    const LogRule* rule_for(std::string_view shape) const noexcept;
};

/// THE CONSERVATIVE DEFAULT, source-traced and deliberately narrow.
///
/// Two categories, each rare BY CONSTRUCTION rather than by hope, and each one a
/// maker reaches for after something went wrong:
///
///   WHAT CODE IS LOADED   the control door's LoadLibrary / ReloadLibrary /
///                         UnloadLibrary / UnloadRole, and the weave manager's
///                         zen.LoadWeave / zen.SwapWeave / zen.ReloadWeave.
///   WHO MAY SPEAK         the Weaver's zen.RequestAuthority / ApproveAuthority /
///                         RefuseAuthority / RevokeAuthority / AuthorityGranted.
///
/// Plus, structurally, every handler failure and every lifecycle transition.
///
/// DELIBERATELY ABSENT, and each for a stated reason: the QUERIES beside those
/// changes (`ListLibraries`, `QueryRole`, `ListLoaded`, `DescribeAuthority`,
/// `AuthorityDescription`, `ManagerState`) — a read is not a change; the handoff
/// vocabulary (`zen.PrepareShutdown`, `zen.Bequest`, `zen.ClaimBequest`) — the
/// replacement it belongs to is already marked by `zen.SwapWeave` and by the
/// structural Died/Revived, and a smaller default is the instruction; and every
/// application shape, because this list is Loom's and a host's traffic is the
/// host's to name.
LoggerSelection default_selection();

// ---------------------------------------------------------------------------
// The logger
// ---------------------------------------------------------------------------

/// What the logger counted. Counters, not events — a logger that narrated its own
/// accounting would be writing the traffic it exists to select from.
struct LoggerCounters {
    std::uint64_t observed = 0;    ///< events the tap handed us
    std::uint64_t selected = 0;    ///< events the selection matched
    std::uint64_t appended = 0;    ///< records written to the stream
    std::uint64_t bytes = 0;       ///< bytes written by this logger to this stream
    std::uint64_t capped = 0;      ///< records a per-shape cap declined
    std::uint64_t unwritable = 0;  ///< records that could not be serialized or written
    std::uint64_t diagnostics = 0; ///< records the host wrote directly
};

class Logger {
public:
    /// Attaches a tap for its whole lifetime. The bus must outlive the logger,
    /// exactly as it must outlive a Recorder or a ConsoleEngine. Holding a
    /// `Switchboard&` IS root authority; no new observation law is needed and none
    /// is invented.
    ///
    /// It writes NOTHING until `open` succeeds — a Logger with no destination is a
    /// live selection and an empty stream, which is honest and costs one map lookup
    /// per observation.
    explicit Logger(Switchboard& bus, LoggerSelection selection = default_selection());
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /// Append to `path`. An ordinary file handle opened for append, written on the
    /// dispatch that produced the record, flushed and closed by `close()` or the
    /// destructor. No scheduler, no background thread, no compaction, no rotation.
    ///
    /// NO GLOBAL BYTE BUDGET. Durable append is uncapped by default and per-shape
    /// caps are the only bound, so ordinary traffic can never consume the horizon a
    /// later critical fact needs. Uncapped means uncapped APPEND — this object
    /// accumulates no records in memory at all, only counters.
    bool open(const std::string& path, std::string* error = nullptr);
    void close();
    bool open() const noexcept;
    const std::string& path() const noexcept { return path_; }

    // ---- selection ----------------------------------------------------------
    const LoggerSelection& selection() const noexcept { return selection_; }
    /// Change what is kept, and KEEP THAT CHANGE. Writes one `PolicyChange` record
    /// describing the transition and sends nothing.
    void select(LoggerSelection next);

    // ---- direct records ------------------------------------------------------
    /// THE HOST'S OWN WORDS, made durable without inventing a message to carry them.
    /// The written record's origin is `Diagnostic`, so no reader can mistake it for
    /// something the bus said.
    bool write(Severity sev, std::string_view source, std::string_view text);
    bool info(std::string_view source, std::string_view text) {
        return write(Severity::Info, source, text);
    }
    bool warn(std::string_view source, std::string_view text) {
        return write(Severity::Warning, source, text);
    }
    bool error(std::string_view source, std::string_view text) {
        return write(Severity::Error, source, text);
    }

    // ---- introspection -------------------------------------------------------
    LoggerCounters counters() const noexcept { return counters_; }
    /// How many records of `shape` this logger has appended — the per-shape cap's
    /// own counter, and the smallest thing a test needs to prove a cap is real.
    std::uint64_t appended_of(std::string_view shape) const noexcept;

    /// The schema the durable stream's records are written against — exposed so a
    /// reader can admit them through the one gate rather than parse them.
    static const std::shared_ptr<const Schema>& record_schema();

    /// Read a stream written by `open` back into records. It re-admits every record
    /// through the gate, so a corrupt or forged file is refused rather than trusted
    /// (Arena's rule, and the reason the stream is values and not text). Returns
    /// false and sets `*error` on an unreadable or malformed file.
    static bool read(const std::string& path, std::vector<LogRecord>* out,
                     std::string* error = nullptr);

private:
    void observe(const BusEvent& e);
    /// Read one event into a durable record, payload included. Shared by the
    /// ordinary path and the cap-boundary one so they cannot capture differently.
    void capture(LogRecord& rec, const BusEvent& e);
    bool append(const LogRecord& rec);
    void note(std::string text);

    Switchboard& bus_;
    ObserverId tap_ = 0;
    LoggerSelection selection_;
    std::map<std::string, std::uint64_t, std::less<>> appended_;

    std::uint64_t next_log_seq_ = 1;
    std::string path_;
    struct Stream;
    std::unique_ptr<Stream> out_;
    LoggerCounters counters_;
};

} // namespace loom

#endif // ZEN_HISTORY_LOGGER_HPP
