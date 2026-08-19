// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_SWITCHBOARD_HPP
#define ZEN_SWITCHBOARD_SWITCHBOARD_HPP

#include <zen/admission.hpp>
#include <zen/registry.hpp>
#include <zen/schema.hpp>
#include <zen/switchboard/grant.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/switchboard/sense.hpp>
#include <zen/switchboard/weave_contract.hpp>
#include <zen/value.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace loom {

/// Why a delivery (or revival) was refused. Conformance refusals carry a
/// loom Error (the gate's verdict); the rest are bus-level routing reasons
/// the gate never sees.
///
/// THE REASONS ARE NARROW ON PURPOSE AND MUST STAY THAT WAY. Each one sends an
/// operator to a different fix, so widening an existing reason to cover a new
/// failure is how a refusal starts lying — report a capacity limit as
/// `CapabilityDenied` and an operator edits a grant that was never the problem.
/// Add a reason rather than stretch one. Each member below therefore names only
/// what makes it *not* its nearest neighbour.
/// MSG-05; docs/laws/messaging-laws.md
/// The order these are decided in: docs/reference/messaging.md#dispatch-model
enum class RefusalReason : std::uint8_t {
    None = 0,
    NoSuchTarget,      ///< directed at a WeaveId that is not registered
    TargetUnavailable, ///< the target is currently dead (awaiting revival)
    NotAccepted,       ///< the target's accept-set has no schema of this (name, version)
    GateRefused,       ///< routing passed, but admit() refused — see `error`
    CapabilityDenied,  ///< the sender's grant does not permit (shape -> target); the gate is
                       ///< never reached. Authorization, not conformance.
    /// A lifecycle/answer authority this Loom did not issue — or one expired,
    /// already spent, or bound to a different conversation or incarnation. NOT
    /// `CapabilityDenied`: the sender's grant may be perfectly correct while the
    /// AUTHORITY DOMAIN is wrong. The grant answers "may you send this shape?";
    /// this answers "is this yours to say here?".
    ForeignAuthority,
    /// A published bound was reached; nothing about authority or conformance was
    /// wrong. NOT `ForeignAuthority`: no forgery happened, the room is just full.
    Exhausted,
    /// The message was authored by a life that has since ended: the sender is
    /// dead, permanently removed, or revived — which makes the speaker a
    /// different life behind the same `WeaveId`. What ended is the utterance's
    /// AUTHOR; the target is fine and the payload is fine.
    /// MSG-03; docs/laws/messaging-laws.md
    SenderLifeEnded,
    /// An authenticated answer arrived for a requester that is no longer the one
    /// that asked: the weave at that `WeaveId` has died and been revived, or its
    /// code has been replaced. The address is right and the occupant is wrong —
    /// which is what separates it from `SenderLifeEnded` (about the AUTHOR) and
    /// from `NoSuchTarget`/`TargetUnavailable` (nobody there, or somebody dead).
    /// ANS-03; docs/laws/answer-authority-laws.md
    AnswerTargetChanged,
    /// A SEALED weave tried to speak into the world. A prepared candidate lives
    /// outside the live world: it may converse with the coordinator preparing it
    /// and with nobody else, and this is what it hears when it tries.
    ///
    /// Deliberately visible, unlike its mirror: a message aimed AT a sealed weave
    /// by anyone but its coordinator is refused as `NoSuchTarget`, so the world
    /// cannot learn that a candidate exists. Do not make the two symmetrical.
    /// PR-01; docs/laws/replacement-laws.md
    SealedSpeech,
    /// A SCHEDULED ADMISSION NO LONGER DESCRIBED THE WORLD when it reached the
    /// head of the queue: a participant died, was reloaded or was removed, the
    /// role moved, the seal changed hands, or its transaction had already ended.
    ///
    /// NOTHING CHANGED — this refuses an ADMISSION, never an activation. Every
    /// other name here says one message failed to arrive; this one says the
    /// change that message was carrying did not happen, so an operator reading it
    /// looks at the participants rather than at a grant, payload or address.
    /// PR-03 (the drift check), PR-07 (the window it can drift in);
    /// docs/laws/replacement-laws.md
    AdmissionRevoked,
    /// A weave deliberately asked to speak as an office it does not hold: the
    /// role is unbound, held by somebody else, or the speaker has no identity to
    /// hold one. Refused at the AUTHORSHIP moment — nothing was queued, and the
    /// statement was NOT downgraded to personal speech. Narrower than
    /// `CapabilityDenied` (the grant may be perfect) and than `ForeignAuthority`
    /// (no capability object was presented).
    /// MSG-07; docs/laws/messaging-laws.md
    RoleAuthorshipDenied,
    /// A LOADED WEAVE CLAIMED A SHAPE THIS LOOM HAS NEVER HEARD OF. The emission
    /// crossed the library/host seam carrying a (name, version) `resolve_schema`
    /// cannot resolve, so there is no door to admit it against; rejected before
    /// anything is queued and before any target is resolved.
    ///
    /// Every neighbour would misdirect: `NotAccepted` blames a target's
    /// accept-set when routing never ran, `GateRefused` blames a payload that may
    /// be perfectly well-formed for the shape its author meant, `NoSuchTarget`
    /// blames an address never consulted. What failed is the SEAM.
    ///
    /// It carries the CLAIMED name and version, the sending artifact, and a
    /// target ONLY where one was actually named — inventing one would replace
    /// silence with a fiction. A diagnostic, not an answer and not a delivery:
    /// send fate remains unobservable to senders.
    /// MSG-08; docs/laws/messaging-laws.md
    /// docs/reference/known-seams.md#sender-cannot-observe-send-fate
    SeamUnresolved,
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

/// What an observer/tap is told about. Deliveries (Delivered/Refused/
/// HandlerFailed) and lifecycle transitions (Died/Revived) flow through the same
/// hook.
///
/// `HandlerFailed` says ONE thing and deliberately not a second: the handler was
/// entered and did not complete normally. Loom does not say what it did - its
/// journal slot stays `Pending`, "no outcome was recorded" (MSG-10) - and the
/// event carries no reason, because Loom has none: a native handler's exception
/// is rethrown to the host unexamined, and a loaded weave's is already caught at
/// the ABI boundary, where only a status crosses. It exists because the
/// alternative was worse in two different ways at once: a native throw produced
/// NO event at all, so an observer's record read as though the delivery never
/// happened, and a loaded weave's failure produced a `Delivered` event, which is
/// the same silence wearing a success's clothes.
enum class EventKind : std::uint8_t { Delivered, Refused, Died, Revived, HandlerFailed };

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
    /// DIAGNOSTICS ONLY, and only for a weave-originated delivery: the sender
    /// life stamped on this envelope at enqueue, and the sender's life right now.
    /// When they differ, the author's life ended while the message waited — so a
    /// journal reader sees the cause rather than inferring it. Both 0 for
    /// host/root sends, which belong to no weave life (MSG-03).
    std::uint64_t sender_life = 0;
    std::uint64_t sender_life_now = 0;
    /// DIAGNOSTICS ONLY, and only for an authenticated answer: what the
    /// conversation expected of its requester, and what that requester is now —
    /// so the journal shows "expected life 1 / incarnation 1, found life 2"
    /// instead of leaving it to be deduced. Zero on every other delivery (ANS-03).
    std::uint64_t expected_requester_life = 0;
    std::uint64_t expected_requester_incarnation = 0;
    std::uint64_t requester_life_now = 0;
    std::uint64_t requester_incarnation_now = 0;
    /// The office this delivery was DELIBERATELY authored as — the envelope's
    /// STAMPED fact, never a lookup of the sender's current role. Empty for
    /// personal speech, which is almost everything. A tap reads historical
    /// authorship here; `role_of(sender)` asks a different question at a
    /// different time and may already disagree.
    /// MSG-07; docs/laws/messaging-laws.md
    std::string authored_role{};
    /// WHICH CONVERSATION, AS THE SENDER NAMED IT - the envelope's correlation,
    /// copied verbatim. A number the sender chooses (ANS-05): it identifies and
    /// it authenticates NOTHING, and 0 is both "none stated" and a legal choice.
    /// It is here because an observer that cannot see which ask an answer belongs
    /// to cannot show a conversation at all; the fact was already on the envelope
    /// and was simply not carried out.
    std::uint64_t correlation = 0;
    /// THE ROLE THIS DELIVERY WAS ADDRESSED TO - the office the SENDER named,
    /// resolved to `target` at dispatch. Empty for a directed send and for a
    /// publication, which name no role at all.
    ///
    /// A DIFFERENT QUESTION FROM `authored_role`, and the two are not
    /// interchangeable: this is whose door was knocked on, that is which office
    /// the speaker spoke as. Without it the resolution is unrecoverable - an
    /// observer sees the WeaveId that answered and cannot tell whether it was
    /// addressed personally or as the holder of a slot.
    std::string addressed_role{};
    /// WHICH DELIVERY WAS BEING DISPATCHED WHEN THIS ONE WAS ENQUEUED. 0 when
    /// nothing was - a host send between turns, or the first message of a turn.
    ///
    /// SYNCHRONOUS DISPATCH ANCESTRY, AND NOTHING WIDER. It answers "this message
    /// was authored from inside the handling of that one", which is exactly what
    /// the single-minded FIFO already knows and never wrote down. It is NOT
    /// causality: a message an external operation produces is enqueued from
    /// inside whatever delivery happened to be draining it (a timer beat, say),
    /// so its dispatch parent is that beat and not the request the operation
    /// belongs to. Long-lived semantic relation is what a correlation or an
    /// application's own operation identifier is for, and this field must never
    /// be read as one.
    std::uint64_t dispatch_parent = 0;
    /// HOW LONG THE HANDLER HELD THE ONE MIND, in nanoseconds, measured on the
    /// steady clock around the `handle` call alone. 0 on every event that ran no
    /// handler (a refusal, a lifecycle transition).
    ///
    /// A DURATION, NEVER A TIME. Loom stamps no message with a clock reading and
    /// this does not begin doing so: it is the elapsed measure of one call, which
    /// is meaningful without any shared notion of when. It is wall/monotonic
    /// rather than CPU time deliberately - in an organism with a single mind,
    /// blocking IS the defect worth seeing, whoever the blocking is spent on.
    std::uint64_t handler_elapsed_ns = 0;
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

/// WHO OWNS A SEALED CANDIDATE — as three facts, never as a `WeaveId` alone.
///
/// A coordinator that dies and revives, or whose code is replaced, is a
/// DIFFERENT participant at the same address, and it inherits neither speech
/// nor conversations. A preparation is a conversation, so the seal must name the
/// exact life and incarnation that made it. The issuing Loom is implicit in the
/// record living on that Loom.
/// PR-03; docs/laws/replacement-laws.md
struct CandidateOwner {
    WeaveId who{};
    std::uint64_t life = 0;
    std::uint64_t incarnation = 0;

