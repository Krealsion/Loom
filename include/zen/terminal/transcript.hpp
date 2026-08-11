// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TERMINAL_TRANSCRIPT_HPP
#define ZEN_TERMINAL_TRANSCRIPT_HPP

// WHAT ONE PARTICIPANT ACTUALLY KNOWS — a record of its own experience, and
// nothing else.
//
// THE ONE RULE THIS FILE EXISTS TO KEEP: every entry is a fact this participant
// legitimately came by. A command it was given. A message it authored. A message
// that was actually delivered to it. Loom's own word that one of those answers an
// ask it made. It is NOT fed by `Switchboard::add_observer`, so it never contains
// another weave's traffic, another weave's refusals, or a delivery outcome the
// sender is not told (see `submitted` below). A transcript that quietly mixed
// host-lens facts into a participant's own record would be the most convincing
// lie in the system, because every line would look the same.
//
// SUBMITTED IS NOT DELIVERED, and the vocabulary refuses to blur them:
//
//   SUBMITTED  I authored this and Loom took it. Whether it was delivered,
//              refused at the gate, refused for want of authority, or dropped for
//              want of a target, I DO NOT KNOW — Loom does not tell a sender its
//              send's fate, and this participant holds no journal and no tap.
//   RECEIVED   this arrived here, past this participant's own door.
//   ANSWERED   ...and Loom itself says it answers an ask I made. Not a message
//              that merely looks like a reply: the provenance is Loom's, and no
//              sender can write it.
//
// The console can say "delivered" because a console holds a `Switchboard&` and
// reads the journal. This cannot, and does not.
//
// A MODEL, NOT OUTPUT. Entries carry structured facts — kind, shape identity,
// the bus-stamped sender, the authored office, the correlation, a stable id for
// the received value — so a terminal renderer, a graphical pane and a future
// executor can each present the same record without one of them having to parse
// another's strings. Nothing here is coloured, wrapped, or escaped; escaping is
// the business of whichever renderer has a terminal to protect.

#include <zen/bounded_history.hpp>
#include <zen/schema.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/value.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

/// THE ORDER THIS PRESENTATION OBSERVED EVENTS IN — and never a global bus order.
///
/// Two participants driven by one host loop can be given the SAME counter, and
/// then a merged chronology means something exact: "this is the sequence in which
/// this presentation learned these things". It is emphatically not "the order
/// Loom did them" — a participant sees its own deliveries and its own authorship,
/// with everything else in between invisible to it. Claiming a canonical global
/// history would need the tap, which is host authority.
class ObservationOrder {
public:
    std::uint64_t next() noexcept { return ++n_; }
    std::uint64_t observed() const noexcept { return n_; }

private:
    std::uint64_t n_ = 0;
};

/// What kind of thing one transcript entry is. Six, and the distinctions between
/// them are exactly the distinctions a person must not have to infer from prose.
enum class TranscriptKind : std::uint8_t {
    /// The presentation asked this participant to do something. Local, always true.
    LocalCommand,
    /// This participant refused, ITSELF, before authoring anything. A local
    /// refusal is not a Loom refusal and never becomes one: nothing was sent, so
    /// nothing was denied.
    LocalRefusal,
    /// A local statement of fact ("stopped waiting", "vocabulary listed").
    LocalNotice,
    /// A message this participant authored and Loom accepted for delivery. Its
    /// FATE IS UNKNOWN HERE.
    Submitted,
    /// A message that was actually delivered to this participant.
    Received,
    /// ...and Loom's provenance says it is the authorized answer to an ask this
    /// participant sent.
    AnswerReceived,
};

const char* name_of(TranscriptKind kind) noexcept;

/// Where a message was addressed. The three modes Loom's ordinary participant
/// surface has, and no fourth.
enum class Addressing : std::uint8_t {
    Weave,   ///< one exact WeaveId
    Role,    ///< whichever weave holds an office AT DELIVERY
    Publish, ///< every accepter of the shape
};

