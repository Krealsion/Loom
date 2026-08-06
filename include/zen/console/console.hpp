// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_CONSOLE_CONSOLE_HPP
#define ZEN_CONSOLE_CONSOLE_HPP

// The Console engine — the first doing-layer component. A fully message-native bus
// participant: it discovers what's registered, composes and gate-sends messages, and
// receives replies into an indexed buffer. Frontend-agnostic and testable with NO
// terminal: this engine is the durable spine every later interface (the terminal now, a
// GUI later) inherits unchanged. It returns DOMAIN DATA (lists, field descriptors,
// received Values) — never formatted text, never a widget tree; formatting is the skin's
// job, replaceable without touching the engine.
//
// The console is the operator's hands on the bus: the most-granted participant (broad
// send + wildcard-accept + the tap + discovery), but each capability is a deliberate
// grant, never a bypass. It knows no shape's meaning — it drives shapes it has never seen.

#include <zen/schema.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/value.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace loom {

// ---- Bounded console history (COLD-2 C-1) ------------------------------------------------------
//
// The console is the operator's window, which makes it exactly the component most likely to be
// left running for weeks — so its retained state is bounded BY DESIGN, never by lifetime
// throughput. That is the same argument kJournalCapacity already won for the bus, applied to the
// surface COLD-2 caught growing: before this, every bus event and every delivered Value was kept
// forever, and the console accepts from any registered participant, so ordinary traffic grew it.
//
// Both windows below are HISTORY — past observations kept for inspection. Nothing is OWED on
// them (a delivered Value's obligation ends the moment it is recorded; nobody must drain either
// one), which is precisely what makes discarding the oldest entry legitimate here and illegitimate
// for a backlog. But never silently: Console::evicted() reports how much was dropped, and the
// reply buffer's mN labels are STABLE IDENTITIES, so an evicted m3 refuses rather than quietly
// re-binding to a newer reply.

/// Bus events retained on the tap. Deliberately the same width as `Switchboard::kJournalCapacity`:
/// one tap entry corresponds to roughly one journal entry, so an operator who can still SEE an
/// event on the tap can still ASK the journal what became of it. A wider tap would show events
/// whose outcome the bus has already forgotten; a narrower one would waste journal the operator
/// can no longer name.
inline constexpr std::size_t kConsoleTapCapacity = 1024;

/// Received reply Values retained in the m1/m2/... buffer. Sixteen times smaller than the tap
/// because the UNIT is far heavier: a TapEvent is a few short strings, while a wire-arrived Value
/// is bounded only by the decode materialization budget (kMaxDecodedCells — megabytes, worst
/// case). Same width as `RemoteConsole::kMaxPendingDelivered`, which bounds the *pending* half of
/// the same client-side reply path: at most that many replies waiting for a schema, at most that
/// many retained once admitted.
inline constexpr std::size_t kConsoleBufferCapacity = 64;

/// The console's bounded history window: the most recent `Capacity` observations in chronological
/// order, the oldest discarded — and COUNTED — when a new one arrives at capacity.
///
/// A ring over one vector, the same shape as the Switchboard's journal (`journal_[seq % cap]`):
/// the slots are claimed once at construction and never reallocated, an insert allocates nothing
/// and is O(1) for the life of the window, and no front-erasure ever shifts a tail. Storage is a
/// function of the capacity alone — never of how many observations have passed through it.
///
/// The slots are reserved UP FRONT rather than grown into deliberately. Leaving it to `push_back`
/// would also be bounded, but only because the two capacities here happen to be powers of two;
/// reserving makes "exactly `Capacity` slots, always" a property of this class instead of a
/// property of the standard library's growth policy, and makes the storage assertion in the tests
/// state that directly.
///
/// Deliberately not a general retention framework: no policies, no configuration, no persistence.
/// It exists because four console history surfaces (the engine's tap, the console weave's reply
/// buffer, and the remote console's copies of both) need exactly these semantics, and writing the
/// ring index arithmetic and the eviction counter four times is how one of them ends up wrong.
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
    /// this window; it is the operator's answer to "is this the complete history?".
    std::uint64_t evicted() const noexcept { return evicted_; }

    /// The i-th RETAINED entry, 0 = oldest retained. A position within the window, never a stable
    /// identity — the caller owns whatever identity it publishes (see ConsoleEngine::buffer_at).
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

    std::vector<T> ring_;
    std::size_t oldest_ = 0;    ///< index of the oldest retained entry (0 until the ring wraps)
    std::uint64_t evicted_ = 0;
};

/// A registered shape's identity.
struct ShapeRef {
    std::string name;
    std::uint32_t version;
};

