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

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace loom {

/// Why a delivery (or a revival) was refused. `GateRefused` carries the gate's `Error`; the
/// others are the bus's own reasons, which the gate never sees. Each points an operator at a
/// different fix, so a new failure gets a new reason rather than a wider old one.
/// MSG-05; docs/laws/messaging-laws.md
/// The order these are decided in: docs/reference/messaging.md#the-envelope-and-the-delivery-order
enum class RefusalReason : std::uint8_t {
    None = 0,
    NoSuchTarget,      ///< directed at a WeaveId that is not registered
    TargetUnavailable, ///< the target is currently dead (awaiting revival)
    NotAccepted,       ///< the target's accept-set has no schema of this (name, version)
    GateRefused,       ///< routing passed, but admit() refused — see `error`
    CapabilityDenied,  ///< the sender's grant does not permit (shape -> target); the gate is
                       ///< never reached. Authorization, not conformance.
    /// A lifecycle or answer authority this Loom did not issue, or one expired, already spent,
    /// or bound to another conversation or incarnation. The sender's grant may be correct: the
    /// grant says whether it may send this shape, this says whether the authority is its to use.
    ForeignAuthority,
    /// A published bound was reached (docs/reference/bounds.md); nothing was forged.
    Exhausted,
    /// The message's author died, was removed, or was revived (a new life behind the same
    /// `WeaveId`) after sending it. The target and the payload are fine.
    /// MSG-03; docs/laws/messaging-laws.md
    SenderLifeEnded,
    /// An authenticated answer arrived for a requester that has since been revived or had its
    /// code replaced: the address is right, and its occupant is not the one that asked.
    /// ANS-03; docs/laws/answer-authority-laws.md
    AnswerTargetChanged,
    /// A sealed candidate tried to speak to anyone but the coordinator preparing it. The mirror
    /// is deliberately not symmetrical: a message aimed at a sealed weave by anyone but its
    /// coordinator is refused as `NoSuchTarget`, so the world cannot learn a candidate exists.
    /// PR-01; docs/laws/replacement-laws.md
    SealedSpeech,
    /// A scheduled admission no longer described the world when it reached the head of the
    /// queue: a participant died, was reloaded or removed, the role moved, the seal changed
    /// hands, or its transaction had ended. Nothing changed: this refuses an admission, never an
    /// activation.
    /// PR-03 (the drift check), PR-07 (the window it can drift in);
    /// docs/laws/replacement-laws.md
    AdmissionRevoked,
    /// A weave asked to speak as an office it does not hold. Refused when the statement is
    /// authored: nothing is queued, and it is not sent as personal speech instead.
    /// MSG-07; docs/laws/messaging-laws.md
    RoleAuthorshipDenied,
    /// A loaded weave sent a (name, version) this Loom cannot resolve, so there is no door to
    /// admit it against. Refused at the library/host seam before anything is queued or any
    /// target resolved; the sender sees a synchronous refusal, never a later dispatch notice.
    /// The event carries the claimed name and version, the sending artifact, and a target only
    /// where one was named.
    /// MSG-08; docs/laws/messaging-laws.md
    /// docs/reference/known-seams.md#rejections-at-the-dynamic-seam
    SeamUnresolved,
    /// The target is held behind a value a joint operation published under one of its own keys
    /// and it could not apply: its `claim_published` hook threw, or returned a non-OK status
    /// across the seam. Nothing is delivered to it until it is reloaded or removed;
    /// `joint_status` and `zen.JointApplied` say so.
    /// SENSE-06; docs/laws/sense-laws.md
    /// docs/reference/joint-publication.md#the-hold-and-diagnostic-access
    ApplicationFailed,
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

/// Thrown by an ordinary `Switchboard::snapshot_bytes` of a weave held behind a published claim
/// it could not apply (SENSE-06). The weave's own exception was rethrown where the showing
/// failed; this is the bus declining to serve that state as a normal snapshot.
/// `SnapshotAccess::Diagnostic` still reads it.
class ApplicationFailedError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The fate of one queued delivery.
struct DeliveryOutcome {
    Disposition disposition = Disposition::Pending;
    Refusal refusal{}; ///< populated iff disposition == Refused
};

/// A fence: one host send, and everything the bus dispatches synchronously because of it (the
/// dispatch ancestry `BusEvent::dispatch_parent` records, and nothing wider). `Open` while any of
/// those is queued or being dispatched; `Settled` once each has been handled or refused. Settled
/// says nothing about answers, deferred work or the rest of the bus, and a fence whose ancestry
/// never stops stays open. Held by the host that opened it; at most `Switchboard::kMaxFences`.
/// docs/reference/messaging.md#fences-when-what-one-send-set-in-motion-has-been-dispatched
struct Fence {
    std::uint64_t value = 0;
    bool valid() const noexcept { return value != 0; }
};

enum class FenceState : std::uint8_t {
    Unknown, ///< never opened here, or already released
    Open,    ///< an envelope it names is queued or being dispatched
    Settled  ///< every envelope it names has been dispatched
};
const char* name_of(FenceState s) noexcept;

/// The delivery a dispatch is in: its seq, the seq of the delivery during which it was queued, and
/// its fence (`Switchboard::current_dispatch`). All zero outside a dispatch.
struct DispatchPosition {
    std::uint64_t seq = 0;
    std::uint64_t parent = 0;
    Fence fence{};
};

/// What an observer is told about: deliveries (`Delivered`, `Refused`, `HandlerFailed`) and
/// lifecycle transitions (`Died`, `Revived`). `HandlerFailed` says only that the handler was
/// entered and did not complete. It carries no reason and the journal records no outcome
/// (MSG-10): a native exception goes to the host unexamined, and a loaded weave's is caught at
/// the ABI boundary, where only a status crosses.
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
    /// Diagnostics, for a weave-originated delivery: the sender's life stamped at enqueue and
    /// its life now. When they differ, the author's life ended while the message waited. Both 0
    /// for a host send, which belongs to no weave life (MSG-03).
    std::uint64_t sender_life = 0;
    std::uint64_t sender_life_now = 0;
    /// Diagnostics, for an authenticated answer: the requester's life and incarnation the
    /// conversation expected, and the requester's now. Zero on every other delivery (ANS-03).
    std::uint64_t expected_requester_life = 0;
    std::uint64_t expected_requester_incarnation = 0;
    std::uint64_t requester_life_now = 0;
    std::uint64_t requester_incarnation_now = 0;
    /// The office this delivery was deliberately authored as, stamped on the envelope; empty
    /// for personal speech. Not the sender's current role: `role_of(sender)` answers that, and
    /// may already differ.
    /// MSG-07; docs/laws/messaging-laws.md
    std::string authored_role{};
    /// The envelope's correlation, as the sender chose it. It identifies a conversation and
    /// authenticates nothing (ANS-05); 0 is both "none stated" and a legal choice.
    std::uint64_t correlation = 0;
    /// The office the sender addressed, resolved to `target` at dispatch; empty for a directed
    /// send and for a publication. On a seam refusal from a role door `target` stays invalid
    /// beside it, so read it as where the message was sent, never as who answered. Not
    /// `authored_role`, the office the speaker spoke as.
    std::string addressed_role{};
    /// The delivery being dispatched when this one was enqueued; 0 for a host send between
    /// turns. Synchronous dispatch ancestry, not causality: what an asynchronous operation
    /// produces names whichever delivery drained it (a timer beat, say), not the request it
    /// belongs to. A correlation or the application's own id says which operation.
    std::uint64_t dispatch_parent = 0;
    /// How long the handler ran, in nanoseconds on the steady clock around the `handle` call; 0
    /// where no handler ran. A duration, never a time of day, and wall time rather than CPU time:
    /// with one dispatching thread, time a handler spends blocked holds up everything else.
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

/// The lifecycle-policy grammar, the one schema the Switchboard itself defines:
/// { max_reloads: Int, revive_from_last_good: Bool }. A Weave's policy() is validated against it,
/// and only these two fields are read.
std::shared_ptr<const Schema> lifecycle_policy_schema();

/// Who owns a sealed candidate: the coordinator's id, life and incarnation, never a `WeaveId`
/// alone. A coordinator revived or replaced at the same id is a different participant and
/// inherits none of its predecessor's conversations, a preparation included.
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

/// Why an admission was refused, returned to the host that asked: admission is a host call,
/// not a delivery, so no tap event carries it.
enum class AdmitRefusal : std::uint8_t {
    None = 0,         ///< scheduled
    ForeignAuthority, ///< the lifecycle authority was not issued by this Loom
    NotACandidate,    ///< missing, dead, or not sealed at all
    OwnerChanged,     ///< the exact coordinator life/incarnation that sealed it is gone
    IncumbentUnfit,   ///< missing, dead, or already sealed
    RoleNotHeld,      ///< the role is empty, or held by somebody other than the incumbent
    /// The candidate cannot receive the committed activation: it does not accept
    /// `zen.Activated`, or this admission's activation does not pass its gate. Asked before
    /// anything moves, so a candidate is never admitted without being told.
    /// PR-08; docs/laws/replacement-laws.md
    CandidateContract,
};

const char* name_of(AdmitRefusal r) noexcept;

/// The result of scheduling an admission; true when scheduled. The call validates, proves the
/// candidate can receive its activation, and queues one envelope that will admit it; it never
/// says the role has moved. That envelope's `ticket` gives the outcome: `Delivered` is admitted
/// and told, `Refused` with `AdmissionRevoked` is that the world drifted and nothing changed. A
/// transaction reads the same fact from its terminal outcome.
/// PR-07; docs/laws/replacement-laws.md
struct AdmitResult {
    bool scheduled = false;
    AdmitRefusal why = AdmitRefusal::None;
    /// The admission's activation delivery; invalid on a refusal, which queues nothing.
    Ticket ticket{};

    explicit operator bool() const noexcept { return scheduled; }
};

/// Who a transaction is bound to: the participant's id, life and incarnation, as for
/// `CandidateOwner`. A replacement belongs to the exact lives that began it.
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

/// An opaque, host-issued transaction handle: not a correlation, not a participant id, and not
/// authority. Every command also proves the caller is the exact bound participant.
struct TxnId {
    std::uint64_t value = 0;
    bool valid() const noexcept { return value != 0; }
    friend bool operator==(TxnId a, TxnId b) noexcept { return a.value == b.value; }
};

/// A transaction's state. `Committed` and `Aborted` are terminal. `AdmissionPending` is the
/// real interval after commit schedules the admission and before it dispatches: the incumbent
/// still serves and the candidate is still sealed, and the transaction can still be aborted.
/// PR-07; docs/laws/replacement-laws.md
enum class TxnState : std::uint8_t { Preparing, Ready, AdmissionPending, Committed, Aborted };

/// Why a transaction ended, or why a command on one was refused: one vocabulary for both.
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
    /// Another active transaction already names this incumbent, or this candidate: two
    /// problems with different fixes, so two reasons (PR-02).
    IncumbentBusy,
    CandidateBusy,
    /// The candidate itself said no, authentically, spending the answer its preparation ask
    /// earned. Preparation ran and refused; nothing failed and nobody vanished.
    CandidateRefused,
    /// Something claiming to be the readiness answer was not: not an authenticated answer, from
    /// the wrong speaker, with another conversation's correlation, or with no preparation
    /// conversation open. One reason for all of these, so a forger is not told which term it
    /// failed. It refuses the command and never ends the transaction.
    InvalidReadiness,
    /// The answer is authentic and its transaction is over: aborted, out of budget, or
    /// committed while the answer was queued. Not `NoSuchTransaction`, which means this Loom
    /// never issued the id.
    LateReadiness,
    /// This transaction's one preparation conversation has already been opened. There is one,
    /// so an answer can never be ambiguous about which ask it answers.
    PreparationAlreadyAsked,
};

/// What an authentically answering candidate said. One door takes both: readiness and refusal
/// differ only in effect, and every authenticity check is the same.
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

/// How a transaction ended, kept until its exact operator takes it.
struct TxnOutcome {
    TxnId id{};
    TxnState state = TxnState::Aborted;
    TxnReason reason = TxnReason::None;
};

/// How a Weave's accept-set is read at delivery. `Listed`, the default: only the `(name,
/// version)` schemas it declares. `AnyRegistered`: any registered shape, gated against that
/// shape's registry schema (an unregistered shape is still refused). A deliberate capability,
/// used by the console to receive replies.
enum class AcceptMode { Listed, AnyRegistered };

/// The in-process message bus: every delivery is gated through Loom's one validator, admit().
/// Dispatch is single-threaded and FIFO: send and publish enqueue, and a dispatch turn
/// (`pump_pending()` or `drain_until_idle()`) delivers. A handler's sends become later
/// deliveries, never reentrant ones. docs/reference/messaging.md
class Switchboard : public Bus {
public:
    Switchboard();
    ~Switchboard() override;

    Switchboard(const Switchboard&) = delete;
    Switchboard& operator=(const Switchboard&) = delete;
    /// Not movable: weaves hold references into a Switchboard, and every authority it issued
    /// is anchored to its identity.
    Switchboard(Switchboard&&) = delete;
    Switchboard& operator=(Switchboard&&) = delete;

    /// Remove a Weave and hand its ownership back, so a host can destroy it and what it holds
    /// (a loaded library, say) in a controlled order. Its role is released. Queued deliveries to
    /// it are refused as `NoSuchTarget`, and queued messages from it as `SenderLifeEnded`: a
    /// message belongs to the life that authored it (MSG-03).
    ///
    /// Its schema claims go with it: a shape it alone named stops resolving, and a shape
    /// another weave also claims survives (LIFE-08).
    ///
    /// `nullptr` means nothing was removed: the id is unknown, or it names the weave whose
    /// callback is running now, since ownership cannot be handed back while Loom is still
    /// running it. Nothing is mutated; retry once that callback exits. Removing a different
    /// weave mid-callback works.
    /// LIFE-06; docs/reference/lifecycle.md#permanent-removal-and-the-active-callback
    std::unique_ptr<Weave> unregister_weave(WeaveId id);

    /// Register a Weave, taking ownership, and return its stable id. Its accepted schemas and
    /// state schema join the bus registry, where every Weave must agree on what a (name, version)
    /// means: a disagreement throws loom::SchemaConflict. Its initial snapshot must conform to its
    /// own state schema (it seeds last-known-good), or std::invalid_argument is thrown.
    ///
    /// Every message the Weave originates is authorized against `grant` at delivery. The default
    /// is the empty grant, so a Weave that sends needs its reach granted here; loom::mount and
    /// the Kernel's admission policy supply one.
    WeaveId register_weave(std::unique_ptr<Weave> weave, Grant grant);
    WeaveId register_weave(std::unique_ptr<Weave> weave); ///< empty grant

    /// As register_weave(weave, grant), and bind the Weave to `role`, a named slot a send may
    /// target instead of a WeaveId (send_to_role, Grant::allow_to_role). A role has one holder:
    /// binding a held role throws std::invalid_argument. The binding survives the holder
    /// reloading and is cleared by unregister_weave.
    WeaveId register_weave(std::unique_ptr<Weave> weave, Grant grant, std::string role);

    /// Register with an accept-mode: `AnyRegistered` accepts any registered shape, gated against
    /// the registry's schema at delivery. A capability distinct from the grant.
    WeaveId register_weave(std::unique_ptr<Weave> weave, Grant grant, AcceptMode accept_mode);

    /// Enqueue a directed delivery to `target`. Returns a Ticket whose outcome is
    /// readable after the delivery is pumped.
    Ticket send(WeaveId target, Message msg) override;

    /// Enqueue a delivery to every alive Weave whose accept-set includes the
    /// payload's (name, version), in registration order. Returns the recipient
    /// count (0 is legal, not an error). Each delivery is independently gated.
    std::size_t publish(Message msg) override;

    /// Send as a specific Weave: `as_sender` is stamped as the sender and the message is
    /// authorized against its grant at delivery, as if it had sent through its own WeaveBus. A
    /// host verb: a bridge uses it to stamp a remote weave's output with the identity of the
    /// connection it arrived on, never with one the payload claims.
    Ticket send_as(WeaveId as_sender, WeaveId target, Message msg);
    std::size_t publish_as(WeaveId as_sender, Message msg);

    /// Role-addressed sends. `send_to_role` is the host's ungated send; `send_as_to_role` stamps
    /// `as_sender` and authorizes against that sender's grant by role at delivery
    /// (Grant::permits_role).
    Ticket send_to_role(std::string_view role, Message msg) override;
    Ticket send_as_to_role(WeaveId as_sender, std::string_view role, Message msg);

    // ---- fences: when what one send set in motion has been dispatched ------------------
    // Host verbs beside `send_as`: the host that injects on somebody's behalf asks when that
    // work has run here. A weave holds no Switchboard and opens no fence.

    /// `send_as`, opening a fence rooted at the envelope it queues; `*fence` receives the
    /// handle. A fenced send made from inside a fenced delivery begins a new fence and is not
    /// counted in the enclosing one. At `kMaxFences` held nothing is queued and the Ticket and
    /// Fence come back invalid; a null `fence` is refused before anything is allocated or queued.
    Ticket send_as_fenced(WeaveId as_sender, WeaveId target, Message msg, Fence* fence);
    /// The role-addressed form, on `send_as_to_role`'s terms.
    Ticket send_as_to_role_fenced(WeaveId as_sender, std::string_view role, Message msg,
                                  Fence* fence);
    /// Open, Settled, or Unknown (never opened here, or released).
    FenceState fence_state(Fence fence) const noexcept;
    /// Forget a fence. Its handle reads Unknown from now on; envelopes it named that are still
    /// queued are dispatched exactly as before and are simply no longer counted. Releasing an
    /// unknown fence is a no-op.
    void release_fence(Fence fence) noexcept;
    /// How many fences are held (open or settled, not yet released).
    std::size_t fences_held() const noexcept { return fences_.size(); }
    /// The most fences one Loom holds at once. A fence is a record a host asks for, so the
    /// bound keeps a host adapter from growing it without limit on a peer's behalf.
    static constexpr std::size_t kMaxFences = 64;

    // ---- deliberate office authorship ---------------------------------------
    // A weave may deliberately author one statement as a role it holds. Membership is verified
    // when the statement is authored, so the role moving while it waits cannot change what it
    // says; the stamped fact is never recomputed. Every other delivery law still applies. A
    // refusal queues nothing and shows on the tap as `RoleAuthorshipDenied`.
    // MSG-07, MSG-04; docs/laws/messaging-laws.md
    // docs/reference/messaging.md#office-authorship-role-authored-provenance

    /// Office-authored direct send. An invalid Ticket means authorship was refused and nothing
    /// was queued; the refusal is on the tap and in the journal.
    Ticket office_send_as(WeaveId as_sender, std::string_view as_role, WeaveId target,
                          Message msg);
    /// Office-authored role-addressed send: `as_role` is the office spoken for, verified now;
    /// `to_role` is the destination, resolved at delivery. Both facts are carried separately.
    Ticket office_send_to_role_as(WeaveId as_sender, std::string_view as_role,
                                  std::string_view to_role, Message msg);
    /// Office-authored publication. "Authorship refused" and "authorized, zero recipients" stay
    /// distinct, and each recipient's delivery is still authorized against the sender's grant.
    OfficePublication office_publish_as(WeaveId as_sender, std::string_view as_role, Message msg);

    /// The Bus office verbs, which the root surface always refuses: a host has no weave identity
    /// and holds no office. A host speaks as a weave through `send_as` and `office_send_as`.
    Ticket office_send(std::string_view as_role, WeaveId target, Message msg) override;
    Ticket office_send_to_role(std::string_view as_role, std::string_view to_role,
                               Message msg) override;
    OfficePublication office_publish(std::string_view as_role, Message msg) override;


    /// Dispatch the work waiting at entry, then return how many deliveries were made: the
    /// ordinary host-loop operation.
    ///
    /// ```cpp
    /// while (running) {
    ///     poll_input();            // the OS, sockets, whatever else this host owns
    ///     bus.pump_pending();      // exactly the backlog that was waiting; control returns
    /// }
    /// ```
    ///
    /// The bound is `pending()` at entry, a count and never a clock: work a handler enqueues
    /// during the call waits for the next turn, so a self-re-arming producer cannot hold the turn
    /// open, and a busy bus still clears its backlog in one turn. It takes no number.
    /// MSG-09; docs/laws/messaging-laws.md
    ///
    /// `stop()` ends the turn early. A call from inside a handler, or on an empty queue,
    /// dispatches nothing and returns 0.
    std::size_t pump_pending();

    /// Dispatch until nothing is queued. Work a handler enqueues during the call belongs to the
    /// call, so this returns only when the participants stop producing, and never while a
    /// perpetual service (a timer that re-arms itself) is loaded. Unbounded by contract (MSG-09).
    /// For a test or script settling before it asserts, a one-shot bootstrap, or a host whose
    /// whole program is the bus and whose exit is `stop()`; a host that wants control back
    /// between turns wants `pump_pending()`. A call from inside a handler does nothing.
    /// docs/reference/messaging.md#two-dispatch-turns-and-the-call-site-says-which
    void drain_until_idle();

    /// End the current turn after the delivery in flight — both operations
    /// honour it, and each reports what it actually did.
    void stop() noexcept { stop_requested_ = true; }

    // ---- Senses --------------------------------------------------------------
    // A Sense is a participant's deliberate, immutable claim of its latest observation, read
    // synchronously and sharing no memory with the claimant. It is visible from the claim call
    // on: with one dispatching thread, nobody else runs before the claiming handler returns.
    // SENSE-01, SENSE-02; docs/laws/sense-laws.md

    /// Claim `value` personally as `claimant`: the host's door; a weave claims through its
    /// WeaveBus. Keyed by (claimant, shape), which no office key reaches.
    SenseClaimResult claim_as(WeaveId claimant, Value value);

    /// Claim `value` as the office `as_role`, verified now (`role_holder(as_role) == claimant`,
    /// as MSG-07 verifies authorship). A claimant that does not hold it is refused
    /// `OfficeNotHeld` and nothing is stored; it is never made a personal claim instead. Keyed
    /// by (role, shape), and recording which weave claimed it.
    SenseClaimResult office_claim_as(WeaveId claimant, std::string_view as_role, Value value);

    /// The latest claim `author` made personally of this shape, gated by `reader`'s grant. The
    /// value is a copy the caller owns.
    SenseReading observe_as(WeaveId reader, WeaveId author, std::string_view shape_name,
                            std::uint32_t shape_version) const;

    /// The latest claim made as the office `role`, gated by `reader`'s grant. After the role
    /// moves this still returns the predecessor's claim, marked
    /// `office_holder_is_current=false`; a reader wanting only the current holder's checks it.
    /// SENSE-03; docs/laws/sense-laws.md
    SenseReading observe_office_as(WeaveId reader, std::string_view role,
                                   std::string_view shape_name,
                                   std::uint32_t shape_version) const;

    /// The host's own ungated observation, as `Switchboard::send` is the ungated send. These
    /// (name, version) overloads would hide `Bus`'s schema-typed ones on a `Switchboard&`; the
    /// `using` declarations keep both reachable.
    using Bus::observe;
    using Bus::observe_office;
    /// And the host's ungated `joint_status(op)` beside the operator's authenticated one.
    using Bus::joint_status;

    SenseReading observe(WeaveId author, std::string_view shape_name,
                         std::uint32_t shape_version) const;
    SenseReading observe_office(std::string_view role, std::string_view shape_name,
                                std::uint32_t shape_version) const;

    /// The claim-set this participant declared, so a consumer can learn what it may claim before
    /// any claim is made. Empty for a weave that declares none.
    std::vector<std::shared_ptr<const Schema>> claimed_schemas(WeaveId id) const;

    /// The emit-set this participant declared at registration (re-read on a code swap): what
    /// each shape means here, and nothing about who may send it, which is the grant's. Empty for
    /// a weave that declares none, and for an unknown id.
    std::vector<std::shared_ptr<const Schema>> emitted_schemas(WeaveId id) const;

    /// How many latest claims are retained now, across both key spaces. Bounded by current keys
    /// (registered weaves times the shapes they declare, plus held roles times shapes): a reload,
    /// a revival or a re-claim replaces the value under its key and adds none.
    std::size_t retained_claim_count() const noexcept {
        return personal_claims_.size() + office_claims_.size();
    }

    // ---- Joint publication of latest claims -----------------------------------
    // The host's half: minting the operator's authority, the `*_as` doors the gated WeaveBus
    // funnels through, and the root views. A weave's verbs are in zen/switchboard/bus.hpp, the
    // account in zen/switchboard/sense.hpp. A commit runs inside the operator's own delivery and
    // calls no participant, gate, observer or I/O while it exchanges. Nothing is durable across
    // a process. SENSE-06, SENSE-07; docs/reference/joint-publication.md

    static constexpr std::size_t kMaxJointOperations = 8;
    static constexpr std::size_t kMaxJointKeys = 4;
    /// The bound on one offered value's serialized size: a joint publication carries facts, not
    /// documents.
    static constexpr std::size_t kMaxJointOfferBytes = 64u * 1024u;

    /// Mint the right to coordinate joint publications for one operator weave over claimants
    /// holding one of `ceiling_roles`. A host door: a weave holds a Bus& and cannot reach it.
    ///
    /// Bound to the operator's current life and incarnation, and expired when either changes; a
    /// successor is authorized only by the host minting again. For an operator that is not
    /// registered or is dead the result is not `valid()`, and is refused `ForeignAuthority` if
    /// presented.
    JointAuthority mint_joint_authority(WeaveId operator_id,
                                        std::vector<std::string> ceiling_roles) const;
    bool issued_here(const JointAuthority& authority) const noexcept {
        const std::shared_ptr<const LoomIdentity> issuer = authority.issuer_.lock();
        return issuer != nullptr && issuer == identity_;
    }

    // A commit publishes: every reader sees the new values at once. Each claimant is then
    // shown its value once, before its next delivery or snapshot (`Weave::claim_published`),
    // and what that came to is recorded per key: Applied, Declined, Failed (the claimant is
    // held until reloaded or removed) or Lost. A record is kept until its operator releases it,
    // or the operator's life or incarnation changes; only a released slot is reused.
    // docs/reference/joint-publication.md#the-showing-and-its-three-answers
    // docs/reference/joint-publication.md#the-records-lifetime-and-release

    /// How a snapshot may meet a publication the weave has not been shown.
    enum class SnapshotAccess : std::uint8_t {
        /// Show the weave every pending publication first, and throw for a weave whose
        /// application failed. The host's ordinary read.
        Ordinary,
        /// The reload's read: pending publications are shown first, but a failed application
        /// does not refuse the read. The successor revived from these bytes is shown the value
        /// again (`swap_state` returns Failed to Pending for it); the failed hook is never
        /// retried on the incarnation that failed.
        Reload,
        /// Run nothing, change nothing: the weave's state as it is, for a diagnostic
        /// or a repair tool that explicitly wants what a held weave holds. The
        /// records stay exactly as they were.
        Diagnostic,
    };

    /// The gated doors the WeaveBus funnels through; the caller is the stamped speaker of a live
    /// delivery, never a payload field.
    JointResult offer_claim_as(WeaveId claimant, std::uint64_t op, Value value);
    JointBegin begin_joint_as(WeaveId caller, const JointAuthority& authority,
                              std::vector<ClaimKey> keys);
    JointResult commit_joint_as(WeaveId caller, const JointAuthority& authority,
                                std::uint64_t op);
    JointResult cancel_joint_as(WeaveId caller, const JointAuthority& authority,
                                std::uint64_t op);
    JointStatus joint_status_as(WeaveId caller, const JointAuthority& authority,
                                std::uint64_t op);
    /// The operator retires a terminal record it has consumed (see `Bus::release_joint`).
    JointResult release_joint_as(WeaveId caller, const JointAuthority& authority,
                                 std::uint64_t op);

    /// The host's ungated views: one operation's state, how many are live, how many records
    /// are held (live or unreleased, which is what counts against `kMaxJointOperations`), and how
    /// many offered bytes are held.
    JointStatus joint_status(std::uint64_t op) const noexcept;
    std::size_t joint_pending() const noexcept;
    std::size_t joint_records() const noexcept;
    std::size_t joint_retained_bytes() const noexcept;
    /// Whether this weave has a joint publication of one of its keys it has not yet been shown
    /// (Pending). A completed showing clears it.
    bool has_unobserved_publication(WeaveId id) const noexcept;
    /// Whether this weave is held: shown a published value, its hook did not complete.
    /// Deliveries to it are refused `ApplicationFailed` and `snapshot_bytes(id)` throws until it
    /// is reloaded or removed; `SnapshotAccess::Diagnostic` reads it as it is.
    bool has_failed_application(WeaveId id) const noexcept;
    /// The worst application state over this weave's keys: Failed, then Lost, Declined, Pending,
    /// Applied, None. What a host asks before deciding to repair.
    JointApplication application_of(WeaveId id) const noexcept;

    /// Record a rejection at a boundary this Loom owns that the bus never saw: the dynamic seam
    /// admits a loaded weave's bytes before routing, so when that fails nothing is queued. A
    /// diagnostic and nothing else: a seq, a journal slot and a tap event, as `refuse_now` makes,
    /// and no answer, delivery or later notice. It carries the claimed (name, version); `target`
    /// stays invalid wherever the emission named none, since a publication has no target and a
    /// role was never resolved. `addressed_role` is the role a role door was handed, reported as
    /// the address named and never as a resolution; empty on every other door.
    /// MSG-08; docs/laws/messaging-laws.md
    ///
    /// Called by the Kernel's seam callbacks, which hold the host's `Switchboard&`.
    void note_seam_refusal(WeaveId sender, WeaveId target, std::string_view claimed_name,
                           std::uint32_t claimed_version, const Refusal& refusal,
                           std::string_view addressed_role = {});

    /// The delivery being dispatched did not complete normally, said by the Kernel's
    /// `HostAdapter`, whose loaded weave reports failure as an ABI status because a library's
    /// exceptions never cross the seam (docs/reference/dynamic-abi.md). It refuses and records
    /// nothing: this delivery emits `HandlerFailed` instead of `Delivered` and no journal outcome,
    /// as a native throw does. Outside a dispatch it does nothing.
    /// MSG-10; docs/laws/messaging-laws.md
    void note_handler_failure() noexcept;

    /// The fate of an issued Ticket: Pending until dispatched. A Ticket older than the journal's
    /// window, or never issued, reads Pending. The window can roll within one turn, so a
    /// consumer that sends more than `kJournalCapacity` envelopes before reading an outcome can
    /// see Pending for one that was decided; the relay and the console stay well inside it.
    DeliveryOutcome outcome(Ticket t) const;

    /// The journal is a ring of the last `kJournalCapacity` delivery outcomes, so a bus that runs
    /// for weeks keeps a bounded footprint.
    static constexpr std::size_t kJournalCapacity = 1024;

    /// How many unfinished deferred conversations one Loom holds at once. A weave asks for this
    /// host-side state, so it is bounded; past it a deferral is refused visibly and the immediate
    /// answer opportunity survives. A record is reclaimed when spent or released, or when either
    /// participant dies or is reloaded.
    static constexpr std::size_t kMaxDeferredAnswers = 64;

    /// Register an observer, told of every delivery and lifecycle event; returns an id for
    /// removal. docs/reference/messaging.md#observation
    ObserverId add_observer(Observer obs);
    void remove_observer(ObserverId id);

    // ---- Lifecycle ----------------------------------------------------------

    /// Serialize the Weave's current snapshot. A weave with a joint-published claim it has not
    /// been shown is shown it first (`Weave::claim_published`), so the bytes agree with what the
    /// bus published. `SnapshotAccess::Ordinary` refuses a weave whose showing fails or that is
    /// held: the weave's own exception for a hook that threw now, `ApplicationFailedError` for one
    /// already held. The other accesses are described on the enum.
    std::string snapshot_bytes(WeaveId id);
    std::string snapshot_bytes(WeaveId id, SnapshotAccess access);

    /// Seal a weave: it becomes a prepared candidate outside the live world, able to converse
    /// only with `coordinator` (PR-01; docs/laws/replacement-laws.md). A host door. Returns false,
    /// changing nothing, if either is missing, the coordinator is dead, the candidate holds a role
    /// (so only a commit moves a role), or the candidate is already sealed (no transfer to a
    /// second owner).
    bool seal_weave(WeaveId candidate, WeaveId coordinator);

    /// Who owns this sealed weave, as the life and incarnation that sealed it; invalid if it is
    /// not sealed.
    CandidateOwner candidate_owner(WeaveId id) const;

    /// Whether this weave is a sealed candidate now; for diagnostics, since routing never asks.
    bool sealed(WeaveId id) const;

    /// Who holds `role` now; the invalid id if nobody. A query for hosts and taps: routing
    /// resolves roles itself at delivery.
    WeaveId role_holder(std::string_view role) const;

    /// The role this weave holds now; empty if none or no such weave. An admission moves a role
    /// with no host call, so read this rather than remembering what was bound at registration.
    std::string role_of(WeaveId id) const;

    /// This participant as it is now: id, life and incarnation (`who` invalid for no such weave).
    /// A snapshot, never a reservation: the weave may die or be reloaded next turn.
    ParticipantRef participant(WeaveId id) const;

    /// Where dispatch is now: the delivery being dispatched, the delivery during which it was
    /// queued, and its fence; all zero outside a dispatch. For host wiring running inside a
    /// delivery that must say which one, such as an observation relay. A read of what the bus
    /// already stamped; it grants nothing.
    DispatchPosition current_dispatch() const noexcept {
        return DispatchPosition{current_dispatch_seq_, current_dispatch_parent_,
                                Fence{current_dispatch_fence_}};
    }

    /// The commit: unseal `candidate` and move `role` from `incumbent` to it, between two
    /// deliveries, so no turn can observe it half-done (MSG-01). Refuses, changing nothing, if the
    /// candidate is not sealed, either weave is missing or dead, or the role is held by anyone
    /// but the incumbent.
    bool commit_candidate(WeaveId candidate, WeaveId incumbent, const std::string& role);

    /// The admission: the commit above, accounting for the incumbent and for activation, and
    /// scheduled rather than performed. Entering the world and being told so are one envelope:
    ///
    ///   at this call      validate; prove the candidate can receive this activation; queue the
    ///                     envelope. The incumbent is still the service.
    ///   at its dispatch   revalidate; admit the activation through the candidate's gate; seal
    ///                     the incumbent, unseal the candidate, move the role, and deliver the
    ///                     activation, with nothing in between.
    ///
    /// The envelope is placed just ahead of the first queued envelope that could reach the
    /// candidate (addressed to the role, or to it directly), so no production message reaches it
    /// before it is told it is alive; traffic queued ahead still reaches the incumbent.
    /// PR-05; docs/laws/replacement-laws.md
    ///
    /// The ordinary grant is not consulted: the activation is Loom's act, authorized by the
    /// `LifecycleAuthority`, and the coordinator's id is stamped as the operator that admitted.
    /// An ordinary `zen.Activated` from a weave still needs a grant and carries no attestation.
    /// PR-08, LIFE-05; docs/reference/prepared-replacement.md#admission
    ///
    /// Refuses, queueing nothing, on any failed precondition. The coordinator that sealed the
    /// candidate must be the same life and incarnation here and at dispatch; the activation's
    /// sender-life stamp comes from that verified record (PR-03).
    AdmitResult admit_candidate(WeaveId candidate, WeaveId incumbent, const std::string& role,
                                const LifecycleAuthority& authority, Message activation,
                                std::int64_t sequence);

    // ---- Prepared replacement ------------------------------------------------
    // A replacement transaction belongs to exact lives, advances through one state machine, and
    // either commits once or ends without disturbing the incumbent. It lives in the Switchboard
    // because only here is every transition that can invalidate a participant seen, removal
    // included, which announces no event.
    // PR-02; docs/laws/replacement-laws.md

    /// How many prepared replacements may be in flight at once: an operator's act, not a
    /// workload.
    static constexpr std::size_t kMaxPreparedReplacements = 8;
    /// How many ended transactions are kept for their operators to collect; the oldest is dropped
    /// beyond it.
    static constexpr std::size_t kMaxTerminalOutcomes = 16;
    /// The largest preparation budget a caller may ask for.
    static constexpr std::uint32_t kMaxPreparationBudget = 1024;

    /// Begin one prepared replacement. Everything is validated before anything is stored, so a
    /// refusal changes nothing and never disturbs the incumbent.
    TxnResult begin_prepared_replacement(WeaveId op, WeaveId coordinator, WeaveId incumbent,
                                         WeaveId candidate, const std::string& role,
                                         std::uint32_t budget);

    /// Spend one unit of the preparation budget: an explicit step, never a clock or a count of
    /// other bus activity. Only while Preparing; at zero the transaction aborts with
    /// `PreparationExhausted`.
    TxnResult tick_preparation(TxnId id);

    // ---- the preparation conversation ----------------------------------------
    // A transaction becomes Ready only when its sealed candidate authentically answers its
    // preparation request; no host door asserts readiness. Opening the conversation is the
    // host's; closing it consumes the candidate's own attested speech, and rests only on facts
    // the bus stamped.
    // PR-04; docs/laws/replacement-laws.md

    /// Open this transaction's one readiness conversation: deliver `ask` to the bound candidate
    /// as the bound coordinator. The correlation is minted here, overwriting `ask`'s, and is the
    /// only one a readiness answer may carry. The send is the ordinary gated one: the coordinator's
    /// grant still governs the shape; this adds a conversation, not authority.
    TxnResult ask_candidate_to_prepare(TxnId id, Message ask);

    /// Consume the candidate's answer to that ask, called by the bound coordinator from inside
    /// the answer's delivery. `id` is the transaction the answer's payload names: a lookup key,
    /// never a credential. What authorizes the transition is read from the delivery (an
    /// authenticated answer, spoken by the bound candidate, to the bound coordinator, with the
    /// awaited correlation) and from the registry (all four participants unchanged, the candidate
    /// still sealed by this coordinator, the role still the incumbent's). Accepting consumes the
    /// conversation, and the candidate's answer authority is already spent.
    TxnResult accept_preparation_answer(TxnId id, PreparationAnswer answer);

    /// Commit: revalidate every participant, then call `admit_candidate`, the only door that
    /// moves a role. On success the transaction is `AdmissionPending`, never `Committed`: the
    /// admission is scheduled, and becomes `Committed` inside its dispatch once the role has moved
    /// and the activation is delivered, or `Aborted` with `AdmissionRefused` if the world drifted.
    /// PR-07; docs/laws/replacement-laws.md
    /// PR-07; docs/laws/replacement-laws.md
    TxnResult commit_prepared_replacement(TxnId id, const LifecycleAuthority& authority,
                                          Message activation, std::int64_t sequence);

    /// Abort from any nonterminal state, by the exact operator.
    TxnResult abort_prepared_replacement(TxnId id, WeaveId op);

    /// Diagnostics: the state of a transaction, and how many slots are in use.
    TxnState transaction_state(TxnId id) const;
    bool transaction_active(TxnId id) const;
    std::size_t active_transactions() const noexcept;

    /// Take the terminal outcome of a transaction this weave began: once, and only by the exact
    /// operator life and incarnation that began it.
    bool take_outcome(WeaveId op, TxnOutcome& out);

    /// As above, narrowed to one transaction, so a caller bound to one (the host authoring
    /// handle) cannot consume a sibling's result.
    bool take_outcome(WeaveId op, TxnId id, TxnOutcome& out);

    /// Mark a Weave dead; it receives nothing until revived. Emits Died. Every deferred
    /// conversation it was a party to ends first, so an observer of `Died` sees them over, and
    /// revival starts with no inherited answer rights.
    /// ANS-04; docs/laws/answer-authority-laws.md
    void kill(WeaveId id);

    /// Revive a Weave from candidate bytes after a crash: parse, then admit against the state
    /// schema. On success, revive and refresh last-known-good; on refusal, the Weave's policy()
    /// decides whether to fall back to last-known-good. Budgeted by the policy's max_reloads, so a
    /// Weave that keeps crashing cannot revive forever. Emits Revived, or Refused.
    ReviveOutcome reload(WeaveId id, std::string_view candidate_bytes);

    /// Swap state on purpose (a code reload, not crash recovery): gate the candidate against the
    /// state schema, revive, refresh last-known-good and mark alive. Spends no reload budget, so a
    /// Weave's crash allowance never blocks a deliberate swap. A gate refusal is clean: there is
    /// no fallback to last-known-good. Emits Revived, or Refused.
    ReviveOutcome swap_state(WeaveId id, std::string_view candidate_bytes);

    // ---- Queries ----------------------------------------------------------

    std::vector<WeaveId> list_weaves() const;
    std::vector<std::shared_ptr<const Schema>> accepted_schemas(WeaveId id) const;

    /// Resolve a registered schema by identity, across every Weave's accept-set and state schema;
    /// nullptr if none is registered. For a host gating a value whose schema it does not hold,
    /// such as a message emitted across the library boundary.
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
        /// The declared claim-set, re-read on a code swap and checked by both claim doors.
        std::vector<std::shared_ptr<const Schema>> claims;
        /// The declared emit-set, re-read on a code swap and registered so its shapes resolve
        /// and conflict loudly. No send consults it: authority is `grant` and `delegated`.
        std::vector<std::shared_ptr<const Schema>> emits;
        std::shared_ptr<const Schema> state_schema;
        /// The one registry claim on the accept-set, claim-set, emit-set and state shape with
        /// everything they nest, taken at registration, retaken on a code swap, and released
        /// when this record is destroyed (LIFE-08).
        SchemaClaimScope schemas;
        Value last_known_good;
        Grant grant;
        std::uint64_t reloads_used = 0;
        bool alive = true;
        /// Which code this id is; advanced by `swap_state` only. A code reload keeps the id, so
        /// this tells the successor from the incarnation that earned a deferred answer.
        std::uint64_t incarnation = 1;
        /// Which life this id is: one continuous period of being alive, advanced on every dead
        /// to alive transition and nowhere else. Separate from `incarnation` so a live code
        /// reload does not invalidate speech already queued: queued envelopes bind to the life,
        /// deferred answers to the incarnation.
        /// MSG-03, ANS-02; docs/laws/messaging-laws.md
        std::uint64_t life = 1;
        /// The coordinator that sealed this weave as a prepared candidate (PR-01); invalid for
        /// an ordinary weave. A candidate receives no publication, ordinary send or role traffic
        /// and speaks only to its coordinator; the routing paths refuse on this field.
        CandidateOwner sealed_by{};
        std::string role{}; ///< the role this Weave holds (empty if none); see roles_
        bool accepts_any = false; ///< AcceptMode::AnyRegistered — accept any registered shape (gated)
        /// Authority a host-minted administrator has installed; empty until a `GrantAuthority`
        /// names this weave. An overlay beside `grant`, never an edit of it: `grant` keeps what
        /// the host said at admission, revocation replaces only this, and a `LiveAuthority` has
        /// no words for the containment already applied to a child. Effective authority is the
        /// union, computed at every delivery by `effective_permits*`. Survives a code swap and a
        /// revival; destroyed with the record.
        /// GATE-05; docs/reference/capabilities.md#live-delegation
        /// GATE-05; docs/reference/capabilities.md#live-delegation
        LiveAuthority delegated{};
        /// The registry claim on the shapes delegated rules name, as a grant's rules have, so a
        /// shape granted live keeps resolving like one granted at mount (LIFE-08). Its own scope:
        /// a replacement claims the new set before releasing the old.
        /// docs/laws/lifecycle-laws.md
        SchemaClaimScope delegated_schemas{};
    };

    /// What an authenticated answer expects at its destination; `present` false for an ordinary
    /// message.
    struct AnswerTarget {
        bool present = false;
        std::uint64_t life = 0;
        std::uint64_t incarnation = 0;
    };

    /// The admission an envelope performs, present on exactly one envelope per admission. One
    /// delivery that moves the role before it is handled, so "admitted but never activated"
    /// cannot be represented. Every participant is named by life and incarnation, so a queued
    /// admission cannot land on a successor.
    /// PR-08; docs/laws/replacement-laws.md
    struct PendingAdmission {
        bool present = false;
        ParticipantRef candidate{};
        ParticipantRef incumbent{};
        /// The coordinator as the seal named it, as `admit_candidate` verified it.
        CandidateOwner owner{};
        std::string role;
        /// The transaction to end when this lands; invalid for a direct admission, which reports
        /// through the envelope's ticket.
        TxnId txn{};
    };

    struct Envelope {
        Message msg;
        WeaveId target{};
        std::uint64_t seq = 0;
        bool gated = false;  ///< true => Weave-originated; authorize against the sender's grant
        std::string role{};  ///< non-empty => role-targeted; resolved to a holder at delivery
        /// The sender's life, stamped at enqueue and compared with its life at delivery; only
        /// when `gated`, and 0 for a host send. On the private envelope, not on `Message`, so a
        /// hoarded Message re-sent later carries its re-sender's life (MSG-03).
        std::uint64_t sender_life = 0;
        /// The requester this answer is for, set only by the answer doors: an ordinary message
        /// reaches whoever occupies its destination at delivery, an answer only the exact
        /// requester (ANS-03). Declared after `sender_life`, where every ordinary enqueue's brace
        /// initializer stops, so an ordinary send cannot carry one.
        AnswerTarget answer_target{};
        /// The preparation ask this is, or on an answer the ask it answers; invalid elsewhere. A
        /// correlation is a number any sender may choose, so this bus-private fact is what shows a
        /// candidate answered the ask rather than some question with the same number.
        /// PR-04, ANS-05; docs/laws/replacement-laws.md
        TxnId preparation{};
        /// The admission this delivery performs before it is delivered; set only by
        /// `admit_candidate`. After `answer_target` so an ordinary enqueue never sets it.
        PendingAdmission admission{};
        /// The delivery being dispatched when this was enqueued; 0 if none. Stamped by the bus,
        /// so a message cannot carry in a fact about itself.
        std::uint64_t dispatch_parent = 0;
        // Captured at authorship with the refusal opt-in; on a refusal notice, with
        // `sender_life`, it binds the exact recipient.
        std::uint64_t refusal_incarnation = 0;
        /// The fence this envelope belongs to (0 for none): the fence of the delivery it was
        /// queued from, or the one a fenced host send opened. Stamped by the bus.
        std::uint64_t fence = 0;
    };

    /// A fence the bus is counting for a host: how many envelopes it names are queued or being
    /// dispatched. Zero is Settled.
    struct FenceRecord {
        std::uint64_t id = 0;
        std::size_t queued = 0;
    };
    FenceRecord* find_fence(std::uint64_t id) noexcept;
    const FenceRecord* find_fence(std::uint64_t id) const noexcept;
    /// Every enqueue funnels through here: the envelope's fence (already stamped) counts it.
    void queue_back(Envelope env);
    /// The envelope a dispatch just finished with no longer counts toward its fence.
    void fence_dispatched(std::uint64_t id) noexcept;
    /// Open a record for a host's fenced send, or 0 at the bound.
    std::uint64_t begin_fence();

    /// The reply authority for the delivery being dispatched: one delivered request, two exact
    /// incarnations, at most one answer, and gone when the delivery returns, so it cannot be
    /// stored, passed, replayed or outlive either participant. Held by the bus rather than the
    /// handler, so it is checked against who is being dispatched, not merely who asks.
    /// ANS-01; docs/laws/answer-authority-laws.md
    struct ReplyAuthority {
        WeaveId requester{};           ///< the request's stamped sender: the only legal recipient
        std::uint64_t correlation = 0; ///< the request's own: the answer may not choose it
        bool spent = false;            ///< one delivery, one answer
        /// The request's shape, so a refusal raised with no message in hand (deferral overflow)
        /// names the right conversation.
        std::shared_ptr<const Schema> shape{};
        /// Who asked, captured when the request was delivered, so an answer never retargets
        /// itself onto whatever the requester has since become. Both answer doors read it.
        std::uint64_t requester_life = 0;
        std::uint64_t requester_incarnation = 0;
        /// The preparation ask this delivery is, if any, so its answer carries the fact back.
        TxnId preparation{};
    };

    // The Bus a handler receives: it stamps the handling Weave's identity on every send and
    // takes the gated path. A Weave holds only this, never the Switchboard, so it speaks only as
    // itself and within its grant; the reply authority is reached through the same door.
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
        // Joint publication: each verb funnels to its `*_as` door with this delivery's stamped
        // speaker.
        JointResult offer_claim(std::uint64_t op, Value value) override {
            return sb_.offer_claim_as(self_, op, std::move(value));
        }
        JointBegin begin_joint(const JointAuthority& authority,
                               std::vector<ClaimKey> keys) override {
            return sb_.begin_joint_as(self_, authority, std::move(keys));
        }
        JointResult commit_joint(const JointAuthority& authority, std::uint64_t op) override {
            return sb_.commit_joint_as(self_, authority, op);
        }
        JointResult cancel_joint(const JointAuthority& authority, std::uint64_t op) override {
            return sb_.cancel_joint_as(self_, authority, op);
        }
        JointStatus joint_status(const JointAuthority& authority, std::uint64_t op) override {
            return sb_.joint_status_as(self_, authority, op);
        }
        JointResult release_joint(const JointAuthority& authority, std::uint64_t op) override {
            return sb_.release_joint_as(self_, authority, op);
        }

    private:
        Switchboard& sb_;
        WeaveId self_;
    };

    /// Only `ask_candidate_to_prepare` passes `preparation`, and only the attesting doors pass a
    /// `provenance`: every ordinary enqueue takes the defaults, which clears what the caller's
    /// Message carried.
    friend struct DispatchRefusalProbe; // test-only counter boundary and retained-size witness
    std::uint64_t allocate_sequence();
    void capture_refusal_recipient(Envelope& env);
    void notify_dispatch_refusal(const Envelope& env, const BusEvent& ev,
                                 bool permitted);
    Ticket enqueue_directed(WeaveId target, Message msg, bool gated,
                            Provenance provenance = Provenance{}, TxnId preparation = TxnId{});
    Ticket enqueue_role(std::string role, Message msg, bool gated,
                        Provenance provenance = Provenance{});
    std::size_t fanout(Message msg, bool gated, Provenance provenance = Provenance{});

    /// Whether `as_sender` holds `as_role` now: the one membership question every office door
    /// asks (MSG-07), read from the table routing uses.
    bool holds_role_now(WeaveId as_sender, std::string_view as_role) const;

    /// Refuse an office-authorship request: `RoleAuthorshipDenied` on the tap and in the journal,
    /// nothing queued, and the invalid ticket returned.
    Ticket refuse_office(WeaveId target, WeaveId as_sender, const Message& msg);

    /// The one write path for an attested answer. Refuses, on the tap and in the journal, when
    /// the caller is not the weave being dispatched, the request had no one to answer, or the
    /// delivery's one answer is spent.
    Ticket answer_as(WeaveId as_sender, Message msg);

    /// The one write path for a lifecycle attestation.
    Ticket announce_as(WeaveId as_sender, const LifecycleAuthority& authority, WeaveId target,
                       Message msg, std::int64_t sequence);

    // ---- live authority administration (GATE-05) -----------------------------

    /// The one write path for a subject's delegated live authority. `caller` is recorded for
    /// diagnostics and never consulted: power comes from holding the capability, not from being
    /// anybody. Refusals are checked in the order that tells a holder which of its own facts went
    /// stale: inert capability, foreign or dead board, missing subject, ceiling.
    GrantChange delegate_authority_as(WeaveId caller, const GrantAuthority& authority,
                                      LiveAuthority requested);

    /// The read half, through the same capability and with the same scope.
    AuthorityView describe_authority_as(WeaveId caller, const GrantAuthority& authority) const;

    // ---- deferred answers (ANS-02) -------------------------------------------

    /// One unfinished conversation, held by the bus, naming both participants' incarnations: a
    /// code reload keeps the id, and the right belongs to the code that earned it.
    struct DeferredRecord {
        std::uint64_t token = 0; ///< 0 = a free slot
        WeaveId requester{};
        std::uint64_t requester_incarnation = 0;
        /// The requester's life when it asked.
        std::uint64_t requester_life = 0;
        WeaveId respondent{};
        std::uint64_t respondent_incarnation = 0;
        std::uint64_t correlation = 0;
        /// The preparation ask this right answers, if any, so a deferred readiness proves what
        /// an immediate one does.
        TxnId preparation{};
    };

    /// The one door every authenticated answer leaves by (ANS-03): `answer_as` and
    /// `spend_deferred_as` both call it.
    Ticket enqueue_answer(WeaveId to, WeaveId as_sender, Message msg, std::uint64_t correlation,
                          std::uint64_t requester_life, std::uint64_t requester_incarnation,
                          TxnId preparation);

    DeferredAnswer defer_answer_as(WeaveId as_sender);
    Ticket spend_deferred_as(WeaveId as_sender, const DeferredAnswer& answer, Message msg);
    void release_deferred_as(WeaveId as_sender, const DeferredAnswer& answer);

    /// Did this board issue this deferred answer? One held natively names its issuer; one held
    /// in a loaded library names none, and its token reaches only its own board.
    bool issued_here_deferred(const DeferredAnswer& answer) const noexcept {
        const std::shared_ptr<const LoomIdentity> issuer = answer.issuer().lock();
        return issuer == nullptr || issuer == identity_;
    }

    /// Drop every deferred conversation with `id` at an incarnation that no longer exists: what
    /// `swap_state` leaves behind. Death is `abandon_deferred_for`'s question.
    void forget_deferred_for(WeaveId id);

    /// Drop every deferred conversation `id` is a party to, at any incarnation: the end of a life
    /// (ANS-04). A killed weave keeps its id and incarnation, so staleness cannot see it. Called
    /// by `kill` and `unregister_weave`.
    void abandon_deferred_for(WeaveId id);

    /// This transaction's one readiness conversation: not asked, open, or consumed.
    enum class Conversation : std::uint8_t { NotAsked, Open, Consumed };

    /// One prepared replacement's bus-private record. Not the public `loom::PreparedReplacement`,
    /// the host's authoring handle over the public primitives; a weave never sees this.
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
        /// The only correlation a readiness answer may carry. Not a secret, so knowing it
        /// authorizes nothing; Loom mints one per transaction.
        std::uint64_t preparation_correlation = 0;
        Conversation conversation = Conversation::NotAsked;
    };

    /// Is this participant still exactly who it was, and still alive?
    bool still(const ParticipantRef& was) const;

    PreparedReplacement* find_txn(TxnId id);
    const PreparedReplacement* find_txn(TxnId id) const;

    /// The one transition into `Ready`, reached only by accepting an authenticated answer that
    /// has proven whose it is. The caller checks the speaker; this checks the world (candidate
    /// still sealed to this coordinator, incumbent still the role holder).
    /// PR-04; docs/laws/replacement-laws.md
    TxnResult accept_authenticated_readiness(PreparedReplacement& txn);

    /// Whether a transaction id this Loom issued has ended, rather than never existed: ids are
    /// minted in order. Confers nothing.
    TxnReason vanished_transaction_reason(TxnId id) const;

    /// End a transaction, record its outcome for its operator, and free the slot.
    void finish_txn(PreparedReplacement& txn, TxnState state, TxnReason reason);

    /// Every transition that can invalidate a participant calls this: it aborts only the
    /// transactions that bind `changed` and whose captured facts no longer hold.
    void invalidate_transactions_for(WeaveId changed);

    /// Advance `rec`'s life if it is dead, that is, if the caller is about to revive it. Every
    /// revival path calls it before marking the record alive.
    void begin_new_life(WeaveRecord& rec);

    DeferredRecord* find_deferred(std::uint64_t token);
    /// This weave's current incarnation, or 0 if it is not registered.
    std::uint64_t incarnation_of(WeaveId id) const;
    /// This weave's current life, or 0 if unregistered: stamped at enqueue, checked at delivery.
    std::uint64_t life_of(WeaveId id) const;

    /// Record and publish a refusal that never became a delivery, at the same altitude as a
    /// delivery refusal.
    Ticket refuse_now(WeaveId target, WeaveId sender, const Message& msg, RefusalReason reason);

    void deliver_one(Envelope env);

    /// Dispatch at most `budget` deliveries, counting work enqueued during the turn: how
    /// `pump_pending()` counts. Private: the public turn takes no number (MSG-09).
    std::size_t dispatch_at_most(std::size_t budget);

    /// Dispatch an admission, whole, in one turn: revalidate, admit the activation through the
    /// candidate's gate, move the topology, end the transaction, deliver the activation. Called
    /// from `deliver_one` like any envelope, and runs one handler, the candidate's. A refusal is
    /// recorded as `AdmissionRevoked` and changes nothing.
    /// PR-08; docs/laws/replacement-laws.md
    void deliver_admission(Envelope env);

    /// Can `candidate` receive this activation: its accept-set and its gate. Returns the admitted
    /// payload so the caller does not gate it twice; `admit()` consumes its input.
    std::optional<Value> activation_deliverable(const WeaveRecord& candidate,
                                                Value payload) const;

    /// Everything an admission requires of the world, asked when it is scheduled and again when
    /// it is dispatched.
    AdmitRefusal admission_blocked(const ParticipantRef& candidate,
                                   const ParticipantRef& incumbent, const CandidateOwner& owner,
                                   const std::string& role) const;

    /// The one admission primitive: `admit_candidate` calls it with no transaction,
    /// `commit_prepared_replacement` with one. One path and one set of checks for both.
    AdmitResult schedule_admission(WeaveId candidate, WeaveId incumbent, const std::string& role,
                                   const LifecycleAuthority& authority, Message activation,
                                   std::int64_t sequence, TxnId txn);

    void emit(const BusEvent& event);
    void record(std::uint64_t seq, Disposition disposition, const Refusal& refusal);

    // ---- the latest-claim repository ----------------------------------------
    // Personal claims are keyed by the claimant's WeaveId, office claims by role, in separate
    // maps, so a personal claim can never be reached through or promoted to an office key
    // (SENSE-04). One record per key: a new claim replaces the value and bumps its revision.
    struct ClaimRecord {
        Value value;
        WeaveId author{};
        std::uint64_t author_life = 0;
        std::uint64_t author_incarnation = 0;
        std::uint64_t revision = 0;
        /// What became of a value a joint commit put here: None for an ordinary claim, then
        /// Pending, Applied, Declined, Failed (the claimant is held) or Lost. Reset to None by the
        /// claimant's next ordinary claim; Failed returns to Pending on a code swap. The hold
        /// and the repair read this, never the operation's slot.
        JointApplication application = JointApplication::None;
        /// Which operation published it (0 for an ordinary claim), and the record's revision
        /// then: the attribution a failure keeps.
        std::uint64_t published_by = 0;
        std::uint64_t published_revision = 0;
    };
    /// key: (claimant id, shape name, shape version)
    using PersonalKey = std::tuple<std::uint64_t, std::string, std::uint32_t>;
    /// key: (role, shape name, shape version)
    using OfficeKey = std::tuple<std::string, std::string, std::uint32_t>;

    std::map<PersonalKey, ClaimRecord> personal_claims_;
    std::map<OfficeKey, ClaimRecord> office_claims_;

    /// The declared claim-set's entry for this shape, or nullptr: the one place the claim door
    /// and discovery ask.
    static const std::shared_ptr<const Schema>* declared_claim(const WeaveRecord& rec,
                                                               std::string_view name,
                                                               std::uint32_t version);
    /// Does this weave's declared claim-set contain the shape?
    bool declares_claim(const WeaveRecord& rec, std::string_view name,
                        std::uint32_t version) const;
    /// A prepared claim and its verdict; `record` is engaged only when accepted.
    struct MadeClaim {
        std::optional<ClaimRecord> record;
        SenseClaimResult result;
    };
    /// What both claim doors do: check the declaration, gate the value, stamp the next revision.
    /// Writes nothing.
    MadeClaim make_claim(const WeaveRecord& rec, Value value, std::uint64_t previous_revision);
    /// A reading's authorship from a stored record, answering the "is that still true?"
    /// questions when read, never storing them.
    SenseAuthorship authorship_of(const ClaimRecord& rec, const std::string& office,
                                  const std::string& name, std::uint32_t version) const;
    /// Drop the claims under keys that stopped meaning anything: a removed weave's personal keys,
    /// an unheld role's office keys. An admission moves a role in place and never leaves it
    /// unheld, so the predecessor's office claim survives it, marked stale.
    void forget_personal_claims(WeaveId id);
    void forget_office_claims(const std::string& role);

    // ---- Joint publication: the bus-private records -------------------------------
    struct JointPart {
        ClaimKey key;
        ParticipantRef claimant{};   ///< exact life + incarnation, bound at begin
        /// The office the key was named by at begin, or its claimant's then: the words a failure
        /// about this part is told in.
        std::string bound_role;
        std::uint64_t revision = 0;  ///< the key's revision at begin
        std::optional<Value> offer;  ///< the admitted next value, until commit/abort
        std::size_t offer_bytes = 0; ///< its serialized size, measured at the offer
        /// After a commit, what became of the value at this claimant: the operation's own copy,
        /// which outlives a removed claimant's record.
        JointApplication application = JointApplication::None;
    };
    /// One operation's record, held from `begin` until released (SENSE-07): a Missing slot is
    /// free. `next_joint_id_` never repeats an id, so a released id names nothing.
    struct JointOp {
        std::uint64_t id = 0;
        ParticipantRef operator_{};
        JointState state = JointState::Missing;
        JointRefusal reason = JointRefusal::None;
        std::vector<JointPart> parts;
        /// The operator was told this operation ended (`zen.JointEnded`).
        bool notified = false;
        /// ...and the last application outcome it was told (`zen.JointApplied`); a repair that
        /// settles again is told again.
        JointApplication applied_told = JointApplication::None;
    };
    std::array<JointOp, kMaxJointOperations> joint_ops_{};
    std::uint64_t next_joint_id_ = 1;

    JointOp* find_joint(std::uint64_t id) noexcept;
    const JointOp* find_joint(std::uint64_t id) const noexcept;
    /// Retire a record: the slot is free and the id names nothing, on the operator's release or
    /// when its life or incarnation changed.
    void retire_joint(JointOp& op) noexcept;
    /// The operation's state, reason and, after a commit, its application over its parts (Failed,
    /// then Lost, Declined, Pending, Applied), naming the part a non-applied aggregate is about.
    JointStatus status_of(const JointOp& op) const;
    /// The notice behind `settle_application`, to the exact operator, once per settlement.
    void notify_joint_applied(JointOp& op, JointApplication what, const JointPart* about);
    /// End an operation: its state, reason, and every offer released. Safe from any transition.
    void finish_joint(JointOp& op, JointState state, JointRefusal reason) noexcept;
    /// The operator half of every operator verb's authority check: a live delivery of the
    /// caller, an authority this board issued, and the caller being the exact participant it was
    /// minted for. Each verb then checks the record's own `operator_`.
    JointRefusal joint_authority_check(WeaveId caller, const JointAuthority& authority) const;
    /// On a lifecycle transition of `changed`: abort each Preparing operation binding it whose
    /// bound life or incarnation no longer holds, retire each record whose operator it no longer
    /// is, and make a removed claimant's Pending part Lost.
    void invalidate_joint_for(WeaveId changed);
    /// Tell the operator the bus ended its operation (`zen.JointEnded`), if it accepts that.
    void notify_joint_ended(JointOp& op);
    /// Abort every Preparing operation that binds this personal key: its claimant claimed
    /// ordinarily, so the bound revision is stale.
    void abort_joint_on_claim(const PersonalKey& key);
    /// What a showing came to: `held` if the weave was already held and nothing was shown;
    /// `failed` if a hook did not complete now, with `op` the publication and `error` any native
    /// exception, rethrown by the caller once the record says what happened.
    struct Showing {
        bool failed = false;
        bool held = false;
        std::uint64_t op = 0;
        std::exception_ptr error;
    };
    /// Show `rec` every joint-published value under its keys it has not seen, by operation then
    /// key, recording each outcome. Stops at the first failure: that key is Failed, later keys
    /// stay Pending, and the weave is held. Shows nothing to a weave already held. Called before
    /// a delivery to `rec` and before its snapshot, never inside a dispatch to another weave.
    Showing observe_published_claims(WeaveRecord& rec);
    /// Record one showing's outcome on the claim record and on the operation that published it,
    /// if still held, then settle the operation's application once every part is in.
    void note_application(const PersonalKey& key, ClaimRecord& record, JointApplication what);
    /// Tell the operator once per settlement (`zen.JointApplied`), naming the operation and, for
    /// a non-application, the exact claimant and the office it was bound through.
    void settle_application(JointOp& op);
    /// A code swap's successor is shown what its predecessor could not apply: every Failed key
    /// of `id` returns to Pending, on the record and on its operation.
    void reset_failed_application_for_successor(WeaveId id) noexcept;

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

    /// One retained outcome, tagged with the seq that owns it: a slot answers only while its
    /// `seq` matches, so an evicted or never-issued seq reads Pending.
    struct JournalSlot {
        std::uint64_t seq = 0; ///< 0 = never written (real seqs start at 1)
        DeliveryOutcome outcome;
    };
    std::vector<JournalSlot> journal_; ///< ring of the last kJournalCapacity outcomes, by seq % cap
    std::uint64_t next_seq_ = 1;

    /// The tap list, each observer held by shared pointer so one that removes itself while being
    /// notified is not destroyed mid-call: `emit()` holds a reference for each call.
    /// MSG-11; docs/laws/messaging-laws.md
    std::vector<std::pair<ObserverId, std::shared_ptr<Observer>>> observers_;
    ObserverId next_observer_id_ = 1;

    /// Is this id still registered? Asked between notifications so a removal made
    /// during an event takes effect within that event.
    bool observer_registered(ObserverId id) const noexcept;

    /// Mint the capability that lets trusted infrastructure attach Loom's lifecycle attestation.
    /// Private and non-static, both on purpose: minting needs the host's own Switchboard, which
    /// a weave never holds, and goes through the one host-wiring function below. The authority
    /// names the board that issued it and is spendable only there.
    /// LIFE-04; docs/laws/lifecycle-laws.md
    LifecycleAuthority lifecycle_authority() noexcept { return LifecycleAuthority{identity_}; }

    /// Was this authority issued by this Loom, and is that Loom still alive? An authority lasts
    /// as long as the Loom that issued it.
    bool issued_here(const LifecycleAuthority& authority) const noexcept {
        const std::shared_ptr<const LoomIdentity> issuer = authority.issuer_.lock();
        return issuer != nullptr && issuer == identity_;
    }

    /// The one expression that yields a LifecycleAuthority, defined in the host-wiring header
    /// `zen/host/lifecycle_wiring.hpp`, which no weave-authoring header includes.
    friend LifecycleAuthority host_lifecycle_authority(Switchboard& bus);

    /// Mint the right to administer one subject's delegated live authority, up to `ceiling`
    /// (GATE-05). Private, behind the host-wiring function below. The subject need not exist
    /// yet: every use checks the live registry anyway.
    GrantAuthority grant_authority(WeaveId subject, LiveAuthority ceiling) {
        return GrantAuthority{identity_, subject, std::move(ceiling)};
    }

    /// Was this administration capability issued by this Loom, and is it still alive? Every
    /// Loom is its own authority domain.
    bool issued_here(const GrantAuthority& authority) const noexcept {
        const std::shared_ptr<const LoomIdentity> issuer = authority.issuer_.lock();
        return issuer != nullptr && issuer == identity_;
    }

    /// The one expression that yields a GrantAuthority, defined in `zen/host/grant_wiring.hpp`.
    friend GrantAuthority host_grant_authority(Switchboard& bus, WeaveId subject,
                                               LiveAuthority ceiling);

    /// This Loom's identity, created and destroyed with the board and shared, weakly, only with
    /// the authorities it issues. A later board at the same address cannot revive a dead one's.
    std::shared_ptr<const LoomIdentity> identity_;

    /// What the delivery being dispatched is: the facts Loom stamped on it, for the handler's
    /// length. Separate from `ReplyAuthority`, which is the right to answer and is spent by
    /// answering; these are what was heard, and nothing spends them.
    struct DeliveryFacts {
        /// Loom's word that this is the authorized answer to a request the recipient sent,
        /// written only by the two answer doors.
        bool answers_ask = false;
        /// The bus-stamped author: not `reply_to`, not the payload.
        WeaveId sender{};
        /// The conversation's correlation, as the ask carried it; for diagnostics, since
        /// `preparation` decides.
        std::uint64_t correlation = 0;
        /// Which preparation ask this answers; invalid unless it answers a real one.
        TxnId preparation{};
    };

    bool in_dispatch_ = false;
    bool stop_requested_ = false;
    /// The recipient of the delivery being dispatched; invalid when none is live, so nobody may
    /// answer.
    WeaveId current_target_{};
    /// The seq of the delivery being dispatched, read by every enqueue path so a message
    /// authored in a handler carries it. 0 outside a dispatch.
    std::uint64_t current_dispatch_seq_ = 0;
    /// ...and the delivery it was queued during, with the same lifetime.
    std::uint64_t current_dispatch_parent_ = 0;
    /// The fence of the delivery being dispatched (0 for none), for the whole of its dispatch,
    /// so what it queues is counted there. A host's fenced send sets it for its one enqueue.
    std::uint64_t current_dispatch_fence_ = 0;
    std::vector<FenceRecord> fences_; ///< bounded; see kMaxFences
    std::uint64_t next_fence_ = 1;    ///< monotonic: a fence id is never reused
    /// Whether the handler being dispatched reported failure across the ABI seam: set only by
    /// `note_handler_failure()`, cleared before each handler call. A native handler throws.
    bool handler_reported_failure_ = false;
    ReplyAuthority authority_{};
    DeliveryFacts delivery_{};

    // ---- scoped bookkeeping across a callback Loom did not write --------------
    // A native `Weave::handle` or a host observer may throw; the exception is the host's, and
    // Loom neither swallows nor translates it. Guards restore Loom's own temporary state on
    // every exit first, so a throw costs that one delivery and not the bus. `stop_requested_`
    // needs none: each turn resets it.
    // MSG-10; docs/laws/messaging-laws.md

    /// Hold `in_dispatch_` for a dispatch turn, restoring the value it found.
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

    /// Hold the delivery context for one handler call, and clear it at the end: a delivery's
    /// answer authority and facts are its own, and dispatch is never reentrant. Each site
    /// assigns the fields itself (an admission mints no reply authority).
    class DeliveryScope {
    public:
        explicit DeliveryScope(Switchboard& sb) noexcept : sb_(sb) {}
        ~DeliveryScope() {
            sb_.current_target_ = WeaveId{};
            sb_.current_dispatch_seq_ = 0;
            sb_.current_dispatch_parent_ = 0;
            sb_.authority_ = ReplyAuthority{};
            sb_.delivery_ = DeliveryFacts{};
        }
        DeliveryScope(const DeliveryScope&) = delete;
        DeliveryScope& operator=(const DeliveryScope&) = delete;

    private:
        Switchboard& sb_;
    };

    /// Hold one envelope's fence for its dispatch, and count it dispatched on any exit: a handler
    /// that throws still consumed its envelope (MSG-10).
    class FenceTurn {
    public:
        FenceTurn(Switchboard& sb, std::uint64_t fence) noexcept
            : sb_(sb), fence_(fence), was_(sb.current_dispatch_fence_) {
            sb_.current_dispatch_fence_ = fence;
        }
        ~FenceTurn() {
            sb_.current_dispatch_fence_ = was_;
            sb_.fence_dispatched(fence_);
        }
        FenceTurn(const FenceTurn&) = delete;
        FenceTurn& operator=(const FenceTurn&) = delete;

    private:
        Switchboard& sb_;
        std::uint64_t fence_;
        std::uint64_t was_;
    };
    std::vector<DeferredRecord> deferred_;      ///< bounded; see kMaxDeferredAnswers
    /// Bounded; a slot is reclaimed when its transaction ends.
    std::vector<PreparedReplacement> txns_;
    std::vector<TxnOutcome> outcomes_;      ///< bounded; oldest dropped
    std::vector<ParticipantRef> outcome_of_; ///< whose outcome each one is
    std::uint64_t next_txn_id_ = 1;
    /// The correlation Loom puts on a preparation ask, never repeated. It does not make an
    /// answer authentic (`Envelope::preparation` does), and is separate from the delivery seq.
    std::uint64_t next_preparation_correlation_ = 1;

    /// Never reused, and unguarded against exhaustion: unlike the persisted activation
    /// sequence, it is process-local, never revived, and advances by one per deferral.
    std::uint64_t next_deferred_token_ = 1;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_SWITCHBOARD_HPP
