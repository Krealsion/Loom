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

#include <zen/bounded_history.hpp>
#include <zen/schema.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/terminal/composer.hpp>
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
//
// The window TYPE itself (`loom::BoundedHistory`) moved down to <zen/bounded_history.hpp> at
// TERM-0, when a terminal participant's transcript became the fifth and sixth surface needing
// exactly these semantics. Its own comment always said it should be written once; the constants
// below stay here, because a capacity is a policy about ONE surface and this is the console's.

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

// The compose-time vocabulary — FieldDesc, ShapeDesc, FieldValue, Ref, Arg, describe_schema and
// the assumption ladder itself — moved down to <zen/terminal/composer.hpp> at TERM-0, so a second
// participant (a terminal session with its own identity and its own vocabulary) could reuse the
// ONE ladder rather than grow a second one. Every name is unchanged, in this same namespace; what
// changed is only which file declares it, and that the ladder can now stop one step before
// sending. `Composed` and `LadderHost` below are the console's original one-shot form, kept
// exactly as they were.

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

/// The result of running the assumption ladder AND gate-sending.
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

/// The assumption ladder's host surface — the two lookups `ComposeSource` already names, plus the
/// one send. Segregated from the frontend Console so the ONE ladder implementation is shared by
/// the in-process engine and the client-side remote console instead of duplicating the placement
/// logic; since TERM-0 the ladder itself is `loom::compose_message` and this is the console's
/// one-shot form over it (the gate stays the unconditional backstop in both transports).
class LadderHost : public ComposeSource {
public:
    virtual loom::Ticket assemble_and_send(loom::WeaveId target,
                                           const std::shared_ptr<const loom::Schema>& schema,
                                           const std::map<std::string, loom::Cell>& cells) = 0;
};

/// Compose by the assumption ladder and, on Ready, assemble + gate-send through the host.
/// named wins -> positional (declaration order, all-or-falls) -> type-directed (unique fit) -> prompt
/// (NeedsInput; never guess on ambiguity, never mis-send).
Composed run_compose_ladder(LadderHost& host, loom::WeaveId target, std::string_view name,
                            std::uint32_t version, const std::vector<Arg>& args);

/// Resolve `$label.field` against a Console's reply buffer — shared by the in-process engine and the
/// remote console (both expose buffer_at()). Reads a scalar Cell off an immutable buffered Value;
/// nullopt + *error on a missing entry, missing field, or non-scalar field (Stage 2 is scalar-only).
std::optional<loom::Cell> resolve_ref_from(const Console& console, const Ref& ref,
                                           std::string* error);

/// The frontend-agnostic console engine — the in-process Console. Construct it over a Switchboard; it
/// registers the console as an in-process Weave (broad grant + accept-any) and subscribes the tap.
class ConsoleEngine : public Console, public LadderHost {
public:
    /// `vocabulary` is the shapes this operator window declares it expects to be TOLD —
    /// normally none. It exists because `AcceptMode::AnyRegistered` means "any shape the
    /// registry can resolve", and the registry learns a shape from some weave's accept-set:
    /// a notification shape that only ever travels to the operator has nobody else to
    /// declare it, and was therefore refused at the console's own door. Declaring it here is
    /// the host saying what this window is for. It is not a grant and not a bypass — a
    /// listed door is gated exactly as the wildcard one is.
    explicit ConsoleEngine(loom::Switchboard& bus,
                           std::vector<std::shared_ptr<const loom::Schema>> vocabulary = {});
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
