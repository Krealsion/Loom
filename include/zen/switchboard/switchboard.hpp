#ifndef ZEN_SWITCHBOARD_SWITCHBOARD_HPP
#define ZEN_SWITCHBOARD_SWITCHBOARD_HPP

#include <zen/admission.hpp>
#include <zen/registry.hpp>
#include <zen/schema.hpp>
#include <zen/switchboard/grant.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/switchboard/weave.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

/// Why a delivery (or revival) was refused. Conformance refusals carry a
/// loom Error (the gate's verdict); the rest are bus-level routing reasons
/// the gate never sees.
enum class RefusalReason : std::uint8_t {
    None = 0,
    NoSuchTarget,      ///< directed at a WeaveId that is not registered
    TargetUnavailable, ///< the target is currently dead (awaiting revival)
    NotAccepted,       ///< the target's accept-set has no schema of this (name, version)
    GateRefused,       ///< routing passed, but admit() refused — see `error`
    CapabilityDenied,  ///< the sender's grant does not permit (shape -> target); the gate is
                       ///< never reached. Authorization, not conformance.
};

const char* name_of(RefusalReason r) noexcept;

/// A structured refusal. When `reason == GateRefused`, `error` is the loom
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
    WeaveId target{};
    WeaveId sender{};
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
/// revive_from_last_good: Bool }. A Weave's policy() is validated against this and
/// only these two fields are read.
std::shared_ptr<const Schema> lifecycle_policy_schema();

/// How a Weave's accept-set is interpreted at delivery. `Listed` (the default): only the
/// explicit `(name, version)` schemas it declares. `AnyRegistered`: a deliberate
/// capability — the Weave accepts **any registered shape**, gated at delivery against
/// the shape's own registry-resolved schema (an unregistered shape is still refused).
/// The console uses `AnyRegistered` to receive replies; ordinary Weaves do not.
enum class AcceptMode { Listed, AnyRegistered };

/// The first live boundary: an in-process message bus that gates every delivery
/// through loom's one validator. It reimplements no validation, schema, or
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

    /// Remove a Weave and hand its ownership back to the caller (or nullptr if
    /// the id is unknown). Used by hosts that must destroy a Weave — and any
    /// resources it holds, such as a loaded library instance — in a controlled
    /// order. Pending deliveries to a removed Weave are refused (NoSuchTarget) at
    /// delivery. Its registered schemas remain published.
    std::unique_ptr<Weave> unregister_weave(WeaveId id);

    /// Register a Weave (the bus takes ownership) and return its stable id. Each
    /// accepted schema and the state schema are registered in the bus registry,
    /// enforcing that all Weaves agree on what a given (name, version) means
    /// (a disagreement throws loom::SchemaConflict). The Weave's initial snapshot
    /// must conform to its own schema (it seeds last-known-good); otherwise
    /// std::invalid_argument is thrown.
    ///
    /// The Weave's authority is its `grant`: every message it originates is
    /// authorized against it at delivery. The default is the **empty** grant —
    /// minimal authority — so a Weave that needs to send must be granted that
    /// reach by the host at registration (see the grant-defaulting in
    /// loom::mount and the kernel's loaded-Weave grant).
    WeaveId register_weave(std::unique_ptr<Weave> weave, Grant grant);
    WeaveId register_weave(std::unique_ptr<Weave> weave); ///< empty grant

    /// As register_weave(weave, grant), but also bind the Weave to `role` — a named
    /// capability slot a send may target instead of a WeaveId (see send_to_role and
    /// Grant::allow_to_role). v1 is singleton: a role has exactly one holder, so
    /// binding a role already held throws std::invalid_argument. The binding
    /// survives the holder reloading (its id is stable) and is cleared on
    /// unregister_weave.
    WeaveId register_weave(std::unique_ptr<Weave> weave, Grant grant, std::string role);

    /// Register with an accept-mode: `AnyRegistered` makes the Weave accept any
    /// registered shape (gated against the registry-resolved schema at delivery) — a
    /// deliberate capability (the console's reply path), distinct from its grant.
    WeaveId register_weave(std::unique_ptr<Weave> weave, Grant grant, AcceptMode accept_mode);

    /// Enqueue a directed delivery to `target`. Returns a Ticket whose outcome is
    /// readable after the delivery is pumped.
    Ticket send(WeaveId target, Message msg) override;

    /// Enqueue a delivery to every alive Weave whose accept-set includes the
    /// payload's (name, version), in registration order. Returns the recipient
    /// count (0 is legal, not an error). Each delivery is independently gated.
    std::size_t publish(Message msg) override;

    /// Inject a message AS a specific Weave: stamp `as_sender` as the
    /// authoritative sender and authorize it against that Weave's grant at
    /// delivery — exactly as if the Weave had sent it through its own WeaveBus.
    /// Held only by the host (root authority), this is how a trusted bridge
    /// re-enters a Weave's output with its identity stamped from the *connection*
    /// it arrived on, never from the payload — the cross-process form of the
    /// in-process WeaveBus identity-binding. A child claiming a different sender
    /// cannot get it.
    Ticket send_as(WeaveId as_sender, WeaveId target, Message msg);
    std::size_t publish_as(WeaveId as_sender, Message msg);

    /// Role-addressed sends. send_to_role is the ungated root authority (held only
    /// by the host); send_as_to_role is the gated path a WeaveBus uses — it stamps
    /// the authoritative sender and authorizes against that sender's grant *by role*
    /// at delivery (see Grant::permits_role).
    Ticket send_to_role(std::string_view role, Message msg) override;
    Ticket send_as_to_role(WeaveId as_sender, std::string_view role, Message msg);

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

    // ---- Lifecycle (mechanics reused from loom) -----------------------

    /// Serialize the Weave's current snapshot to native bytes.
    std::string snapshot_bytes(WeaveId id) const;

    /// Mark a Weave dead; it stops receiving deliveries until revived. Emits Died.
    void kill(WeaveId id);

    /// Revive a Weave from candidate bytes: parse -> admit(Unverified, state
    /// schema). On success, revive() and refresh last-known-good. On refusal, the
    /// Weave's policy() (validated against the fixed grammar) decides whether to
    /// fall back to last-known-good. Emits Revived (or Refused).
    ///
    /// This is the **crash-revival** path: it is budgeted (checks/decrements the
    /// policy's max_reloads) so a crash-thrashing Weave cannot revive forever. A
    /// future supervisor calls this on crash.
    ReviveOutcome reload(WeaveId id, std::string_view candidate_bytes);

    /// Intentional state swap (hot-reload of code, not crash recovery): gate the
    /// candidate against the state schema, then revive(), refresh last-known-good,
    /// and mark alive. Unlike reload(), it spends **no budget** (no max_reloads
    /// check or decrement) — a deliberate swap to fixed code must never be blocked
    /// by a Weave's crash-revival allowance. A gate refusal is a **clean refusal**:
    /// there is no last-known-good fallback (the caller is swapping in known code
    /// and a malformed candidate should fail visibly, not silently roll back).
    /// Emits Revived on success, Refused on a gate refusal.
    ReviveOutcome swap_state(WeaveId id, std::string_view candidate_bytes);

    // ---- Queries ----------------------------------------------------------

    std::vector<WeaveId> list_weaves() const;
    std::vector<std::shared_ptr<const Schema>> accepted_schemas(WeaveId id) const;

    /// Resolve a registered schema by identity, across every Weave's accept-set
    /// and state schema. nullptr if the system knows no such schema. Used by a
    /// host that must gate a value whose schema the system knows but the caller
    /// does not hold (e.g. a message emitted across the library boundary).
    std::shared_ptr<const Schema> resolve_schema(std::string_view name,
                                                 std::uint32_t version) const;
    Weave* weave(WeaveId id);
    const Weave* weave(WeaveId id) const;
    bool alive(WeaveId id) const;
    std::size_t pending() const noexcept { return queue_.size(); }

