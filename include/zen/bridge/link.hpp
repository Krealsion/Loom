// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_BRIDGE_LINK_HPP
#define ZEN_BRIDGE_LINK_HPP

// HOW AN ORDINARY WEAVE SPEAKS ACROSS A LINK, and what it hears back.
//
// A host that links to another host mounts a LINK -- an ordinary participant of its own that
// holds the socket (`zen/bridge/client.hpp`) and an office local weaves address. A local weave
// cannot name a far participant in its own vocabulary: the far office is a string only the
// crossing understands, so the ask crosses the local bus inside an ENVELOPE the link accepts,
// carrying the far address and the serialized message.
//
// ONE CROSSING, ONE ANSWER, WITH LOOM'S WORD ON IT. The link answers every `Ask` exactly once,
// through Loom's own answer door -- `mail.answer` when it can say at once, a deferred answer
// (ANS-02) when the far side has to speak first -- so what the asker receives is attested by
// THIS bus as the answer to the asker's own ask of the link (`mail.answers_ask()`), bound by
// Loom to the asker's correlation and the asker's incarnation. The answer is one of:
//
//   the far owner's answer   the far bus attested it as the answer to this crossing; it is
//                            re-admitted through THIS bus's gate and handed over as the shape
//                            the asker declared it accepts
//   `Outcome`                the link's own word for a crossing that did not come back that way
//
// Nothing else the far side says is an answer. A far participant's ORDINARY word to the link's
// session -- even under this crossing's correlation, even in the very shape the asker expects
// -- is not handed to the asker at all; it is recorded (below) and settles nothing. A payload in
// the link's own vocabulary (`loom.link.*`) is never handed over as an answer either, so a far
// owner cannot speak in the link's voice.
//
// WHICH CROSSING. The asker's correlation names the conversation on THIS bus and nowhere else:
// two askers each keep a book, and each book's first correlation is 1. The link puts its OWN
// number on the wire -- the crossing's `attempt`, minted by the link, never reused in its life
// -- and translates the far reply back to the asker and conversation that asked. Each session
// the link opens is a new epoch: a reconnect ends every crossing of the old one as `lost`, and
// nothing that arrived for the old session can settle a crossing of the new.
//
// THE OUTCOMES, kept apart because they send an asker to different places:
//
//   Refused          nothing was submitted: this link refused the ask (no correlation, too many
//                    open, no room for another unfinished conversation), or the far host dropped
//                    the send before its bus (SendRefused: malformed, an unknown far shape, its
//                    gate, this session not admitted, no settlement it could follow)
//   DispatchRefused  the far bus refused the delivery (its `zen.DispatchRefused`, MSG-12)
//   Unlinked         there was no far session to send on -- not connected, denied, or lost --
//                    so nothing was submitted
//   Lost             the link went down AFTER the send was submitted: the outcome is UNKNOWN.
//                    Not a failure, not a refusal, and never a reason to resend on its own.
//
// Silence is the fifth outcome and has no shape: a far participant that has not answered has
// not answered, and the asker's book still holds the conversation.
//
// SETTLEMENT. An ask with `settle` is answered only once the far host has ALSO said that what
// the ask set in motion on its bus has all been dispatched (`loom::Fence`, the bridge's
// `Settled`): the far owner's answer is held until then. An asker that injects input and then
// wants a picture of what the input did asks for this, and asks for the picture after it. It
// says nothing about work the far side deferred -- a timer, a later turn, another host.
//
// WHAT THE LOCAL HISTORY KEEPS. Every frame the far host ships to the link's session becomes a
// `Crossed` record the link says to ITSELF on this bus, before it acts on it: the far session,
// the name the far host established, the far bus's stamp and office, the attempt, what kind of
// frame it was and the bytes. The host's Recorder and Logger see it as the ordinary delivery it
// is -- the link's account of what arrived, never a local observation of the far execution --
// and an answer the link then hands on names that record as its dispatch parent. Only the
// link's own `Crossed` acts; anybody else's is data.
//
// THE PAYLOAD CROSSES AS BYTES, BY DESIGN. A typed nested field would make every link compile
// against every shape it carries; bytes make the link a carrier and keep the meaning with the
// two participants that own it. `ask_role` is the one line an asker writes.

#include <zen/switchboard/message.hpp>
#include <zen/weave/shape.hpp>
#include <zen/serialize.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>

namespace loom::link {

/// ASK ACROSS THE LINK: deliver `payload` (serialized value bytes) to the far office `role`, or
/// to the far WeaveId `target` when `role` is empty. The asker's correlation on THIS message is
/// the one its answer comes back under; `settle` holds the answer until the far host says what
/// the ask set in motion there has been dispatched.
struct Ask {
    std::string role;   ///< the far office; empty means "the far WeaveId in `target`"
    std::int64_t target = 0;
    loom::Bytes payload;
    bool settle = false;
    using ZenSelf = Ask;
    static constexpr const char* zen_name = "loom.link.Ask";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(role), ZEN_FIELD(target), ZEN_FIELD(payload),
                               ZEN_FIELD(settle));
    }
};