const char* name_of(Addressing mode) noexcept;

/// ONE OBSERVATION, WITH ITS PROVENANCE KEPT APART FROM ITS PAYLOAD.
///
/// The trusted fields below come from Loom and cannot be written by a sender:
/// `sender` is the bus stamp, `authored_role` is an office Loom verified at the
/// authorship moment, and `answers_ask` is provenance no ordinary enqueue can
/// produce. A payload field that happens to be called "requester" is NOT one of
/// them and never gets copied into one — that conflation is the exact mistake the
/// Weaver vocabulary is shaped to prevent, and re-making it one layer up would
/// undo it.
struct TranscriptEntry {
    /// This presentation's observation order (see ObservationOrder).
    std::uint64_t seq = 0;
    TranscriptKind kind = TranscriptKind::LocalNotice;
    /// WHICH PARTICIPANT KNOWS THIS. A merged visual chronology is useful; a
    /// merged identity is not, so every entry names its own lens.
    WeaveId lens{};

    /// Local prose, for the three local kinds. Empty for message entries — a
    /// renderer builds their text from the structured facts, so two renderers
    /// cannot disagree about what a message entry says by disagreeing about a
    /// string this core happened to bake.
    std::string text;

    // ---- message facts (Submitted / Received / AnswerReceived) --------------
    std::string shape;             ///< the payload's shape name
    std::uint32_t version = 0;     ///< ...and its version
    std::uint64_t correlation = 0; ///< Loom's correlation; on an answer it is the ASK's own

    /// Submitted: where it was addressed. Meaningless for inbound entries.
    Addressing addressing = Addressing::Weave;
    WeaveId target{};  ///< Submitted, Addressing::Weave
    std::string role;  ///< Submitted, Addressing::Role
    std::size_t recipients = 0; ///< Submitted, Addressing::Publish — the fanout count Loom returned

    /// Received: the BUS-STAMPED sender. Trusted, and not a payload field.
    WeaveId sender{};
    /// Received: the office the sender DELIBERATELY spoke as, verified by Loom at
    /// authorship — or empty, which means exactly "spoken personally".
    std::string authored_role;
    /// Received: Loom's own provenance. True only for an authorized answer.
    bool answers_ask = false;
    /// AnswerReceived: the local ask number this answers, or 0 when it answers no
    /// ask this participant is still tracking (a cancelled one, or one already
    /// settled). An authenticated answer with nothing to match is still an
    /// authenticated answer; it is not evidence of anything else.
    std::uint64_t answers = 0;

    /// Received: the stable id of the retained payload (`received(id)`), or 0 if
    /// the value was not retained. An id is an IDENTITY, not a position: once the
    /// received store evicts it, the id refuses rather than re-binding to a newer
    /// message.
    std::uint64_t message = 0;
};

/// A retained inbound message: its stable id and the EXACT Value that arrived.
///
/// The value is kept verbatim — not escaped, not truncated, not normalized. A
/// core that sanitized what it stored would be deciding, once and for everybody,
/// what every future renderer is allowed to see. Escaping is the renderer's job
/// and happens at the last moment (see `safe_terminal_text`).
struct ReceivedMessage {
    ReceivedMessage(WeaveId sender_, std::string authored_role_, bool answers_ask_,
                    std::uint64_t correlation_, loom::Value value_)
        : sender(sender_), authored_role(std::move(authored_role_)), answers_ask(answers_ask_),
          correlation(correlation_), value(std::move(value_)) {}

    std::uint64_t id = 0; ///< assigned by Transcript::retain; stable for life
    WeaveId sender{};     ///< the bus stamp — trusted, never a payload field
    std::string authored_role; ///< the office Loom verified at authorship, or empty
    bool answers_ask = false;
    std::uint64_t correlation = 0;
    loom::Value value;
};