/// A live Weave, as discovery sees it: its id and the shapes it accepts.
struct WeaveInfo {
    loom::WeaveId id;
    std::vector<ShapeRef> accepts;
};

/// One field of a shape, read from the registry (compose-time guidance).
struct FieldDesc {
    std::string name;
    std::string type; ///< the kind's spelling ("Int", "Text", "List<Int>", "Message(Foo v1)")
    bool required;
};

/// A shape's full description (its fields), for the operator to fill.
struct ShapeDesc {
    std::string name;
    std::uint32_t version;
    std::vector<FieldDesc> fields;
};

/// A typed field value for compose-by-name. Stage 1 carries the scalar kinds; Message/
/// List fields are not composable yet (a required one left unset is caught at the gate).
using FieldValue = std::variant<std::int64_t, double, std::string, bool, loom::Bytes>;

/// The fate of a submitted message, surfaced from the gate (never a silent mis-send).
struct SendOutcome {
    bool delivered = false;
    bool refused = false;
    std::string reason; ///< the gate's verdict / routing refusal, on refusal
};

/// A buffered reply (m1, m2, …): its label, shape, and the received Value (read fields by
/// name off `value`). The Stage-2 `$m1.field` reference syntax reads from this.
struct BufferEntry {
    std::string label;
    std::string name;
    std::uint32_t version;
    loom::Value value;
};

/// A reference to a field of a buffered reply — `$m1.count` → {label "m1", field "count"}.
/// A reference is a *wire*: one message's output read into another's input. The engine
/// owns resolution (the terminal only lexes the `$label.field` token).
struct Ref {
    std::string label; ///< the buffer label, e.g. "m1"
    std::string field; ///< a field of that entry's Value
};

/// One argument to the assumption ladder: a literal or a reference, optionally named
/// (`field=…`). A named arg is assigned to that field; a bare (unnamed) arg is a
/// positional/type-directed candidate.
struct Arg {
    std::optional<std::string> name;       ///< set iff `field=…` (the named rung)
    std::variant<FieldValue, Ref> value;   ///< a literal value or a reference
};

/// The result of running the assumption ladder.
struct Composed {
    enum class Status { Ready, NeedsInput, Error };
    Status status = Status::Error;
    loom::Ticket ticket{};            ///< Ready: the assembled, gate-sent message's ticket
    std::string error;                   ///< Error: the compose-time verdict
    std::vector<FieldDesc> open_fields;  ///< NeedsInput: the still-unfilled fields
    std::vector<std::string> unplaced;   ///< NeedsInput: args (rendered) the ladder could not place
};

/// A copied bus event for the operator's window on the live bus (the tap).
struct TapEvent {
    std::string kind;    ///< "Delivered" / "Refused" / "Died" / "Revived"
    loom::WeaveId target;
    loom::WeaveId sender;
    std::string schema;  ///< the payload/state shape name
    std::string refusal; ///< the refusal reason (empty unless Refused)
};

class ConsoleWeave; // the console's own raw Weave (buffers received Values); defined in the .cpp

/// Per-region change flags for message-driven partial redraw (the retained-mode / Zengine point): a
/// UI repaints only the regions whose data changed, and the change signal is bus messages. A
/// top-level type (not nested) so the Console interface below can return it. Set inside the single
/// bus observer (record_tap) as events arrive during pump(): `buffer` on a reply delivered to the
/// console, `weaves` on a Weave dying/reviving, `tap` on any bus event. The compose/guidance regions
/// are keystroke-driven (the input loop redraws them), so they are not tracked here.
struct Dirty {
    bool weaves = false;
    bool buffer = false;
    bool tap = false;
    bool any() const noexcept { return weaves || buffer || tap; }
};

/// How much bounded console history has been discarded — per region, mirroring Dirty (the weave
/// list has no row because it is a REPLACED snapshot of who is registered now, not a history).
/// This is the operator's line between "this is the complete history" and "older evidence was
/// evicted": a bounded diagnostic surface that pretended to be complete would trade a memory lie
/// for an observability lie.
///
/// `buffer` is also the reply buffer's LABEL BASE. Labels are stable identities, not positions, so
/// the retained entries are always m(buffer + 1) ... m(buffer + buffer_size()): a reference that
/// resolved to m7 yesterday either still resolves to that same reply or refuses — it never
/// silently re-binds to a different one. A caller walking the buffer walks that range, not
/// 1..buffer_size().
struct Evicted {
    std::uint64_t tap = 0;    ///< bus events dropped from the tap window (kConsoleTapCapacity)
    std::uint64_t buffer = 0; ///< replies dropped from the m-buffer (kConsoleBufferCapacity)
    bool any() const noexcept { return tap != 0 || buffer != 0; }
};

