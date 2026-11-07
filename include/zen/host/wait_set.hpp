// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_WAIT_SET_HPP
#define ZEN_HOST_WAIT_SET_HPP

// What a source of outside events waits on, said as a claim, and what the host's one wait tells
// it back. A source -- a window, a console reader, a child's pipes, a crossing's sockets -- claims
// its `WaitSet` as its office, its latest word; a host that rests through `loom::host::Reactor`
// (zen/host/reactor.hpp) reads the claims of the offices it trusts, waits once on all of them and
// on its own deadline, and sends each office a `Waited`. Only the host sends one: a source reads
// one from any other sender as nothing. The handles mean something only in this process.
// docs/guides/running-loom.md#a-host-that-rests-until-something-is-ready

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace loom::host {

/// WHAT A SOURCE WAITS ON, claimed as its office. `handles` are waited on for readiness and never
/// consumed by the wait (an auto-reset event is the one exception: the wait that sees it resets
/// it, so its source is told once per signal): on Windows each is a `HANDLE` waited for signalled,
/// on POSIX a descriptor waited for readable or hung up. `writable` is POSIX's: descriptors waited
/// for room to write (Windows says that through a socket's event). `messages` is Windows': the
/// dispatch thread's own message queue, for an OS window that thread owns; a POSIX host cannot
/// hold it. A claim that names nothing is held: its source has nothing to poll for until it claims
/// again. A source that cannot be waited on claims nothing it waits on and keeps polling. A source
/// claims anew after every wake it takes, unchanged or not, since a wait that finds the same claim
/// ready again holds it no longer.
struct WaitSet {
    std::vector<std::int64_t> handles;
    std::vector<std::int64_t> writable;
    bool messages = false;

    using ZenSelf = WaitSet;
    static constexpr const char* zen_name = "loom.host.WaitSet";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(handles), ZEN_FIELD(writable), ZEN_FIELD(messages));
    }
};

/// WHAT THE HOST'S WAIT SAYS TO A SOURCE'S OFFICE, about the claim of revision `revision`.
/// `held`: the wait holds it, so what it names reaches the source through the wait and the source
/// may stop polling; false says why not in `why`, and the source polls as it would with no host
/// waiting. `ready`: something it names is ready now. A source is told when the wait first holds
/// its claim, when it stops holding it, and at each wake; nothing else.
struct Waited {
    std::int64_t revision = 0;
    bool held = false;
    bool ready = false;
    std::string why;

    using ZenSelf = Waited;
    static constexpr const char* zen_name = "loom.host.Waited";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(revision), ZEN_FIELD(held), ZEN_FIELD(ready),
                               ZEN_FIELD(why));
    }
};

} // namespace loom::host

#endif // ZEN_HOST_WAIT_SET_HPP
