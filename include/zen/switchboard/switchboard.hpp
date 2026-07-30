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
    /// A lifecycle/answer authority was presented that this Loom did not issue —
    /// or that is expired, already spent, or bound to a different conversation or
    /// incarnation. DISTINCT FROM CapabilityDenied on purpose (R2B-2): the
    /// sender's grant may be perfectly correct while the AUTHORITY DOMAIN is
    /// wrong, and reporting that as "you lack the grant" sends an operator
    /// looking in exactly the wrong place. The grant answers "may you send this
    /// shape?"; this answers "is this yours to say here?".
    ForeignAuthority,
    /// A published bound was reached, and nothing about authority or conformance
    /// was wrong. Kept distinct from ForeignAuthority for the same reason that one
    /// is distinct from CapabilityDenied: reporting a capacity limit as a foreign
    /// capability would send an operator hunting a forgery that never happened.
    Exhausted,
    /// The message was authored by a life that has since ended (R2B-2b): the
    /// sender is dead, has been permanently removed, or has been revived — which
    /// makes the speaker a different life behind the same `WeaveId`.
    ///
    /// Its own reason for the same reason the two above have theirs. "No such
    /// target" would be a lie (the target is fine), a gate error would blame the
    /// payload, and `CapabilityDenied` would send an operator to edit a grant that
    /// was never the problem. What ended was the utterance's author.
    SenderLifeEnded,
    /// An authenticated answer arrived for a requester that is no longer the one
    /// that asked (R2B-2c): the weave at that `WeaveId` has since died and been
    /// revived, or its code has been replaced. The address is right and the
    /// occupant is wrong.
    ///
    /// Distinct from `SenderLifeEnded`, which is about the AUTHOR of a message,
    /// and from `NoSuchTarget`/`TargetUnavailable`, which are about an address
    /// that resolves to nobody or to somebody dead. Here the target exists, is
    /// alive, and is simply not who the conversation was with.
    AnswerTargetChanged,
    /// A SEALED weave tried to speak into the world (R2B-3). A prepared candidate
    /// exists outside the live world: it may converse with the coordinator that is
    /// preparing it, and with nobody else. This is what it hears when it tries.
    ///
    /// Deliberately visible, unlike the mirror case: a message aimed AT a sealed
    /// weave by anyone other than its coordinator is refused as `NoSuchTarget`, so
    /// the world cannot learn that a candidate exists. The candidate's own attempts
    /// are the operator's business and are named.
    SealedSpeech,
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
    /// DIAGNOSTICS ONLY, and only for a weave-originated delivery (R2B-2b): the
    /// sender life this envelope was stamped with at enqueue, and the sender's life
    /// right now. When they differ, the message was authored by a life that has
    /// since ended — and a journal reader can see exactly that rather than having
    /// to infer it. Both are 0 for host/root sends, which belong to no weave life.
    std::uint64_t sender_life = 0;
    std::uint64_t sender_life_now = 0;
    /// DIAGNOSTICS ONLY, and only for an authenticated answer (R2B-2c): what the
    /// conversation expected of its requester, and what that requester is now. A
    /// journal reader can therefore see the causal event — "expected life 1 /
    /// incarnation 1, found life 2 / incarnation 1" — instead of inferring it.
    /// Zero on every other kind of delivery.
    std::uint64_t expected_requester_life = 0;
    std::uint64_t expected_requester_incarnation = 0;
    std::uint64_t requester_life_now = 0;
    std::uint64_t requester_incarnation_now = 0;
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

/// WHO OWNS A SEALED CANDIDATE (R2B-3b).
///
/// R2B-3a bound a seal to a coordinator's `WeaveId` alone, which was enough to
/// prove isolation and is NOT enough to own a transaction: a coordinator that
/// dies and revives, or whose code is replaced, is a different participant at
/// the same address — and R2B-2b/2c already established that such a successor
/// inherits neither speech nor conversations. A preparation is a conversation.
/// So ownership carries the same three facts every other authority in this
/// codebase carries, and the issuing Loom is implicit in living on its record.
struct CandidateOwner {
    WeaveId who{};
    std::uint64_t life = 0;
    std::uint64_t incarnation = 0;

