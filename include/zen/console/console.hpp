// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_CONSOLE_CONSOLE_HPP
#define ZEN_CONSOLE_CONSOLE_HPP

// The operator's console: a bus participant that discovers what is registered, composes and
// gate-sends messages, and keeps the replies and bus events it sees in bounded windows. It
// returns data, never text or widgets, so a terminal, a remote client or a GUI can drive it.
// It is the most-granted participant (broad send, any registered shape accepted, the tap,
// discovery), each capability a grant and none a bypass, and it knows no shape's meaning.
// docs/reference/terminal.md#the-console-is-a-different-thing-and-stays-one

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
// The tap and the reply buffer are history, past observations on which nothing is owed, so each
// keeps a fixed window and discards its oldest entry, counted in `Console::evicted()`. A reply's
// label (m1, m2, ...) is a stable identity: an evicted one refuses rather than naming a newer
// reply. A held conversation is owed, so its bound refuses a new one instead of evicting.
// The window type is `loom::BoundedHistory`; docs/reference/bounds.md#console-operator-history

/// Bus events retained on the tap. Deliberately the same width as `Switchboard::kJournalCapacity`:
/// one tap entry corresponds to roughly one journal entry, so an operator who can still SEE an
/// event on the tap can still ASK the journal what became of it. A wider tap would show events
/// whose outcome the bus has already forgotten; a narrower one would waste journal the operator
/// can no longer name.
inline constexpr std::size_t kConsoleTapCapacity = 1024;

/// Conversations this console holds for its callers at once: the ones still open and the
/// answered ones whose answer has not been taken. Not a history window: a new tracked
/// conversation is refused at capacity (the send still goes, untracked), and nothing held is
/// evicted. A slot is spent at the ask and returned at `take_settled` or `forget_ask`.
/// docs/reference/bounds.md#console-operator-history
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

// The compose vocabulary (FieldDesc, ShapeDesc, FieldValue, Ref, Arg, describe_schema) and the
// assumption ladder are in <zen/terminal/composer.hpp>, shared with the terminal session.
// `Composed` and `LadderHost` below are the console's one-shot form: compose and send at once.

/// The fate of a submitted message, surfaced from the gate (never a silent mis-send).
struct SendOutcome {
    bool delivered = false;
    bool refused = false;
    std::string reason; ///< the gate's verdict / routing refusal, on refusal
};

/// Whether a send opens a conversation the console holds for its caller. `Untracked`: the
/// message goes, nothing is held, and whatever comes back is the reply window's history.
/// `Tracked`: the message goes and one slot of `kConsoleAskCapacity` is held until the caller
/// takes the answer (`take_settled`) or forgets the conversation (`forget_ask`); a caller that
/// never collects runs out of slots and is told so (`ask == 0`).
enum class ConsoleTracking : std::uint8_t { Untracked, Tracked };

/// What submitting produced: the queued send and the conversation it opened. `ask` is the
/// handle in the console's own ask book; it is 0 when the message could not be composed, when
/// the send was `Untracked`, and when every slot was held. In the last case the send still
/// went and only the tracking was refused, which a caller must not read as "no answer yet".
struct Submitted {
    loom::Ticket ticket{};
    std::uint64_t ask = 0;
    bool sent() const noexcept { return ticket.valid(); }
};

/// A buffered reply (m1, m2, ...): its label, shape and received Value, which a `$m1.field`
/// reference reads, and who sent it about what. The console accepts any registered shape, so
/// any admitted weave's `zen.Result` can land here: only `sender` and `correlation` together
/// tell an answer to this console's question from one it did not ask for (`loom::AskBook`).
/// docs/reference/messaging.md#the-askers-own-book
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
    /// Ready: the conversation this send opened in the engine's own ask book when it was sent
    /// `ConsoleTracking::Tracked`; 0 when it was untracked — which every send through the
    /// `Console` interface is — or when every slot was held (the send still went; only the
    /// tracking was refused). Ask `ConsoleEngine::settled(ask)` which arrival, if any, is
    /// entitled to answer it.
    std::uint64_t ask = 0;
};

/// A copied bus event for the operator's window on the live bus (the tap).
struct TapEvent {
    /// "Delivered", "Refused", "Died", "Revived" or "HandlerFailed" (the handler was entered
    /// and did not complete). The remote client adds "BridgeRefused", which is not a bus event
    /// (zen/bridge/protocol.hpp).
    std::string kind;
    loom::WeaveId target;
    loom::WeaveId sender;
    std::string schema;  ///< the payload/state shape name
    std::string refusal; ///< the refusal reason (empty unless Refused)
};

