// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TERMINAL_SESSION_HPP
#define ZEN_TERMINAL_SESSION_HPP

// THE TERMINAL SESSION — an ordinary Loom weave that a person, a pane, or a
// script can drive.
//
//     the Kernel enforces.  the Weaver decides.  the session acts.
//
// This is the third line, made into something you can hold. The Weaver withheld the
// name from its demo because a thirty-line weave with no transcript, no composer
// and no command language had not earned it; what follows is the attempt to earn
// it, and the test of whether it did is that everything below is either an
// ORDINARY PARTICIPANT'S POWER or is not here at all.
//
// WHAT IT IS. One WeaveId. One admission grant the host chose. One vocabulary the
// host supplied. Its own transcript of its own experience. It composes typed
// messages from real schemas, authors them under its own identity, receives what
// its doors admit, asks and recognizes Loom's authenticated answers, and asks a
// policy office for more authority when it needs some.
//
// WHAT IT IS NOT, said plainly because "terminal" sounds like "root":
//
//     no Switchboard&, no Kernel&, no IsolationHost&, no host root send
//     no whole-bus tap, and therefore no third party's traffic
//     no `allow_any`, no `observe_any`, no load capability, no filesystem,
//         no network — being a terminal confers NOTHING
//     no weave enumeration, no role directory, no registry read
//     no journal, so no delivery outcomes: it cannot tell you a send landed
//     no way to speak as anybody else, because there is no verb for it
//
// A terminal is powerful because it can ask for authority and say true things
// about what it knows — not because it stands outside Loom. Every one of the
// absences above is a power some other component in this tree legitimately has,
// held by something that legitimately IS the host.
//
// TWO IDENTITIES, ONE SCREEN. A presentation may show a governed session and an
// operator seat side by side; `TerminalDesk` is that pairing, and it refuses to
// pair a participant with itself. The same keyboard does not imply the same
// sender, and nothing here will ever re-route one participant's command through
// the other.

#include <zen/switchboard/weave_contract.hpp>
#include <zen/terminal/composer.hpp>
#include <zen/terminal/transcript.hpp>
#include <zen/terminal/vocabulary.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

/// The conventional office a policy delegate holds (the Weaver's reference
/// wiring binds this one). AN ADDRESS, NEVER A POWER: naming it grants nothing,
/// reaching it proves nothing, and a host is free to use another.
inline constexpr const char* kPolicyOffice = "loom.weaver";

/// WHERE A MESSAGE IS GOING — the three modes Loom's ordinary participant surface
/// has, and no fourth. In particular there is no "target 0": an unaddressed send
/// is not a mode, it is a mistake, and the core refuses it locally.
struct Address {
    Addressing mode = Addressing::Weave;
    WeaveId target{};
    std::string role;

    static Address to_weave(WeaveId id) { return Address{Addressing::Weave, id, {}}; }
    static Address to_role(std::string office) {
        return Address{Addressing::Role, WeaveId{}, std::move(office)};
    }
    static Address to_all() { return Address{Addressing::Publish, WeaveId{}, {}}; }

    /// Is this address expressible at all? An address is not authority — a
    /// perfectly well-formed one is refused at delivery all the time.
    bool well_formed() const noexcept {
        switch (mode) {
        case Addressing::Weave:
            return target.valid();
        case Addressing::Role:
            return !role.empty();
        case Addressing::Publish:
            return true;
        }
        return false;
    }
};

/// THE ONE OUTBOUND DOOR A TERMINAL PARTICIPANT IS GIVEN, bound to ONE identity.
///
/// A weave's own `Bus` exists only for the duration of a delivery — it is handed
/// to `handle()` and is gone when the handler returns — so a participant driven
/// by a keyboard rather than by an incoming message has nothing to speak through.
/// That is a real gap in the substrate's authoring surface, and this is the
/// narrowest honest bridge across it.
///
/// IT NAMES AN IDENTITY. IT CONFERS NO AUTHORITY. Every message that leaves here
/// is stamped with `self()` by the bus and authorized, at delivery, against THAT
/// weave's own effective authority — baseline union delegated — by the same
/// predicates every other delivery is checked with. So a channel handed to a
/// participant with an empty grant can say nothing at all, and a channel is never
/// a way around the Kernel. What it removes is the requirement to be inside a
/// handler; what it does not remove is a single check.
///
/// AND THERE IS NO VERB FOR SPEAKING AS SOMEBODY ELSE. `self()` is fixed when the
/// host binds the channel; no method takes a sender. A terminal core holding one
/// of these cannot impersonate the operator seat beside it, cannot speak as the
/// host, and cannot widen itself — not because it is refused, but because the
/// sentence has nowhere to put the other identity.
///
/// The implementation is HOST WIRING (`zen/host/terminal_wiring.hpp`), which is
/// where the `Switchboard&` lives. Nothing in this file knows that type exists.
class ParticipantChannel {
public:
    virtual ~ParticipantChannel() = default;