    bool valid() const noexcept { return who.valid(); }
    friend bool operator==(const CandidateOwner& a, const CandidateOwner& b) noexcept {
        return a.who == b.who && a.life == b.life && a.incarnation == b.incarnation;
    }
};

/// Why an admission was refused, for the host that asked.
///
/// Admission is a host call, not a delivery, so there is no message to refuse
/// and no tap event to carry a `RefusalReason`. The reason therefore rides the
/// answer the caller already receives — named rather than a bare false, because
/// "the coordinator that sealed this is not the one standing here now" and "the
/// role moved under you" send an operator to entirely different places.
enum class AdmitRefusal : std::uint8_t {
    None = 0,         ///< scheduled
    ForeignAuthority, ///< the lifecycle authority was not issued by this Loom
    NotACandidate,    ///< missing, dead, or not sealed at all
    OwnerChanged,     ///< the exact coordinator life/incarnation that sealed it is gone
    IncumbentUnfit,   ///< missing, dead, or already sealed
    RoleNotHeld,      ///< the role is empty, or held by somebody other than the incumbent
    /// THE CANDIDATE CANNOT RECEIVE THE COMMITTED ACTIVATION: it does not accept
    /// `zen.Activated` at all, or the exact activation this admission would
    /// deliver does not pass its own gate. Asked HERE, before anything moves,
    /// because discovering it at delivery means discovering it after the role has
    /// already moved — publicly admitted and never told.
    /// PR-08; docs/laws/replacement-laws.md
    CandidateContract,
};

const char* name_of(AdmitRefusal r) noexcept;

/// The result of SCHEDULING an admission. Convertible to bool so the common
/// `REQUIRE(bus.admit_candidate(...))` reads as it should, with `why` for the
/// cases that care.
///
/// `scheduled`, NOT `ok` — the field name is the contract. This call performs no
/// admission: it validates, proves the candidate can receive its activation, and
/// places ONE envelope that will do the whole thing at once. It reports that the
/// envelope is there, never that the role has moved.
///
/// `ticket` is that envelope's, and it is how a direct caller learns the real
/// outcome: `Delivered` = admitted AND told; `Refused` with `AdmissionRevoked` =
/// the world drifted and nothing changed. A transaction caller reads the same
/// fact from its terminal outcome.
/// PR-07; docs/laws/replacement-laws.md
struct AdmitResult {
    bool scheduled = false;
    AdmitRefusal why = AdmitRefusal::None;
    /// The admission-activation delivery. Invalid on a refusal, because a
    /// refusal queues nothing.
    Ticket ticket{};

    explicit operator bool() const noexcept { return scheduled; }
};

/// WHO A TRANSACTION IS BOUND TO — the same three facts, for the same reason as
/// `CandidateOwner` above: a participant that dies and revives, or whose code is
/// replaced, is a different participant at the same address, and a prepared
/// replacement belongs to the exact lives that began it. `CandidateOwner` is
/// this shape for the seal; this is it for the other three roles.
/// PR-02; docs/laws/replacement-laws.md
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

/// The whole state machine. Five states, and the two terminals are terminal.
///
/// `AdmissionPending` is not decoration: commit SCHEDULES the admission, and the
/// interval before it dispatches is real and observable. Throughout it the
/// incumbent is still the public service and the candidate is still sealed, so
/// the state must exist, must be abortable, and must hold its slot and its
/// candidate exclusivity exactly as `Preparing` and `Ready` do.
/// PR-07; docs/laws/replacement-laws.md
enum class TxnState : std::uint8_t { Preparing, Ready, AdmissionPending, Committed, Aborted };

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
    /// Another active transaction already names this incumbent, or this
    /// candidate. Two facts, named separately: "somebody is already replacing
    /// that service" and "that successor is already promised elsewhere" are
    /// different problems with different fixes (PR-02).
    IncumbentBusy,
    CandidateBusy,
    /// THE CANDIDATE ITSELF SAID NO — authentically, spending the one answer
    /// authority the preparation ask earned it. The only ending here that is
    /// neither a mechanism failure nor a participant vanishing: preparation ran
    /// and its verdict was no, so an operator looks at the successor's own
    /// judgement rather than at the topology.
    CandidateRefused,
    /// Something arrived claiming to be the readiness answer and was not: not an
    /// authenticated answer, from the wrong speaker, carrying another
    /// conversation's correlation, or offered when this transaction's one
    /// preparation conversation was never opened or is already consumed.
    /// DELIBERATELY ONE REASON for all of those — telling a forger *which* term
    /// it failed is telling it what to fix next.
    ///
    /// A refusal of the COMMAND, never a terminal result: hostile traffic does
    /// not get to end a legitimate transaction.
    InvalidReadiness,
    /// The answer is authentic and the transaction it names is over: aborted,
    /// out of budget, or committed while the answer sat in the queue. NOT
    /// `NoSuchTransaction`, which means this Loom never issued that id — "you are
    /// too late" and "that never existed" send an operator to different places,
    /// and a terminal record is never resurrected to say either.
    LateReadiness,
    /// This transaction's one preparation conversation has already been opened.
    /// There is exactly one, deliberately: a second ask would leave a candidate
    /// holding an authority for a correlation the transaction no longer expects,
    /// and "which of my two asks is this answering?" is a question the design
    /// refuses to have.
    PreparationAlreadyAsked,
};

/// What an authentically-answering candidate said. ONE DOOR TAKES BOTH, because
/// readiness and refusal differ in nothing but their effect: every authority
/// check — that this is an answer, that the candidate spoke it, that it belongs
/// to this conversation — is identical, and giving them separate doors is how two
/// definitions of "authentic" start to drift apart.
enum class PreparationAnswer : std::uint8_t {
    Ready,   ///< preparation completed; the transaction may become Ready
    Refused, ///< preparation will not complete; the transaction ends, once
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

    /// Remove a Weave and hand its ownership back to the caller. Used by hosts
    /// that must destroy a Weave — and any resources it holds, such as a loaded
    /// library instance — in a controlled order. Pending deliveries to a removed
    /// Weave are refused (NoSuchTarget) at delivery, and any role it held is
    /// released — the slot is free for a successor.
    ///
    /// ITS SCHEMA CLAIMS GO WITH IT (LIFE-08). Erasing the record destroys the
    /// `SchemaClaimScope` built at registration, so this weave's accept-set,
    /// claim-set, state shape and grant-named send shapes each stop resolving
    /// UNLESS something else still claims them. A shape two weaves accept
    /// survives the first one's removal; a shape only this weave named does not.
    /// docs/laws/lifecycle-laws.md
    ///
    /// `nullptr` MEANS NOTHING WAS REMOVED, and there are two ways to earn it:
    /// the id is unknown, or the id names the weave whose callback is running
    /// right now. The second refuses TOTALLY — the check precedes every mutation
    /// — because success hands back a unique owner the caller may destroy at
    /// once, and Loom cannot both transfer unique ownership and keep the object.
    /// Retry after the callback exits (by return or by unwind) and it succeeds.
    /// Exact to the ACTIVE TARGET, not to `in_dispatch_`: removing a *different*
    /// weave mid-callback still works, which the transaction layer relies on.
    /// LIFE-06; docs/reference/lifecycle.md#permanent-removal-and-the-active-callback
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

    // ---- deliberate office authorship ---------------------------------------
    //
    //     A weave may deliberately author one statement in the capacity of a
    //     role it currently holds. Loom verifies that membership at the
    //     authorship moment and carries the office fact as immutable delivery
    //     provenance. Merely holding the role attaches nothing.
    //
    // THE AUTHORIZATION MOMENT IS AUTHORSHIP/ENQUEUE, deliberately: deciding at
    // delivery instead would let a statement's meaning change because the role
    // moved while it waited in the queue. Once stamped the fact is HISTORICAL —
    // delivery never recomputes it. Office authorship changes why a recipient may
    // trust who spoke, never where the sender may speak or what it may emit: every
    // other delivery law (sender life, the seal, the ordinary grant, routing)
    // still applies unchanged.
    //
    // The `*_as` forms are the verified doors (the sender is stamped from the
    // caller's authority, never from a payload). A refusal queues NOTHING and is
    // visible on the tap as `RoleAuthorshipDenied` — never downgraded to personal
    // speech. The Bus-inherited root forms below them refuse always: a root has no
    // identity and holds no office.
    // MSG-07, MSG-04; docs/laws/messaging-laws.md
    // docs/reference/messaging.md#office-authorship-role-authored-provenance

