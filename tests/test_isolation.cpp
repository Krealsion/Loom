// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The B2 milestone: a Weave hosted out-of-process is indistinguishable to the bus
// from one hosted in-process, a crashing child is contained (host survives, bounded
// reload, then quarantine), and the process boundary is gated exactly like every
// other boundary — malformed child output is gate-refused, an emitted message is
// authorized against the child's grant, and the sender is stamped from the
// connection (a child cannot forge it). The host never blocks on a child.

#include <doctest.h>

#include "enforcement_gate.hpp"
#include "switchboard_fixtures.hpp"
#include "weavelib/office_protocol.hpp"

#include <zen/isolation/host.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/switchboard.hpp>

#include <cerrno>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace loom;
using namespace loom;
using namespace loom;
using namespace loom;
using namespace sbfx;

namespace {
const std::string kHostExe = ZEN_WEAVE_HOST_EXE;

// Matches the net-probe weave's emitted shape (NetResult{code}); the child reports
// the connect() errno here so a test can read it off the bus.
std::shared_ptr<const Schema> netresult_schema() {
    static const auto s = SchemaBuilder("NetResult", 1).field("code", Kind::Int).build();
    return s;
}
// Matches the fs-probe weave's emitted shape (B4): the errno of each filesystem reach.
std::shared_ptr<const Schema> fsresult_schema() {
    static const auto s = SchemaBuilder("FsResult", 1)
                              .field("secret_read", Kind::Int)
                              .field("scratch_write", Kind::Int)
                              .field("outside_write", Kind::Int)
                              .field("noexec_exec", Kind::Int)
                              .build();
    return s;
}
// Matches the fork-bomb weave's emitted shape (B5): how many forks succeeded.
std::shared_ptr<const Schema> forkresult_schema() {
    static const auto s = SchemaBuilder("ForkResult", 1).field("forked", Kind::Int).build();
    return s;
}
} // namespace

TEST_SUITE("isolation") {

TEST_CASE("out-of-process delivery and reply are indistinguishable from in-process") {
    Switchboard bus;
    Kernel kernel(bus);
    IsolationHost host(bus, kHostExe);

    Registered rec_in = register_probe(bus, {pong_schema()});
    Registered rec_out = register_probe(bus, {pong_schema()});

    // The very same .so, mounted both ways onto the same bus.
    LoadResult in = kernel.load("in", ZEN_SO_WEAVE);
    REQUIRE_MESSAGE(in.ok, in.error);
    Grant g;
    g.allow("Pong", 1, rec_out.id); // it needs exactly: send Pong to its recorder
    OutOfProcessResult out = host.mount("out", ZEN_SO_WEAVE, std::move(g));
    REQUIRE_MESSAGE(out.ok, out.error);
    CHECK(bus.alive(out.id));

    // Same Ping to each; same reply expected.
    bus.send(in.id, Message(ping(42), WeaveId{}, rec_in.id));
    bus.send(out.id, Message(ping(42), WeaveId{}, rec_out.id));

    const bool done = host.run_until(
        [&] {
            return !rec_in.weave->handled_names.empty() && !rec_out.weave->handled_names.empty();
        },
        2000);
    REQUIRE(done);

    CHECK(rec_in.weave->handled_names[0] == "Pong");
    CHECK(rec_out.weave->handled_names[0] == rec_in.weave->handled_names[0]);
    CHECK(rec_out.weave->handled_values[0] == rec_in.weave->handled_values[0]);
    CHECK(rec_out.weave->handled_values[0] == 42);
}

TEST_CASE("the sender of a child's emitted message is stamped from the connection") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    Registered recorder = register_probe(bus, {pong_schema()});

    WeaveId seen_sender{};
    bus.add_observer([&](const BusEvent& e) {
        if (e.kind == EventKind::Delivered && e.schema_name == "Pong") {
            seen_sender = e.sender;
        }
    });

    Grant g;
    g.allow("Pong", 1, recorder.id);
    OutOfProcessResult r = host.mount("worker", ZEN_SO_WEAVE, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);

    bus.send(r.id, Message(ping(5), WeaveId{}, recorder.id));
    REQUIRE(host.run_until([&] { return !recorder.weave->handled_names.empty(); }, 2000));

    // The Emit frame carries no sender field by construction; the host stamps the
    // proxy's id from the connection the bytes arrived on. The child cannot forge it.
    CHECK(seen_sender == r.id);
}

TEST_CASE("a malformed message emitted by a child is refused by the host gate, never routed") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    Registered recorder = register_probe(bus, {pong_schema()});

    Grant g;
    g.allow("Pong", 1, recorder.id); // authorized — so refusal is the gate's, not the grant's
    OutOfProcessResult r = host.mount("bad", ZEN_SO_BADMSG, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);

    bus.send(r.id, Message(ping(1), WeaveId{}, recorder.id));
    // Step well past the round-trip; had the Pong been admitted, it would have arrived.
    (void)host.run_until([] { return false; }, 80);

    // The child emitted a Pong missing 'seq'; the host gate refused it host-side.
    CHECK(recorder.weave->handled_names.empty());
    CHECK(host.is_mounted("bad"));      // the host shrugged it off
    CHECK_FALSE(host.quarantined("bad"));
}