/// THE LINK'S OWN WORD ABOUT A CROSSING THAT DID NOT COME BACK AS A FAR ANSWER, answered with
/// Loom's answer authority under the asker's correlation. `state` is one of the spellings below;
/// `reason` is the far host's own sentence where there is one.
struct Outcome {
    std::string state;
    std::string reason;
    std::string shape;         ///< the shape that was sent, as the asker named it
    std::int64_t version = 0;
    std::int64_t attempt = 0;  ///< the link's number for the crossing; 0 when nothing was submitted
    using ZenSelf = Outcome;
    static constexpr const char* zen_name = "loom.link.Outcome";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(state), ZEN_FIELD(reason), ZEN_FIELD(shape),
                               ZEN_FIELD(version), ZEN_FIELD(attempt));
    }
};

inline constexpr const char* kOutcomeRefused = "refused";
inline constexpr const char* kOutcomeDispatchRefused = "dispatch-refused";
inline constexpr const char* kOutcomeUnlinked = "unlinked";
inline constexpr const char* kOutcomeLost = "lost";

/// WHAT THE LINK KNOWS ABOUT ITSELF, answered to whoever asks it. A reading, never authority.
struct StatusRequested {
    using ZenSelf = StatusRequested;
    static constexpr const char* zen_name = "loom.link.StatusRequested";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

struct Status {
    std::string name;             ///< the link's name, as the host configured it
    std::string endpoint;         ///< where it connects
    std::string state;            ///< "connecting" / "admitted" / "denied" / "lost" / "closed"
    std::int64_t session = 0;     ///< the far host's session id for this link, when admitted
    std::string established_name; ///< the name the far host established, when admitted
    std::string detail;           ///< the far host's reason, when denied
    std::int64_t submitted = 0;   ///< asks this link put on the wire, all time
    std::int64_t answered = 0;    ///< far answers handed to a local asker
    std::int64_t outcomes = 0;    ///< link outcomes told (refused, dispatch-refused, unlinked, lost)
    std::int64_t open = 0;        ///< asks submitted and not yet answered by anything
    std::int64_t ignored = 0;     ///< far words that settled nothing: ordinary speech, late or unknown
    using ZenSelf = Status;
    static constexpr const char* zen_name = "loom.link.Status";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(endpoint), ZEN_FIELD(state),
                               ZEN_FIELD(session), ZEN_FIELD(established_name),
                               ZEN_FIELD(detail), ZEN_FIELD(submitted), ZEN_FIELD(answered),
                               ZEN_FIELD(outcomes), ZEN_FIELD(open), ZEN_FIELD(ignored));
    }
};

/// WHAT ARRIVED ON THE LINK'S SESSION, AS THE LINK READ IT: one far frame, said by the link to
/// itself so this host's history keeps it (see the header). Every number in it is the FAR
/// host's, or the link's own -- none is an identity on this bus -- and every name is the far
/// host's word.
struct Crossed {
    std::string link;          ///< the link's name
    std::int64_t epoch = 0;    ///< the link's own count of sessions: which connection this came on
    std::int64_t session = 0;  ///< the far host's session id for that connection
    std::string established;   ///< the name the far host established for that connection
    std::string kind;          ///< one of the spellings below
    std::int64_t attempt = 0;  ///< the crossing this names (the far correlation); 0 for `ended`
    std::int64_t far_sender = 0; ///< the far bus's stamp on a delivery
    std::string far_role;      ///< the office the far sender spoke as, by the far bus's word
    std::string shape;         ///< the far payload's claimed shape, when there is one
    std::int64_t version = 0;
    std::string reason;        ///< the far host's sentence, or why the session ended
    loom::Bytes payload;       ///< the far payload, as it crossed
    using ZenSelf = Crossed;
    static constexpr const char* zen_name = "loom.link.Crossed";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(link), ZEN_FIELD(epoch), ZEN_FIELD(session),
                               ZEN_FIELD(established), ZEN_FIELD(kind), ZEN_FIELD(attempt),
                               ZEN_FIELD(far_sender), ZEN_FIELD(far_role), ZEN_FIELD(shape),
                               ZEN_FIELD(version), ZEN_FIELD(reason), ZEN_FIELD(payload));
    }
};

inline constexpr const char* kCrossedAnswer = "answer";          ///< the far bus attested it
inline constexpr const char* kCrossedDispatchRefused = "dispatch-refused";
inline constexpr const char* kCrossedMessage = "message";        ///< ordinary far speech
inline constexpr const char* kCrossedSendRefused = "send-refused";
inline constexpr const char* kCrossedSettled = "settled";
inline constexpr const char* kCrossedEnded = "ended";            ///< the session is over

/// The office a link holds, from its name: `loom.link.<name>`.
inline std::string role_of(std::string_view link_name) {
    return "loom.link." + std::string(link_name);
}

/// Compose the envelope for one typed message to a far office.
template <class T>
Ask ask_role(std::string_view far_role, const T& msg, bool settle = false) {
    Ask a;
    a.role = std::string(far_role);
    const std::string bytes = loom::serialize(loom::to_value(msg));
    a.payload.assign(bytes.begin(), bytes.end());
    a.settle = settle;
    return a;
}

/// Compose the envelope for one typed message to a far WeaveId.
template <class T>
Ask ask_target(std::uint64_t far_target, const T& msg, bool settle = false) {
    Ask a;
    a.target = static_cast<std::int64_t>(far_target);
    const std::string bytes = loom::serialize(loom::to_value(msg));
    a.payload.assign(bytes.begin(), bytes.end());
    a.settle = settle;
    return a;
}

} // namespace loom::link

#endif // ZEN_BRIDGE_LINK_HPP
