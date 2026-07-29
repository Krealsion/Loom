#ifndef ZEN_SWITCHBOARD_SWITCHBOARD_HPP
#define ZEN_SWITCHBOARD_SWITCHBOARD_HPP

#include <zen/admission.hpp>
#include <zen/registry.hpp>
#include <zen/schema.hpp>
#include <zen/switchboard/grant.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/switchboard/weave_contract.hpp>
#include <zen/value.hpp>

#include <cstddef>
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
    /// NOT MOVABLE, and now said out loud rather than left as a side effect of
    /// the deleted copy. A Switchboard IS an authority domain: weaves hold
    /// references into it, its identity anchors every authority it ever issued,
    /// and "the same Loom, at a different address" is not a state this design
    /// has a meaning for. Deleting the move makes the meaningless case
    /// unrepresentable instead of accidentally supported.
    Switchboard(Switchboard&&) = delete;
    Switchboard& operator=(Switchboard&&) = delete;

    /// Remove a Weave and hand its ownership back to the caller (or nullptr if
    /// the id is unknown). Used by hosts that must destroy a Weave — and any
    /// resources it holds, such as a loaded library instance — in a controlled
    /// order. Pending deliveries to a removed Weave are refused (NoSuchTarget) at
    /// delivery. Its registered schemas remain published, and any role it held is
    /// released — the slot is free for a successor.
    ///
    /// Symmetrically, and less obviously: pending deliveries *from* a removed
    /// Weave are refused too, as CapabilityDenied. A gated message is authorized
    /// by looking its sender's grant up at DELIVERY time, so a sender that no
    /// longer exists cannot be authorized and the message fails closed. An
    /// unregistered Weave's in-flight replies therefore die with it. That is the
    /// safe direction to fail, but it is a real effect a caller unloading a live
    /// participant should expect (see the manager suite, which pins it).
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

    /// The fate of a previously-issued Ticket (Pending until pumped). The journal retains
    /// only the most recent `kJournalCapacity` outcomes (see below), so a Ticket older than
    /// that window — or one never issued — reads as Pending. This is loss-free *provided a
    /// single pump() does not deliver more than `kJournalCapacity` envelopes between a
    /// Ticket's submit and its read*: the window can roll *within one pump* if a handler
    /// cascades past the capacity, so a consumer that batches many sends before one pump, or
    /// reads an outcome after such a cascade, can see Pending for a Ticket that was in fact
    /// Delivered/Refused. Every current consumer stays inside that breath — the relay tracks
    /// its own pending (kMaxRelayPending), the console reads one outcome per pump — so the
    /// window is not a live hazard today; a future high-fan-out consumer must size for it.
    DeliveryOutcome outcome(Ticket t) const;

    /// The delivery journal is a bounded ring: it keeps the outcomes of the last
    /// `kJournalCapacity` deliveries, not one per message ever sent. A bus is exactly
    /// the component that runs for weeks, so its footprint must be bounded by design,
    /// never by lifetime throughput (audit F-6). The window is far larger than any real
    /// per-pump delivery count in the current tree (no consumer batches or cascades this
    /// many before reading — see outcome() for the sufficiency condition), so eviction
    /// never touches a live read today; published and pinned, like kMaxRelayPending.
    static constexpr std::size_t kJournalCapacity = 1024;

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

    /// THE REPLY AUTHORITY FOR THE DELIVERY BEING DISPATCHED — bus-owned, one at
    /// a time, and gone the moment that delivery returns.
    ///
    /// This is the narrowest V1 the law allows, and the narrowness is the design
    /// rather than a corner cut: *a reply authority belongs to one delivered
    /// request between two exact incarnations and authorizes at most one matching
    /// response.* Because it lives and dies with the handler, every question a
    /// longer-lived token would owe an answer to is answered by construction —
    /// it cannot be stored, named, passed, replayed, reused for another request,
    /// or survive either participant's death, reload, or the role changing hands,
    /// because there is nothing to survive. A weave that wants to answer later
    /// answers ordinarily, and its answer is ordinary — which is the truth.
    ///
    /// KEPT HERE RATHER THAN IN THE HANDLER'S STACK FRAME, deliberately. Dispatch
    /// is single-threaded and non-reentrant, so "the current delivery" is a real
    /// singular thing the bus can name. Holding it here means the authority is
    /// checked against WHO IS BEING DISPATCHED, not merely against who is asking
    /// — so a WeaveBus that outlived its delivery (already undefined behaviour,
    /// and outside this phase's threat altitude) finds the authority of a
    /// different delivery and is refused, instead of quietly borrowing it.
    struct ReplyAuthority {
        WeaveId requester{};           ///< the request's stamped sender: the only legal recipient
        std::uint64_t correlation = 0; ///< the request's own: the answer may not choose it
        bool spent = false;            ///< one delivery, one answer
    };

    // The Bus a handler actually receives: it stamps the handling Weave's identity
    // onto every send and routes through the *gated* path. A Weave holds only this
    // — never the concrete Switchboard — so it cannot send except as itself and
    // subject to its grant. (Switchboard::send/publish, held only by the host, are
    // the ungated root authority.) This split is the trust boundary, and it is why
    // the reply authority is reached through here too: the door that can attest is
    // the same door that cannot lie about who is speaking.
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
        Ticket answer(Message msg) override { return sb_.answer_as(self_, std::move(msg)); }
        Ticket announce_lifecycle(const LifecycleAuthority& authority, WeaveId target, Message msg,
                                  std::int64_t sequence) override {
            return sb_.announce_as(self_, authority, target, std::move(msg), sequence);
        }

    private:
        Switchboard& sb_;
        WeaveId self_;
    };

    Ticket enqueue_directed(WeaveId target, Message msg, bool gated,
                            Provenance provenance = Provenance{});
    Ticket enqueue_role(std::string role, Message msg, bool gated);
    std::size_t fanout(Message msg, bool gated);

    /// The one write path for an attested answer. Refuses — visibly, on the tap
    /// and in the journal — when the caller is not the weave currently being
    /// dispatched, when the request had no one to answer, or when the delivery's
    /// one answer is already spent.
    Ticket answer_as(WeaveId as_sender, Message msg);

    /// The one write path for a lifecycle attestation.
    Ticket announce_as(WeaveId as_sender, const LifecycleAuthority& authority, WeaveId target,
                       Message msg, std::int64_t sequence);

    /// Record and publish a refusal that never became a delivery, so a failure of
    /// authority is visible at the same altitude as a failure of capability.
    Ticket refuse_now(WeaveId target, WeaveId sender, const Message& msg, RefusalReason reason);

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

    /// One retained delivery outcome, tagged with the seq that owns it. The tag makes
    /// ring reuse unambiguous: record()/outcome() act on journal_[seq % kJournalCapacity]
    /// only while its `seq` still matches — a later wrap evicts the old owner, and a
    /// read of an evicted (or never-issued) seq is Pending, exactly as an unknown seq.
    struct JournalSlot {
        std::uint64_t seq = 0; ///< 0 = never written (real seqs start at 1)
        DeliveryOutcome outcome;
    };
    std::vector<JournalSlot> journal_; ///< ring of the last kJournalCapacity outcomes, by seq % cap
    std::uint64_t next_seq_ = 1;

    std::vector<std::pair<ObserverId, Observer>> observers_;
    ObserverId next_observer_id_ = 1;

    /// Mint the capability that lets trusted infrastructure attach Loom's
    /// lifecycle attestation.
    ///
    /// PRIVATE AND NON-STATIC, and both halves are load-bearing. Non-static
    /// means minting requires the Switchboard ITSELF — the host's own object —
    /// and a weave never holds one; it is handed a `Bus&`, which has no such
    /// member. That is the boundary this codebase already draws between `send`
    /// (root) and `send_as`, and lifecycle minting belongs on the same side of
    /// it. Private means even code holding a Switchboard must come through the
    /// one named host-wiring function below rather than helping itself.
    ///
    /// R2B-1 shipped this as a PUBLIC STATIC, which was no boundary at all: any
    /// weave could write `Switchboard::lifecycle_authority()` from anywhere,
    /// with no instance and no host involvement, and — given an exact grant for
    /// `zen.Activated` — manufacture a lifecycle fact for another incarnation.
    /// A private constructor behind a reachable factory protects nothing.
    /// R2B-1b: the authority carries WHICH board issued it. Minting from any
    /// board is still legal — anyone may own a Switchboard — but the result is
    /// only spendable through the board it names.
    LifecycleAuthority lifecycle_authority() noexcept { return LifecycleAuthority{identity_}; }

    /// Was this authority issued by THIS Loom, and is that Loom still alive?
    ///
    /// The check lives inside trusted Switchboard machinery, never in consumer
    /// code — a consumer holding an authority has no way to ask the question and
    /// no business answering it. `lock()` failing is a destroyed issuer, which is
    /// the lifetime rule: an authority lasts exactly as long as the Loom that
    /// issued it.
    bool issued_here(const LifecycleAuthority& authority) const noexcept {
        const std::shared_ptr<const LoomIdentity> issuer = authority.issuer_.lock();
        return issuer != nullptr && issuer == identity_;
    }

    /// The ONE expression in the system that yields a LifecycleAuthority. It is
    /// defined in `zen/host/lifecycle_wiring.hpp` — a host-wiring header that no
    /// weave-authoring header includes — so the name is not even visible to
    /// ordinary weave source, and the object it needs is one a weave never has.
    friend LifecycleAuthority host_lifecycle_authority(Switchboard& bus);

    /// This Loom's own identity — created with the board, destroyed with it, and
    /// shared with nothing except the authorities it issues (weakly). Two boards
    /// alive at once hold two distinct identities; a board that dies takes its
    /// identity's control block with it, so an authority from a dead world can
    /// never be revived by a later board landing on the same address.
    std::shared_ptr<const LoomIdentity> identity_;

    bool in_dispatch_ = false;
    bool stop_requested_ = false;
    /// The delivery currently being dispatched, and its one reply authority.
    /// Meaningful only inside deliver_one's call to handle(); `current_target_`
    /// invalid means no delivery is live, so nobody may answer.
    WeaveId current_target_{};
    ReplyAuthority authority_{};
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_SWITCHBOARD_HPP