TEST_CASE("a child's emitted message is authorized against its grant: CapabilityDenied") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    Registered recorder = register_probe(bus, {pong_schema()});

    std::vector<TapRecord> taps;
    bus.add_observer([&](const BusEvent& e) { taps.push_back(to_record(e)); });

    // Mount with the EMPTY grant — minimal authority, may send nothing.
    OutOfProcessResult r = host.mount("muzzled", ZEN_SO_WEAVE, Grant{});
    REQUIRE_MESSAGE(r.ok, r.error);

    bus.send(r.id, Message(ping(3), WeaveId{}, recorder.id));
    (void)host.run_until(
        [&] {
            for (const TapRecord& t : taps) {
                if (t.reason == RefusalReason::CapabilityDenied) {
                    return true;
                }
            }
            return false;
        },
        2000);

    bool denied = false;
    for (const TapRecord& t : taps) {
        if (t.reason == RefusalReason::CapabilityDenied && t.schema == "Pong") {
            denied = true;
        }
    }
    CHECK(denied);                              // authorization, before the gate
    CHECK(recorder.weave->handled_names.empty()); // and it never reached the recorder
}

TEST_CASE("a crashing child is contained: the host survives, reloads, then quarantines") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    Registered recorder = register_probe(bus, {pong_schema()});

    int died = 0;
    int revived = 0;
    bus.add_observer([&](const BusEvent& e) {
        if (e.kind == EventKind::Died) {
            ++died;
        }
        if (e.kind == EventKind::Revived) {
            ++revived;
        }
    });

    Grant g;
    g.allow("Pong", 1, recorder.id);
    OutOfProcessResult r = host.mount("crasher", ZEN_SO_CRASHER, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);

    // It is healthy first: a benign ping is answered.
    bus.send(r.id, Message(ping(1), WeaveId{}, recorder.id));
    REQUIRE(host.run_until([&] { return !recorder.weave->handled_names.empty(); }, 2000));

    // The magic ping aborts the child mid-handle. The host must not crash: it
    // reloads from the host-owned snapshot; the reloaded child aborts again on
    // revive; after the budget (3) is spent the Weave is quarantined.
    bus.send(r.id, Message(ping(0xDEAD), WeaveId{}, recorder.id));
    const bool quarantined = host.run_until([&] { return host.quarantined("crasher"); }, 5000);

    REQUIRE(quarantined);
    CHECK_FALSE(bus.alive(r.id)); // dead, surfaced on the bus
    CHECK(died >= 1);             // it crashed at least once
    CHECK(revived >= 1);          // and came back at least once before exhausting its budget
    CHECK(host.containment("crasher").find("quarantined") != std::string::npos);

    // The host process is alive and well: a freshly mounted Weave still works.
    Registered rec2 = register_probe(bus, {pong_schema()});
    Grant g2;
    g2.allow("Pong", 1, rec2.id);
    OutOfProcessResult r2 = host.mount("fresh", ZEN_SO_WEAVE, std::move(g2));
    REQUIRE_MESSAGE(r2.ok, r2.error);
    bus.send(r2.id, Message(ping(7), WeaveId{}, rec2.id));
    REQUIRE(host.run_until([&] { return !rec2.weave->handled_names.empty(); }, 2000));
    CHECK(rec2.weave->handled_values.back() == 7);
}

TEST_CASE("the host never blocks on a child: a silent child cannot stall the bus") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    Registered recorder = register_probe(bus, {pong_schema()});

    OutOfProcessResult silent = host.mount("silent", ZEN_SO_SILENT, Grant{});
    REQUIRE_MESSAGE(silent.ok, silent.error);

    // An in-process worker that DOES reply, so we can observe the bus still serving.
    Registered worker = register_probe(bus, {ping_schema()});
    worker.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
        b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
    };

    // Send the silent child a Ping it will never answer, and the worker one it will.
    bus.send(silent.id, Message(ping(1), WeaveId{}, recorder.id));
    bus.send(worker.id, Message(ping(99), WeaveId{}, recorder.id));

    REQUIRE(host.run_until([&] { return !recorder.weave->handled_names.empty(); }, 500));
    CHECK(recorder.weave->handled_values.back() == 99); // served despite the silent child
}

TEST_CASE("honest containment status: generated from what was actually imposed") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);

    CHECK(host.containment("nope") == "not mounted");

    // Granting Network keeps this host-independent (no sandbox needed to mount, so
    // it succeeds whether or not this host can enforce a namespace).
    Grant g;
    g.with_os_capabilities(os_cap::Network);
    OutOfProcessResult r = host.mount("w", ZEN_SO_WEAVE, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);

    const std::string status = host.containment("w");
    CHECK(status.find("isolated") != std::string::npos);
    CHECK(status.find("network: granted") != std::string::npos); // honest: not contained, by grant
}

