// The B2 milestone: a Shard hosted out-of-process is indistinguishable to the bus
// from one hosted in-process, a crashing child is contained (host survives, bounded
// reload, then quarantine), and the process boundary is gated exactly like every
// other boundary — malformed child output is gate-refused, an emitted message is
// authorized against the child's grant, and the sender is stamped from the
// connection (a child cannot forge it). The host never blocks on a child.

#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/isolation/host.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/switchboard.hpp>

#include <cerrno>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace zen;
using namespace zen::sb;
using namespace zen::kernel;
using namespace zen::isolation;
using namespace sbfx;

namespace {
const std::string kHostExe = ZEN_SHARD_HOST_EXE;

// Matches the net-probe shard's emitted shape (NetResult{code}); the child reports
// the connect() errno here so a test can read it off the bus.
std::shared_ptr<const Schema> netresult_schema() {
    static const auto s = SchemaBuilder("NetResult", 1).field("code", Kind::Int).build();
    return s;
}
// Matches the fs-probe shard's emitted shape (B4): the errno of each filesystem reach.
std::shared_ptr<const Schema> fsresult_schema() {
    static const auto s = SchemaBuilder("FsResult", 1)
                              .field("secret_read", Kind::Int)
                              .field("scratch_write", Kind::Int)
                              .field("outside_write", Kind::Int)
                              .field("noexec_exec", Kind::Int)
                              .build();
    return s;
}
// Matches the fork-bomb shard's emitted shape (B5): how many forks succeeded.
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
    LoadResult in = kernel.load("in", ZEN_SO_SHARD);
    REQUIRE_MESSAGE(in.ok, in.error);
    Grant g;
    g.allow("Pong", 1, rec_out.id); // it needs exactly: send Pong to its recorder
    OutOfProcessResult out = host.mount("out", ZEN_SO_SHARD, std::move(g));
    REQUIRE_MESSAGE(out.ok, out.error);
    CHECK(bus.alive(out.id));

    // Same Ping to each; same reply expected.
    bus.send(in.id, Message(ping(42), ShardId{}, rec_in.id));
    bus.send(out.id, Message(ping(42), ShardId{}, rec_out.id));

    const bool done = host.run_until(
        [&] {
            return !rec_in.shard->handled_names.empty() && !rec_out.shard->handled_names.empty();
        },
        2000);
    REQUIRE(done);

    CHECK(rec_in.shard->handled_names[0] == "Pong");
    CHECK(rec_out.shard->handled_names[0] == rec_in.shard->handled_names[0]);
    CHECK(rec_out.shard->handled_values[0] == rec_in.shard->handled_values[0]);
    CHECK(rec_out.shard->handled_values[0] == 42);
}

TEST_CASE("the sender of a child's emitted message is stamped from the connection") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    Registered recorder = register_probe(bus, {pong_schema()});

    ShardId seen_sender{};
    bus.add_observer([&](const BusEvent& e) {
        if (e.kind == EventKind::Delivered && e.schema_name == "Pong") {
            seen_sender = e.sender;
        }
    });

    Grant g;
    g.allow("Pong", 1, recorder.id);
    OutOfProcessResult r = host.mount("worker", ZEN_SO_SHARD, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);

    bus.send(r.id, Message(ping(5), ShardId{}, recorder.id));
    REQUIRE(host.run_until([&] { return !recorder.shard->handled_names.empty(); }, 2000));

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

    bus.send(r.id, Message(ping(1), ShardId{}, recorder.id));
    // Step well past the round-trip; had the Pong been admitted, it would have arrived.
    (void)host.run_until([] { return false; }, 80);

    // The child emitted a Pong missing 'seq'; the host gate refused it host-side.
    CHECK(recorder.shard->handled_names.empty());
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
    OutOfProcessResult r = host.mount("muzzled", ZEN_SO_SHARD, Grant{});
    REQUIRE_MESSAGE(r.ok, r.error);

    bus.send(r.id, Message(ping(3), ShardId{}, recorder.id));
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
    CHECK(recorder.shard->handled_names.empty()); // and it never reached the recorder
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
    bus.send(r.id, Message(ping(1), ShardId{}, recorder.id));
    REQUIRE(host.run_until([&] { return !recorder.shard->handled_names.empty(); }, 2000));

    // The magic ping aborts the child mid-handle. The host must not crash: it
    // reloads from the host-owned snapshot; the reloaded child aborts again on
    // revive; after the budget (3) is spent the Shard is quarantined.
    bus.send(r.id, Message(ping(0xDEAD), ShardId{}, recorder.id));
    const bool quarantined = host.run_until([&] { return host.quarantined("crasher"); }, 5000);

    REQUIRE(quarantined);
    CHECK_FALSE(bus.alive(r.id)); // dead, surfaced on the bus
    CHECK(died >= 1);             // it crashed at least once
    CHECK(revived >= 1);          // and came back at least once before exhausting its budget
    CHECK(host.containment("crasher").find("quarantined") != std::string::npos);

    // The host process is alive and well: a freshly mounted Shard still works.
    Registered rec2 = register_probe(bus, {pong_schema()});
    Grant g2;
    g2.allow("Pong", 1, rec2.id);
    OutOfProcessResult r2 = host.mount("fresh", ZEN_SO_SHARD, std::move(g2));
    REQUIRE_MESSAGE(r2.ok, r2.error);
    bus.send(r2.id, Message(ping(7), ShardId{}, rec2.id));
    REQUIRE(host.run_until([&] { return !rec2.shard->handled_names.empty(); }, 2000));
    CHECK(rec2.shard->handled_values.back() == 7);
}

