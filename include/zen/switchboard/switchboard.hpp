#ifndef ZEN_SWITCHBOARD_SWITCHBOARD_HPP
#define ZEN_SWITCHBOARD_SWITCHBOARD_HPP

#include <zen/admission.hpp>
#include <zen/registry.hpp>
#include <zen/schema.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/switchboard/shard.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zen::sb {

/// Why a delivery (or revival) was refused. Conformance refusals carry a
/// zen-core Error (the gate's verdict); the rest are bus-level routing reasons
/// the gate never sees.
enum class RefusalReason : std::uint8_t {
    None = 0,
    NoSuchTarget,      ///< directed at a ShardId that is not registered
    TargetUnavailable, ///< the target is currently dead (awaiting revival)
    NotAccepted,       ///< the target's accept-set has no schema of this (name, version)
    GateRefused,       ///< routing passed, but admit() refused — see `error`
};

const char* name_of(RefusalReason r) noexcept;

/// A structured refusal. When `reason == GateRefused`, `error` is the zen-core
/// gate error (kind, field path, expected/actual).
struct Refusal {
    RefusalReason reason = RefusalReason::None;
    Error error{};

    std::string message() const;
};

enum class Disposition : std::uint8_t { Pending, Delivered, Refused };

/// The fate of one queued delivery.
struct DeliveryOutcome {
    Disposition disposition = Disposition::Pending;
    Refusal refusal{}; ///< populated iff disposition == Refused
};

/// What an observer/tap is told about. Deliveries (Delivered/Refused) and
/// lifecycle transitions (Died/Revived) flow through the same hook.
enum class EventKind : std::uint8_t { Delivered, Refused, Died, Revived };

struct BusEvent {
    EventKind kind = EventKind::Delivered;
    std::uint64_t seq = 0;  ///< delivery seq (0 for lifecycle events)
    ShardId target{};
    ShardId sender{};
    std::string schema_name;       ///< payload (delivery) or state (lifecycle) schema
    std::uint32_t schema_version = 0;
    Refusal refusal{};             ///< for Refused, and for a failed/fallback Revived
    bool from_last_known_good = false; ///< for Revived
    const Value* payload = nullptr; ///< for Delivered; valid only during the callback
};

using Observer = std::function<void(const BusEvent&)>;
using ObserverId = std::uint64_t;

/// The result of a revival attempt.
struct ReviveOutcome {
    bool revived = false;
    bool from_last_known_good = false;
    bool reloads_exhausted = false;
    bool policy_malformed = false;
    Refusal refusal{}; ///< why the candidate was refused, when applicable
};

/// The fixed lifecycle-policy grammar — the ONE schema the Switchboard hard-codes
/// (its own grammar, not an application type): { max_reloads: Int,
/// revive_from_last_good: Bool }. A Shard's policy() is validated against this and
/// only these two fields are read.
std::shared_ptr<const Schema> lifecycle_policy_schema();

/// The first live boundary: an in-process message bus that gates every delivery
/// through zen-core's one validator. It reimplements no validation, schema, or
/// serialization logic — it routes Values and calls admit().
///
/// Dispatch is single-threaded and FIFO: send/publish enqueue; pump() drains.
/// A handler that sends during handling enqueues a *later* delivery — delivery is
/// never reentrant, and ordering is deterministic.
class Switchboard : public Bus {
public:
    Switchboard();
    ~Switchboard() override;

    Switchboard(const Switchboard&) = delete;
    Switchboard& operator=(const Switchboard&) = delete;

    /// Remove a Shard and hand its ownership back to the caller (or nullptr if
    /// the id is unknown). Used by hosts that must destroy a Shard — and any
    /// resources it holds, such as a loaded library instance — in a controlled
    /// order. Pending deliveries to a removed Shard are refused (NoSuchTarget) at
    /// delivery. Its registered schemas remain published.
    std::unique_ptr<Shard> unregister_shard(ShardId id);