    ParticipantChannel(const ParticipantChannel&) = delete;
    ParticipantChannel& operator=(const ParticipantChannel&) = delete;

    /// The one weave every message from this channel is stamped as. Fixed at
    /// binding; there is no setter.
    virtual WeaveId self() const noexcept = 0;

    /// Author `payload` to one exact weave. The returned Ticket is a HOST-SIDE
    /// journal handle: a participant cannot read an outcome from it, and this
    /// core never tries — see the sender-fate seam.
    virtual Ticket send(WeaveId target, Value payload, std::uint64_t correlation) = 0;

    /// Author `payload` to whoever holds `office` AT DELIVERY.
    virtual Ticket send_to_role(std::string_view office, Value payload,
                                std::uint64_t correlation) = 0;

    /// Author `payload` to every accepter of its shape; returns the fanout count.
    /// That count is the one delivery fact an ordinary sender does get, and it
    /// says how many deliveries were QUEUED — never how many landed.
    virtual std::size_t publish(Value payload, std::uint64_t correlation) = 0;

protected:
    ParticipantChannel() = default;
};

/// WHY A TERMINAL OPERATION DID OR DID NOT HAPPEN — LOCALLY.
///
/// Every value here is something this participant decided about its own act,
/// before anything was authored. None of them is a Loom refusal: a remote refusal
/// arrives as a MESSAGE, if it arrives at all, and shows up in the transcript as
/// one. Collapsing the two would teach a reader that "refused" means the far end
/// said no, which for an ordinary sender is exactly what it never means.
enum class TerminalOutcome : std::uint8_t {
    Submitted,     ///< authored and handed to Loom. Its FATE IS NOT KNOWN HERE.
    NotAttached,   ///< this participant has no identity yet; nothing was authored
    UnknownShape,  ///< not in this participant's vocabulary (which is not the world's)
    BadArguments,  ///< the composer refused: no such field, wrong type, assigned twice
    NeedsInput,    ///< the ladder will not guess; the open fields are named
    BadAddress,    ///< no target, or an empty office, or a publication used as an ask
    TooManyAsks,   ///< this participant already holds the most conversations it will track
    NoSuchAsk,     ///< cancel named an ask that is not outstanding
};

const char* name_of(TerminalOutcome outcome) noexcept;

/// The local result of one terminal operation.
struct TerminalResult {
    TerminalOutcome outcome = TerminalOutcome::NotAttached;
    std::string detail;                 ///< prose for a person; empty on success
    std::vector<FieldDesc> open_fields; ///< NeedsInput: what is still unfilled
    std::vector<std::string> unplaced;  ///< NeedsInput: values the ladder could not place
    std::uint64_t ask = 0;              ///< ask(): the local number now outstanding
    std::uint64_t entry = 0;            ///< the transcript entry this act recorded

    explicit operator bool() const noexcept { return outcome == TerminalOutcome::Submitted; }
};

/// ONE CONVERSATION THIS PARTICIPANT IS WAITING ON.
///
/// `correlation` is the load-bearing field and it is LOOM'S, not an invention of
/// this core: an answer is delivered carrying the correlation of the ask it
/// answers (`Switchboard::enqueue_answer`), for the immediate and the deferred
/// path alike. So "which of my asks is this?" is answered by Loom's own record,
/// exactly as "is this a real answer at all?" is. `id` is only the small number a
/// person types.
struct PendingAsk {
    std::uint64_t id = 0;
    std::uint64_t correlation = 0;
    std::string shape;
    std::uint32_t version = 0;
    Addressing addressing = Addressing::Weave;
    WeaveId target{};
    std::string role;
    std::uint64_t submitted = 0; ///< the observation seq of the Submitted entry
};

/// The most conversations one participant will track at once.
///
/// A BOUND, NOT A LIMITATION OF LOOM. Loom correlates any number; this is the
/// terminal refusing to grow an unbounded map because a user held down a key. The
/// (N+1)th ask is refused LOCALLY and nothing is authored, so the N already
/// outstanding are untouched — a new ask must never be able to displace a
/// conversation somebody is waiting on.
inline constexpr std::size_t kMaxOutstandingAsks = 8;