/// The frontend-facing console surface — what a renderer/controller (the TUI now, a GUI later, the
/// remote client) drives, INDEPENDENT of where the bus lives. ConsoleEngine implements it in-process
/// (direct bus calls); RemoteConsole implements it over the operator-protocol on a socket. This is
/// the decision-#2 unification: "a remote console cannot hold a Switchboard& across a socket", so the
/// frontend depends on THIS interface and only the transport differs. Discovery and the tap stop
/// being privileged host-side methods baked into one class and become an interface a remote
/// transport answers with messages.
class Console {
public:
    virtual ~Console() = default;

    // Discovery (registry-read; works on shapes never seen).
    virtual std::vector<WeaveInfo> weaves() const = 0;
    virtual std::optional<ShapeDesc> describe(std::string_view name, std::uint32_t version) const = 0;

    // Compose + gated send via the assumption ladder (named -> positional -> type-directed -> prompt).
    virtual Composed compose(loom::WeaveId target, std::string_view name, std::uint32_t version,
                             const std::vector<Arg>& args) = 0;

    // The reply buffer (m1, m2, ...) — bounded at kConsoleBufferCapacity retained entries.
    /// How many replies are RETAINED (not how many arrived — see evicted()).
    virtual std::size_t buffer_size() const = 0;
    /// The reply whose stable label is `mN`, or nullopt if N never arrived or was evicted. N is an
    /// IDENTITY, not a position: the retained range is m(evicted().buffer + 1) .. m(evicted().buffer
    /// + buffer_size()), and a label outside it refuses rather than answering with another reply.
    virtual std::optional<BufferEntry> buffer_at(std::size_t label_number) const = 0;

    // The tap (operator's window on the live bus) + the message-driven dirty signal.
    /// The retained tap window, oldest retained first (at most kConsoleTapCapacity entries).
    virtual std::vector<TapEvent> tap() const = 0;
    virtual Dirty take_dirty() = 0;

    /// How much history each bounded window has discarded. Never resets while the console lives;
    /// a fresh console starts a fresh window at zero.
    virtual Evicted evicted() const = 0;

    // Drive the transport so sends are delivered and replies/tap arrive (in-process: pump the bus;
    // remote: flush + poll the socket and process the pushed frames).
    virtual void pump() = 0;
};

/// The assumption ladder's host surface — segregated from the frontend Console so the ONE ladder
/// implementation (run_compose_ladder) is shared by the in-process engine and the client-side remote
/// console instead of duplicating ~150 lines of intricate placement logic. The three operations the
/// ladder needs: resolve a schema, resolve a `$mN.field` reference off the buffer, and assemble +
/// gate-send the composed Value (the gate stays the unconditional backstop in both).
class LadderHost {
public:
    virtual ~LadderHost() = default;
    virtual std::shared_ptr<const loom::Schema> resolve_schema(std::string_view name,
                                                               std::uint32_t version) const = 0;
    virtual std::optional<loom::Cell> resolve_ref(const Ref& ref, std::string* error) const = 0;
    virtual loom::Ticket assemble_and_send(loom::WeaveId target,
                                           const std::shared_ptr<const loom::Schema>& schema,
                                           const std::map<std::string, loom::Cell>& cells) = 0;
};

/// The assumption ladder, extracted as a free function over LadderHost so local + remote share it.
/// named wins -> positional (declaration order, all-or-falls) -> type-directed (unique fit) -> prompt
/// (NeedsInput; never guess on ambiguity, never mis-send). On Ready it assembles + gate-sends.
Composed run_compose_ladder(LadderHost& host, loom::WeaveId target, std::string_view name,
                            std::uint32_t version, const std::vector<Arg>& args);

/// Resolve `$label.field` against a Console's reply buffer — shared by the in-process engine and the
/// remote console (both expose buffer_at()). Reads a scalar Cell off an immutable buffered Value;
/// nullopt + *error on a missing entry, missing field, or non-scalar field (Stage 2 is scalar-only).
std::optional<loom::Cell> resolve_ref_from(const Console& console, const Ref& ref,
                                           std::string* error);

/// Build a ShapeDesc (name/version + each field's type spelling and required-ness) from a resolved
/// Schema — shared by the in-process describe() (over the bus registry) and the remote describe()
/// (over a schema reconstructed from a Schema reply).
ShapeDesc describe_schema(const loom::Schema& schema);