    /// Verified office-authored direct send. Invalid Ticket = authorship
    /// refused, nothing queued (the refusal is on the tap and in the journal).
    Ticket office_send_as(WeaveId as_sender, std::string_view as_role, WeaveId target,
                          Message msg);
    /// Verified office-authored role-addressed send. `as_role` is the office
    /// spoken for (verified now); `to_role` is the destination slot (resolved
    /// at delivery). Carried separately end to end — an office-authored send to
    /// another office preserves BOTH facts without conflating them.
    Ticket office_send_to_role_as(WeaveId as_sender, std::string_view as_role,
                                  std::string_view to_role, Message msg);
    /// Verified office-authored publication. Keeps "authorship refused" and
    /// "authorized, zero recipients" distinct; each recipient's delivery is
    /// still independently authorized against the sender's ordinary grant.
    OfficePublication office_publish_as(WeaveId as_sender, std::string_view as_role, Message msg);

    /// The Bus office verbs, as the ROOT surface: refused, visibly. A root has
    /// no weave identity, so it holds no office and cannot deliberately speak
    /// for one — `send_as`/`office_send_as` exist precisely so a trusted host
    /// speaks AS a weave when it must.
    Ticket office_send(std::string_view as_role, WeaveId target, Message msg) override;
    Ticket office_send_to_role(std::string_view as_role, std::string_view to_role,
                               Message msg) override;
    OfficePublication office_publish(std::string_view as_role, Message msg) override;


    /// Deliver until the queue drains. Single-threaded, FIFO, non-reentrant: a
    /// reentrant call (from within a handler) is a no-op.
    void pump();
    void run() { pump(); }
    void stop() noexcept { stop_requested_ = true; }

    /// DISPATCH EXACTLY WHAT WAS ALREADY QUEUED, THEN GIVE THE CALLER BACK
    /// CONTROL. Returns how many were dispatched.
    ///
    /// THE bounded turn, and the one an event-loop host wants: `pump()` drains to
    /// empty, so a perpetual service (a repeating Timer re-arms inside its own
    /// handler) means it never returns and the outer loop never polls again.
    ///
    /// THE BOUND IS `pending()` AT ENTRY, and it deliberately takes no number.
    /// Work a handler enqueues DURING this call lands behind that snapshot and
    /// waits for the next turn — which is exactly what a self-re-arming producer
    /// cannot get around. A count from the queue rather than a clock, so it stays
    /// deterministic; the boundary lands between two envelopes, so FIFO is exact;
    /// and a busy bus still clears its whole backlog in one turn.
    ///
    /// DO NOT ADD A NUMERIC BUDGET. `pump_bounded(n)` shipped and was withdrawn:
    /// sizing `n` means knowing the producer's rate, and with a real Zengine Timer
    /// 64 throttled a live round-trip 17x while a value large enough not to
    /// throttle was drain-to-empty with the starvation back. The count survives
    /// only as this function's private implementation.
    /// MSG-09; docs/laws/messaging-laws.md
    ///
    /// `stop()` ends the turn early here too, and the return value reports what
    /// actually happened. Non-reentrant, like every pump. An empty queue is a
    /// no-op returning 0.
    std::size_t pump_pending();

    // ---- Senses --------------------------------------------------------------
    //
    // A Sense is a deliberate immutable claim of the latest observation a
    // participant has made available: read synchronously, truthfully authored,
    // predicting nothing about queued work, sharing no memory with the claimant.
    //
    // VISIBILITY IS AT THE SUCCESSFUL CLAIM CALL. Nothing defers it to handler
    // completion, so there is no settlement step to forget — and the simpler rule
    // costs nothing, because dispatch is single-threaded and non-reentrant
    // (MSG-01): no other participant can run between a claim call and the end of
    // the handler that made it, so only the claimant observing itself could ever
    // tell the two rules apart.
    // SENSE-01, SENSE-02; docs/laws/sense-laws.md

    /// CLAIM `value` AS `claimant`, PERSONALLY (host/root door; the gated weave
    /// path is `claim_as`). The key is (claimant, shape) — a personal claim is
    /// not reachable through any office key, which is what makes "holding an
    /// office is not claiming as one" structural rather than a convention.
    SenseClaimResult claim_as(WeaveId claimant, Value value);

    /// CLAIM `value` DELIBERATELY AS THE OFFICE `as_role`. Verified at the claim
    /// moment (`role_holder(as_role) == claimant`), exactly as office authorship
    /// verifies at the authorship moment (MSG-07). A claimant that does not hold
    /// the office is refused `OfficeNotHeld` and NOTHING is stored — never
    /// downgraded to a personal claim.
    ///
    /// The key is (role, shape). The claim also records WHICH weave authored it,
    /// so a later role movement can be reported rather than hidden.
    SenseClaimResult office_claim_as(WeaveId claimant, std::string_view as_role, Value value);

    /// THE LATEST CLAIM `author` MADE PERSONALLY of this shape, as an observation
    /// gated by `reader`'s grant. Never reaches into the claimant: the value is a
    /// copy the caller owns.
    SenseReading observe_as(WeaveId reader, WeaveId author, std::string_view shape_name,
                            std::uint32_t shape_version) const;

    /// THE LATEST CLAIM MADE AS THE OFFICE `role`, gated by `reader`'s grant.
    ///
    /// ROLE MOVEMENT NEVER REWRITES HISTORY: after a replacement moves the role
    /// this still returns the PREDECESSOR's claim, stamped
    /// `office_holder_is_current=false` rather than relabelled or withheld.
    /// Returning nothing was the rejected alternative — it collapses "this office
    /// has never claimed" and "this office's claim is the previous holder's" into
    /// one empty answer. A reader wanting the strict view writes
    /// `if (r && r.by.office_holder_is_current)`.
    /// SENSE-03; docs/laws/sense-laws.md
    SenseReading observe_office_as(WeaveId reader, std::string_view role,
                                   std::string_view shape_name,
                                   std::uint32_t shape_version) const;

    /// The host's own ungated observation — root authority reads, exactly as
    /// `Switchboard::send` is the ungated send.
    ///
    /// These take a (name, version) pair where `Bus`'s virtuals take a resolved
    /// `Schema`, so without the `using` below the name-lookup rules would hide
    /// the base overloads on every `Switchboard&` — a participant reaching the
    /// bus through a `Switchboard` reference would silently lose the schema-typed
    /// door. Both spellings stay reachable, and `-Woverloaded-virtual` stays
    /// quiet because the hiding is declared rather than accidental.
    using Bus::observe;
    using Bus::observe_office;

    SenseReading observe(WeaveId author, std::string_view shape_name,
                         std::uint32_t shape_version) const;
    SenseReading observe_office(std::string_view role, std::string_view shape_name,
                                std::uint32_t shape_version) const;

    /// WHAT SENSES CAN THIS PARTICIPANT PROVIDE — the declared claim-set, so a
    /// consumer can discover capability before any runtime claim happens rather
    /// than after one accidentally appears. Empty for a weave that declares none.
    std::vector<std::shared_ptr<const Schema>> claimed_schemas(WeaveId id) const;

    /// How many latest claims are retained right now, across both key spaces.
    /// The lifecycle witness reads this: it is bounded by MEANINGFUL CURRENT KEYS
    /// (registered weaves x shapes they declare, plus held roles x shapes), never
    /// by the number of claims ever made and never one entry per historical
    /// incarnation. A reload, a revival or a thousand re-claims replace the value
    /// under one key; they do not add keys.
    std::size_t retained_claim_count() const noexcept {
        return personal_claims_.size() + office_claims_.size();
    }

    /// RECORD A REJECTION THAT HAPPENED AT A BOUNDARY THIS LOOM OWNS BUT THE BUS
    /// NEVER SAW. The dynamic seam admits a loaded weave's bytes host-side
    /// *before* routing, so when that fails nothing is queued and no
    /// delivery-time refusal can report it.
    ///
    /// A DIAGNOSTIC, and deliberately nothing else: a seq, a journal slot and a
    /// tap event, exactly like `refuse_now`, so a seam rejection reads at the same
    /// altitude as a capability refusal — but no answer provenance, no delivery,
    /// and no sender-visible future.
    ///
    /// It carries the CLAIMED (name, version), since the shape could not be
    /// resolved and there is no schema object to name; and `target` stays the
    /// invalid id wherever the emission named none. MANUFACTURE NOTHING HERE — a
    /// publication has no target, and a role is a slot resolved at a delivery that
    /// never happened; inventing one would replace silence with a fiction.
    /// MSG-08; docs/laws/messaging-laws.md
    ///
    /// Callable by the host (holding a `Switchboard&` is already root authority)
    /// because the Kernel's seam callbacks are the only intended caller and they
    /// hold exactly that.
    void note_seam_refusal(WeaveId sender, WeaveId target, std::string_view claimed_name,
                           std::uint32_t claimed_version, const Refusal& refusal);