TEST_CASE("network is OS-enforced: a child without the Network grant cannot reach the network") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);

    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Network},
                            "network is OS-enforced: a no-net child cannot reach the network");

    // Sandboxed: the default grant withholds Network, but we DO allow it to send
    // NetResult — so any failure to reach the network is the OS sandbox, not the bus
    // grant (the gate/authorization let the result through).
    std::int64_t contained_code = 1; // sentinel: stays 1 only if no result arrives
    Registered rec1 = register_probe(bus, {netresult_schema()});
    rec1.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
        contained_code = in.payload.get("code")->as_int();
    };
    Grant sandboxed;
    sandboxed.allow("NetResult", 1, rec1.id);
    OutOfProcessResult s = host.mount("contained", ZEN_SO_NETPROBE, std::move(sandboxed));
    REQUIRE_MESSAGE(s.ok, s.error);
    CHECK(host.containment("contained").find("network: contained") != std::string::npos);

    bus.send(s.id, Message(ping(1), WeaveId{}, rec1.id));
    REQUIRE(host.run_until([&] { return !rec1.weave->handled_names.empty(); }, 2000));
    CHECK(rec1.weave->handled_names.back() == "NetResult"); // it still emitted (sandbox != muzzle)
    CHECK(contained_code == ENETUNREACH); // no interface → unreachable, enforced by the OS

    // Granted Network: the same probe now reaches the stack (port closed → refused).
    std::int64_t granted_code = 0;
    Registered rec2 = register_probe(bus, {netresult_schema()});
    rec2.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
        granted_code = in.payload.get("code")->as_int();
    };
    Grant granted;
    granted.allow("NetResult", 1, rec2.id).with_os_capabilities(os_cap::Network);
    OutOfProcessResult g = host.mount("granted", ZEN_SO_NETPROBE, std::move(granted));
    REQUIRE_MESSAGE(g.ok, g.error);
    CHECK(host.containment("granted").find("network: granted") != std::string::npos);

    bus.send(g.id, Message(ping(1), WeaveId{}, rec2.id));
    REQUIRE(host.run_until([&] { return !rec2.weave->handled_names.empty(); }, 2000));
    CHECK(granted_code != ENETUNREACH);  // it CAN reach the network
    CHECK(granted_code == ECONNREFUSED); // specifically: stack reachable, nothing listening
}

TEST_CASE("filesystem is OS-enforced: secret absent, scratch writable, host root read-only") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Filesystem},
                            "filesystem is OS-enforced: secret absent / scratch writable / root RO");
    // A sentinel secret OUTSIDE any granted scope (under /tmp, which the allow-list view
    // does not bind), so the probe proves the secret is ABSENT from the view, not hidden.
    { std::ofstream f("/tmp/zen_b4_secret.txt"); f << "TOPSECRET\n"; }

    // The fs-probe IS allowed to send FsResult, so a read/write/exec failure is the OS
    // sandbox, not the bus grant (sandbox != muzzle). These outer vars are filled by the
    // probe's reply.
    std::int64_t secret = -1, scratch = -1, outside = -1, noexec = -1;
    auto run_probe = [&](const char* name, Grant grant) {
        Registered rec = register_probe(bus, {fsresult_schema()});
        rec.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
            secret = in.payload.get("secret_read")->as_int();
            scratch = in.payload.get("scratch_write")->as_int();
            outside = in.payload.get("outside_write")->as_int();
            noexec = in.payload.get("noexec_exec")->as_int();
        };
        grant.allow("FsResult", 1, rec.id);
        OutOfProcessResult r = host.mount(name, ZEN_SO_FSPROBE, std::move(grant));
        REQUIRE_MESSAGE(r.ok, r.error);
        bus.send(r.id, Message(ping(1), WeaveId{}, rec.id));
        REQUIRE(host.run_until([&] { return !rec.weave->handled_names.empty(); }, 3000));
        CHECK(rec.weave->handled_names.back() == "FsResult"); // it still emitted: sandbox != muzzle
    };

    SUBCASE("WriteScoped: scratch writable, secret absent, host root read-only") {
        run_probe("ws", Grant{}.with_filesystem(FsAccess::WriteScoped));
        CHECK(secret != 0);  // the secret is not in the view
        CHECK(scratch == 0); // the scratch dir is writable
        CHECK(outside != 0); // writing the host root is refused (read-only base)
    }
    SUBCASE("WriteNoExec: a file written to scratch cannot be executed") {
        run_probe("wnx", Grant{}.with_filesystem(FsAccess::WriteNoExec));
        CHECK(scratch == 0);      // still writable
        CHECK(noexec == EACCES);  // execve of the written file → EACCES, enforced by the mount
    }
    SUBCASE("None: nothing writable, secret absent") {
        run_probe("none", Grant{}.with_filesystem(FsAccess::None));
        CHECK(secret != 0);
        CHECK(outside != 0);
    }
}

TEST_CASE("WriteAnywhere is the honest opt-out: reaches host paths, reported not contained") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    { std::ofstream f("/tmp/zen_b4_secret.txt"); f << "TOPSECRET\n"; }

    Registered rec = register_probe(bus, {fsresult_schema()});
    std::int64_t secret = -1;
    rec.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
        secret = in.payload.get("secret_read")->as_int();
    };
    Grant g;
    g.allow("FsResult", 1, rec.id).with_filesystem(FsAccess::WriteAnywhere);
    OutOfProcessResult r = host.mount("wa", ZEN_SO_FSPROBE, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(host.containment("wa").find("WriteAnywhere") != std::string::npos);
    CHECK(host.containment("wa").find("not contained") != std::string::npos);

    bus.send(r.id, Message(ping(1), WeaveId{}, rec.id));
    REQUIRE(host.run_until([&] { return !rec.weave->handled_names.empty(); }, 3000));
    CHECK(secret == 0); // unrestricted: it CAN read the host secret — real power, by grant
}

TEST_CASE("a filesystem-contained mount is confirmed in a distinct mount namespace") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Filesystem},
                            "filesystem containment confirmed in a distinct mount namespace");
    OutOfProcessResult r = host.mount("c", ZEN_SO_WEAVE, Grant{}); // default → fs contained
    REQUIRE_MESSAGE(r.ok, r.error);
    const std::string s = host.containment("c");
    CHECK(s.find("filesystem: contained") != std::string::npos);
    CHECK(s.find("confirmed: child mountns distinct from host") != std::string::npos);
}

