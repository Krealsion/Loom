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

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace loom {

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

    // The reply buffer (m1, m2, ...).
    virtual std::size_t buffer_size() const = 0;
    virtual std::optional<BufferEntry> buffer_at(std::size_t one_based_index) const = 0;

    // The tap (operator's window on the live bus) + the message-driven dirty signal.
    virtual std::vector<TapEvent> tap() const = 0;
    virtual Dirty take_dirty() = 0;

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
    std::optional<BufferEntry> buffer_at(std::size_t one_based_index) const override;

    // ---- The tap (operator's window on the live bus) ----
    std::vector<TapEvent> tap() const override { return tap_; }

    /// Read AND CLEAR the accumulated per-region dirty flags (consume-once, so a renderer pumps
    /// then repaints exactly the changed regions). Not const — it resets the flags.
    Dirty take_dirty() noexcept override;

    /// Convenience: drive the bus so sends are delivered and replies buffered.
    void pump() override;

private:
    loom::Ticket assemble_and_send(loom::WeaveId target,
                                      const std::shared_ptr<const loom::Schema>& schema,
                                      const std::map<std::string, loom::Cell>& cells) override;
    void record_tap(const loom::BusEvent& e);

    loom::Switchboard& bus_;
    ConsoleWeave* weave_ = nullptr; // owned by the bus; non-owning here
    loom::WeaveId console_id_{};
    loom::ObserverId tap_obs_ = 0;
    std::uint64_t correlation_ = 0;
    std::vector<TapEvent> tap_;
    Dirty dirty_; // accumulated by record_tap; drained by take_dirty
};

} // namespace loom

#endif // ZEN_CONSOLE_CONSOLE_HPP