/// Entries retained by a participant's transcript. Wide, because an entry is a
/// few short strings and a person scrolling back wants a session's worth.
inline constexpr std::size_t kTranscriptCapacity = 256;

/// Inbound VALUES retained for inspection and for `$rN.field` references. Four
/// times smaller than the transcript because the UNIT is far heavier: an entry is
/// metadata, while a received Value is bounded only by the decode materialization
/// budget. The same split, and the same reasoning, as the console's tap and reply
/// windows.
inline constexpr std::size_t kReceivedCapacity = 64;

/// A participant's own record: a bounded window of entries, plus a bounded window
/// of the values it received.
///
/// EVICTION CANNOT COST CONVERSATION STATE. Pending asks live in the session, not
/// in here, precisely so that scrolling past the horizon can never lose the fact
/// that this participant is still waiting for an answer. A transcript is history;
/// history may be forgotten. An outstanding conversation is not history.
class Transcript {
public:
    void record(TranscriptEntry entry) { entries_.push(std::move(entry)); }

    /// Retain one inbound value and return its stable id.
    std::uint64_t retain(ReceivedMessage message) {
        const std::uint64_t id = ++next_id_;
        message.id = id;
        received_.push(std::move(message));
        return id;
    }

    /// Everything retained, oldest first. BY VALUE: a presentation may hold this
    /// across anything at all, including the deliveries that evict its oldest
    /// entries and the destruction of the participant itself.
    std::vector<TranscriptEntry> entries() const { return entries_.snapshot(); }

    /// The most recent `n` retained entries, oldest first.
    std::vector<TranscriptEntry> tail(std::size_t n) const;

    /// The retained inbound message with this stable id, or nullopt when it never
    /// arrived or was evicted. Never another message.
    std::optional<ReceivedMessage> received(std::uint64_t id) const;

    std::size_t size() const noexcept { return entries_.size(); }
    std::uint64_t evicted() const noexcept { return entries_.evicted(); }
    std::size_t received_size() const noexcept { return received_.size(); }
    std::uint64_t received_evicted() const noexcept { return received_.evicted(); }
    /// The id of the newest retained inbound message (0 if none ever arrived).
    std::uint64_t last_received_id() const noexcept { return next_id_; }

private:
    friend struct TerminalHistoryProbe;

    BoundedHistory<TranscriptEntry, kTranscriptCapacity> entries_;
    BoundedHistory<ReceivedMessage, kReceivedCapacity> received_;
    std::uint64_t next_id_ = 0;
};

/// RENDER `raw` SO IT CANNOT DRIVE THE TERMINAL IT IS PRINTED ON.
///
/// A general message terminal displays strings somebody else wrote, so a `Text`
/// field is an injection surface: an escape sequence could reposition the cursor,
/// erase the line above, or repaint the trusted facts a person is reading.
///
/// IT IS A RENDERER'S RULE, NOT A WIRE LAW, and the difference is the point.
/// Nothing in this core mutates a message: values are retained exactly as they
/// arrived, and the escaping happens at the last moment, in the one presentation
/// that has a terminal to protect. A graphical pane draws text as glyphs and must
/// NOT use this — it would show a person backslash-escapes for no reason.
///
/// IT KEEPS UTF-8, deliberately, and this is where it differs from the Weaver's
/// `safe_operator_text`. That one escapes every non-ASCII byte because it guards
/// one small security-critical surface where being unreadable is cheaper than
/// being wrong. A terminal that did the same would corrupt every ordinary message
/// carrying a name, so this escapes exactly the bytes that can steer a terminal —
/// C0 controls and DEL — and passes everything at 0x80 and above through
/// untouched. That is sufficient: no UTF-8 sequence can encode a C0 byte, because
/// every continuation byte is >= 0x80.
std::string safe_terminal_text(std::string_view raw);

} // namespace loom

#endif // ZEN_TERMINAL_TRANSCRIPT_HPP