/// AN ORDINARY LOOM WEAVE THAT A PRESENTATION CAN DRIVE.
///
/// Owned by the bus, like every weave (`host_mount_terminal`). A presentation
/// holds a reference and reads snapshots; it never owns the participant's
/// lifetime by accident, and closing a pane therefore kills nothing. Ending the
/// participant is the host's explicit act, and when it happens the delegated
/// authority ends with it — a WeaveId is never reused, so nothing can inherit it.
class TerminalSession final : public loom::Weave {
public:
    /// `label` is what a presentation calls this participant ("session",
    /// "operator"). It is a NAME FOR A PERSON TO READ and nothing else: it is
    /// never sent, never compared, and confers no authority — the identity that
    /// matters is the WeaveId the host registered.
    ///
    /// `order` lets two participants share one observation counter so a merged
    /// chronology means something (see ObservationOrder). Omitted, this
    /// participant counts alone.
    TerminalSession(std::string label, TerminalVocabulary vocabulary,
                    std::shared_ptr<ObservationOrder> order = nullptr);
    ~TerminalSession() override;

    /// Bind this participant's one outbound door. Called by host wiring after
    /// registration, because the identity does not exist until then.
    void attach(std::unique_ptr<ParticipantChannel> channel);
    bool attached() const noexcept { return channel_ != nullptr; }

    /// This participant's WeaveId, or the invalid id before it is attached.
    WeaveId id() const noexcept { return self_; }
    const std::string& label() const noexcept { return label_; }
    const std::shared_ptr<ObservationOrder>& order() const noexcept { return order_; }

    // ---- type knowledge ----------------------------------------------------

    const TerminalVocabulary& vocabulary() const noexcept { return vocabulary_; }

    /// Describe a shape this participant knows. Knowing a shape is never a claim
    /// that anything currently accepts it, or that this participant may send it.
    std::optional<ShapeDesc> describe(std::string_view name, std::uint32_t version) const {
        return vocabulary_.describe(name, version);
    }

    /// Run the assumption ladder WITHOUT authoring anything — for a presentation
    /// that wants to show a message before it goes.
    Composition compose(std::string_view name, std::uint32_t version,
                        const std::vector<Arg>& args) const;

    /// Resolve `$rN.field` against this participant's retained inbound messages.
    std::optional<loom::Cell> resolve_ref(const Ref& ref, std::string* error) const;

    // ---- acting ------------------------------------------------------------

    /// FIRE AND FORGET. Author one message; expect no answer. On success the
    /// transcript says SUBMITTED, which is the whole truth available: an ordinary
    /// sender is not told whether its message was delivered.
    TerminalResult send(const Address& to, std::string_view name, std::uint32_t version,
                        const std::vector<Arg>& args);

    /// AUTHOR ONE MESSAGE AND WAIT FOR LOOM'S OWN ANSWER TO IT.
    ///
    /// An ask is an ordinary gated send that this participant additionally
    /// remembers, keyed by the correlation Loom will echo back. Nothing about the
    /// message is special, no protocol is added, and the far end is not obliged to
    /// answer — an ask that is never answered simply stays outstanding.
    ///
    /// A publication cannot be an ask: it has no one respondent, so there is no
    /// conversation for Loom to authorize.
    TerminalResult ask(const Address& to, std::string_view name, std::uint32_t version,
                       const std::vector<Arg>& args);

    /// ASK A POLICY OFFICE FOR ONE EXACT SEND RIGHT — sugar, and only sugar, over
    /// `ask(to_role(office), "zen.RequestAuthority" v1, {shape, version, to_role,
    /// purpose})`.
    ///
    /// There is no Weaver in this core: no include, no type, no branch. The office
    /// is an address, the request is an ordinary composed message out of the
    /// vocabulary the host supplied, and if the host did not supply that shape this
    /// is an ordinary UnknownShape. Approval, when it comes, grants authority and
    /// PERFORMS NOTHING — see `ask`'s answer handling, which stores a fact and
    /// sends nothing at all.
    TerminalResult request_authority(std::string_view shape, std::int64_t version,
                                     std::string_view to_role, std::string_view purpose,
                                     std::string_view office = kPolicyOffice);

    /// Ask a policy office what this participant's authority currently is. The same
    /// sugar, over `zen.DescribeAuthority v1`. The answer is rendered by the
    /// office from the Kernel's own values; this core keeps no authority state of
    /// its own and could not, because it has nothing to read.
    TerminalResult describe_authority(std::string_view office = kPolicyOffice);

    /// STOP WAITING — locally, and only locally.
    ///
    /// There is no cancellation vocabulary in Loom, so this cancels nothing at the
    /// far end: whatever was asked may still be being done, and its answer may
    /// still arrive. If it does it is recorded as the authenticated answer it is,
    /// matched to no outstanding ask.
    TerminalResult cancel_ask(std::uint64_t ask_id);

