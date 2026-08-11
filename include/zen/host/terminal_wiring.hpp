// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_TERMINAL_WIRING_HPP
#define ZEN_HOST_TERMINAL_WIRING_HPP

// HOST WIRING — not part of the terminal-authoring surface.
//
// The same discipline `grant_wiring.hpp` and `lifecycle_wiring.hpp` keep, for the
// same reason: everything in this tree that needs a `Switchboard&` should be
// findable in one grep, in a file no participant-side header includes. A terminal
// core that could reach the Switchboard would be a host wearing a terminal's name,
// so the one expression that touches it lives here instead.
//
// WHAT IT MINTS, AND WHAT THAT IS NOT. `host_participant_channel` produces an
// outbound door bound to one WeaveId. It is built on `Switchboard::send_as`,
// which is host root authority — the host chooses whose identity the door carries
// — and every message that leaves it is then authorized AT DELIVERY against that
// weave's own effective authority, by the same predicates every other delivery
// is checked with. So:
//
//     the HOST decides WHO a terminal participant is
//     the KERNEL decides WHAT that participant may say
//
// and neither can be talked out of its half. Handing a participant a channel
// widens nothing: a channel bound to a weave with an empty grant can say nothing
// at all, and there is no verb on it that takes a sender, so the participant it
// was given to cannot speak as anybody else.
//
// WHY IT EXISTS AT ALL. A weave's own `Bus` is handed to `handle()` and is gone
// when the handler returns, so a participant driven by a keyboard has no way to
// speak: nothing delivers it a message when a person presses return. The
// alternative — having the presentation root-send a "do this" message into the
// participant so it can speak from inside a handler — was rejected, and the
// reason is the whole phase: it makes every terminal command a wire shape, and it
// makes the presentation a root sender. This is the narrower bridge.

#include <zen/switchboard/grant.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/terminal/session.hpp>

#include <memory>
#include <string>

namespace loom {

/// Bind an outbound door to `who`, on `bus`.
///
/// Requires the Switchboard by reference — which is the boundary, not a
/// formality: only something that already holds the host's authority can decide
/// which identity a door carries.
std::unique_ptr<ParticipantChannel> host_participant_channel(Switchboard& bus, WeaveId who);

/// What a host keeps after mounting a terminal participant.
///
/// The bus OWNS the participant, like every weave. `session` is non-owning, so a
/// presentation holding it can be built and destroyed without ending the
/// participant — closing a pane kills nothing — and ending the participant stays
/// the host's explicit act (`unregister_weave`), after which this pointer must
/// not be used.
struct MountedTerminal {
    WeaveId id{};
    TerminalSession* session = nullptr;
};

/// Register `session` with `grant` as its admission baseline, then bind its door.
///
/// The two steps are in this order because they must be: the identity does not
/// exist until the bus assigns it, and the door carries the identity.
MountedTerminal host_mount_terminal(Switchboard& bus, std::unique_ptr<TerminalSession> session,
                                    Grant grant);

/// As above, and bind the participant to `role`. A role is an address, never a
/// power — a terminal that holds one has gained nothing but a name others can
/// send to.
MountedTerminal host_mount_terminal(Switchboard& bus, std::unique_ptr<TerminalSession> session,
                                    Grant grant, std::string role);

} // namespace loom

#endif // ZEN_HOST_TERMINAL_WIRING_HPP