/// The frontend-agnostic console engine — the in-process Console. Construct it over a Switchboard; it
/// registers the console as an in-process Weave (broad grant + accept-any) and subscribes the tap.
class ConsoleEngine : public Console, public LadderHost {
public:
    explicit ConsoleEngine(loom::Switchboard& bus);
    ~ConsoleEngine() override;
    ConsoleEngine(const ConsoleEngine&) = delete;
    ConsoleEngine& operator=(const ConsoleEngine&) = delete;

    loom::WeaveId console_id() const noexcept { return console_id_; }

    // ---- Discovery (registry-read; works on shapes the console has never seen) ----
    std::vector<WeaveInfo> weaves() const override;
    std::optional<ShapeDesc> describe(std::string_view name, std::uint32_t version) const override;

    // ---- Compose + gated send ----
    /// One-shot: set fields by name, assemble, and gate-send to `target`. Returns the send
    /// Ticket (the send is enqueued — pump(), then read outcome()). On a compose-time error
    /// (no such shape/field, or a type mismatch) returns an invalid Ticket and sets *error.
    loom::Ticket submit(loom::WeaveId target, std::string_view name, std::uint32_t version,
                           const std::map<std::string, FieldValue>& fields,
                           std::string* error = nullptr);

    /// The fate of a previously-submitted Ticket (after pump()).
    SendOutcome outcome(loom::Ticket t) const;

    // ---- Stage 2: references + the assumption ladder (the dataflow brain) ----
    /// Resolve `$label.field` off the indexed buffer to a typed scalar Cell — a reference
    /// *read* of an immutable buffered Value (it cannot mutate the buffer). Returns nullopt
    /// + sets *error on a missing entry, a missing field, or a non-scalar field (Stage 2 is
    /// scalar-only). Independently testable. (LadderHost: the in-process engine reads its buffer.)
    std::optional<loom::Cell> resolve_ref(const Ref& ref, std::string* error = nullptr) const override;

    /// Resolve a registered schema by identity (LadderHost): the in-process engine reads the bus's
    /// registry. (The remote console reads its own registry, filled by Describe replies.)
    std::shared_ptr<const loom::Schema> resolve_schema(std::string_view name,
                                                       std::uint32_t version) const override;

    /// Compose by the assumption ladder: assign literal/reference args (each optionally
    /// named) to the target's fields — named wins, then positional (declaration order), then
    /// type-directed, else NeedsInput (prompt — never guess on genuine ambiguity, never
    /// mis-send). On Ready it assembles and gate-sends (the gate is the unconditional
    /// backstop). A wrong-typed named arg or a bad reference is a clean Error. Delegates to the
    /// shared run_compose_ladder (this engine IS the LadderHost).
    Composed compose(loom::WeaveId target, std::string_view name, std::uint32_t version,
                     const std::vector<Arg>& args) override;

    // ---- Reply buffer (m1, m2, …) ----
    std::size_t buffer_size() const override;
    std::optional<BufferEntry> buffer_at(std::size_t label_number) const override;

    // ---- The tap (operator's window on the live bus) ----
    std::vector<TapEvent> tap() const override { return tap_.snapshot(); }

    /// What each bounded window has discarded (and the buffer's stable-label base).
    Evicted evicted() const override;

    /// Read AND CLEAR the accumulated per-region dirty flags (consume-once, so a renderer pumps
    /// then repaints exactly the changed regions). Not const — it resets the flags.
    Dirty take_dirty() noexcept override;

    /// Convenience: drive the bus so sends are delivered and replies buffered.
    void pump() override;

private:
    friend struct ConsoleHistoryProbe; ///< reads tap_ / the reply window's own storage (see above)

    loom::Ticket assemble_and_send(loom::WeaveId target,
                                      const std::shared_ptr<const loom::Schema>& schema,
                                      const std::map<std::string, loom::Cell>& cells) override;
    void record_tap(const loom::BusEvent& e);
    /// The reply window the console's own Weave holds. Defined in the .cpp, where ConsoleWeave is
    /// complete — the type is opaque here, which is exactly why buffer_at cannot reach it inline.
    const BoundedHistory<loom::Value, kConsoleBufferCapacity>& reply_history() const;

    loom::Switchboard& bus_;
    ConsoleWeave* weave_ = nullptr; // owned by the bus; non-owning here
    loom::WeaveId console_id_{};
    loom::ObserverId tap_obs_ = 0;
    std::uint64_t correlation_ = 0;
    BoundedHistory<TapEvent, kConsoleTapCapacity> tap_; // history: bounded window, oldest evicted
    Dirty dirty_; // accumulated by record_tap; drained by take_dirty
};

} // namespace loom

#endif // ZEN_CONSOLE_CONSOLE_HPP