class ConsoleWeave; // the console's own raw Weave (buffers arrivals); defined in the .cpp

/// One arrival as the reply window keeps it: the delivered Value and the routing facts that
/// say whose it is. Its label is not stored: a label is its place in the stable sequence
/// (`Evicted::buffer`), and `BufferEntry` is this with that label computed.
struct ConsoleArrival {
    loom::Value payload;
    loom::WeaveId sender{};
    std::uint64_t correlation = 0;
    bool answers_ask = false;
};

/// Which regions of a console's view changed, so a UI repaints only those. Set as bus events
/// arrive: `buffer` on a reply delivered to the console, `weaves` on a weave dying or reviving,
/// `tap` on any bus event. The compose and guidance regions change on keystrokes, so the input
/// loop redraws them and they are not tracked here.
struct Dirty {
    bool weaves = false;
    bool buffer = false;
    bool tap = false;
    bool any() const noexcept { return weaves || buffer || tap; }
};

/// How much of each bounded window has been discarded, so an operator can tell a complete
/// history from one whose oldest evidence was evicted. The weave list has no row: it is a
/// snapshot, not a history. `buffer` is also the reply labels' base: the retained entries are
/// m(buffer + 1) ... m(buffer + buffer_size()), and a label outside that range refuses rather
/// than naming another reply.
struct Evicted {
    std::uint64_t tap = 0;    ///< bus events dropped from the tap window (kConsoleTapCapacity)
    std::uint64_t buffer = 0; ///< replies dropped from the m-buffer (kConsoleBufferCapacity)
    bool any() const noexcept { return tap != 0 || buffer != 0; }
};

/// The frontend-facing console, independent of where the bus lives: `ConsoleEngine` implements
/// it in process and `RemoteConsole` over the bridge protocol on a socket, so a frontend
/// written against it runs on either. Remotely, discovery and the tap are answered by messages.
class Console {
public:
    virtual ~Console() = default;

    // Discovery (registry-read; works on shapes never seen).
    virtual std::vector<WeaveInfo> weaves() const = 0;
    virtual std::optional<ShapeDesc> describe(std::string_view name, std::uint32_t version) const = 0;

    // Compose and gate-send by the assumption ladder (named, positional, type-directed, then
    // prompt). Untracked on every implementation: nothing is held, `Composed::ask` is 0, and the
    // reply is the window's to show. Attributed conversations are `ConsoleEngine`'s, through its
    // `ConsoleTracking` overloads; `RemoteConsole` keeps no ask book.
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

    // Drive the transport so sends are delivered and replies and tap events arrive. The
    // console's contract, not the Switchboard's: in process it is
    // `Switchboard::drain_until_idle()`, unbounded (MSG-09), which suits an operator issuing one
    // command and reading its outcome; remotely it flushes and polls the socket once.
    virtual void pump() = 0;
};

/// The assumption ladder's host: `ComposeSource`'s two lookups plus the one send, apart from
/// the frontend `Console` so the in-process engine and the remote console share one ladder
/// (`loom::compose_message`). The gate is the backstop on both transports.
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

/// Resolve `$label.field` against a Console's reply buffer, for the in-process engine and the
/// remote console alike. Reads a scalar Cell off an immutable buffered Value; nullopt, and
/// *error says why, on a missing entry, a missing field or a non-scalar field.
std::optional<loom::Cell> resolve_ref_from(const Console& console, const Ref& ref,
                                           std::string* error);

/// The frontend-agnostic console engine — the in-process Console. Construct it over a Switchboard; it
/// registers the console as an in-process Weave (broad grant + accept-any) and subscribes the tap.
class ConsoleEngine : public Console, public LadderHost {
public:
    /// `vocabulary`: the shapes this operator window expects to be told, normally none. The
    /// console accepts any shape the registry can resolve, and the registry learns a shape from
    /// some weave's accept-set, so a notification only the operator receives is declared here.
    /// It is not a grant: a listed door is gated as the wildcard one is.
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
    /// One-shot: set fields by name, assemble and gate-send to `target`. The send is queued:
    /// turn the bus, then read `outcome()` for its fate and, when `Tracked`, `settled()` for its
    /// answer. On a compose error (no such shape or field, a type mismatch) the ticket is
    /// invalid, `ask` is 0 and *error says why. Tracking has no default: holding a conversation
    /// is a duty to collect it, and the call site says whether it took one on.
    Submitted submit(loom::WeaveId target, std::string_view name, std::uint32_t version,
                     const std::map<std::string, FieldValue>& fields, ConsoleTracking tracking,
                     std::string* error = nullptr);