TEST_CASE("the host never blocks on a child: a silent child cannot stall the bus") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    Registered recorder = register_probe(bus, {pong_schema()});

    OutOfProcessResult silent = host.mount("silent", ZEN_SO_SILENT, Grant{});
    REQUIRE_MESSAGE(silent.ok, silent.error);

    // An in-process worker that DOES reply, so we can observe the bus still serving.
    Registered worker = register_probe(bus, {ping_schema()});
    worker.shard->on_handle = [](const Message& in, Bus& b, ProbeShard&) {
        b.send(in.reply_to, Message(pong(in.payload.get("seq")->as_int())));
    };

    // Send the silent child a Ping it will never answer, and the worker one it will.
    bus.send(silent.id, Message(ping(1), ShardId{}, recorder.id));
    bus.send(worker.id, Message(ping(99), ShardId{}, recorder.id));

    REQUIRE(host.run_until([&] { return !recorder.shard->handled_names.empty(); }, 500));
    CHECK(recorder.shard->handled_values.back() == 99); // served despite the silent child
}

TEST_CASE("honest containment status: generated from what was actually imposed") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);

    CHECK(host.containment("nope") == "not mounted");

    // Granting Network keeps this host-independent (no sandbox needed to mount, so
    // it succeeds whether or not this host can enforce a namespace).
    Grant g;
    g.with_os_capabilities(os_cap::Network);
    OutOfProcessResult r = host.mount("w", ZEN_SO_SHARD, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);

    const std::string status = host.containment("w");
    CHECK(status.find("isolated") != std::string::npos);
    CHECK(status.find("network: granted") != std::string::npos); // honest: not contained, by grant
}

TEST_CASE("network is OS-enforced: a child without the Network grant cannot reach the network") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);

    if (!host.enforcement().enforceable(Capability::Network)) {
        WARN("no unprivileged network namespace on this host; skipping the OS-enforced check");
        return;
    }

    // Sandboxed: the default grant withholds Network, but we DO allow it to send
    // NetResult — so any failure to reach the network is the OS sandbox, not the bus
    // grant (the gate/authorization let the result through).
    std::int64_t contained_code = 1; // sentinel: stays 1 only if no result arrives
    Registered rec1 = register_probe(bus, {netresult_schema()});
    rec1.shard->on_handle = [&](const Message& in, Bus&, ProbeShard&) {
        contained_code = in.payload.get("code")->as_int();
    };
    Grant sandboxed;
    sandboxed.allow("NetResult", 1, rec1.id);
    OutOfProcessResult s = host.mount("contained", ZEN_SO_NETPROBE, std::move(sandboxed));
    REQUIRE_MESSAGE(s.ok, s.error);
    CHECK(host.containment("contained").find("network: contained") != std::string::npos);

    bus.send(s.id, Message(ping(1), ShardId{}, rec1.id));
    REQUIRE(host.run_until([&] { return !rec1.shard->handled_names.empty(); }, 2000));
    CHECK(rec1.shard->handled_names.back() == "NetResult"); // it still emitted (sandbox != muzzle)
    CHECK(contained_code == ENETUNREACH); // no interface → unreachable, enforced by the OS

    // Granted Network: the same probe now reaches the stack (port closed → refused).
    std::int64_t granted_code = 0;
    Registered rec2 = register_probe(bus, {netresult_schema()});
    rec2.shard->on_handle = [&](const Message& in, Bus&, ProbeShard&) {
        granted_code = in.payload.get("code")->as_int();
    };
    Grant granted;
    granted.allow("NetResult", 1, rec2.id).with_os_capabilities(os_cap::Network);
    OutOfProcessResult g = host.mount("granted", ZEN_SO_NETPROBE, std::move(granted));
    REQUIRE_MESSAGE(g.ok, g.error);
    CHECK(host.containment("granted").find("network: granted") != std::string::npos);

    bus.send(g.id, Message(ping(1), ShardId{}, rec2.id));
    REQUIRE(host.run_until([&] { return !rec2.shard->handled_names.empty(); }, 2000));
    CHECK(granted_code != ENETUNREACH);  // it CAN reach the network
    CHECK(granted_code == ECONNREFUSED); // specifically: stack reachable, nothing listening
}

