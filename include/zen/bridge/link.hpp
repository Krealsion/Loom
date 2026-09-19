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
// carrying the far address and the serialized message. What comes back is not enveloped: the
// link re-admits the far payload through the LOCAL gate and delivers it to the asker as an
// ordinary message -- the shape the asker declared it accepts, under the correlation the asker
// chose, stamped by the local bus as the LINK'S speech. An asker settles it exactly as it settles
// any answer: `loom::AskBook` over (correlation, bus-stamped sender == the link).
//
// WHAT THE LINK SAYS BESIDE AN ANSWER. Every crossing has more outcomes than "answered", and a
// link must not invent one it does not have. `Outcome` is the link's own word to the asker for
// the ones that are not a far participant's answer:
//
//   Refused          the far host dropped the send before its bus (SendRefused: malformed,
//                    an unknown far shape, its gate, or this session not admitted)
//   DispatchRefused  the far bus refused the delivery (its `zen.DispatchRefused`, MSG-12)
//   Unlinked         there was no far session to send on -- not connected, denied, or lost --
//                    so nothing was submitted
//   Lost             the link went down AFTER the send was submitted: the outcome is UNKNOWN.
//                    Not a failure, not a refusal, and never a reason to resend on its own.
//
// Silence is the fifth outcome and has no shape: a far participant that has not answered has
// not answered, and the asker's book still holds the conversation.
//
// THE PAYLOAD CROSSES AS BYTES, BY DESIGN. A typed nested field would make every link compile
// against every shape it carries; bytes make the link a carrier and keep the meaning with the
// two participants that own it. `ask_across` is the one line an asker writes.

#include <zen/switchboard/message.hpp>
#include <zen/weave/shape.hpp>
#include <zen/serialize.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>

namespace loom::link {

/// ASK ACROSS THE LINK: deliver `payload` (serialized value bytes) to the far office `role`, or
/// to the far WeaveId `target` when `role` is empty. The correlation the asker puts on THIS
/// message is the one the far answer comes back under.
struct Ask {
    std::string role;   ///< the far office; empty means "the far WeaveId in `target`"
    std::int64_t target = 0;
    loom::Bytes payload;
    using ZenSelf = Ask;
    static constexpr const char* zen_name = "loom.link.Ask";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(role), ZEN_FIELD(target), ZEN_FIELD(payload));
    }
};

/// THE LINK'S OWN WORD ABOUT A CROSSING THAT DID NOT COME BACK AS A FAR ANSWER. Sent to the
/// asker under the ask's correlation. `state` is one of the spellings above; `reason` is the far
/// host's own sentence where there is one.
struct Outcome {
    std::string state;
    std::string reason;
    std::string shape;         ///< the shape that was sent, as the asker named it
    std::int64_t version = 0;
    using ZenSelf = Outcome;
    static constexpr const char* zen_name = "loom.link.Outcome";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(state), ZEN_FIELD(reason), ZEN_FIELD(shape),
                               ZEN_FIELD(version));
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
    std::int64_t answered = 0;    ///< far deliveries handed to a local asker
    std::int64_t outcomes = 0;    ///< link outcomes told (refused, dispatch-refused, unlinked, lost)
    std::int64_t open = 0;        ///< asks submitted and not yet settled by anything
    using ZenSelf = Status;
    static constexpr const char* zen_name = "loom.link.Status";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(endpoint), ZEN_FIELD(state),
                               ZEN_FIELD(session), ZEN_FIELD(established_name),
                               ZEN_FIELD(detail), ZEN_FIELD(submitted), ZEN_FIELD(answered),
                               ZEN_FIELD(outcomes), ZEN_FIELD(open));
    }
};

/// The office a link holds, from its name: `loom.link.<name>`.
inline std::string role_of(std::string_view link_name) {
    return "loom.link." + std::string(link_name);
}

/// Compose the envelope for one typed message to a far office.
template <class T>
Ask ask_role(std::string_view far_role, const T& msg) {
    Ask a;
    a.role = std::string(far_role);
    const std::string bytes = loom::serialize(loom::to_value(msg));
    a.payload.assign(bytes.begin(), bytes.end());
    return a;
}

/// Compose the envelope for one typed message to a far WeaveId.
template <class T>
Ask ask_target(std::uint64_t far_target, const T& msg) {
    Ask a;
    a.target = static_cast<std::int64_t>(far_target);
    const std::string bytes = loom::serialize(loom::to_value(msg));
    a.payload.assign(bytes.begin(), bytes.end());
    return a;
}

} // namespace loom::link

#endif // ZEN_BRIDGE_LINK_HPP