TEST_CASE("filesystem fail-safe + dev-mode: unenforceable refuses by default; dev-mode marks it") {
    // Force ONLY filesystem unenforceable (network stays enforceable), so both branches
    // are covered wherever CI runs, never claiming containment we did not impose.
    const auto forced = [] {
        EnforcementReport rep;
        rep.capabilities.push_back({Capability::Network, true, "user+net namespace", ""});
        rep.capabilities.push_back({Capability::Filesystem, false, "", "forced unavailable"});
        return rep;
    };
    SUBCASE("strict refuses, naming the gap") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.override_enforcement_for_test(forced());
        OutOfProcessResult r = host.mount("x", ZEN_SO_WEAVE, Grant{}); // default → wants fs contained
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("filesystem") != std::string::npos);
        CHECK_FALSE(host.is_mounted("x"));
    }
    SUBCASE("dev-mode proceeds, filesystem visibly uncontained") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.override_enforcement_for_test(forced());
        host.set_dev_mode(true);
        OutOfProcessResult r = host.mount("x", ZEN_SO_WEAVE, Grant{});
        REQUIRE_MESSAGE(r.ok, r.error);
        const std::string s = host.containment("x");
        CHECK(s.find("filesystem: NOT CONTAINED") != std::string::npos);
        CHECK(s.find("filesystem: contained") == std::string::npos); // never a false claim
    }
}

TEST_CASE("a memory bomb is OOM-killed within its cgroup; the host survives, then quarantines") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Resources},
                            "a memory bomb is OOM-killed within its cgroup");
    Registered rec = register_probe(bus, {pong_schema()});
    int died = 0;
    bus.add_observer([&](const BusEvent& e) {
        if (e.kind == EventKind::Died) {
            ++died;
        }
    });

    ResourceLimits lim;
    lim.memory_bytes = 64LL * 1024 * 1024; // 64 MiB cap; the bomb allocates ~200 MiB
    Grant g;
    g.allow("Pong", 1, rec.id).with_resources(lim);
    OutOfProcessResult r = host.mount("bomb", ZEN_SO_MEMBOMB, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(host.containment("bomb").find("resources: contained") != std::string::npos);

    bus.send(r.id, Message(ping(1), WeaveId{}, rec.id));
    const bool quarantined = host.run_until([&] { return host.quarantined("bomb"); }, 8000);
    REQUIRE(quarantined);
    CHECK(died >= 1);                        // OOM-killed (within its cgroup) at least once
    CHECK(rec.weave->handled_names.empty()); // killed before it could reply

    // The host is unharmed: a fresh Weave still works.
    Registered rec2 = register_probe(bus, {pong_schema()});
    Grant g2;
    g2.allow("Pong", 1, rec2.id);
    OutOfProcessResult r2 = host.mount("fresh", ZEN_SO_WEAVE, std::move(g2));
    REQUIRE_MESSAGE(r2.ok, r2.error);
    bus.send(r2.id, Message(ping(7), WeaveId{}, rec2.id));
    REQUIRE(host.run_until([&] { return !rec2.weave->handled_names.empty(); }, 2000));
    CHECK(rec2.weave->handled_values.back() == 7);
}

TEST_CASE("negative control: the same allocation under a high cap survives (the cap is the cause)") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Resources},
                            "resource negative control: the same allocation under a high cap survives");
    Registered rec = register_probe(bus, {pong_schema()});
    ResourceLimits lim;
    lim.memory_bytes = 512LL * 1024 * 1024; // 512 MiB cap; the same ~200 MiB fits
    Grant g;
    g.allow("Pong", 1, rec.id).with_resources(lim);
    OutOfProcessResult r = host.mount("roomy", ZEN_SO_MEMBOMB, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);
    bus.send(r.id, Message(ping(5), WeaveId{}, rec.id));
    REQUIRE(host.run_until([&] { return !rec.weave->handled_names.empty(); }, 3000));
    CHECK(rec.weave->handled_names.back() == "Pong"); // survived → the cap, not the alloc, kills
    CHECK_FALSE(host.quarantined("roomy"));
}

TEST_CASE("a fork-bomb is bounded by pids.max; the host survives") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Resources},
                            "a fork-bomb is bounded by pids.max");
    Registered rec = register_probe(bus, {forkresult_schema()});
    std::int64_t forked = -1;
    rec.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
        forked = in.payload.get("forked")->as_int();
    };
    ResourceLimits lim;
    lim.pids = 64; // the weave tries 4000 forks; the cap must bound it
    Grant g;
    g.allow("ForkResult", 1, rec.id).with_resources(lim);
    OutOfProcessResult r = host.mount("forkbomb", ZEN_SO_FORKBOMB, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);
    bus.send(r.id, Message(ping(1), WeaveId{}, rec.id));
    REQUIRE(host.run_until([&] { return !rec.weave->handled_names.empty(); }, 5000));
    CHECK(forked > 0);
    CHECK(forked <= 64); // bounded by pids.max, not the 4000 it attempted
}

