// The B2 milestone: a Weave hosted out-of-process is indistinguishable to the bus
// from one hosted in-process, a crashing child is contained (host survives, bounded
// reload, then quarantine), and the process boundary is gated exactly like every
// other boundary — malformed child output is gate-refused, an emitted message is
// authorized against the child's grant, and the sender is stamped from the
// connection (a child cannot forge it). The host never blocks on a child.

#include <doctest.h>

#include "enforcement_gate.hpp"
#include "switchboard_fixtures.hpp"

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

TEST_CASE("resource note honesty: a memory cap is claimed only where the controller is delegated") {
    // Audit F-20 — the honesty lattice's one absolute rule: never report enforcement we
    // did not impose. cgroup_create_leaf writes memory.max ONLY where the memory
    // controller is delegated, so the note must NOT print `memory<=…` on a host that
    // cannot enforce it (the pids-only posture the auditor reproduced live in
    // sec2_probe.cpp). resource_note is a pure function, so this pins the honesty on any
    // detection posture without needing a live pids-without-memory cgroup base.
    ResourceCaps caps;
    caps.memory_max = 256 * 1024 * 1024; // a computed 256 MiB cap
    caps.pids_max = 64;

    SUBCASE("full-resources posture: the memory cap is named honestly") {
        const std::string note = resource_note(caps, /*memory_enforceable=*/true);
        CHECK(note.find("memory<=256MiB") != std::string::npos);
        CHECK(note.find("pids<=64") != std::string::npos);
        CHECK(note.find("UNCAPPED") == std::string::npos);
    }
    SUBCASE("pids-only posture: memory is positively stated uncapped, never claimed capped") {
        const std::string note = resource_note(caps, /*memory_enforceable=*/false);
        CHECK(note.find("memory<=") == std::string::npos);  // never claims a cap it cannot impose
        CHECK(note.find("UNCAPPED") != std::string::npos);  // positively states memory uncapped
        CHECK(note.find("pids<=64") != std::string::npos);  // pids is still honestly bounded
    }
    SUBCASE("a grant opt-out reads distinctly from an unenforceable host, honest either way") {
        ResourceCaps opted = caps;
        opted.memory_max = -1; // unlimited-by-grant (an intentional opt-out, not a host limit)
        CHECK(resource_note(opted, true).find("unlimited-by-grant") != std::string::npos);
        // even opted-out, an unenforceable host must never imply a cap exists
        CHECK(resource_note(opted, false).find("memory<=") == std::string::npos);
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