    bool valid() const noexcept { return who.valid(); }
    friend bool operator==(const CandidateOwner& a, const CandidateOwner& b) noexcept {
        return a.who == b.who && a.life == b.life && a.incarnation == b.incarnation;
    }
};

/// Why an admission was refused, for the host that asked (R2B-3b-1a).
///
/// Admission is a host call, not a delivery, so there is no message to refuse and
/// no tap event to carry a `RefusalReason`. The reason belongs in the answer the
/// caller already receives — and it is named rather than a bare false, because
/// "the coordinator that sealed this is not the one standing here now" and "the
/// role moved under you" send an operator to entirely different places.
enum class AdmitRefusal : std::uint8_t {
    None = 0,         ///< admitted
    ForeignAuthority, ///< the lifecycle authority was not issued by this Loom
    NotACandidate,    ///< missing, dead, or not sealed at all
    OwnerChanged,     ///< the exact coordinator life/incarnation that sealed it is gone
    IncumbentUnfit,   ///< missing, dead, or already sealed
    RoleNotHeld,      ///< the role is empty, or held by somebody other than the incumbent
};

const char* name_of(AdmitRefusal r) noexcept;

/// The result of an admission attempt. Convertible to bool so the common
/// `REQUIRE(bus.admit_candidate(...))` reads as it should, with `why` for the
/// cases that care.
struct AdmitResult {
    bool ok = false;
    AdmitRefusal why = AdmitRefusal::None;

    explicit operator bool() const noexcept { return ok; }
};

/// WHO A TRANSACTION IS BOUND TO (R2B-3b-2).
///
/// The same three facts every authority in this codebase carries, for the same
/// reason: a participant that dies and revives, or whose code is replaced, is a
/// different participant at the same address, and a prepared replacement belongs
/// to the exact lives that began it. `CandidateOwner` is this shape for the seal;
/// this is it for the other three roles a transaction names.
struct ParticipantRef {
    WeaveId who{};
    std::uint64_t life = 0;
    std::uint64_t incarnation = 0;

    bool valid() const noexcept { return who.valid(); }
    friend bool operator==(const ParticipantRef& a, const ParticipantRef& b) noexcept {
        return a.who == b.who && a.life == b.life && a.incarnation == b.incarnation;
    }
    friend bool operator!=(const ParticipantRef& a, const ParticipantRef& b) noexcept {
        return !(a == b);
    }
};

/// An opaque, host-issued transaction handle. Never a correlation a weave chose,
/// never a participant id, and never authority on its own: every command proves
/// the caller is the exact bound participant as well as naming the transaction.
struct TxnId {
    std::uint64_t value = 0;
    bool valid() const noexcept { return value != 0; }
    friend bool operator==(TxnId a, TxnId b) noexcept { return a.value == b.value; }
};

/// The whole state machine. Four states, and the two terminals are terminal.
enum class TxnState : std::uint8_t { Preparing, Ready, Committed, Aborted };

/// Why a transaction ended, or why a command was refused. One vocabulary for
/// both, because "you may not do that now" and "this is how it ended" are the
/// same question asked at different moments.
enum class TxnReason : std::uint8_t {
    None = 0,
    ExplicitAbort,
    PreparationExhausted,
    OperatorChanged,
    CoordinatorChanged,
    IncumbentChanged,
    CandidateChanged,
    RoleChanged,
    CapacityExhausted,
    CommitPreconditionFailed,
    AdmissionRefused,
    NoSuchTransaction,
    WrongState,
    NotTheOwner,
    PreconditionFailed,
};

const char* name_of(TxnState s) noexcept;
const char* name_of(TxnReason r) noexcept;

/// The result of beginning a transaction, or of any command on one.
struct TxnResult {
    bool ok = false;
    TxnId id{};
    TxnReason why = TxnReason::None;

    explicit operator bool() const noexcept { return ok; }
};