    // ---- conversation state (never derived from the transcript) -------------

    bool awaiting() const noexcept { return !pending_.empty(); }
    std::size_t outstanding() const noexcept { return pending_.size(); }
    std::vector<PendingAsk> pending() const { return pending_; }
    /// Is this ask still outstanding? False for a settled, cancelled or unknown
    /// one — a presentation's await loop stops on false either way, and the
    /// transcript says which it was.
    bool waiting_on(std::uint64_t ask_id) const noexcept;

    // ---- what this participant knows ---------------------------------------

    const Transcript& transcript() const noexcept { return transcript_; }

    /// The retained inbound message with this stable id, or nullopt.
    std::optional<ReceivedMessage> received(std::uint64_t id) const {
        return transcript_.received(id);
    }

    /// Record that the presentation gave this participant a command. Local truth,
    /// recorded so a chronology reads as a session rather than as a list of
    /// effects with no causes.
    std::uint64_t record_command(std::string text);
    /// Record a local statement of fact by the presentation.
    std::uint64_t record_notice(std::string text);

    // ---- the Weave contract -------------------------------------------------

    std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const override;
    /// THE HANDLER THAT SENDS NOTHING. It records what arrived, matches an answer
    /// to an outstanding ask by Loom's correlation, and returns. There is no reply
    /// here, no retry here, and no branch that authors a message — which is what
    /// makes "approval never replays intent" a property of the shape of this
    /// function rather than a rule somebody has to keep.
    void handle(const loom::Message& in, loom::Bus& bus) override;
    loom::Value snapshot() const override;
    loom::Value policy() const override;
    void revive(const loom::Value& state) override;

private:
    class Source; ///< the ComposeSource view of this participant (schema + ref lookup)

    TerminalResult author(const Address& to, std::string_view name, std::uint32_t version,
                          const std::vector<Arg>& args, bool as_ask);
    std::uint64_t record_local(TranscriptKind kind, std::string text);

    std::string label_;
    TerminalVocabulary vocabulary_;
    std::shared_ptr<ObservationOrder> order_;
    std::unique_ptr<ParticipantChannel> channel_;
    WeaveId self_{};

    Transcript transcript_;
    std::vector<PendingAsk> pending_; ///< bounded by kMaxOutstandingAsks
    std::uint64_t correlation_ = 0;    ///< this participant's own, monotonic, never 0
    std::uint64_t next_ask_ = 0;
    std::int64_t received_count_ = 0;  ///< total observed, for the state snapshot
    std::int64_t submitted_count_ = 0; ///< total authored, likewise
};

/// One entry of a merged chronology, carrying the lens that knows it.
struct DeskEntry {
    std::string lens; ///< the participant's label
    TranscriptEntry entry;
};

/// ONE PRESENTATION, TWO PARTICIPANTS — AND NEVER ONE.
///
/// A screen may show a governed session and an operator seat together; that is
/// convenient and it is not a merge. This type is where the refusal to merge them
/// lives:
///
///   - it will not pair a participant with itself, and says so at CONSTRUCTION
///     rather than at the decision that would abuse it — exactly as `loom::Weaver`
///     refuses an operator seat that is its own governed subject, and for the same
///     reason: a session that can approve its own requests makes "no weave can
///     widen its own authority" false by one line of wiring, silently;
///   - it hands out a participant only when a caller NAMES one, so authorship is
///     always an explicit choice. There is no fallback, no retry as the other
///     seat, and no notion of "the current identity" to get out of sync;
///   - its merged chronology labels every line with the lens that knows it, and
///     requires the two to share one ObservationOrder — otherwise "merged" would
///     be an interleaving this presentation made up.
class TerminalDesk {
public:
    /// Throws std::invalid_argument when the two are the same participant, when
    /// either is unattached, or when they do not share an observation order.
    TerminalDesk(TerminalSession& acting, TerminalSession& operator_seat);

    /// The governed participant — the one that does the work.
    TerminalSession& acting() const noexcept { return *acting_; }
    /// The seat whose word a policy delegate treats as the user's. A DIFFERENT
    /// weave, with a different grant, whose messages leave through its own door.
    TerminalSession& operator_seat() const noexcept { return *operator_; }

    /// Both transcripts in one chronology, oldest first, each line labelled with
    /// the participant that knows it. BY VALUE — a presentation may hold it across
    /// anything.
    std::vector<DeskEntry> chronology() const;

private:
    TerminalSession* acting_;
    TerminalSession* operator_;
};

} // namespace loom

#endif // ZEN_TERMINAL_SESSION_HPP
