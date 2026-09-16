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
#include <zen/weave/ask_book.hpp>

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

// ---- Bounded console history ------------------------------------------------------------------
//
// The console is the operator's window, which makes it exactly the component most likely to be
// left running for weeks — so its retained state is bounded BY DESIGN, never by lifetime
// throughput. That is the same argument kJournalCapacity already won for the bus, applied to the
// surface that grows without a bound: unbounded, every bus event and every delivered Value is kept
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
// later, when a terminal participant's transcript became the fifth and sixth surface needing
// exactly these semantics. Its own comment always said it should be written once; the constants
// below stay here, because a capacity is a policy about ONE surface and this is the console's.

/// Bus events retained on the tap. Deliberately the same width as `Switchboard::kJournalCapacity`:
/// one tap entry corresponds to roughly one journal entry, so an operator who can still SEE an
/// event on the tap can still ASK the journal what became of it. A wider tap would show events
/// whose outcome the bus has already forgotten; a narrower one would waste journal the operator
/// can no longer name.
inline constexpr std::size_t kConsoleTapCapacity = 1024;

/// Conversations this console will track at once — the bound on its own ask book.
///
/// It is not the reply window's number and must not become it. The reply buffer is HISTORY,
/// where discarding the oldest entry loses an operator's view; the ask book is a record of
/// what this participant is still WAITING ON, where discarding the oldest would make the
/// console forget a question it asked (`loom::AskBook` refuses at capacity for exactly that
/// reason). Thirty-two is an operator's number: a person driving a console by hand does not
/// hold dozens of open questions, and a host that somehow does is told so in a sentence
/// naming the command that lists them, rather than silently losing one.
inline constexpr std::size_t kConsoleAskCapacity = 32;

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
// the assumption ladder itself — lives in <zen/terminal/composer.hpp>, so a second
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

/// WHAT SUBMITTING PRODUCED: the queued send, and the conversation it opened.
///
/// `ask` is the local handle in the console's own ask book — the thing a caller holds to
/// find out later whether an arrival was entitled to answer THIS question. It is 0 when the
/// message could not be composed at all, and also when the book was full: the send still
/// happened, and only the tracking was refused, which a caller that cares must notice
/// rather than read as "no answer yet".
struct Submitted {
    loom::Ticket ticket{};
    std::uint64_t ask = 0;
    bool sent() const noexcept { return ticket.valid(); }
};

/// A buffered reply (m1, m2, …): its label, shape, the received Value (read fields by
/// name off `value`), and WHO SAID IT ABOUT WHAT. The Stage-2 `$m1.field` reference syntax
/// reads from `value`.
///
/// THE LAST THREE FIELDS ARE THE ENTRY'S ATTRIBUTION, and the window used to drop them on
/// the floor. A console is registered `AcceptMode::AnyRegistered`, so anything the registry
/// can resolve lands in this buffer — including a `zen.Result` any admitted weave may send
/// under the ordinary poke-answer baseline. Without `sender` and `correlation` there is no
/// way to tell that `zen.Result` apart from the one the operator's own question earned, and a
/// host that read "the newest entry" as "the answer" administered whatever the newest entry
/// happened to name. The two are a PAIR and neither alone is enough (`loom::AskBook`):
/// a correlation identifies a conversation and authenticates nothing; a bus-stamped sender
/// says who spoke and not what about.
struct BufferEntry {
    std::string label;
    std::string name;
    std::uint32_t version;
    loom::Value value;
    /// Stamped by Loom at delivery — never read from a payload, never chooseable by a sender.
    loom::WeaveId sender{};
    /// The conversation the sender named. 0 means "named none", which is what an
    /// unsolicited notification looks like and is never a settlement.
    std::uint64_t correlation = 0;
    /// Did Loom itself attest this as THE one authorized answer to a request this console
    /// sent (`Provenance::answers_ask`)? Stronger than the pair above and strictly rarer: a
    /// middleman relaying somebody else's answer (`loom::relay`) sends an ordinary message,
    /// so a true, correctly attributed answer can arrive with this false.
    bool answers_ask = false;
};

/// The result of running the assumption ladder AND gate-sending.
struct Composed {
    enum class Status { Ready, NeedsInput, Error };
    Status status = Status::Error;
    loom::Ticket ticket{};            ///< Ready: the assembled, gate-sent message's ticket
    std::string error;                   ///< Error: the compose-time verdict
    std::vector<FieldDesc> open_fields;  ///< NeedsInput: the still-unfilled fields
    std::vector<std::string> unplaced;   ///< NeedsInput: args (rendered) the ladder could not place
    /// Ready: the conversation this send opened in the engine's own ask book, or 0 when the
    /// book was full (the send still went; only the tracking was refused). Ask
    /// `ConsoleEngine::settled(ask)` which arrival, if any, is entitled to answer it.
    std::uint64_t ask = 0;
};

/// A copied bus event for the operator's window on the live bus (the tap).
struct TapEvent {
    /// "Delivered" / "Refused" / "Died" / "Revived" / "HandlerFailed" (RTH-1). The
    /// remote client adds one more, "BridgeRefused", which is honestly NOT a bus
    /// event -- see bridge/protocol.hpp.
    std::string kind;
    loom::WeaveId target;
    loom::WeaveId sender;
    std::string schema;  ///< the payload/state shape name
    std::string refusal; ///< the refusal reason (empty unless Refused)
};