    /// Register a Shard (the bus takes ownership) and return its stable id. Each
    /// accepted schema and the state schema are registered in the bus registry,
    /// enforcing that all Shards agree on what a given (name, version) means
    /// (a disagreement throws zen::SchemaConflict). The Shard's initial snapshot
    /// must conform to its own schema (it seeds last-known-good); otherwise
    /// std::invalid_argument is thrown.
    ShardId register_shard(std::unique_ptr<Shard> shard);

    /// Enqueue a directed delivery to `target`. Returns a Ticket whose outcome is
    /// readable after the delivery is pumped.
    Ticket send(ShardId target, Message msg) override;

    /// Enqueue a delivery to every alive Shard whose accept-set includes the
    /// payload's (name, version), in registration order. Returns the recipient
    /// count (0 is legal, not an error). Each delivery is independently gated.
    std::size_t publish(Message msg) override;

    /// Deliver until the queue drains. Single-threaded, FIFO, non-reentrant: a
    /// reentrant call (from within a handler) is a no-op.
    void pump();
    void run() { pump(); }
    void stop() noexcept { stop_requested_ = true; }

    /// The fate of a previously-issued Ticket (Pending until pumped).
    DeliveryOutcome outcome(Ticket t) const;

    /// Register an observer/tap; it is notified of every delivery and lifecycle
    /// event. Returns an id for removal.
    ObserverId add_observer(Observer obs);
    void remove_observer(ObserverId id);

    // ---- Lifecycle (mechanics reused from zen-core) -----------------------

    /// Serialize the Shard's current snapshot to native bytes.
    std::string snapshot_bytes(ShardId id) const;

    /// Mark a Shard dead; it stops receiving deliveries until revived. Emits Died.
    void kill(ShardId id);

    /// Revive a Shard from candidate bytes: parse -> admit(Unverified, state
    /// schema). On success, revive() and refresh last-known-good. On refusal, the
    /// Shard's policy() (validated against the fixed grammar) decides whether to
    /// fall back to last-known-good. Emits Revived (or Refused).
    ReviveOutcome reload(ShardId id, std::string_view candidate_bytes);

    // ---- Queries ----------------------------------------------------------

    std::vector<ShardId> list_shards() const;
    std::vector<std::shared_ptr<const Schema>> accepted_schemas(ShardId id) const;

    /// Resolve a registered schema by identity, across every Shard's accept-set
    /// and state schema. nullptr if the system knows no such schema. Used by a
    /// host that must gate a value whose schema the system knows but the caller
    /// does not hold (e.g. a message emitted across the library boundary).
    std::shared_ptr<const Schema> resolve_schema(std::string_view name,
                                                 std::uint32_t version) const;
    Shard* shard(ShardId id);
    const Shard* shard(ShardId id) const;
    bool alive(ShardId id) const;
    std::size_t pending() const noexcept { return queue_.size(); }

private:
    struct ShardRecord {
        ShardId id{};
        std::unique_ptr<Shard> shard;
        std::vector<std::shared_ptr<const Schema>> accept;
        std::shared_ptr<const Schema> state_schema;
        Value last_known_good;
        std::uint64_t reloads_used = 0;
        bool alive = true;
    };

    struct Envelope {
        Message msg;
        ShardId target{};
        std::uint64_t seq = 0;
    };

    void deliver_one(Envelope env);
    void emit(const BusEvent& event);
    void record(std::uint64_t seq, Disposition disposition, const Refusal& refusal);

    ShardRecord* find(ShardId id);
    const ShardRecord* find(ShardId id) const;
    static const std::shared_ptr<const Schema>* accept_match(const ShardRecord& rec,
                                                             std::string_view name,
                                                             std::uint32_t version);

    Registry registry_;
    std::map<std::uint64_t, ShardRecord> shards_;
    std::uint64_t next_shard_id_ = 1;

    std::deque<Envelope> queue_;
    std::vector<DeliveryOutcome> journal_; ///< indexed by delivery seq (slot 0 unused)
    std::uint64_t next_seq_ = 1;

    std::vector<std::pair<ObserverId, Observer>> observers_;
    ObserverId next_observer_id_ = 1;

    bool in_dispatch_ = false;
    bool stop_requested_ = false;
};

} // namespace zen::sb

#endif // ZEN_SWITCHBOARD_SWITCHBOARD_HPP
