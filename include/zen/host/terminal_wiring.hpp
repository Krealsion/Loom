// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_TERMINAL_WIRING_HPP
#define ZEN_HOST_TERMINAL_WIRING_HPP

// Host wiring, not part of the terminal-authoring surface: like grant_wiring.hpp and
// lifecycle_wiring.hpp, the one place a terminal needs the `Switchboard&`, in a file no
// participant-side header includes. docs/reference/terminal.md
//
// `host_participant_channel` makes an outbound door bound to one WeaveId, built on
// `Switchboard::send_as`: the host decides who a terminal participant is, and each message is
// authorized at delivery against that weave's own effective authority, which decides what it
// may say. A channel widens nothing: bound to an empty grant it says nothing, and no verb on it
// takes a sender.
//
// It exists because a weave's `Bus` lives only while its handler runs, and a participant
// driven by a keyboard has no handler running when a person presses return. Root-sending the
// participant a "do this" message instead would make every terminal command a wire shape and the
// presentation a root sender.

#include <zen/switchboard/grant.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/terminal/session.hpp>

#include <memory>
#include <string>

namespace loom {

/// Bind an outbound door to `who`, on `bus`. Needing the Switchboard is the boundary: only the
/// host decides which identity a door carries.
std::unique_ptr<ParticipantChannel> host_participant_channel(Switchboard& bus, WeaveId who);

/// What a host keeps after mounting a terminal participant. The bus owns the participant, as
/// every weave; `session` is non-owning, so a presentation holding it can come and go without
/// ending the participant. Ending it is the host's act (`unregister_weave`), after which the
/// pointer must not be used.
struct MountedTerminal {
    WeaveId id{};
    TerminalSession* session = nullptr;
};

/// Register `session` with `grant` as its admission baseline, then bind its door, in that order
/// because the door carries the identity the bus assigns.
MountedTerminal host_mount_terminal(Switchboard& bus, std::unique_ptr<TerminalSession> session,
                                    Grant grant);

/// As above, and bind the participant to `role`: an address others can send to, never a power.
MountedTerminal host_mount_terminal(Switchboard& bus, std::unique_ptr<TerminalSession> session,
                                    Grant grant, std::string role);

} // namespace loom

#endif // ZEN_HOST_TERMINAL_WIRING_HPP