TEST_CASE("resource note + attestation honesty: the full delegation matrix (memory x pids)") {
    // Audit F-20 AND its pids mirror (N-1) — the honesty lattice's one absolute rule: never
    // report enforcement we did not impose. cgroup_create_leaf writes memory.max ONLY where the
    // memory controller is delegated, and pids.max ONLY where the pids controller is delegated
    // (sandbox.cpp:482/488), so neither the note NOR the attestation may claim a cap the leaf
    // will not set. The original F-20 pin watched only the pids-only posture and never the
    // symmetric memory-only one, so the mirror over-claimed unwatched. resource_note and
    // resource_attestation are pure, so this pins ALL FOUR postures — including memory-only,
    // which no live cgroup on this memory+pids host can produce — with no live cgroup.
    ResourceCaps caps;
    caps.memory_max = 256 * 1024 * 1024; // a computed 256 MiB cap
    caps.pids_max = 64;

    // ---- resource_note: the 2x2 delegation matrix ----
    SUBCASE("both delegated: both caps named honestly") {
        const std::string note = resource_note(caps, /*mem=*/true, /*pids=*/true);
        CHECK(note.find("memory<=256MiB") != std::string::npos);
        CHECK(note.find("pids<=64") != std::string::npos);
        CHECK(note.find("UNCAPPED") == std::string::npos);
    }
    SUBCASE("pids-only (memory NOT delegated): memory UNCAPPED, pids asserted [F-20 pin, kept]") {
        const std::string note = resource_note(caps, /*mem=*/false, /*pids=*/true);
        CHECK(note.find("memory<=") == std::string::npos);       // never a cap it cannot impose
        CHECK(note.find("memory UNCAPPED") != std::string::npos); // positively stated
        CHECK(note.find("pids<=64") != std::string::npos);        // pids is real here
    }
    SUBCASE("memory-only (pids NOT delegated): pids UNCAPPED, memory asserted [N-1 mirror]") {
        const std::string note = resource_note(caps, /*mem=*/true, /*pids=*/false);
        CHECK(note.find("memory<=256MiB") != std::string::npos);  // memory is real here
        CHECK(note.find("pids<=") == std::string::npos);          // never a pids cap it cannot impose
        CHECK(note.find("pids UNCAPPED") != std::string::npos);   // positively stated
    }
    SUBCASE("neither delegated: both dimensions UNCAPPED, no cap claimed") {
        const std::string note = resource_note(caps, /*mem=*/false, /*pids=*/false);
        CHECK(note.find("memory UNCAPPED") != std::string::npos);
        CHECK(note.find("pids UNCAPPED") != std::string::npos);
        CHECK(note.find("<=") == std::string::npos); // no cap claimed in either dimension
    }
    SUBCASE("a grant memory opt-out reads distinctly from an unenforceable host") {
        ResourceCaps opted = caps;
        opted.memory_max = -1; // unlimited-by-grant (intentional opt-out, not a host limit)
        CHECK(resource_note(opted, true, true).find("memory unlimited-by-grant") !=
              std::string::npos);
        // even opted-out, an unenforceable host must never imply a cap exists
        CHECK(resource_note(opted, false, true).find("memory<=") == std::string::npos);
    }

    // ---- resource_attestation: the fork-bomb-stop claim is delegation-qualified on pids ----
    SUBCASE("attestation asserts the fork-bomb stop only where pids is delegated [N-1 mirror]") {
        const std::string att_pids =
            resource_attestation(resource_note(caps, true, true), /*pids=*/true, /*confirmed=*/true);
        CHECK(att_pids.find("bounds a fork-bomb") != std::string::npos);            // real here
        CHECK(att_pids.find("pid in leaf, limits read back") != std::string::npos); // live-path phrasing
        CHECK(att_pids.find("FORK-BOMB STOP NOT ENFORCEABLE") == std::string::npos);

        const std::string att_nopids = resource_attestation(resource_note(caps, true, false),
                                                            /*pids=*/false, /*confirmed=*/true);
        CHECK(att_nopids.find("FORK-BOMB STOP NOT ENFORCEABLE") != std::string::npos); // honest
        CHECK(att_nopids.find("ALWAYS bounds a fork-bomb") == std::string::npos);      // retired absolute
        // prominence (judgment d): the gap is in the HEADLINE, not buried — parity with
        // "network: NOT CONTAINED", so a status scan can't miss an absent fork-bomb stop.
        CHECK(att_nopids.compare(0, 20, "resources: contained") != 0); // not the plain headline
        CHECK(att_nopids.find("memory contained but FORK-BOMB STOP NOT ENFORCEABLE") !=
              std::string::npos);
        // the confirmed clause must not imply a pids readback the host never performed
        CHECK(att_nopids.find("pids not delegated — no pids.max to confirm") != std::string::npos);
        CHECK(att_nopids.find("pid in leaf, limits read back") == std::string::npos);
    }
}