/// How a transaction ended, kept only until its EXACT operator takes it.
struct TxnOutcome {
    TxnId id{};
    TxnState state = TxnState::Aborted;
    TxnReason reason = TxnReason::None;
};

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

    /// How many unfinished conversations one Loom will hold at once (R2B-2).
    ///
    /// BOUNDED AND PUBLISHED, like the journal above and for the same reason: a
    /// deferred answer is host-side state a WEAVE asks for, so an unbounded one
    /// would be a memory hole any weave could dig by deferring and never
    /// answering. Exceeding it refuses visibly and leaves the caller's immediate
    /// answer opportunity intact — a refused deferral is not a lost conversation.
    /// Records are reclaimed on spending, on release, and when either participant
    /// dies or is reloaded, so a LEAKED capability costs one slot only until its
    /// owner does.
    static constexpr std::size_t kMaxDeferredAnswers = 64;

    /// Register an observer/tap; it is notified of every delivery and lifecycle
    /// event. Returns an id for removal.
    ObserverId add_observer(Observer obs);
    void remove_observer(ObserverId id);

    // ---- Lifecycle (mechanics reused from loom) -----------------------

    /// Serialize the Weave's current snapshot to native bytes.
    std::string snapshot_bytes(WeaveId id) const;

    /// SEAL a weave: it becomes a prepared candidate outside the live world,
    /// able to converse only with `coordinator` (R2B-3).
    ///
    /// Host/root authority, like `kill` and `unregister_weave`. Refuses if the
    /// weave holds a role — a candidate with a public role is a contradiction, and
    /// making that impossible here means commit is the only way a role can move.
    /// Returns false if there is no such weave, or it already holds a role.
    /// Refuses if the candidate or coordinator is missing, if the coordinator is
    /// DEAD (a life that cannot converse cannot own a preparation), if the
    /// candidate holds a role, or if the candidate is ALREADY SEALED — resealing
    /// would silently transfer a prepared candidate to a second owner, and this
    /// errand deliberately adds no transfer semantics.
    bool seal_weave(WeaveId candidate, WeaveId coordinator);

    /// Who owns this sealed weave, as the exact life and incarnation that sealed
    /// it. An invalid owner means the weave is not sealed.
    CandidateOwner candidate_owner(WeaveId id) const;

    /// Is this weave currently a sealed candidate? (Diagnostics and pins; the
    /// routing decisions are made inside the bus, never by asking.)
    bool sealed(WeaveId id) const;

    /// Who holds `role` right now — the invalid id if nobody does. A read-only
    /// query for hosts and taps; routing resolves the role itself at delivery and
    /// never consults this.
    WeaveId role_holder(std::string_view role) const;

    /// THE COMMIT. One operation, one visible change (R2B-3).
    ///
    /// Unseals `candidate` and moves `role` from `incumbent` to it. Everything an
    /// ordinary observer could notice — the candidate becoming reachable, the role
    /// changing hands, the incumbent ceasing to hold it — happens here, between two
    /// deliveries, so no pump can run in the middle of it. That is the atomic
    /// visibility boundary this phase needed, and it is a property of the
    /// single-threaded queue rather than a lock: `pump()` dispatches one envelope
    /// at a time and is non-reentrant, so a commit performed outside `deliver_one`
    /// (or wholly within one handler) cannot be observed half-done.
    ///
    /// Refuses — changing NOTHING — if the candidate is not sealed, if either
    /// weave is missing or dead, or if the role is held by anyone other than the
    /// incumbent. A refused commit is observationally identical to no commit.
    bool commit_candidate(WeaveId candidate, WeaveId incumbent, const std::string& role);

    /// THE ADMISSION (R2B-3b): the commit above, extended to account for the
    /// incumbent and for activation ordering. One operation, one visible change.
    ///
    ///   before   incumbent public          candidate sealed
    ///   after    incumbent sealed for      candidate admitted, its activation
    ///            retirement (coordinator-  already ahead of any production
    ///            private only)             that could reach it
    ///
    /// `activation` is enqueued as an attested lifecycle fact — and NOT at the
    /// tail. Role resolution happens at delivery, so a role-addressed message
    /// queued before the commit would otherwise be delivered to the candidate
    /// BEFORE its activation. It is instead placed immediately ahead of the first
    /// queued envelope that could reach this candidate (one addressed to the
    /// committed role, or to the candidate directly), which is the narrowest
    /// placement that makes activation the candidate's first live delivery while
    /// leaving every other message's order untouched. Nothing is dropped.
    ///
    /// Refuses — changing NOTHING — on any failed precondition.
    /// THE OWNER MUST STILL BE THE OWNER (R2B-3b-1a). The seal records an exact
    /// coordinator life and incarnation; admission verifies that the participant
    /// standing at that address today IS that one. A trusted host caller holding a
    /// perfectly good lifecycle authority still cannot admit a candidate whose
    /// coordinator died and revived, was reloaded into new code, or was removed —
    /// because the preparation belonged to a life, and that life is over.
    ///
    /// The activation's own sender-life stamp is taken from the VERIFIED owner
    /// record rather than from a fresh lookup of the coordinator id, so it can
    /// never describe a successor.
    AdmitResult admit_candidate(WeaveId candidate, WeaveId incumbent, const std::string& role,
                                const LifecycleAuthority& authority, Message activation,
                                std::int64_t sequence);

    // ---- Prepared replacement (R2B-3b-2) -----------------------------------
    //
    // THE TRANSACTION LIVES HERE, and the reason is question 4 of the phase's own
    // investigation: this is the only place that sees every transition that can
    // invalidate a participant. `kill` announces Died and `swap_state` announces
    // Revived — but `unregister_weave` announces NOTHING, so a registry living
    // anywhere else and watching events would silently miss permanent removal.
    // Inline notification from the transitions themselves needs no observer
    // framework and exposes no transaction policy to any weave.
    //
    //     A replacement transaction belongs to exact lives, advances through one
    //     finite state machine, and either commits once or disappears without
    //     disturbing the incumbent.

    /// How many prepared replacements may be in flight at once. Small on purpose:
    /// this is an operator-driven lifecycle act, not a workload.
    static constexpr std::size_t kMaxPreparedReplacements = 8;
    /// How many ended transactions are remembered for their operator to collect.
    /// Bounded separately, because a terminal result is evidence rather than work,
    /// and the oldest is dropped rather than growing without limit.
    static constexpr std::size_t kMaxTerminalOutcomes = 16;
    /// The largest preparation budget a caller may ask for.
    static constexpr std::uint32_t kMaxPreparationBudget = 1024;

    /// Begin one prepared replacement. Validates EVERYTHING before storing
    /// anything, so a refusal changes nothing — in particular it never touches the
    /// incumbent, whose whole point is that it has not been disturbed.
    TxnResult begin_prepared_replacement(WeaveId op, WeaveId coordinator, WeaveId incumbent,
                                         WeaveId candidate, const std::string& role,
                                         std::uint32_t budget);

    /// Spend one unit of the preparation budget. THE DETERMINISTIC UNIT: an
    /// explicit step, not a clock, not a sleep, and not a count of unrelated bus
    /// activity. Only decrements while Preparing; at zero the transaction aborts
    /// with PreparationExhausted.
    TxnResult tick_preparation(TxnId id);

    /// TRUSTED NATIVE SCAFFOLDING FOR R2B-3b-3, and deliberately not a message
    /// shape. The real readiness answer is an authenticated conversation between
    /// the coordinator and the candidate; this seam exists so the STATE MACHINE
    /// can be proven before that conversation exists, and the next slice replaces
    /// its caller rather than adding a second way to become ready.
    TxnResult mark_candidate_ready(TxnId id, WeaveId coordinator, WeaveId candidate);

    /// Commit: revalidate every exact participant, then delegate the topology
    /// change to `admit_candidate` — which remains the SOLE admission mutation.
    /// The transaction layer never moves a role itself.
    TxnResult commit_prepared_replacement(TxnId id, const LifecycleAuthority& authority,
                                          Message activation, std::int64_t sequence);

    /// Abort from any nonterminal state, by the exact operator.
    TxnResult abort_prepared_replacement(TxnId id, WeaveId op);

    /// Diagnostics: the state of a transaction, and how many slots are in use.
    TxnState transaction_state(TxnId id) const;
    bool transaction_active(TxnId id) const;
    std::size_t active_transactions() const noexcept;

    /// Take the terminal outcome of a transaction this weave began. Consumed
    /// once, and only by the EXACT operator life and incarnation that started it —
    /// a successor inherits no result, exactly as it inherits no conversation.
    bool take_outcome(WeaveId op, TxnOutcome& out);

    /// Mark a Weave dead; it stops receiving deliveries until revived. Emits Died.
    ///
    /// THIS IS THE DEATH TRANSITION, and committing it ends every unfinished
    /// deferred conversation that weave was a party to (R2B-2a) — before `Died` is
    /// announced, so an observer of the death sees those conversations already
    /// over. Revival, last-known-good fallback and quarantine therefore all begin
    /// from the same place: no inherited answer rights.
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
        /// WHICH CODE this id currently is. A WeaveId is never reused, so it
        /// already distinguishes a swap successor — but a code reload replaces the
        /// code behind a STABLE id, so only this distinguishes that successor from
        /// the incarnation that earned a deferred answer.
        ///
        /// Bumped in `swap_state` — and ONLY there. Corrected in R2B-2a, where the
        /// original claim ("wherever the bus commits new code") was read as covering
        /// `reload()` too: it does not, and never did. `reload()` is crash REVIVAL
        /// of the same code, so it advances nothing here; what ends the
        /// conversations of a life that ended is `abandon_deferred_for` at `kill`.
        std::uint64_t incarnation = 1;
        /// WHICH LIFE this id currently is — one continuous period of being alive
        /// (R2B-2b). Advanced on every dead -> alive transition, and NOWHERE else.
        ///
        /// A SECOND FIELD, DELIBERATELY, because one cannot carry both concepts:
        ///
        ///   code incarnation   changes when the code behind an id is replaced,
        ///                      while the weave never stopped living
        ///   life generation    changes when a life ends and another begins behind
        ///                      the same id, whatever the code
        ///
        /// Collapsing them would make a live code reload invalidate speech already
        /// in the queue — the same weave, still alive, mid-sentence — which is a
        /// semantic change nobody asked for. They are used where each belongs:
        /// deferred answers bind to the incarnation, queued envelopes to the life.
        std::uint64_t life = 1;
        /// OUTSIDE THE WORLD, AND WHOSE CONVERSATION IT IS (R2B-3). Invalid (0) for
        /// every ordinary weave. When valid, this record is a prepared CANDIDATE:
        /// it is loaded, constructed and real, but it is not a participant. It
        /// receives no publications, no ordinary sends and no role traffic, and it
        /// may speak only to the weave named here — the coordinator preparing it.
        ///
        /// A candidate is not "registered and please don't route to it": the
        /// routing paths themselves refuse, which is why this lives on the record
        /// the router already consults rather than in a table beside it.
        CandidateOwner sealed_by{};
        std::string role{}; ///< the role this Weave holds (empty if none); see roles_
        bool accepts_any = false; ///< AcceptMode::AnyRegistered — accept any registered shape (gated)
    };

    /// What an authenticated answer expects to find at its destination.
    ///
    /// `present` is what distinguishes "this is an answer, check the occupant"
    /// from "this is an ordinary message, deliver by the ordinary rules" — a flag
    /// rather than a reserved life value, so no number has to carry an implicit
    /// meaning.
    struct AnswerTarget {
        bool present = false;
        std::uint64_t life = 0;
        std::uint64_t incarnation = 0;
    };

    struct Envelope {
        Message msg;
        WeaveId target{};
        std::uint64_t seq = 0;
        bool gated = false;  ///< true => Weave-originated; authorize against the sender's grant
        std::string role{};  ///< non-empty => role-targeted; resolved to a holder at delivery
        /// WHICH LIFE AUTHORED THIS (R2B-2b). Stamped by the bus at enqueue from
        /// the sender's current life, compared against the sender's life at
        /// delivery. Meaningful only when `gated` — a host/root send belongs to no
        /// weave life, and 0 there means "not weave speech", not a magic life.
        ///
        /// IT LIVES ON THE ENVELOPE AND NOT ON `Message`, which is the whole point:
        /// `Envelope` is private to the Switchboard and has no wire form, no
        /// schema, and no constructor a weave can reach. So there is nothing for a
        /// weave to read, copy, forge or replay — a hoarded `Message` re-sent later
        /// is stamped afresh with its *re-sender's* life, because the stamp was
        /// never part of what it hoarded.
        std::uint64_t sender_life = 0;
        /// WHICH REQUESTER THIS ANSWER IS FOR (R2B-2c). Present only on envelopes
        /// queued through an answer door — `answer_as` and `spend_deferred_as` —
        /// and absent on every ordinary send, because ordinary messages are
        /// deliberately addressed to a logical destination and should reach
        /// whoever legitimately occupies it at delivery. An authenticated answer
        /// is not that: its meaning already names one exact conversation between
        /// exact participants, so it names them here too.
        ///
        /// LAST, and deliberately: every ordinary enqueue brace-initializes this
        /// struct up to `sender_life` and stops, so an ordinary send cannot carry a
        /// target expectation even by accident. Only `enqueue_answer` sets it.
        AnswerTarget answer_target{};
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
        /// The shape of the request being answered, carried so that a refusal
        /// raised where no message is in hand — deferral overflow — can still say
        /// WHICH conversation it refused instead of naming an unrelated schema.
        std::shared_ptr<const Schema> shape{};
        /// WHO ASKED, captured AT DELIVERY of the request rather than recomputed
        /// when an answer is eventually produced (R2B-2c). Recomputing later would
        /// mean an answer silently retargets itself onto whatever the requester
        /// has become — which is the entire failure this phase exists to prevent.
        /// Both answer doors read the requester's identity from here, so there is
        /// one capture point and no drift between the immediate and deferred paths.
        std::uint64_t requester_life = 0;
        std::uint64_t requester_incarnation = 0;
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
        DeferredAnswer make_deferred_answer() override { return sb_.defer_answer_as(self_); }
        Ticket spend_deferred(const DeferredAnswer& answer, Message msg) override {
            return sb_.spend_deferred_as(self_, answer, std::move(msg));
        }
        void release_deferred(const DeferredAnswer& answer) override {
            sb_.release_deferred_as(self_, answer);
        }
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

    // ---- deferred answers (R2B-2) -------------------------------------------

    /// ONE UNFINISHED CONVERSATION, held by the bus.
    ///
    /// It records both participants' EXACT INCARNATIONS, not just their ids —
    /// which is the load-bearing part. A WeaveId is never reused, so it
    /// distinguishes a swap successor for free; it does NOT distinguish a RELOAD
    /// successor, because reload replaces the code behind a stable id. Handler-
    /// surviving authority must not accidentally become reload-surviving
    /// authority, so the incarnation counter is what makes "the participant that
    /// earned the right" mean the code that earned it.
    struct DeferredRecord {
        std::uint64_t token = 0; ///< 0 = a free slot
        WeaveId requester{};
        std::uint64_t requester_incarnation = 0;
        /// The requester's LIFE at the moment it asked (R2B-2c). The incarnation
        /// above distinguishes replaced code behind a stable id; this
        /// distinguishes a different life behind it.
        std::uint64_t requester_life = 0;
        WeaveId respondent{};
        std::uint64_t respondent_incarnation = 0;
        std::uint64_t correlation = 0;
    };

    /// The ONE door every authenticated answer leaves by (R2B-2c).
    ///
    /// Both `answer_as` and `spend_deferred_as` funnel through here, so the
    /// target expectation, the provenance and the recipient are decided in a
    /// single place. Two nearly-identical enqueues either side of a registry is
    /// exactly the shape that drifts.
    Ticket enqueue_answer(WeaveId to, WeaveId as_sender, Message msg, std::uint64_t correlation,
                          std::uint64_t requester_life, std::uint64_t requester_incarnation);

    DeferredAnswer defer_answer_as(WeaveId as_sender);
    Ticket spend_deferred_as(WeaveId as_sender, const DeferredAnswer& answer, Message msg);
    void release_deferred_as(WeaveId as_sender, const DeferredAnswer& answer);

    /// Did THIS board issue this deferred answer? A capability held natively
    /// carries its issuer; one held inside a dynamic library carries none, and is
    /// board-relative structurally (its token can only reach its own registry).
    bool issued_here_deferred(const DeferredAnswer& answer) const noexcept {
        const std::shared_ptr<const LoomIdentity> issuer = answer.issuer().lock();
        return issuer == nullptr || issuer == identity_;
    }

    /// Drop every unfinished conversation either side of which is `id` at an
    /// incarnation that no longer exists. THE QUESTION HERE IS STALENESS, which is
    /// the right question when the CODE behind a still-living id was replaced:
    /// `swap_state` bumps the incarnation and this sweeps what the predecessor
    /// left. It is deliberately NOT the question death asks — see
    /// `abandon_deferred_for`.
    void forget_deferred_for(WeaveId id);

    /// Drop every unfinished conversation `id` is a party to, in either direction
    /// and at ANY incarnation, unconditionally.
    ///
    /// THE QUESTION HERE IS THE END OF A LIFE (R2B-2a), and staleness cannot ask
    /// it: a killed weave keeps its id AND its incarnation, so nothing about the
    /// record looks stale — yet the participant that earned the right is gone.
    /// Called at `kill` (death, which the isolation supervisor drives on a crashed
    /// child before reviving it from the host-owned snapshot) and at
    /// `unregister_weave` (permanent removal). Being unconditional also makes it
    /// order-independent: unlike the staleness sweep, it does not depend on whether
    /// the record has already been erased from `weaves_`.
    void abandon_deferred_for(WeaveId id);

    /// One prepared replacement, remembered in full. Nothing here is inferred
    /// from the absence of a field: the state is a state, and the reason is a
    /// reason.
    struct PreparedReplacement {
        TxnId id{};
        ParticipantRef op{};
        ParticipantRef coordinator{};
        ParticipantRef incumbent{};
        ParticipantRef candidate{};
        std::string role;
        TxnState state = TxnState::Preparing;
        std::uint32_t budget = 0;
        TxnReason reason = TxnReason::None;
    };

    /// Snapshot a participant as it is right now.
    ParticipantRef participant(WeaveId id) const;
    /// Is this participant still exactly who it was, and still alive?
    bool still(const ParticipantRef& was) const;

    PreparedReplacement* find_txn(TxnId id);
    const PreparedReplacement* find_txn(TxnId id) const;

    /// End a transaction, record its outcome for its operator, and free the slot.
    void finish_txn(PreparedReplacement& txn, TxnState state, TxnReason reason);

    /// EVERY TRANSITION THAT CAN INVALIDATE A PARTICIPANT CALLS THIS, inline.
    /// Aborts only the transactions that BIND `changed` and whose captured facts
    /// no longer hold — never the whole registry, which would make one weave's
    /// death everybody's problem.
    void invalidate_transactions_for(WeaveId changed);

    /// Advance `rec`'s life generation iff it is currently dead — i.e. iff the
    /// caller is about to bring it back. Called by every revival path before it
    /// marks the record alive (R2B-2b).
    void begin_new_life(WeaveRecord& rec);

    DeferredRecord* find_deferred(std::uint64_t token);
    /// This weave's current incarnation, or 0 if it is not registered.
    std::uint64_t incarnation_of(WeaveId id) const;
    /// This weave's current life generation, or 0 if it is not registered. Used to
    /// stamp an envelope at enqueue and to check it at delivery.
    std::uint64_t life_of(WeaveId id) const;

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
    std::vector<DeferredRecord> deferred_;      ///< bounded; see kMaxDeferredAnswers
    /// Monotonic; a token is never reused. DELIBERATELY UNGUARDED against
    /// exhaustion, unlike the activation sequence, and the difference is the
    /// reason: that one is PERSISTED and revived from state bytes, so a caller can
    /// hand it a large value and it must refuse rather than wrap. This one is
    /// process-local, never serialized, never revived, and advances by exactly one
    /// per deferral — so 2^64 is not a number an adversary can approach, only one a
    /// process can outlive by any measure.
    /// Bounded, and small. Slots are reclaimed the moment a transaction ends.
    std::vector<PreparedReplacement> txns_;
    std::vector<TxnOutcome> outcomes_;      ///< bounded; oldest dropped
    std::vector<ParticipantRef> outcome_of_; ///< whose outcome each one is
    std::uint64_t next_txn_id_ = 1;

    std::uint64_t next_deferred_token_ = 1;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_SWITCHBOARD_HPP
