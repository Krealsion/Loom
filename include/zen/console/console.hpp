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

namespace zen::console {

/// A registered shape's identity.
struct ShapeRef {
    std::string name;
    std::uint32_t version;
};

/// A live Shard, as discovery sees it: its id and the shapes it accepts.
struct ShardInfo {
    zen::sb::ShardId id;
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
using FieldValue = std::variant<std::int64_t, double, std::string, bool, zen::Bytes>;

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
    zen::Value value;
};

/// A copied bus event for the operator's window on the live bus (the tap).
struct TapEvent {
    std::string kind;    ///< "Delivered" / "Refused" / "Died" / "Revived"
    zen::sb::ShardId target;
    zen::sb::ShardId sender;
    std::string schema;  ///< the payload/state shape name
    std::string refusal; ///< the refusal reason (empty unless Refused)
};

class ConsoleShard; // the console's own raw Shard (buffers received Values); defined in the .cpp

/// The frontend-agnostic console engine. Construct it over a Switchboard; it registers the
/// console as an in-process Shard (broad grant + accept-any) and subscribes the tap.
class ConsoleEngine {
public:
    explicit ConsoleEngine(zen::sb::Switchboard& bus);
    ~ConsoleEngine();
    ConsoleEngine(const ConsoleEngine&) = delete;
    ConsoleEngine& operator=(const ConsoleEngine&) = delete;

    zen::sb::ShardId console_id() const noexcept { return console_id_; }

    // ---- Discovery (registry-read; works on shapes the console has never seen) ----
    std::vector<ShardInfo> shards() const;
    std::optional<ShapeDesc> describe(std::string_view name, std::uint32_t version) const;

    // ---- Compose + gated send ----
    /// One-shot: set fields by name, assemble, and gate-send to `target`. Returns the send
    /// Ticket (the send is enqueued — pump(), then read outcome()). On a compose-time error
    /// (no such shape/field, or a type mismatch) returns an invalid Ticket and sets *error.
    zen::sb::Ticket submit(zen::sb::ShardId target, std::string_view name, std::uint32_t version,
                           const std::map<std::string, FieldValue>& fields,
                           std::string* error = nullptr);

    /// Step-wise builder for the guided walk: begin -> set_field… -> send.
    bool begin(zen::sb::ShardId target, std::string_view name, std::uint32_t version,
               std::string* error = nullptr);
    bool set_field(std::string_view field, const FieldValue& value, std::string* error = nullptr);
    zen::sb::Ticket send(std::string* error = nullptr);

    /// The fate of a previously-submitted Ticket (after pump()).
    SendOutcome outcome(zen::sb::Ticket t) const;

    // ---- Reply buffer (m1, m2, …) ----
    std::size_t buffer_size() const;
    std::optional<BufferEntry> buffer_at(std::size_t one_based_index) const;

    // ---- The tap (operator's window on the live bus) ----
    std::vector<TapEvent> tap() const { return tap_; }

    /// Convenience: drive the bus so sends are delivered and replies buffered.
    void pump();

private:
    struct Compose {
        zen::sb::ShardId target;
        std::shared_ptr<const zen::Schema> schema;
        std::map<std::string, zen::Cell> cells;
    };

    zen::sb::Ticket assemble_and_send(zen::sb::ShardId target,
                                      const std::shared_ptr<const zen::Schema>& schema,
                                      const std::map<std::string, zen::Cell>& cells);
    void record_tap(const zen::sb::BusEvent& e);

    zen::sb::Switchboard& bus_;
    ConsoleShard* shard_ = nullptr; // owned by the bus; non-owning here
    zen::sb::ShardId console_id_{};
    zen::sb::ObserverId tap_obs_ = 0;
    std::uint64_t correlation_ = 0;
    std::vector<TapEvent> tap_;
    std::optional<Compose> compose_;
};

} // namespace zen::console

#endif // ZEN_CONSOLE_CONSOLE_HPP