TEST_CASE("resources: confirmation, fail-safe, dev-mode, and the memory opt-out") {
    const auto forced = [] {
        EnforcementReport rep;
        rep.capabilities.push_back({Capability::Network, true, "net", ""});
        rep.capabilities.push_back({Capability::Filesystem, true, "fs", ""});
        rep.capabilities.push_back({Capability::Resources, false, "", "forced unavailable"});
        return rep;
    };

    SUBCASE("a resource-contained Weave's limits are confirmed") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Resources},
                                "a resource-contained Weave's limits are confirmed");
        OutOfProcessResult r = host.mount("rc", ZEN_SO_WEAVE, Grant{}); // default → contained
        REQUIRE_MESSAGE(r.ok, r.error);
        const std::string s = host.containment("rc");
        CHECK(s.find("resources: contained") != std::string::npos);
        CHECK(s.find("pid in leaf, limits read back") != std::string::npos);
    }
    SUBCASE("strict refuses when resources are unenforceable") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.override_enforcement_for_test(forced());
        OutOfProcessResult r = host.mount("x", ZEN_SO_WEAVE, Grant{});
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("resource") != std::string::npos);
    }
    SUBCASE("dev-mode runs resource-uncontained, visibly marked") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.override_enforcement_for_test(forced());
        host.set_dev_mode(true);
        OutOfProcessResult r = host.mount("x", ZEN_SO_WEAVE, Grant{});
        REQUIRE_MESSAGE(r.ok, r.error);
        CHECK(host.containment("x").find("resources: NOT CONTAINED") != std::string::npos);
    }
    SUBCASE("the memory opt-out uncaps memory but stays pids-bounded") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Resources},
                                "the memory opt-out uncaps memory but stays pids-bounded");
        OutOfProcessResult r = host.mount("u", ZEN_SO_WEAVE, Grant{}.with_unlimited_memory());
        REQUIRE_MESSAGE(r.ok, r.error);
        const std::string s = host.containment("u");
        CHECK(s.find("resources: contained") != std::string::npos);     // Enforced, never Granted
        CHECK(s.find("memory unlimited-by-grant") != std::string::npos); // the memory cap is opted out
        CHECK(s.find("pids<=") != std::string::npos);                   // pids is still bounded
        CHECK(s.find("NOT CONTAINED") == std::string::npos);
    }
}

TEST_CASE("no grant licenses a fork-bomb: pids stays bounded even with memory unlimited") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Resources},
                            "no grant licenses a fork-bomb: pids stays bounded under unlimited memory");
    Registered rec = register_probe(bus, {forkresult_schema()});
    std::int64_t forked = -1;
    rec.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
        forked = in.payload.get("forked")->as_int();
    };
    ResourceLimits lim;
    lim.pids = 64; // the weave tries 4000 forks; pids.max must still bound it
    Grant g;
    g.allow("ForkResult", 1, rec.id).with_resources(lim).with_unlimited_memory();
    OutOfProcessResult r = host.mount("fb", ZEN_SO_FORKBOMB, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);
    // Memory is uncapped by the grant, yet the leaf still carries a real pids.max.
    CHECK(host.containment("fb").find("memory unlimited-by-grant") != std::string::npos);
    bus.send(r.id, Message(ping(1), WeaveId{}, rec.id));
    REQUIRE(host.run_until([&] { return !rec.weave->handled_names.empty(); }, 5000));
    CHECK(forked > 0);
    CHECK(forked <= 64); // bounded by pids.max — the memory opt-out did NOT remove it
}

TEST_CASE("the memory opt-out lets a memory-bomb survive the cap that would OOM-kill it") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Resources},
                            "the memory opt-out lets a memory-bomb survive the OOM cap");
    Registered rec = register_probe(bus, {pong_schema()});
    Grant g;
    g.allow("Pong", 1, rec.id).with_unlimited_memory(); // memory uncapped; pids still bounded
    OutOfProcessResult r = host.mount("um", ZEN_SO_MEMBOMB, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(host.containment("um").find("pids<=") != std::string::npos); // still pids-bounded
    bus.send(r.id, Message(ping(5), WeaveId{}, rec.id));
    REQUIRE(host.run_until([&] { return !rec.weave->handled_names.empty(); }, 3000));
    CHECK(rec.weave->handled_names.back() == "Pong"); // survived the ~200 MiB alloc (uncapped)
    CHECK_FALSE(host.quarantined("um"));
}

TEST_CASE("fail-safe + dev-mode: an unenforceable host refuses by default; dev-mode overrides") {
    // Force Network unenforceable regardless of the real host, so BOTH branches are
    // covered wherever CI runs (the never-claim-what-we-didn't-impose discipline).
    EnforcementReport forced;
    forced.capabilities.push_back(CapabilityStatus{Capability::Network, false, "", "forced"});

    SUBCASE("strict (default): the mount refuses, naming the gap") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.override_enforcement_for_test(forced);

        OutOfProcessResult r = host.mount("x", ZEN_SO_WEAVE, Grant{});
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("refused") != std::string::npos);
        CHECK(r.error.find("network") != std::string::npos);
        CHECK_FALSE(host.is_mounted("x"));
    }

    SUBCASE("dev-mode: proceeds, visibly uncontained, never falsely claiming containment") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.override_enforcement_for_test(forced);
        host.set_dev_mode(true);

        OutOfProcessResult r = host.mount("x", ZEN_SO_WEAVE, Grant{});
        REQUIRE_MESSAGE(r.ok, r.error);
        const std::string status = host.containment("x");
        CHECK(status.find("NOT CONTAINED") != std::string::npos);
        CHECK(status.find("network: contained") == std::string::npos); // never a false claim
    }
}

TEST_CASE("a contained mount is positively confirmed in a distinct network namespace") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Network},
                            "network containment positively confirmed in a distinct netns");
    OutOfProcessResult r = host.mount("c", ZEN_SO_WEAVE, Grant{}); // default grant → contained
    REQUIRE_MESSAGE(r.ok, r.error);
    const std::string status = host.containment("c");
    CHECK(status.find("network: contained") != std::string::npos);
    CHECK(status.find("confirmed") != std::string::npos); // VERIFIED (distinct netns), not inferred
}