TEST_CASE("filesystem is OS-enforced: secret absent, scratch writable, host root read-only") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Filesystem)) {
        WARN("no unprivileged mount namespace on this host; skipping the OS-enforced fs check");
        return;
    }
    // A sentinel secret OUTSIDE any granted scope (under /tmp, which the allow-list view
    // does not bind), so the probe proves the secret is ABSENT from the view, not hidden.
    { std::ofstream f("/tmp/zen_b4_secret.txt"); f << "TOPSECRET\n"; }

    // The fs-probe IS allowed to send FsResult, so a read/write/exec failure is the OS
    // sandbox, not the bus grant (sandbox != muzzle). These outer vars are filled by the
    // probe's reply.
    std::int64_t secret = -1, scratch = -1, outside = -1, noexec = -1;
    auto run_probe = [&](const char* name, Grant grant) {
        Registered rec = register_probe(bus, {fsresult_schema()});
        rec.shard->on_handle = [&](const Message& in, Bus&, ProbeShard&) {
            secret = in.payload.get("secret_read")->as_int();
            scratch = in.payload.get("scratch_write")->as_int();
            outside = in.payload.get("outside_write")->as_int();
            noexec = in.payload.get("noexec_exec")->as_int();
        };
        grant.allow("FsResult", 1, rec.id);
        OutOfProcessResult r = host.mount(name, ZEN_SO_FSPROBE, std::move(grant));
        REQUIRE_MESSAGE(r.ok, r.error);
        bus.send(r.id, Message(ping(1), ShardId{}, rec.id));
        REQUIRE(host.run_until([&] { return !rec.shard->handled_names.empty(); }, 3000));
        CHECK(rec.shard->handled_names.back() == "FsResult"); // it still emitted: sandbox != muzzle
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
    rec.shard->on_handle = [&](const Message& in, Bus&, ProbeShard&) {
        secret = in.payload.get("secret_read")->as_int();
    };
    Grant g;
    g.allow("FsResult", 1, rec.id).with_filesystem(FsAccess::WriteAnywhere);
    OutOfProcessResult r = host.mount("wa", ZEN_SO_FSPROBE, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(host.containment("wa").find("WriteAnywhere") != std::string::npos);
    CHECK(host.containment("wa").find("not contained") != std::string::npos);

    bus.send(r.id, Message(ping(1), ShardId{}, rec.id));
    REQUIRE(host.run_until([&] { return !rec.shard->handled_names.empty(); }, 3000));
    CHECK(secret == 0); // unrestricted: it CAN read the host secret — real power, by grant
}

TEST_CASE("a filesystem-contained mount is confirmed in a distinct mount namespace") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Filesystem)) {
        WARN("no unprivileged mount namespace here; skipping the fs confirmation check");
        return;
    }
    OutOfProcessResult r = host.mount("c", ZEN_SO_SHARD, Grant{}); // default → fs contained
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
        OutOfProcessResult r = host.mount("x", ZEN_SO_SHARD, Grant{}); // default → wants fs contained
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("filesystem") != std::string::npos);
        CHECK_FALSE(host.is_mounted("x"));
    }
    SUBCASE("dev-mode proceeds, filesystem visibly uncontained") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.override_enforcement_for_test(forced());
        host.set_dev_mode(true);
        OutOfProcessResult r = host.mount("x", ZEN_SO_SHARD, Grant{});
        REQUIRE_MESSAGE(r.ok, r.error);
        const std::string s = host.containment("x");
        CHECK(s.find("filesystem: NOT CONTAINED") != std::string::npos);
        CHECK(s.find("filesystem: contained") == std::string::npos); // never a false claim
    }
}