    /// THE DELIVERY BEING DISPATCHED RIGHT NOW DID NOT COMPLETE NORMALLY - said
    /// by the only caller in a position to know, the Kernel's `HostAdapter`,
    /// whose loaded weave reports failure as an ABI status rather than as an
    /// exception (`docs/reference/dynamic-abi.md`: a library's exceptions are
    /// caught at the seam and never reach this bus).
    ///
    /// It records nothing and refuses nothing. It sets one bit for the length of
    /// this delivery, so `deliver_one` emits `HandlerFailed` instead of
    /// `Delivered` and writes no journal outcome - which is exactly what the
    /// native throwing path already does. Both seams, one fact, one word for it.
    ///
    /// Outside a dispatch it does nothing at all: there is no delivery to be
    /// speaking about, and inventing one would be worse than saying nothing.
    /// Callable by the host because holding a `Switchboard&` is already root
    /// authority - the same reason `note_seam_refusal` is.
    /// MSG-10; docs/laws/messaging-laws.md
    void note_handler_failure() noexcept;

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

    /// How many unfinished conversations one Loom will hold at once.
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
    /// able to converse only with `coordinator` (PR-01; docs/laws/replacement-laws.md).
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

    /// Which role this weave holds right now — empty if none, or if there is no
    /// such weave. The same fact as `role_holder` read from the other end.
    ///
    /// IT EXISTS SO NOBODY HAS TO CACHE IT. A prepared replacement committing —
    /// or a direct `admit_candidate` — moves a role with NO host call at all, so a
    /// host that remembers what it bound at registration is holding an answer that
    /// becomes a lie the moment an admission lands. Read-only and derived from the
    /// same table routing binds, so it cannot drift from it.
    std::string role_of(WeaveId id) const;

    /// THE COMMIT. One operation, one visible change.
    ///
    /// Unseals `candidate` and moves `role` from `incumbent` to it. Everything an
    /// ordinary observer could notice — the candidate becoming reachable, the role
    /// changing hands, the incumbent ceasing to hold it — happens between two
    /// deliveries, so no pump can run in the middle of it. That atomicity is a
    /// property of the single-threaded queue rather than a lock (MSG-01): `pump()`
    /// dispatches one envelope at a time and is non-reentrant, so a commit
    /// performed outside `deliver_one` (or wholly within one handler) cannot be
    /// observed half-done.
    ///
    /// Refuses — changing NOTHING — if the candidate is not sealed, if either
    /// weave is missing or dead, or if the role is held by anyone other than the
    /// incumbent. A refused commit is observationally identical to no commit.
    bool commit_candidate(WeaveId candidate, WeaveId incumbent, const std::string& role);

    /// THE ADMISSION: the commit above, extended to account for the incumbent and
    /// for activation — and SCHEDULED, not performed.
    ///
    ///     Entering the world and being told that you entered it are one event.
    ///
    /// ONE ENVELOPE IS BOTH, so no state exists in which only one happened:
    ///
    ///   at this call      validate everything; prove the candidate can receive
    ///                     this exact activation; place the envelope. Topology
    ///                     is UNTOUCHED — the incumbent is still the service.
    ///   at its dispatch   revalidate; admit the payload through the candidate's
    ///                     own gate; THEN seal the incumbent, unseal the
    ///                     candidate and move the role; then hand the candidate
    ///                     its activation, in the same dispatch, with nothing
    ///                     whatever in between.
    ///
    /// PLACEMENT: immediately ahead of the first queued envelope that could reach
    /// this candidate (addressed to the committed role, or to the candidate
    /// directly). Role resolution is a DELIVERY-time decision, so a tail append
    /// would let production reach a weave not yet told it is alive. This is the
    /// narrowest placement that prevents that; nothing is dropped, and traffic
    /// queued ahead still resolves to the incumbent, which is the truthful answer
    /// for a message enqueued while the incumbent was the service.
    /// PR-05; docs/laws/replacement-laws.md
    ///
    /// THE ORDINARY GRANT IS NOT CONSULTED, and that is a law rather than an
    /// omission: a committed activation is not the coordinator's speech. It is
    /// Loom's own act, authorized by the `LifecycleAuthority` presented here. The
    /// coordinator's id is stamped as the OPERATOR IDENTITY a consumer's lineage
    /// rule needs — who admitted, not who is speaking. Nothing widens for
    /// ordinary messages: `announce_lifecycle` is still a send, still gated,
    /// still grant-checked, and an ordinary `zen.Activated` from a weave still
    /// needs the grant and still carries no attestation.
    /// PR-08, LIFE-05; docs/reference/prepared-replacement.md#admission
    ///
    /// Refuses — queueing nothing and changing NOTHING — on any failed
    /// precondition, including a candidate that cannot receive the activation.
    /// THE OWNER MUST STILL BE THE OWNER, here and again at dispatch: a trusted
    /// host caller holding a perfectly good lifecycle authority still cannot admit
    /// a candidate whose coordinator died and revived, was reloaded, or was
    /// removed, because the preparation belonged to a LIFE and that life is over.
    /// The activation's sender-life stamp is taken from the VERIFIED owner record
    /// rather than a fresh lookup, so it can never describe a successor (PR-03).
    AdmitResult admit_candidate(WeaveId candidate, WeaveId incumbent, const std::string& role,
                                const LifecycleAuthority& authority, Message activation,
                                std::int64_t sequence);

    // ---- Prepared replacement ------------------------------------------------
    //
    //     A replacement transaction belongs to exact lives, advances through one
    //     finite state machine, and either commits once or disappears without
    //     disturbing the incumbent.
    //
    // THE TRANSACTION LIVES HERE because this is the only place that sees EVERY
    // transition that can invalidate a participant. `kill` announces Died and
    // `swap_state` announces Revived, but `unregister_weave` announces NOTHING —
    // so a registry living anywhere else and watching events would silently miss
    // permanent removal. Do not move it to an observer: inline notification from
    // the transitions themselves needs no framework and exposes no transaction
    // policy to any weave.
    // PR-02; docs/laws/replacement-laws.md

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

    // ---- the preparation conversation ----------------------------------------
    //
    //     A transaction becomes ready only when the exact sealed candidate
    //     authentically answers the exact preparation request that belongs to
    //     that transaction.
    //
    // There is NO host door that simply asserts readiness. `mark_candidate_ready`
    // was withdrawn: the transition into `Ready` is private, and the acceptance of
    // a real answer is the only expression that reaches it. Do not add a second.
    //
    // The two halves below are deliberately asymmetric. Opening the conversation
    // is HOST authority (the transaction layer already is: begin, tick, commit and
    // abort all live here). Closing it is not authority at all — it is the
    // consumption of the candidate's own attested speech, and every fact it rests
    // on is one the bus stamped rather than one a caller supplied.
    // PR-04; docs/laws/replacement-laws.md

    /// Open this transaction's ONE readiness conversation: deliver `ask` to the
    /// bound candidate AS the bound coordinator, through the sealed
    /// coordinator-only door.
    ///
    /// THE CORRELATION IS LOOM'S, not the caller's — minted here, written over
    /// whatever `ask` carried, and remembered on the transaction as the only
    /// correlation a readiness answer may bear. That is what makes a copied
    /// correlation worthless: the number is not a secret, it is simply not
    /// something a second conversation can also be issued.
    ///
    /// The send is the ordinary gated one, so the coordinator's grant still
    /// governs the shape and the seal still governs the reach. This call adds a
    /// conversation the transaction will recognise; it adds no authority to speak.
    TxnResult ask_candidate_to_prepare(TxnId id, Message ask);

    /// Consume the candidate's answer to that ask — FROM INSIDE ITS DELIVERY.
    ///
    /// Called by the bound coordinator while handling the answer. `id` is the
    /// transaction the ANSWER'S PAYLOAD names, and it is a lookup key, never a
    /// credential: everything that authorizes the transition is read from the
    /// delivery being dispatched (is this an authenticated answer? is the caller
    /// the bound coordinator? is the speaker the bound candidate? is this the
    /// correlation the transaction is waiting for?) and from the registry (are all
    /// four participants still exactly who they were, is the candidate still
    /// sealed by this coordinator, does the incumbent still hold the role?).
    ///
    /// So a payload naming another transaction satisfies neither: the id finds a
    /// record whose expected correlation this delivery does not carry.
    ///
    /// Accepting consumes the conversation, so the same answer cannot be offered
    /// twice — and the candidate's own answer authority was already spent, which
    /// is the second, independent wall.
    TxnResult accept_preparation_answer(TxnId id, PreparationAnswer answer);

    /// Commit: revalidate every exact participant, then delegate to
    /// `admit_candidate` — which remains the SOLE admission mutation. The
    /// transaction layer never moves a role itself.
    ///
    /// IT DOES NOT RETURN `Committed`, and must not be made to. `admit_candidate`
    /// schedules, so on success this transaction becomes `AdmissionPending` and
    /// the caller is told the admission is scheduled — never that it happened.
    /// `Committed` is terminalized inside the admission dispatch, after topology
    /// has actually moved and the activation is guaranteed; if the world drifts
    /// first it terminalizes `Aborted` with `AdmissionRefused` and the incumbent
    /// is still the service. Reporting success here would be an over-report: it
    /// would leave a later delivery check to decide whether the successor was ever
    /// told it was alive.
    /// PR-07; docs/laws/replacement-laws.md
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

