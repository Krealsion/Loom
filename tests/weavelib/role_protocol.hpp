// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_WEAVELIB_ROLE_PROTOCOL_HPP
#define ZEN_TESTS_WEAVELIB_ROLE_PROTOCOL_HPP

// The role-authorship parity vocabulary (MSG-07), shared by the loadable
// role-worker fixture and the native kernel suite — the same header on both
// sides of the .so seam, so the shapes cannot drift apart.

#include <zen/weave.hpp>

#include <cstdint>
#include <string>

namespace role_speech {

/// Tell the worker which act to perform. `mode` is direct, to-role, publish, personal,
/// forge-direct, forge-publish, bound-direct, bound-to-role or bound-publish. The bound
/// modes answer
/// their report, and `seen_role` is the ticket's refusal name.
struct RoleCommand {
    std::string mode;
    std::int64_t target = 0;
    ZEN_SHAPE(RoleCommand, 1, ZEN_FIELD(mode), ZEN_FIELD(target));
};

/// What the worker says — as a role, or personally, shape identical.
struct WorkerNews {
    std::string note;
    ZEN_SHAPE(WorkerNews, 1, ZEN_FIELD(note));
};

/// What the worker observed, reported back to whoever commanded it. For an
/// outbound act: `authored` is whether the authorship was accepted, and
/// `recipients` the publication fanout (0 elsewhere). For "heard": `authored`
/// is mail.authored_from_role("worker.a") on the delivery it just received and
/// `seen_role` is mail.authored_role() verbatim.
struct RoleReport {
    std::string what;
    bool authored = false;
    std::int64_t recipients = 0;
    std::string seen_role;
    ZEN_SHAPE(RoleReport, 1, ZEN_FIELD(what), ZEN_FIELD(authored), ZEN_FIELD(recipients),
              ZEN_FIELD(seen_role));
};

} // namespace role_speech

#endif // ZEN_TESTS_WEAVELIB_ROLE_PROTOCOL_HPP