TEST_CASE("a memory bomb is OOM-killed within its cgroup; the host survives, then quarantines") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Resources)) {
        WARN("no cgroup-v2 delegation here; run the isolation suite under a delegated scope");
        return;
    }
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

    bus.send(r.id, Message(ping(1), ShardId{}, rec.id));
    const bool quarantined = host.run_until([&] { return host.quarantined("bomb"); }, 8000);
    REQUIRE(quarantined);
    CHECK(died >= 1);                        // OOM-killed (within its cgroup) at least once
    CHECK(rec.shard->handled_names.empty()); // killed before it could reply

    // The host is unharmed: a fresh Shard still works.
    Registered rec2 = register_probe(bus, {pong_schema()});
    Grant g2;
    g2.allow("Pong", 1, rec2.id);
    OutOfProcessResult r2 = host.mount("fresh", ZEN_SO_SHARD, std::move(g2));
    REQUIRE_MESSAGE(r2.ok, r2.error);
    bus.send(r2.id, Message(ping(7), ShardId{}, rec2.id));
    REQUIRE(host.run_until([&] { return !rec2.shard->handled_names.empty(); }, 2000));
    CHECK(rec2.shard->handled_values.back() == 7);
}

TEST_CASE("negative control: the same allocation under a high cap survives (the cap is the cause)") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Resources)) {
        WARN("no cgroup-v2 delegation here; skipping the resource negative control");
        return;
    }
    Registered rec = register_probe(bus, {pong_schema()});
    ResourceLimits lim;
    lim.memory_bytes = 512LL * 1024 * 1024; // 512 MiB cap; the same ~200 MiB fits
    Grant g;
    g.allow("Pong", 1, rec.id).with_resources(lim);
    OutOfProcessResult r = host.mount("roomy", ZEN_SO_MEMBOMB, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);
    bus.send(r.id, Message(ping(5), ShardId{}, rec.id));
    REQUIRE(host.run_until([&] { return !rec.shard->handled_names.empty(); }, 3000));
    CHECK(rec.shard->handled_names.back() == "Pong"); // survived → the cap, not the alloc, kills
    CHECK_FALSE(host.quarantined("roomy"));
}

TEST_CASE("a fork-bomb is bounded by pids.max; the host survives") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Resources)) {
        WARN("no cgroup-v2 delegation here; skipping the fork-bomb check");
        return;
    }
    Registered rec = register_probe(bus, {forkresult_schema()});
    std::int64_t forked = -1;
    rec.shard->on_handle = [&](const Message& in, Bus&, ProbeShard&) {
        forked = in.payload.get("forked")->as_int();
    };
    ResourceLimits lim;
    lim.pids = 64; // the shard tries 4000 forks; the cap must bound it
    Grant g;
    g.allow("ForkResult", 1, rec.id).with_resources(lim);
    OutOfProcessResult r = host.mount("forkbomb", ZEN_SO_FORKBOMB, std::move(g));
    REQUIRE_MESSAGE(r.ok, r.error);
    bus.send(r.id, Message(ping(1), ShardId{}, rec.id));
    REQUIRE(host.run_until([&] { return !rec.shard->handled_names.empty(); }, 5000));
    CHECK(forked > 0);
    CHECK(forked <= 64); // bounded by pids.max, not the 4000 it attempted
}

