// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_AUTHORITY_COMMAND_HPP
#define ZEN_HOST_AUTHORITY_COMMAND_HPP

// The `authority` command: the words a person decides with, over an `AuthorityStore`, the same
// in every host that keeps its decisions there. `loom-host`'s console answers it, and so can any
// host built from the installed package. The command owns the grammar, every sentence it prints
// and the order of each change: a decision is written to the store first, and only a write that
// landed is installed on the bus. What it asks of the host is one thing the host alone can do:
// carry a question from its operator seat to its warden and bring back the answer Loom attributed
// to it (`AuthorityDesk`), because which participant is the seat, and how it waits, are the host's.

#include <zen/host/authority.hpp>
#include <zen/host/warden.hpp>
#include <zen/value.hpp>

#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace loom::host {

/// What one question to the warden came to. `reply` is the answer Loom attributed to exactly
/// this question (its correlation and the bus-stamped sender, the warden); without one,
/// `trouble` says why in the host's own words -- refused at the gate, still pending, or never
/// sent -- and the command prints it as it stands.
struct WardenAnswer {
    std::optional<Value> reply;
    std::string trouble;
};

/// The host's half of the command: two conversations from its operator seat, the participant
/// the warden was built to obey (`HostWarden`'s `seat`). Each sends one message to the warden
/// and returns what it came to; neither prints, decides or retries anything.
class AuthorityDesk {
public:
    virtual ~AuthorityDesk() = default;

    /// Send `zen.host.AuthoritySync{artifact}`: make the bus agree with the store for one
    /// artifact. Asked only for an artifact the warden governs.
    virtual WardenAnswer sync(const std::string& artifact) = 0;

    /// Send `zen.host.DescribeAuthority{artifact}`: what the bus permits it now, baseline and
    /// delegated apart.
    virtual WardenAnswer describe(const std::string& artifact) = 0;
};

/// Run one `authority ...` line and print its answer to `out`. `words` are the line as the host
/// split it, `words[0]` being `authority`. `warden` is the host's, asked only whether it governs
/// an artifact, so that a decision about one nothing has loaded says so instead of asking.
void run_authority_command(AuthorityStore& store, const HostWarden& warden, AuthorityDesk& desk,
                           const std::vector<std::string>& words, std::ostream& out);

/// The grammar's lines for a host's `help`, each ending in a newline.
const char* authority_help();

} // namespace loom::host

#endif // ZEN_HOST_AUTHORITY_COMMAND_HPP