TEST_CASE("a SURPRISE sandbox-entry failure fails safe: refuses in strict AND dev mode") {
    // The probe passes (Network IS enforceable here), but the *real* entry is forced
    // to fail at spawn — the catastrophic path to rule out. It must refuse in BOTH
    // modes: a surprise failure of an INTENDED enforcement never downgrades to running
    // wide-open, and the Weave never runs (untrusted code never started).
    Switchboard probe_bus;
    IsolationHost probe_host(probe_bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(probe_host.enforcement(), {Capability::Network},
                            "forced real-entry-failure prereq (network must be enforceable to test it)");

    SUBCASE("strict mode refuses") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.force_entry_failure_for_test(true);
        OutOfProcessResult r = host.mount("x", ZEN_SO_WEAVE, Grant{}); // default → intends contained
        CHECK_FALSE(r.ok);
        CHECK_FALSE(host.is_mounted("x"));
    }
    SUBCASE("dev-mode ALSO refuses — a surprise failure is not a known gap") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.set_dev_mode(true);
        host.force_entry_failure_for_test(true);
        OutOfProcessResult r = host.mount("x", ZEN_SO_WEAVE, Grant{});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(host.is_mounted("x"));
    }
}

TEST_CASE("unmount tears the child down cleanly and the proxy leaves the bus") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);

    OutOfProcessResult r = host.mount("w", ZEN_SO_WEAVE, Grant{});
    REQUIRE_MESSAGE(r.ok, r.error);
    const WeaveId id = r.id;
    CHECK(bus.alive(id));

    host.unmount("w");
    CHECK_FALSE(host.is_mounted("w"));

    Ticket t = bus.send(id, Message(ping(1)));
    bus.pump();
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::NoSuchTarget);
}

// ---- R2B-2: deferred answers are in-process only, and FAIL CLOSED out of it ----

TEST_CASE("R2B-2: an out-of-process weave gets no deferred-answer capability, and says so by "
          "having none rather than by holding one the pipe cannot honour") {
    // THE COST OF V1's SCOPE, PAID WHERE IT IS INCURRED. Cross-process capability
    // is deliberately out of scope, so the child host hands the library NO
    // deferral door. This pins the direction of that gap: the child gets nothing
    // (and its conversation simply goes unanswered), rather than being handed a
    // token the parent could not validate across the pipe.
    Switchboard bus;
    IsolationHost host(bus, kHostExe);

    // Counter v4, the DEFERS state contract, spelled out locally on purpose.
    static const auto steward_state = SchemaBuilder("Counter", 4)
                                          .field("count", Kind::Int)
                                          .field("deferred", Kind::Int)
                                          .field("spent", Kind::Int)
                                          .field("token", Kind::Int)
                                          .build();

    Registered asker = register_probe(bus, {pong_schema(), tick_schema()});
    OutOfProcessResult out = host.mount("steward", ZEN_SO_DEFERS, Grant{}.allow_any());
    REQUIRE_MESSAGE(out.ok, out.error);

    int answers = 0;
    asker.weave->on_handle = [&](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Tick") {
            if (in.payload.get("n")->as_int() == 1) {
                b.send(out.id, Message(ping(5), WeaveId{}, WeaveId{}, 0x5EEDu));
            } else {
                b.send(out.id, Message(greet("now")));
            }
            return;
        }
        ++answers;
    };

    // Deliberately ASSERTION-FREE: this runs inside the poll predicate, and an
    // assertion there would make the suite's assertion count depend on how many
    // times the loop happened to spin.
    const auto field = [&](const char* name) -> std::int64_t {
        Unverified u = parse(bus.snapshot_bytes(out.id));
        Admission a = admit(u, steward_state);
        if (!a.ok()) {
            return -1;
        }
        const Cell* c = a.value().get(name);
        return c == nullptr ? -1 : c->as_int();
    };

    bus.send(asker.id, Message(tick(1)));
    REQUIRE(host.run_until([&] { return field("count") >= 1; }, 2000));
    // It handled the ask, and came away with NOTHING to answer later with.
    CHECK(field("deferred") == 0);
    CHECK(field("token") == 0);

    // The completion arrives and the attempt fails, quietly and safely: no answer,
    // no crash, and above all no answer minted from a token nobody could check.
    // REQUIRED, not hoped for: every assertion below is an ABSENCE, so without a
    // positive control that the completion really was handled, a child that never
    // woke up would pass this test.
    bus.send(asker.id, Message(tick(2)));
    REQUIRE(host.run_until([&] { return field("count") >= 2; }, 2000));
    CHECK(field("spent") == 0);
    CHECK(answers == 0);
}

// ---- R2D-0: office authorship is in-process only, and FAILS CLOSED out of it ----