private:
    struct WeaveRecord {
        WeaveId id{};
        std::unique_ptr<Weave> weave;
        std::vector<std::shared_ptr<const Schema>> accept;
        std::shared_ptr<const Schema> state_schema;
        Value last_known_good;
        Grant grant;
        std::uint64_t reloads_used = 0;
        bool alive = true;
        std::string role{}; ///< the role this Weave holds (empty if none); see roles_
        bool accepts_any = false; ///< AcceptMode::AnyRegistered — accept any registered shape (gated)
    };

    struct Envelope {
        Message msg;
        WeaveId target{};
        std::uint64_t seq = 0;
        bool gated = false;  ///< true => Weave-originated; authorize against the sender's grant
        std::string role{};  ///< non-empty => role-targeted; resolved to a holder at delivery
    };

    // The Bus a handler actually receives: it stamps the handling Weave's identity
    // onto every send and routes through the *gated* path. A Weave holds only this
    // — never the concrete Switchboard — so it cannot send except as itself and
    // subject to its grant. (Switchboard::send/publish, held only by the host, are
    // the ungated root authority.) This split is the trust boundary.
    class WeaveBus : public Bus {
    public:
        WeaveBus(Switchboard& sb, WeaveId self) noexcept : sb_(sb), self_(self) {}
        Ticket send(WeaveId target, Message msg) override {
            return sb_.send_as(self_, target, std::move(msg));
        }
        std::size_t publish(Message msg) override { return sb_.publish_as(self_, std::move(msg)); }
        Ticket send_to_role(std::string_view role, Message msg) override {
            return sb_.send_as_to_role(self_, role, std::move(msg));
        }

    private:
        Switchboard& sb_;
        WeaveId self_;
    };

    Ticket enqueue_directed(WeaveId target, Message msg, bool gated);
    Ticket enqueue_role(std::string role, Message msg, bool gated);
    std::size_t fanout(Message msg, bool gated);

    void deliver_one(Envelope env);
    void emit(const BusEvent& event);
    void record(std::uint64_t seq, Disposition disposition, const Refusal& refusal);

    WeaveRecord* find(WeaveId id);
    const WeaveRecord* find(WeaveId id) const;
    static const std::shared_ptr<const Schema>* accept_match(const WeaveRecord& rec,
                                                             std::string_view name,
                                                             std::uint32_t version);

    Registry registry_;
    std::map<std::uint64_t, WeaveRecord> weaves_;
    std::map<std::string, WeaveId> roles_; ///< role name -> its singleton holder's id
    std::uint64_t next_weave_id_ = 1;

    std::deque<Envelope> queue_;
    std::vector<DeliveryOutcome> journal_; ///< indexed by delivery seq (slot 0 unused)
    std::uint64_t next_seq_ = 1;

    std::vector<std::pair<ObserverId, Observer>> observers_;
    ObserverId next_observer_id_ = 1;

    bool in_dispatch_ = false;
    bool stop_requested_ = false;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_SWITCHBOARD_HPP