    /// The same law, narrowed to EXACTLY one transaction. The store is
    /// per-operator and may hold several results at once; a caller bound to one
    /// transaction — the host authoring handle — must not consume a sibling's.
    /// Same exact-operator binding, same consume-once; only the question narrows.
    bool take_outcome(WeaveId op, TxnId id, TxnOutcome& out);

    /// Mark a Weave dead; it stops receiving deliveries until revived. Emits Died.
    ///
    /// THIS IS THE DEATH TRANSITION, and committing it ends every unfinished
    /// deferred conversation that weave was a party to — before `Died` is
    /// announced, so an observer of the death sees those conversations already
    /// over. Revival, last-known-good fallback and quarantine therefore all begin
    /// from the same place: no inherited answer rights.
    /// ANS-04; docs/laws/answer-authority-laws.md
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
        /// THE DECLARED CLAIM-SET — the Senses this weave says it can
        /// claim. Recorded at registration (and re-read on a code swap, since the
        /// successor's contract is its own), registered so the shapes resolve and
        /// are discoverable, and checked by both claim doors.
        std::vector<std::shared_ptr<const Schema>> claims;
        std::shared_ptr<const Schema> state_schema;
        /// WHY THE THREE LISTS ABOVE STILL RESOLVE (LIFE-08). One live claim on the
        /// union of this weave's accept-set, claim-set and state shape. It is
        /// acquired as a single transaction at registration, re-acquired on a
        /// code swap, and released by the destruction of this record — which is
        /// the whole of the cleanup, and why no removal path can forget it.
        ///
        /// The Registry is told nothing about WeaveId. It counts claims; the
        /// Switchboard owns weave lifetime, and this member is where the two
        /// facts meet.
        SchemaClaimScope schemas;
        Value last_known_good;
        Grant grant;
        std::uint64_t reloads_used = 0;
        bool alive = true;
        /// WHICH CODE this id currently is. A WeaveId is never reused, so it
        /// already distinguishes a swap successor — but a code reload replaces the
        /// code behind a STABLE id, so only this distinguishes that successor from
        /// the incarnation that earned a deferred answer.
        ///
        /// BUMPED IN `swap_state`, AND ONLY THERE. `reload()` is crash REVIVAL of
        /// the same code, so it advances nothing here; what ends the conversations
        /// of a life that ended is `abandon_deferred_for` at `kill`.
        std::uint64_t incarnation = 1;
        /// WHICH LIFE this id currently is — one continuous period of being alive.
        /// Advanced on every dead -> alive transition, and NOWHERE else.
        ///
        /// A SECOND FIELD, DELIBERATELY, because one cannot carry both concepts:
        ///
        ///   code incarnation   changes when the code behind an id is replaced,
        ///                      while the weave never stopped living
        ///   life generation    changes when a life ends and another begins behind
        ///                      the same id, whatever the code
        ///
        /// Collapsing them would make a live code reload invalidate speech already
        /// in the queue — the same weave, still alive, mid-sentence. They are used
        /// where each belongs: deferred answers bind to the incarnation, queued
        /// envelopes to the life.
        /// MSG-03, ANS-02; docs/laws/messaging-laws.md
        std::uint64_t life = 1;
        /// OUTSIDE THE WORLD, AND WHOSE CONVERSATION IT IS (PR-01). Invalid (0) for
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
        /// AUTHORITY A HOST-MINTED ADMINISTRATOR HAS INSTALLED SINCE. Empty for
        /// every weave until a `GrantAuthority` names it, so "a weave gains
        /// nothing by existing" survives delegation existing.
        ///
        /// AN OVERLAY BESIDE `grant`, NOT AN EDIT TO IT. Do not collapse the two:
        ///
        ///   TRUTH        `grant` still says exactly what the host said at
        ///                admission, forever.
        ///   SAFETY       `Grant` also carries containment policy already consumed
        ///                into a child's namespace/cgroup. A mutable `grant` would
        ///                be an API shaped like it could revisit those. This field
        ///                is a `LiveAuthority`, which has no words for them.
        ///   BASELINE     revocation replaces THIS and cannot reach `grant`, so a
        ///                narrow administrator can never strip a subject of what
        ///                the host gave it — structurally, not by remembering to
        ///                check.
        ///
        /// Effective authority is the UNION of the two, computed at every delivery
        /// by `effective_permits*` and by nothing else. Scoped to the WeaveId
        /// exactly as `grant` is, so it survives a code swap and a revival and is
        /// destroyed with this record. VALUE-INITIALIZED here rather than at the
        /// registration site, so the empty floor is a property of the type instead
        /// of a line somebody has to remember to write.
        /// GATE-05; docs/reference/capabilities.md#live-delegation-grant-0
        LiveAuthority delegated{};
        /// WHY THE SHAPES A DELEGATED RULE NAMES STILL RESOLVE (LIFE-08).
        /// docs/laws/lifecycle-laws.md
        ///
        /// A grant's named send rules take a registry claim at registration, so a
        /// producer authorized for a shape keeps that shape meaning something even
        /// after whoever defined it unmounts. A delegated rule authorizes speech in
        /// exactly the same way, so it earns exactly the same claim — otherwise
        /// "granted at mount" and "granted live" would decay differently, and the
        /// second one's failure would read as "I have never heard of that shape".
        ///
        /// Its own scope, separate from `schemas`, because it has its own lifetime:
        /// replacement re-claims the new set BEFORE releasing this one, so a shape
        /// present in both never falls to zero claims mid-swap.
        SchemaClaimScope delegated_schemas{};
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

    /// THE ADMISSION AN ENVELOPE *IS* — present on exactly one envelope per
    /// admission, and on nothing else in the system.
    ///
    /// A FIELD rather than a second queue or a scheduler, because what is being
    /// scheduled is not "an action, and then a message": it is one delivery whose
    /// dispatch happens to move production topology first. One object is what
    /// makes "publicly admitted but never activated" unrepresentable — nothing to
    /// drop, nothing to reorder, no second step that could be refused alone.
    ///
    /// Every participant is a full `ParticipantRef`/`CandidateOwner`, so a queued
    /// admission that outlives its world cannot land on a successor: an id is
    /// never reused, and a life or an incarnation that moved is a different
    /// participant at the same address.
    /// PR-08; docs/laws/replacement-laws.md
    struct PendingAdmission {
        bool present = false;
        ParticipantRef candidate{};
        ParticipantRef incumbent{};
        /// The coordinator exactly as the seal named it — the same three facts
        /// `admit_candidate` verified when it scheduled this.
        CandidateOwner owner{};
        std::string role;
        /// The transaction to terminalize when this lands, if any. Invalid for a
        /// direct admission, which has no transaction to end and reports through
        /// this envelope's ticket instead.
        TxnId txn{};
    };

    struct Envelope {
        Message msg;
        WeaveId target{};
        std::uint64_t seq = 0;
        bool gated = false;  ///< true => Weave-originated; authorize against the sender's grant
        std::string role{};  ///< non-empty => role-targeted; resolved to a holder at delivery
        /// WHICH LIFE AUTHORED THIS. Stamped by the bus at enqueue from the
        /// sender's current life, compared against the sender's life at delivery.
        /// Meaningful only when `gated` — a host/root send belongs to no weave
        /// life, and 0 there means "not weave speech", not a magic life.
        ///
        /// IT LIVES ON THE ENVELOPE AND NOT ON `Message`, which is the whole
        /// point: `Envelope` is private to the Switchboard, with no wire form, no
        /// schema and no constructor a weave can reach — so a hoarded `Message`
        /// re-sent later is stamped afresh with its *re-sender's* life, because the
        /// stamp was never part of what it hoarded (MSG-03).
        std::uint64_t sender_life = 0;
        /// WHICH REQUESTER THIS ANSWER IS FOR. Present only on envelopes queued
        /// through an answer door — `answer_as` and `spend_deferred_as` — and
        /// absent on every ordinary send, because an ordinary message is addressed
        /// to a logical destination and should reach whoever legitimately occupies
        /// it at delivery. An authenticated answer already names one exact
        /// conversation between exact participants, so it names them here (ANS-03).
        ///
        /// LAST, and deliberately: every ordinary enqueue brace-initializes this
        /// struct up to `sender_life` and stops, so an ordinary send cannot carry a
        /// target expectation even by accident. Only `enqueue_answer` sets it.
        AnswerTarget answer_target{};
        /// WHICH PREPARATION ASK THIS IS — or, on an answer, which one it answers.
        /// Invalid on every other envelope in the system.
        ///
        /// IT EXISTS RATHER THAN A CORRELATION COMPARISON because a correlation is
        /// a number a sender chooses. Loom minting one makes it unique, not
        /// unforgeable: any sender may put any number on any message, so believing
        /// a matching correlation would be believing the candidate answered *some*
        /// question numbered N, not that it answered THE ask. Today that gap is
        /// only a coordinator's capacity to confuse itself; the moment a
        /// coordinator is an untrusted loaded weave it becomes the ability to
        /// declare readiness by asking the candidate anything at all.
        ///
        /// It rides `answer_target`'s rails for `answer_target`'s reason: bus-
        /// private, no wire form, nothing to read, copy or replay. Carried from the
        /// ask into the delivery's reply authority (and a deferred record), and
        /// back out through the one answer door.
        /// PR-04, ANS-05; docs/laws/replacement-laws.md
        TxnId preparation{};
        /// WHAT THIS DELIVERY *DOES* BEFORE IT IS DELIVERED. Set by exactly one
        /// caller — `admit_candidate` — and absent on every other envelope, so no
        /// ordinary enqueue path can move production topology even by accident.
        /// LAST, like `answer_target` and for the same reason: every ordinary
        /// enqueue brace-initializes this struct and stops before it.
        PendingAdmission admission{};
        /// WHICH DELIVERY WAS BEING DISPATCHED WHEN THIS ENVELOPE WAS ENQUEUED.
        /// Stamped by the bus from `current_dispatch_seq_`, never by a caller -
        /// the same discipline `sender_life` and `provenance` keep, and for the
        /// same reason: a fact ABOUT a message must not be one the message can
        /// carry in. 0 when nothing was being dispatched.
        std::uint64_t dispatch_parent = 0;
    };