TEST_CASE("resources: confirmation, fail-safe, dev-mode, and the unlimited opt-out") {
    const auto forced = [] {
        EnforcementReport rep;
        rep.capabilities.push_back({Capability::Network, true, "net", ""});
        rep.capabilities.push_back({Capability::Filesystem, true, "fs", ""});
        rep.capabilities.push_back({Capability::Resources, false, "", "forced unavailable"});
        return rep;
    };

    SUBCASE("a resource-contained Shard's limits are confirmed") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        if (!host.enforcement().enforceable(Capability::Resources)) {
            WARN("no cgroup-v2 delegation here; skipping the resource confirmation");
            return;
        }
        OutOfProcessResult r = host.mount("rc", ZEN_SO_SHARD, Grant{}); // default → contained
        REQUIRE_MESSAGE(r.ok, r.error);
        const std::string s = host.containment("rc");
        CHECK(s.find("resources: contained") != std::string::npos);
        CHECK(s.find("pid in leaf, limits read back") != std::string::npos);
    }
    SUBCASE("strict refuses when resources are unenforceable") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.override_enforcement_for_test(forced());
        OutOfProcessResult r = host.mount("x", ZEN_SO_SHARD, Grant{});
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("resource") != std::string::npos);
    }
    SUBCASE("dev-mode runs resource-uncontained, visibly marked") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.override_enforcement_for_test(forced());
        host.set_dev_mode(true);
        OutOfProcessResult r = host.mount("x", ZEN_SO_SHARD, Grant{});
        REQUIRE_MESSAGE(r.ok, r.error);
        CHECK(host.containment("x").find("resources: NOT CONTAINED") != std::string::npos);
    }
    SUBCASE("unlimited is the honest opt-out") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        OutOfProcessResult r = host.mount("u", ZEN_SO_SHARD, Grant{}.with_unlimited_resources());
        REQUIRE_MESSAGE(r.ok, r.error);
        CHECK(host.containment("u").find("resources: unlimited") != std::string::npos);
    }
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

        OutOfProcessResult r = host.mount("x", ZEN_SO_SHARD, Grant{});
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

        OutOfProcessResult r = host.mount("x", ZEN_SO_SHARD, Grant{});
        REQUIRE_MESSAGE(r.ok, r.error);
        const std::string status = host.containment("x");
        CHECK(status.find("NOT CONTAINED") != std::string::npos);
        CHECK(status.find("network: contained") == std::string::npos); // never a false claim
    }
}

TEST_CASE("a contained mount is positively confirmed in a distinct network namespace") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Network)) {
        WARN("network namespace not enforceable here; skipping the confirmation check");
        return;
    }
    OutOfProcessResult r = host.mount("c", ZEN_SO_SHARD, Grant{}); // default grant → contained
    REQUIRE_MESSAGE(r.ok, r.error);
    const std::string status = host.containment("c");
    CHECK(status.find("network: contained") != std::string::npos);
    CHECK(status.find("confirmed") != std::string::npos); // VERIFIED (distinct netns), not inferred
}

TEST_CASE("a SURPRISE sandbox-entry failure fails safe: refuses in strict AND dev mode") {
    // The probe passes (Network IS enforceable here), but the *real* entry is forced
    // to fail at spawn — the catastrophic path to rule out. It must refuse in BOTH
    // modes: a surprise failure of an INTENDED enforcement never downgrades to running
    // wide-open, and the Shard never runs (untrusted code never started).
    Switchboard probe_bus;
    IsolationHost probe_host(probe_bus, kHostExe);
    if (!probe_host.enforcement().enforceable(Capability::Network)) {
        WARN("network not enforceable here; cannot exercise the forced real-entry failure");
        return;
    }

    SUBCASE("strict mode refuses") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.force_entry_failure_for_test(true);
        OutOfProcessResult r = host.mount("x", ZEN_SO_SHARD, Grant{}); // default → intends contained
        CHECK_FALSE(r.ok);
        CHECK_FALSE(host.is_mounted("x"));
    }
    SUBCASE("dev-mode ALSO refuses — a surprise failure is not a known gap") {
        Switchboard bus;
        IsolationHost host(bus, kHostExe);
        host.set_dev_mode(true);
        host.force_entry_failure_for_test(true);
        OutOfProcessResult r = host.mount("x", ZEN_SO_SHARD, Grant{});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(host.is_mounted("x"));
    }
}

TEST_CASE("unmount tears the child down cleanly and the proxy leaves the bus") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);

    OutOfProcessResult r = host.mount("w", ZEN_SO_SHARD, Grant{});
    REQUIRE_MESSAGE(r.ok, r.error);
    const ShardId id = r.id;
    CHECK(bus.alive(id));

    host.unmount("w");
    CHECK_FALSE(host.is_mounted("w"));

    Ticket t = bus.send(id, Message(ping(1)));
    bus.pump();
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::NoSuchTarget);
}

} // TEST_SUITE