class ConsoleWeave; // the console's own raw Weave (buffers arrivals); defined in the .cpp

/// ONE ARRIVAL, AS THE REPLY WINDOW KEEPS IT: the delivered Value and the routing facts
/// that say whose it is.
///
/// The window used to keep the Value alone, and a Value cannot answer "who said this,
/// about which question" — which is why a host reading the newest one could not tell an
/// answer it had earned from an unsolicited message any loaded weave may author. The
/// label is deliberately NOT stored: a label is a function of position in the stable
/// sequence (see `Evicted::buffer`), and storing it would be a second, driftable answer
/// to which entry this is. `BufferEntry` is this plus that computed label, which is the
/// shape a caller reads.
struct ConsoleArrival {
    loom::Value payload;
    loom::WeaveId sender{};
    std::uint64_t correlation = 0;
    bool answers_ask = false;
};

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

    // Drive the transport so sends are delivered and replies/tap arrive (in-process: a bus
    // dispatch turn; remote: flush + poll the socket and process the pushed frames).
    //
    // Deliberately still spelled `pump` — it is the CONSOLE's contract, not the
    // Switchboard's, and the two implementations do different things. In-process it is
    // `Switchboard::drain_until_idle()`, which suits an operator lens issuing one command
    // and reading its outcome, and would suit a console sharing a bus with a perpetual
    // service no better than any other drain (FRIC-1).
    virtual void pump() = 0;
};

/// The assumption ladder's host surface — the two lookups `ComposeSource` already names, plus the
/// one send. Segregated from the frontend Console so the ONE ladder implementation is shared by
/// the in-process engine and the client-side remote console instead of duplicating the placement
/// logic; the ladder itself is `loom::compose_message` and this is the console's
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
    /// One-shot: set fields by name, assemble, and gate-send to `target`. The send is
    /// enqueued — turn the bus, then read `outcome()` for its fate and `settled()` for its
    /// answer. On a compose-time error (no such shape/field, or a type mismatch) the ticket
    /// is invalid, `ask` is 0, and *error says why.
    Submitted submit(loom::WeaveId target, std::string_view name, std::uint32_t version,
                     const std::map<std::string, FieldValue>& fields,
                     std::string* error = nullptr);

    // ---- The ask book: WHICH ARRIVAL IS ENTITLED TO ANSWER WHICH QUESTION ----
    //
    // The console is a participant that asks, so it keeps the asker's record
    // (`loom::AskBook`) rather than reading its own presentation history as a result. Every
    // correlation this console stamps is drawn from that one book — a second counter beside
    // it could stamp an ordinary send with a number an open conversation is already using,
    // and an answer to that send would then settle the conversation.
    //
    // Settlement happens AT ARRIVAL, inside the console weave's own delivery, so a bounded
    // reply window cannot evict an answer before a caller asks about it.

    /// The arrival that settled `ask`, or nullopt while the conversation is still open.
    ///
    /// NULLOPT IS A REAL ANSWER — "still pending" — and this never invents a completion to
    /// avoid returning it. A settled conversation is remembered here until the caller
    /// `forget_ask`s it, so a duplicate or late copy of the same answer is inert.
    std::optional<BufferEntry> settled(std::uint64_t ask) const;

    /// Is `ask` still open? False for one that settled, one that was forgotten, and one that
    /// never existed — the caller's own record says which.
    bool awaiting(std::uint64_t ask) const noexcept;

    /// Every conversation this console is still waiting on, oldest first.
    std::vector<loom::PendingAsk> open_asks() const;

    /// Stop tracking one conversation — LOCALLY. Nothing at the far end is told anything;
    /// a later answer to a forgotten ask matches nothing and settles nothing. Also drops a
    /// settled answer that has been read. True if there was anything to forget.
    bool forget_ask(std::uint64_t ask);

    /// How many conversations this console will track at once, and how many it is using.
    std::size_t ask_capacity() const noexcept;
    std::size_t asks_outstanding() const noexcept;

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
    const BoundedHistory<ConsoleArrival, kConsoleBufferCapacity>& reply_history() const;

    /// Settle an arrival against the book, from inside the console weave's own delivery.
    void record_arrival(const loom::Message& in);

    loom::Switchboard& bus_;
    ConsoleWeave* weave_ = nullptr; // owned by the bus; non-owning here
    loom::WeaveId console_id_{};
    loom::ObserverId tap_obs_ = 0;
    /// THE ONE CORRELATION SEQUENCE THIS PARTICIPANT HAS. Every send stamps a number from
    /// this book — tracked when it opened a conversation, `mint_correlation()` when the book
    /// was full — so no ordinary send can ever carry a number an open conversation is using.
    loom::AskBook book_{kConsoleAskCapacity};
    /// Answers that settled a conversation, held until the caller reads and forgets them, so
    /// a settlement survives the reply window evicting the entry that carried it.
    std::map<std::uint64_t, BufferEntry> settled_;
    /// The conversation the last assemble_and_send opened; read immediately by submit() and
    /// compose(), which are the only callers and are single-threaded with it.
    std::uint64_t last_ask_ = 0;
    BoundedHistory<TapEvent, kConsoleTapCapacity> tap_; // history: bounded window, oldest evicted
    Dirty dirty_; // accumulated by record_tap; drained by take_dirty
};

} // namespace loom

#endif // ZEN_CONSOLE_CONSOLE_HPP