    // ---- The ask book: which arrival is entitled to answer which question ----
    // Every correlation this console stamps comes from its one `loom::AskBook`, so no ordinary
    // send carries a number an open conversation is using. An arrival settles a conversation
    // inside the console weave's own delivery, so the reply window cannot evict an answer first.
    // A held conversation is its caller's until taken or forgotten: nothing here expires one.

    /// The arrival that settled `ask`, or nullopt while it is still open: a real answer, never
    /// an invented completion. A read: the answer stays held, and keeps its slot, until
    /// `take_settled` or `forget_ask`. A duplicate or late copy of it is inert.
    std::optional<BufferEntry> settled(std::uint64_t ask) const;

    /// COLLECT an answer: hand it over and release its slot. nullopt, and nothing released,
    /// while the conversation is still open or for an ask this console is not holding.
    std::optional<BufferEntry> take_settled(std::uint64_t ask);

    /// Is `ask` still open? False for one that settled, one that was forgotten, and one that
    /// never existed — the caller's own record says which.
    bool awaiting(std::uint64_t ask) const noexcept;

    /// Every conversation this console is still waiting on, oldest first.
    std::vector<loom::PendingAsk> open_asks() const;

    /// Every conversation that has an answer waiting to be taken, oldest first — so a caller
    /// can always find, and free, every slot it holds.
    std::vector<std::uint64_t> answered_asks() const;

    /// Stop holding one conversation — LOCALLY. For an open one, nothing at the far end is
    /// told anything: whatever was asked may still be happening, and a later answer matches
    /// nothing and settles nothing (this is `forget`, not `cancel`, because Loom has no
    /// cancellation vocabulary). For an answered one, the unread answer is discarded. Either
    /// way the slot is released. True if there was anything to forget.
    bool forget_ask(std::uint64_t ask);

    /// The bound, and what is spent against it.
    std::size_t ask_capacity() const noexcept;
    /// Conversations still open (no answer has settled them).
    std::size_t asks_outstanding() const noexcept;
    /// Everything this console holds for its callers: open conversations plus answers not yet
    /// taken. This is the number compared with `ask_capacity()`.
    std::size_t asks_held() const noexcept;

    /// The fate of a previously-submitted Ticket (after pump()).
    SendOutcome outcome(loom::Ticket t) const;

    // ---- References and the assumption ladder ----
    /// Resolve `$label.field` off the reply buffer to a scalar Cell, a read of an immutable
    /// buffered Value. nullopt, and *error says why, on a missing entry, a missing field or a
    /// non-scalar field.
    std::optional<loom::Cell> resolve_ref(const Ref& ref, std::string* error = nullptr) const override;

    /// Resolve a registered schema by identity (LadderHost): the in-process engine reads the bus's
    /// registry. (The remote console reads its own registry, filled by Describe replies.)
    std::shared_ptr<const loom::Schema> resolve_schema(std::string_view name,
                                                       std::uint32_t version) const override;

    /// Compose by the assumption ladder: each literal or reference arg, optionally named, goes
    /// to a field, named first, then positional (declaration order), then type-directed, else
    /// NeedsInput; it never guesses on ambiguity. On Ready it assembles and gate-sends. A
    /// wrong-typed named arg or a bad reference is an Error. This override is untracked, as
    /// every `Console` is.
    Composed compose(loom::WeaveId target, std::string_view name, std::uint32_t version,
                     const std::vector<Arg>& args) override;
    /// ...and this is the in-process form that may hold the conversation it opens.
    Composed compose(loom::WeaveId target, std::string_view name, std::uint32_t version,
                     const std::vector<Arg>& args, ConsoleTracking tracking);

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
    friend struct ConsoleHistoryProbe; ///< tests read the windows' own storage through it

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
    /// Answers that settled a conversation, held until the caller takes or forgets them, so a
    /// settlement survives the reply window evicting the entry that carried it. Each one still
    /// occupies the slot its conversation was opened with, so `book_.outstanding() +
    /// settled_.size()` never exceeds `kConsoleAskCapacity`.
    std::map<std::uint64_t, BufferEntry> settled_;
    /// What the next assemble_and_send is to do about tracking, and the conversation it opened.
    /// Set and read by submit() and compose() around the one send, single-threaded with it.
    ConsoleTracking next_tracking_ = ConsoleTracking::Untracked;
    std::uint64_t last_ask_ = 0;
    BoundedHistory<TapEvent, kConsoleTapCapacity> tap_; // history: bounded window, oldest evicted
    Dirty dirty_; // accumulated by record_tap; drained by take_dirty
};

} // namespace loom

#endif // ZEN_CONSOLE_CONSOLE_HPP