TEST_CASE("R2D-0: an out-of-process weave really holding the role still cannot author office "
          "speech across the pipe — refused honestly, never downgraded, in both directions") {
    // THE SAME LAW THE ANSWER DOORS PAY, EXTENDED TO THE OFFICE (v5): the pipe
    // carries no attestation, so the child gets no authorship door outbound and
    // no authored-role fact inbound. What makes this case sharp is that the
    // membership itself is REAL — the mounted weave holds "worker.a" host-side —
    // so the only wall standing is the pipe's, and it must refuse rather than
    // downgrade or pretend.
    Switchboard bus;
    IsolationHost host(bus, kHostExe);

    static const auto worker_state =
        SchemaBuilder("OfficeWorkerState", 1).field("acts", Kind::Int).build();

    auto reports = std::make_shared<std::vector<office::OfficeReport>>();
    auto news_count = std::make_shared<int>(0);
    auto commander_probe = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{
        schema_of<office::OfficeReport>(), schema_of<office::WorkerNews>()});
    ProbeWeave* commander_raw = commander_probe.get();
    const WeaveId commander =
        bus.register_weave(std::move(commander_probe), Grant{}.allow_any(), "commander");
    commander_raw->on_handle = [reports, news_count](const Message& in, Bus&, ProbeWeave&) {
        if (in.payload.schema().name() == std::string_view(office::OfficeReport::zen_name)) {
            reports->push_back(from_value<office::OfficeReport>(in.payload));
        } else {
            ++*news_count;
        }
    };

    OutOfProcessResult out =
        host.mount("worker", ZEN_SO_OFFICE_WORKER, Grant{}.allow_any(), "worker.a");
    REQUIRE_MESSAGE(out.ok, out.error);
    REQUIRE(bus.role_holder("worker.a") == out.id); // the membership is real, host-side

    // Assertion-free poll helper, exactly as the deferred-answer case above.
    const auto acts = [&]() -> std::int64_t {
        Unverified u = parse(bus.snapshot_bytes(out.id));
        Admission a = admit(u, worker_state);
        if (!a.ok()) {
            return -1;
        }
        const Cell* c = a.value().get("acts");
        return c == nullptr ? -1 : c->as_int();
    };

    // OUTBOUND: the child deliberately asks to speak as the office it holds.
    office::OfficeCommand cmd;
    cmd.mode = "direct";
    cmd.target = static_cast<std::int64_t>(commander.value);
    bus.send_as(commander, out.id, Message(to_value(cmd)));
    REQUIRE(host.run_until([&] { return acts() >= 1 && !reports->empty(); }, 2000));
    REQUIRE(!reports->empty());
    CHECK((*reports)[0].what == "direct");
    CHECK_FALSE((*reports)[0].authored); // refused at the seam — and the child was TOLD
    CHECK(*news_count == 0);             // nothing was downgraded to a personal send

    // INBOUND: office speech TO the child. The fact is stamped and real
    // host-side; the pipe cannot vouch for it, so the child reads exactly
    // nothing rather than an unbacked flag.
    auto dispatcher_probe = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{ping_schema()});
    const WeaveId dispatcher =
        bus.register_weave(std::move(dispatcher_probe), Grant{}.allow_any(), "dispatcher");
    REQUIRE(bus.office_send_as(dispatcher, "dispatcher", out.id,
                               Message(to_value(office::WorkerNews{"flash"})))
                .valid());
    REQUIRE(host.run_until([&] { return acts() >= 2 && reports->size() >= 2; }, 2000));
    CHECK((*reports)[1].what == "heard");
    CHECK_FALSE((*reports)[1].authored);
    CHECK((*reports)[1].seen_role.empty());
}

// ---- Harness honesty (Part 1): the fail-by-default asymmetry, proven in-suite ----

TEST_CASE("harness honesty: an unprovable security proof FAILS by default, skips only on opt-out") {
    // No host / no real enforcement needed — exercise the gate's DECISION directly, so this proof
    // of the asymmetry stays green everywhere. Filesystem forced unenforceable, the others fine.
    EnforcementReport forced;
    forced.capabilities.push_back({Capability::Network, true, "forced", ""});
    forced.capabilities.push_back({Capability::Filesystem, false, "", "forced unavailable for the test"});
    forced.capabilities.push_back({Capability::Resources, true, "forced", ""});

    std::string missing;
    const zenh::Gate g = zenh::enforcement_decision(forced, {Capability::Filesystem}, missing);
    CHECK(missing == capability_name(Capability::Filesystem)); // the message names the missing cap

    if (zenh::require_enforcement_strict()) {
        CHECK(g == zenh::Gate::FailHard);     // the default on ANY host: an unprovable proof FAILS
    } else {
        CHECK(g == zenh::Gate::SkipDegraded); // only ZEN_ALLOW_UNENFORCEABLE=1 => a marked skip
    }

    // An enforceable capability always proceeds (the count-bumping path) — never a spurious fail.
    EnforcementReport ok;
    ok.capabilities.push_back({Capability::Filesystem, true, "forced", ""});
    std::string none;
    CHECK(zenh::enforcement_decision(ok, {Capability::Filesystem}, none) == zenh::Gate::Proceed);
    CHECK(none.empty());
}

// Keep this LAST in the file: a positive tally so a green can never mean "every OS-enforcement case
// silently skipped." (Relies on doctest's default registration order; a --order-by=rand run would
// instead assert the count in a reporter hook.)
TEST_CASE("enforcement coverage: the OS-enforcement proofs actually executed, not silently skipped") {
    if (!zenh::require_enforcement_strict() || zenh::degraded_run()) {
        MESSAGE("degraded run (opt-out set): the enforcement-coverage floor is relaxed");
        return;
    }
    // 12 OS-enforcement guard sites in this suite; on a provisioned host every one executes.
    MESSAGE("OS-enforcement cases executed: " << zenh::enforced_case_count());
    CHECK(zenh::enforced_case_count() >= 12);
}

} // TEST_SUITE