    /// THE REPLY AUTHORITY FOR THE DELIVERY BEING DISPATCHED — bus-owned, one at
    /// a time, and gone the moment that delivery returns.
    ///
    /// The narrowness IS the design, not a corner cut: *a reply authority belongs
    /// to one delivered request between two exact incarnations and authorizes at
    /// most one matching response.* Because it lives and dies with the handler,
    /// every question a longer-lived token would owe an answer to is answered by
    /// construction — it cannot be stored, named, passed, replayed, reused, or
    /// survive either participant's death, reload, or the role changing hands,
    /// because there is nothing to survive. A weave that wants to answer later
    /// answers ordinarily, and its answer is ordinary — which is the truth.
    ///
    /// KEPT HERE RATHER THAN IN THE HANDLER'S STACK FRAME, deliberately. Dispatch
    /// is single-threaded and non-reentrant, so "the current delivery" is a real
    /// singular thing the bus can name. Holding it here means the authority is
    /// checked against WHO IS BEING DISPATCHED, not merely against who is asking
    /// — so a WeaveBus that outlived its delivery (already undefined behaviour)
    /// finds a different delivery's authority and is refused, instead of quietly
    /// borrowing it.
    /// ANS-01; docs/laws/answer-authority-laws.md
    struct ReplyAuthority {
        WeaveId requester{};           ///< the request's stamped sender: the only legal recipient
        std::uint64_t correlation = 0; ///< the request's own: the answer may not choose it
        bool spent = false;            ///< one delivery, one answer
        /// The shape of the request being answered, carried so that a refusal
        /// raised where no message is in hand — deferral overflow — can still say
        /// WHICH conversation it refused instead of naming an unrelated schema.
        std::shared_ptr<const Schema> shape{};
        /// WHO ASKED, captured AT DELIVERY of the request rather than recomputed
        /// when an answer is eventually produced. Recomputing later would mean an
        /// answer silently retargets itself onto whatever the requester has since
        /// become, which is exactly the failure this expectation exists to prevent.
        /// Both answer doors read the requester's identity from here, so there is
        /// one capture point and no drift between the immediate and deferred paths.
        std::uint64_t requester_life = 0;
        std::uint64_t requester_incarnation = 0;
        /// The preparation ask this delivery IS, if it is one — so the
        /// answer it authorizes can carry the same fact back out. Invalid for
        /// every ordinary delivery, which is every delivery but one per
        /// transaction.
        TxnId preparation{};
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
        Ticket office_send(std::string_view as_role, WeaveId target, Message msg) override {
            return sb_.office_send_as(self_, as_role, target, std::move(msg));
        }
        Ticket office_send_to_role(std::string_view as_role, std::string_view to_role,
                                   Message msg) override {
            return sb_.office_send_to_role_as(self_, as_role, to_role, std::move(msg));
        }
        OfficePublication office_publish(std::string_view as_role, Message msg) override {
            return sb_.office_publish_as(self_, as_role, std::move(msg));
        }
        SenseClaimResult claim(Value value) override {
            return sb_.claim_as(self_, std::move(value));
        }
        SenseClaimResult office_claim(std::string_view as_role, Value value) override {
            return sb_.office_claim_as(self_, as_role, std::move(value));
        }
        SenseReading observe(WeaveId author, std::shared_ptr<const Schema> shape) override {
            return sb_.observe_as(self_, author, shape->name(), shape->version());
        }
        SenseReading observe_office(std::string_view role, std::shared_ptr<const Schema> shape) override {
            return sb_.observe_office_as(self_, role, shape->name(), shape->version());
        }
        GrantChange delegate_authority(const GrantAuthority& authority,
                                       LiveAuthority requested) override {
            return sb_.delegate_authority_as(self_, authority, std::move(requested));
        }
        AuthorityView describe_authority(const GrantAuthority& authority) override {
            return sb_.describe_authority_as(self_, authority);
        }

    private:
        Switchboard& sb_;
        WeaveId self_;
    };

    /// `preparation` is set by exactly one caller — `ask_candidate_to_prepare` —
    /// and is the invalid id everywhere else, so the ordinary send paths cannot
    /// mark an envelope as a preparation ask even by accident.
    ///
    /// `provenance` defaults to NONE on all three, which is the clearing rule
    /// itself: an ordinary enqueue overwrites whatever the caller's Message
    /// carried with the default, so only the attesting doors — which pass one
    /// explicitly — ever queue a non-empty fact.
    Ticket enqueue_directed(WeaveId target, Message msg, bool gated,
                            Provenance provenance = Provenance{}, TxnId preparation = TxnId{});
    Ticket enqueue_role(std::string role, Message msg, bool gated,
                        Provenance provenance = Provenance{});
    std::size_t fanout(Message msg, bool gated, Provenance provenance = Provenance{});

    /// THE ONE MEMBERSHIP QUESTION every office-authorship door asks (MSG-07):
    /// does `as_sender` hold `as_role` right now, at the moment it deliberately
    /// asks to speak as it? True iff the role is bound and its holder is exactly
    /// this sender. It reads the same table routing binds — no cache, no copy.
    bool holds_role_now(WeaveId as_sender, std::string_view as_role) const;

    /// Refuse an office-authorship request: visible on the tap and in the
    /// journal as `RoleAuthorshipDenied`, and NOTHING queued. Returns the
    /// invalid ticket every refused authorship door hands back.
    Ticket refuse_office(WeaveId target, WeaveId as_sender, const Message& msg);

    /// The one write path for an attested answer. Refuses — visibly, on the tap
    /// and in the journal — when the caller is not the weave currently being
    /// dispatched, when the request had no one to answer, or when the delivery's
    /// one answer is already spent.
    Ticket answer_as(WeaveId as_sender, Message msg);

    /// The one write path for a lifecycle attestation.
    Ticket announce_as(WeaveId as_sender, const LifecycleAuthority& authority, WeaveId target,
                       Message msg, std::int64_t sequence);

    // ---- live authority administration (GATE-05) -----------------------------

    /// THE ONE WRITE PATH for a subject's delegated live authority.
    ///
    /// `caller` is the weave that presented the capability. It is recorded in the
    /// argument list because a Weaver's diagnostics deserve to know who acted —
    /// and it is DELIBERATELY NOT CONSULTED for authorization. Power here comes
    /// from possessing the capability, never from being anybody in particular;
    /// checking a name as well would quietly invent a second, weaker rule that a
    /// magic role could later satisfy.
    ///
    /// Order of refusal, and it is the informative order rather than the cheap
    /// one: inert capability, then foreign/dead board, then missing subject, then
    /// ceiling. A holder is entitled to know which of its own facts went stale;
    /// none of these reveals anything about a weave it does not already govern.
    GrantChange delegate_authority_as(WeaveId caller, const GrantAuthority& authority,
                                      LiveAuthority requested);

    /// The read half, through the same capability and with the same scope.
    AuthorityView describe_authority_as(WeaveId caller, const GrantAuthority& authority) const;

    // ---- deferred answers (ANS-02) -------------------------------------------

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
        /// The requester's LIFE at the moment it asked. The incarnation
        /// above distinguishes replaced code behind a stable id; this
        /// distinguishes a different life behind it.
        std::uint64_t requester_life = 0;
        WeaveId respondent{};
        std::uint64_t respondent_incarnation = 0;
        std::uint64_t correlation = 0;
        /// The preparation ask this retained right answers, if any.
        /// Carried so a DEFERRED readiness proves exactly what an immediate one
        /// proves — one definition of readiness, not two.
        TxnId preparation{};
    };

    /// The ONE door every authenticated answer leaves by (ANS-03).
    ///
    /// Both `answer_as` and `spend_deferred_as` funnel through here, so the
    /// target expectation, the provenance and the recipient are decided in a
    /// single place. Two nearly-identical enqueues either side of a registry is
    /// exactly the shape that drifts.
    Ticket enqueue_answer(WeaveId to, WeaveId as_sender, Message msg, std::uint64_t correlation,
                          std::uint64_t requester_life, std::uint64_t requester_incarnation,
                          TxnId preparation);

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
    /// THE QUESTION HERE IS THE END OF A LIFE (ANS-04), and staleness cannot ask
    /// it: a killed weave keeps its id AND its incarnation, so nothing about the
    /// record looks stale — yet the participant that earned the right is gone.
    /// Called at `kill` (death, which the isolation supervisor drives on a crashed
    /// child before reviving it from the host-owned snapshot) and at
    /// `unregister_weave` (permanent removal). Being unconditional also makes it
    /// order-independent: unlike the staleness sweep, it does not depend on whether
    /// the record has already been erased from `weaves_`.
    void abandon_deferred_for(WeaveId id);

    /// The life of this transaction's ONE readiness conversation.
    /// Three named states rather than two flags or an inference from a zero
    /// correlation: "nobody has asked yet" and "the answer has been spent" are
    /// different facts, and a transaction that confuses them would accept a
    /// second readiness or refuse the first.
    enum class Conversation : std::uint8_t { NotAsked, Open, Consumed };

    /// One prepared replacement, remembered in full. Nothing here is inferred
    /// from the absence of a field: the state is a state, and the reason is a
    /// reason.
    ///
    /// DISTINCT from the public `loom::PreparedReplacement` — that is a
    /// host-side authoring HANDLE that composes this class's public primitives;
    /// this is the bus-private RECORD those primitives act on. Same concept,
    /// deliberately two scopes: a weave can never see this one.
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
        /// WHAT A LATER ANSWER MUST PROVE IT IS, kept privately here
        /// and nowhere a weave can read it. It is not a secret — a correlation
        /// travels on the wire and the candidate necessarily learns it — which is
        /// exactly why knowing it authorizes nothing. What makes it useful is that
        /// Loom minted it for one conversation and will mint no second one.
        std::uint64_t preparation_correlation = 0;
        Conversation conversation = Conversation::NotAsked;
    };

    /// Snapshot a participant as it is right now.
    ParticipantRef participant(WeaveId id) const;
    /// Is this participant still exactly who it was, and still alive?
    bool still(const ParticipantRef& was) const;

    PreparedReplacement* find_txn(TxnId id);
    const PreparedReplacement* find_txn(TxnId id) const;

    /// THE ONE STATE TRANSITION INTO `Ready`, and it is private — reachable from
    /// exactly one expression in the system: the acceptance of an authenticated
    /// answer that has already proven whose it is. Keep it that way.
    ///
    /// The checks it still makes are the ones about the WORLD (is the candidate
    /// still sealed to this coordinator, is the incumbent still the role holder);
    /// the checks about the SPEAKER live in the caller, where the delivery is.
    /// PR-04; docs/laws/replacement-laws.md
    TxnResult accept_authenticated_readiness(PreparedReplacement& txn);

    /// Did the transaction this id names simply end, rather than never exist?
    /// Ids are minted monotonically, so one below the next is one this Loom
    /// issued — which is the whole difference between "too late" and "no such
    /// thing". Says nothing about WHICH transaction, and confers nothing.
    TxnReason vanished_transaction_reason(TxnId id) const;

    /// End a transaction, record its outcome for its operator, and free the slot.
    void finish_txn(PreparedReplacement& txn, TxnState state, TxnReason reason);

    /// EVERY TRANSITION THAT CAN INVALIDATE A PARTICIPANT CALLS THIS, inline.
    /// Aborts only the transactions that BIND `changed` and whose captured facts
    /// no longer hold — never the whole registry, which would make one weave's
    /// death everybody's problem.
    void invalidate_transactions_for(WeaveId changed);

    /// Advance `rec`'s life generation iff it is currently dead — i.e. iff the
    /// caller is about to bring it back. Called by every revival path before it
    /// marks the record alive.
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

    /// Dispatch at most `budget` deliveries, counting work enqueued mid-turn.
    /// PRIVATE, and deliberately so: as public API it asked a host to size its
    /// turn against a producer rate it cannot know, and the Rule Garden showed
    /// that number is unpickable in practice. `pump_pending()` is the only
    /// bounded turn Loom offers; this is just how it counts.
    std::size_t dispatch_at_most(std::size_t budget);

    /// DISPATCH AN ADMISSION — the whole of it, in one queue turn.
    ///
    /// Revalidate; prove the activation deliverable by admitting it through the
    /// candidate's own gate; move the topology; terminalize the transaction; hand
    /// the candidate its activation. Nothing runs between any two of those, so
    /// there is no observable committed topology in which the candidate has not
    /// been told.
    ///
    /// It is NOT reentrant delivery: this is called from `deliver_one` at the top
    /// of a queue turn, exactly like an ordinary envelope, and it invokes exactly
    /// one handler — the candidate's — as its own delivery.
    ///
    /// A refusal is recorded and emitted like any other refused delivery, with
    /// `AdmissionRevoked`, and changes nothing at all.
    /// PR-08; docs/laws/replacement-laws.md
    void deliver_admission(Envelope env);

    /// Can `candidate` receive exactly this activation? Answers the recipient's
    /// half of the contract — the accept-set door and the gate — and hands back
    /// the admitted payload, so a caller that is about to deliver it does not gate
    /// it twice. `admit()` consumes its candidate, so this takes the Value by
    /// value and the caller ends up with the trusted result or with nothing.
    std::optional<Value> activation_deliverable(const WeaveRecord& candidate,
                                                Value payload) const;

    /// Everything an admission requires of the WORLD, asked identically when the
    /// admission is scheduled and again when it is dispatched. One function, so
    /// the two moments cannot drift apart.
    AdmitRefusal admission_blocked(const ParticipantRef& candidate,
                                   const ParticipantRef& incumbent, const CandidateOwner& owner,
                                   const std::string& role) const;

    /// THE ONE ADMISSION PRIMITIVE, plus the one fact the public door has no
    /// business knowing: which transaction, if any, this admission is ending.
    ///
    /// `admit_candidate` is this with no transaction; `commit_prepared_replacement`
    /// is this with one. THERE IS NO SECOND PATH AND NO SECOND SET OF CHECKS —
    /// which is the only thing keeping direct and transaction admission from
    /// diverging. A fix applied to one of them alone leaves the other reachable.
    AdmitResult schedule_admission(WeaveId candidate, WeaveId incumbent, const std::string& role,
                                   const LifecycleAuthority& authority, Message activation,
                                   std::int64_t sequence, TxnId txn);

    void emit(const BusEvent& event);
    void record(std::uint64_t seq, Disposition disposition, const Refusal& refusal);

    // ---- the latest-claim repository ----------------------------------------
    //
    // TWO KEY SPACES, DELIBERATELY. A personal claim is keyed by the claimant's
    // WeaveId; an office claim is keyed by the role name. They are separate maps
    // so a personal claim cannot be reached through an office key and cannot be
    // promoted into one — law 5 ("holding the office is not claiming as the
    // office") is therefore structural, not a check somebody could forget.
    //
    // LATEST, NOT HISTORY. One record per key; a new claim REPLACES the value in
    // place and bumps its revision. Nothing accumulates. Historical logging is
    // the tap's job and always was.
    struct ClaimRecord {
        Value value;
        WeaveId author{};
        std::uint64_t author_life = 0;
        std::uint64_t author_incarnation = 0;
        std::uint64_t revision = 0;
    };
    /// key: (claimant id, shape name, shape version)
    using PersonalKey = std::tuple<std::uint64_t, std::string, std::uint32_t>;
    /// key: (role, shape name, shape version)
    using OfficeKey = std::tuple<std::string, std::string, std::uint32_t>;

    std::map<PersonalKey, ClaimRecord> personal_claims_;
    std::map<OfficeKey, ClaimRecord> office_claims_;

    /// The declared claim-set's entry for this shape, or nullptr. The one place
    /// the question is asked, so the claim door and discovery cannot drift — and
    /// it hands back the schema itself, so the claim door needs no second lookup
    /// to find the door it already matched.
    static const std::shared_ptr<const Schema>* declared_claim(const WeaveRecord& rec,
                                                               std::string_view name,
                                                               std::uint32_t version);
    /// Does this weave's declared claim-set contain the shape?
    bool declares_claim(const WeaveRecord& rec, std::string_view name,
                        std::uint32_t version) const;
    /// A prepared claim and its verdict, so the two claim doors share every check
    /// and differ only in which map they write. `record` is engaged iff the
    /// verdict accepted — there is no half-made claim to misread.
    struct MadeClaim {
        std::optional<ClaimRecord> record;
        SenseClaimResult result;
    };
    /// Everything both claim doors do identically: check the declaration, gate
    /// the value, and stamp the next revision. Writes nothing.
    MadeClaim make_claim(const WeaveRecord& rec, Value value, std::uint64_t previous_revision);
    /// Fill the authorship of a reading from a stored record, resolving the two
    /// "is that still true?" questions AT READ TIME (never stored, so they cannot
    /// go stale in the repository itself).
    SenseAuthorship authorship_of(const ClaimRecord& rec, const std::string& office,
                                  const std::string& name, std::uint32_t version) const;
    /// DROP EVERY CLAIM UNDER KEYS THAT STOPPED MEANING ANYTHING. Called where a
    /// weave is removed (its personal keys) and where a role becomes unheld (its
    /// office keys). A replacement's admission overwrites the role holder IN
    /// PLACE and never passes through unheld, so this never fires during one —
    /// which is exactly why the predecessor's office claim survives it, stamped
    /// stale rather than deleted or relabelled.
    void forget_personal_claims(WeaveId id);
    void forget_office_claims(const std::string& role);

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

    /// THE TAP LIST, HELD BY SHARED POINTER — and the indirection buys exactly one
    /// thing, a lifetime. An observer may remove itself while it is being
    /// notified, and erasing the vector element would destroy the `std::function`
    /// whose body is still running. `emit()` holds a strong reference for the
    /// length of each call, so the callable outlives the registration that named
    /// it. One allocation per `add_observer` — a registration, not a hot path — in
    /// exchange for none per notification.
    /// MSG-11; docs/laws/messaging-laws.md
    std::vector<std::pair<ObserverId, std::shared_ptr<Observer>>> observers_;
    ObserverId next_observer_id_ = 1;

    /// Is this id still registered? Asked between notifications so a removal made
    /// during an event takes effect within that event.
    bool observer_registered(ObserverId id) const noexcept;

    /// Mint the capability that lets trusted infrastructure attach Loom's
    /// lifecycle attestation.
    ///
    /// PRIVATE AND NON-STATIC, and both halves are load-bearing — DO NOT RELAX
    /// EITHER. Non-static means minting requires the Switchboard ITSELF, the
    /// host's own object, which a weave never holds (it is handed a `Bus&`, with
    /// no such member). Private means even code holding a Switchboard must come
    /// through the one named host-wiring function below rather than helping
    /// itself. As a public static — which it once was — any weave could write
    /// `Switchboard::lifecycle_authority()` from anywhere, with no instance and no
    /// host involvement, and manufacture a lifecycle fact for another incarnation;
    /// a private constructor behind a reachable factory protects nothing.
    ///
    /// The authority also carries WHICH board issued it. Minting from any board
    /// stays legal — anyone may own a Switchboard — but the result is spendable
    /// only through the board it names.
    /// LIFE-04; docs/laws/lifecycle-laws.md
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

    /// Mint the right to administer ONE subject's delegated live authority, up to
    /// `ceiling` and no further (GATE-05). Private, behind the one host-wiring
    /// function below, for the reason lifecycle minting is: a weave holds a `Bus&`
    /// and there is no `Switchboard&` in its hands to reach this with.
    ///
    /// The subject is NOT required to exist yet, and that is not laxity: a
    /// capability must be checked against the live registry at every use anyway
    /// (a subject can die at any time), so validating once at mint would add a
    /// second, weaker check whose passing means nothing later. One check, at the
    /// moment it decides something.
    GrantAuthority grant_authority(WeaveId subject, LiveAuthority ceiling) {
        return GrantAuthority{identity_, subject, std::move(ceiling)};
    }

    /// Was this administration capability issued by THIS Loom, and is that Loom
    /// still alive? The same question `issued_here(const LifecycleAuthority&)`
    /// asks, for the same reason and with the same answer for a dead issuer:
    /// every Loom is its own authority domain, and an authority minted from a
    /// decoy board is entirely real — somewhere else.
    bool issued_here(const GrantAuthority& authority) const noexcept {
        const std::shared_ptr<const LoomIdentity> issuer = authority.issuer_.lock();
        return issuer != nullptr && issuer == identity_;
    }

    /// The ONE expression in the system that yields a GrantAuthority, defined in
    /// `zen/host/grant_wiring.hpp` beside its lifecycle counterpart.
    friend GrantAuthority host_grant_authority(Switchboard& bus, WeaveId subject,
                                               LiveAuthority ceiling);

    /// This Loom's own identity — created with the board, destroyed with it, and
    /// shared with nothing except the authorities it issues (weakly). Two boards
    /// alive at once hold two distinct identities; a board that dies takes its
    /// identity's control block with it, so an authority from a dead world can
    /// never be revived by a later board landing on the same address.
    std::shared_ptr<const LoomIdentity> identity_;

    /// WHAT THE DELIVERY BEING DISPATCHED *IS* — the facts Loom stamped on it,
    /// held for the length of the handler and gone when it returns.
    ///
    /// A SECOND STRUCT ALONGSIDE `ReplyAuthority`, deliberately, even though two
    /// of its three fields are today assigned from the same expressions. DO NOT
    /// MERGE THEM: they answer opposite questions. `ReplyAuthority` is *the right
    /// to speak next* and is SPENT by exercising it; this is *what was just heard*
    /// and is spent by nothing. Reading readiness out of a half-consumed reply
    /// authority would silently stop working the day a coordinator answers the
    /// answer.
    struct DeliveryFacts {
        /// Loom's own word that this is THE authorized answer to a request the
        /// recipient sent. Read from the envelope's provenance, which no enqueue
        /// path except the two answer doors can write.
        bool answers_ask = false;
        /// The bus-stamped author. Not `reply_to`, not anything in the payload.
        WeaveId sender{};
        /// The conversation's correlation — Loom's, chosen when the ask was
        /// enqueued and copied onto the answer by `enqueue_answer`. Kept for the
        /// diagnostic and the redundant-but-true check; the fact below is the one
        /// that decides.
        std::uint64_t correlation = 0;
        /// Which preparation ask this answers, if any. Invalid unless this
        /// delivery is the answer to a real `ask_candidate_to_prepare`.
        TxnId preparation{};
    };

    bool in_dispatch_ = false;
    bool stop_requested_ = false;
    /// The delivery currently being dispatched, and its one reply authority.
    /// Meaningful only inside deliver_one's call to handle(); `current_target_`
    /// invalid means no delivery is live, so nobody may answer.
    WeaveId current_target_{};
    /// THE SEQ OF THE DELIVERY BEING DISPATCHED - the number `current_target_`
    /// is the recipient of. Set at the same statement, cleared by the same
    /// guard, and read by every enqueue path so a message authored inside a
    /// handler carries the delivery it was authored from. 0 outside a dispatch.
    std::uint64_t current_dispatch_seq_ = 0;
    /// ...AND WHETHER THAT DELIVERY'S HANDLER REPORTED FAILURE ACROSS THE ABI
    /// SEAM. Only `note_handler_failure()` sets it; `deliver_one` clears it
    /// before every handler call and reads it after. A native handler needs no
    /// such bit - it throws, and the catch is the report.
    bool handler_reported_failure_ = false;
    ReplyAuthority authority_{};
    DeliveryFacts delivery_{};

    // ---- scoped bookkeeping across a callback Loom did not write --------------
    //
    // Loom calls two kinds of foreign code: a native `Weave::handle`, and a host
    // observer. Either may throw, and the exception is the HOST'S to handle —
    // Loom neither swallows it, translates it into a refusal, nor terminates.
    // What Loom owes is that its own temporary state is restored before the
    // exception leaves, so an escaped throw costs the host that one delivery and
    // not the bus.
    //
    // GUARDS RATHER THAN AN ASSIGNMENT AFTER THE CALL, because "after the call" is
    // only one of the exit paths. `stop_requested_` deliberately has no guard: it
    // is re-initialised at the top of every dispatch turn, so it cannot carry
    // anything across one.
    // MSG-10; docs/laws/messaging-laws.md

    /// Hold `in_dispatch_` for the length of a dispatch turn. Restores the value
    /// it found rather than a constant, so the guard states the invariant it
    /// keeps ("the turn leaves dispatch as it found it") instead of a fact about
    /// today's only caller.
    class DispatchGuard {
    public:
        explicit DispatchGuard(Switchboard& sb) noexcept : sb_(sb), was_(sb.in_dispatch_) {
            sb_.in_dispatch_ = true;
        }
        ~DispatchGuard() { sb_.in_dispatch_ = was_; }
        DispatchGuard(const DispatchGuard&) = delete;
        DispatchGuard& operator=(const DispatchGuard&) = delete;

    private:
        Switchboard& sb_;
        bool was_;
    };

    /// Hold the ambient delivery context for the length of ONE handler call.
    ///
    /// It clears rather than restores, because that is the truth being kept: a
    /// delivery's answer authority and delivery facts belong to that delivery
    /// alone, and dispatch is non-reentrant, so there is never an outer delivery
    /// to hand them back to. Each site still ASSIGNS the fields itself — the
    /// ordinary path mints a reply authority, an admission deliberately mints
    /// none — and the guard is only responsible for the end.
    class DeliveryScope {
    public:
        explicit DeliveryScope(Switchboard& sb) noexcept : sb_(sb) {}
        ~DeliveryScope() {
            sb_.current_target_ = WeaveId{};
            sb_.current_dispatch_seq_ = 0;
            sb_.authority_ = ReplyAuthority{};
            sb_.delivery_ = DeliveryFacts{};
        }
        DeliveryScope(const DeliveryScope&) = delete;
        DeliveryScope& operator=(const DeliveryScope&) = delete;

    private:
        Switchboard& sb_;
    };
    std::vector<DeferredRecord> deferred_;      ///< bounded; see kMaxDeferredAnswers
    /// Bounded, and small. Slots are reclaimed the moment a transaction ends.
    std::vector<PreparedReplacement> txns_;
    std::vector<TxnOutcome> outcomes_;      ///< bounded; oldest dropped
    std::vector<ParticipantRef> outcome_of_; ///< whose outcome each one is
    std::uint64_t next_txn_id_ = 1;
    /// The correlation Loom puts on a preparation ask. Monotonic, so no two
    /// preparation conversations ever share one — which is what the correlation
    /// term is for. It is NOT what makes an answer authentic (a sender may write
    /// any correlation on any message); `Envelope::preparation` is. Kept separate
    /// from the delivery seq so a correlation never doubles as a queue position.
    std::uint64_t next_preparation_correlation_ = 1;

    /// Monotonic; a token is never reused. DELIBERATELY UNGUARDED against
    /// exhaustion, unlike the activation sequence, and the difference is the
    /// reason: that one is PERSISTED and revived from state bytes, so a caller can
    /// hand it a large value and it must refuse rather than wrap. This one is
    /// process-local, never serialized, never revived, and advances by exactly one
    /// per deferral — so 2^64 is not a number an adversary can approach, only one a
    /// process can outlive by any measure.
    std::uint64_t next_deferred_token_ = 1;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_SWITCHBOARD_HPP
