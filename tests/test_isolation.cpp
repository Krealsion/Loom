// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The B2 milestone: a Weave hosted out-of-process is indistinguishable to the bus
// from one hosted in-process, a crashing child is contained (host survives, bounded
// reload, then quarantine), and the process boundary is gated exactly like every
// other boundary — malformed child output is gate-refused, an emitted message is
// authorized against the child's grant, and the sender is stamped from the
// connection (a child cannot forge it). The host never blocks on a child.

#include <doctest.h>

// This suite owns the "isolation" OS-enforcement population; the gate keys its tally by
// this name so isolation's executions can never satisfy another suite's floor (POP-02).
#define ZEN_ENFORCEMENT_DOMAIN "isolation"
#include "enforcement_gate.hpp"
#include "switchboard_fixtures.hpp"
#include "weavelib/office_protocol.hpp"

#include <zen/isolation/channel.hpp>
#include <zen/isolation/host.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/switchboard.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace loom {
/// The R2F-C observation instrument (see the friend declaration in zen/isolation/channel.hpp):
/// reads the channel's OWN retained buffers, so the bounded-storage law is stated as an assertion
/// about transport state rather than inferred from process memory -- RSS is allocator- and
/// OS-sensitive and cannot tell "capacity remains reusable" from "sent bytes remain part of the
/// live buffer". Those are different claims and only the second is F-18. This adds no member and
/// no code path: channel.cpp's object file is byte-identical with and without the declaration.
struct ChannelStorageProbe {
    static std::size_t live(const Channel& c) { return c.outbox_.size(); }
    static std::size_t sent(const Channel& c) { return c.out_pos_; }
    static std::size_t unsent(const Channel& c) { return c.outbox_.size() - c.out_pos_; }
    static std::size_t inbox(const Channel& c) { return c.inbox_.size(); }
};
} // namespace loom

using namespace loom;
using namespace loom;
using namespace loom;
using namespace loom;
using namespace sbfx;

namespace {
const std::string kHostExe = ZEN_WEAVE_HOST_EXE;

/// What the memory-bomb fixture reports when it did NOT die (see `detonate()` in
/// `weavelib/test_weave.cpp`). Mirrored here rather than shared because the
/// fixture is a separate artifact built into its own `.so`; the two failures stay
/// distinct so "the kernel refused the allocation" can never be read as "the
/// cgroup killed it".
constexpr std::int64_t kBombAllocRefused = -101; ///< refused BEFORE any pressure
constexpr std::int64_t kBombSurvived = -102;     ///< the full page walk completed

// Matches the net-probe weave's emitted shape (NetResult{code}); the child reports
// the connect() errno here so a test can read it off the bus.
std::shared_ptr<const Schema> netresult_schema() {
    static const auto s = SchemaBuilder("NetResult", 1).field("code", Kind::Int).build();
    return s;
}
/// The byte the net probe pushes down a connection it opened. Mirrored from
/// `weavelib/test_weave.cpp` rather than shared through a header, exactly as
/// `kBombAllocRefused` above is: the fixture is a separate artifact in its own `.so`.
constexpr char kNetProbeToken = 'Z';

/// THE ENDPOINT THE NETWORK CASE OWNS (BL-VER-08).
///
/// The positive control used to prove "this child can reach the network" from the
/// errno of a connect() to port 1 — ECONNREFUSED meaning *reachable, nothing
/// listening*. That made the proof depend on how the HOST answers a closed port, and
/// on a WSL2 mirrored-networking host it does not answer at all: the SYN is
/// black-holed and connect() blocks for the kernel's whole retry budget (~124 s
/// measured, BL-VER-07). The test was asking somebody else's closed door to prove
/// that its own door opens.
///
/// So the test brings its own door. A listener on 127.0.0.1:0 — the kernel picks the
/// port, so nothing is reserved, occupied, firewalled or guessed — is established
/// BEFORE any child is mounted, and its port is handed to the probe. The positive
/// witness becomes success rather than a particular kind of failure.
///
/// The child cannot reach this by inheritance: the exec boundary closes every
/// descriptor but the control fd and the standard three (C-2), so a child that
/// arrives here arrived over the network. It is marked close-on-exec anyway, to say
/// so.
class TestOwnedEndpoint {
public:
    TestOwnedEndpoint() {
        listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        REQUIRE(listener_ >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // the kernel chooses: no fixed port, no reservation
        REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listener_, 4) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
        REQUIRE(port_ != 0); // bound and listening before any child exists
    }
    ~TestOwnedEndpoint() {
        if (accepted_ >= 0) {
            ::close(accepted_);
        }
        if (listener_ >= 0) {
            ::close(listener_);
        }
    }
    TestOwnedEndpoint(const TestOwnedEndpoint&) = delete;
    TestOwnedEndpoint& operator=(const TestOwnedEndpoint&) = delete;

    /// The port to tell a probe. Live from construction, so readiness is a fact
    /// established by listen()/getsockname() and never a sleep.
    std::int64_t port() const { return port_; }

    /// Take the one connection this endpoint expects and read the token off it.
    /// Returns the byte received, or -1 if none arrived. Bounded by `budget_ms` —
    /// poll() reports readiness the kernel already knows, so nothing here waits on a
    /// guess: a connect() that returned 0 has already been queued here.
    int accept_token(int budget_ms) {
        pollfd pfd{listener_, POLLIN, 0};
        if (::poll(&pfd, 1, budget_ms) != 1) {
            return -1; // nobody connected
        }
        accepted_ = ::accept(listener_, nullptr, nullptr);
        if (accepted_ < 0) {
            return -1;
        }
        pollfd rfd{accepted_, POLLIN, 0};
        if (::poll(&rfd, 1, budget_ms) != 1) {
            return -1; // connected, but no byte ever came
        }
        char byte = 0;
        return ::recv(accepted_, &byte, 1, 0) == 1 ? static_cast<unsigned char>(byte) : -1;
    }

private:
    int listener_ = -1;
    int accepted_ = -1;
    std::int64_t port_ = 0;
};
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
// Matches the env-probe weave's emitted shape (C-2a): the child's COMPLETE environment.
std::shared_ptr<const Schema> envresult_schema() {
    static const auto s = SchemaBuilder("EnvResult", 1)
                              .field("count", Kind::Int)
                              .field("ld_count", Kind::Int)
                              .field("secret_present", Kind::Int)
                              .field("names", Kind::Text)
                              .build();
    return s;
}
// Matches the fd-probe weave's emitted shape (C-2): the descriptor inventory the child
// actually holds, whether the parked one can still move bytes, and — kept separate on
// purpose — whether the network namespace is still imposed.
std::shared_ptr<const Schema> fdresult_schema() {
    static const auto s = SchemaBuilder("FdResult", 1)
                              .field("open_low", Kind::Int)
                              .field("open_high", Kind::Int)
                              .field("parked_write", Kind::Int)
                              .field("fresh_connect", Kind::Int)
                              .build();
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

    // The endpoint belongs to this test, and exists before either child does. Both
    // halves are aimed at THIS port, so the only meaningful difference between them is
    // the grant (BL-VER-08). A live endpoint also makes the negative half say more than
    // it used to: the contained child is now refused a destination that provably works.
    TestOwnedEndpoint endpoint;

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

    bus.send(s.id, Message(ping(endpoint.port()), WeaveId{}, rec1.id));
    REQUIRE(host.run_until([&] { return !rec1.weave->handled_names.empty(); }, 2000));
    CHECK(rec1.weave->handled_names.back() == "NetResult"); // it still emitted (sandbox != muzzle)
    // Its netns has no interface at all, so the kernel refuses before a SYN is ever
    // sent — the same verdict whatever is listening on the far side, which is why this
    // stays the exact errno and not "anything nonzero". A dead endpoint or a broken
    // probe must not be able to impersonate containment.
    CHECK(contained_code == ENETUNREACH);

    // Granted Network: no netns is imposed, so the child is in this process's own
    // network namespace and the endpoint above is genuinely its 127.0.0.1 too. The
    // same probe, the same port, one grant apart.
    std::int64_t granted_code = -1; // sentinel: never a value the probe can report
    Registered rec2 = register_probe(bus, {netresult_schema()});
    rec2.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
        granted_code = in.payload.get("code")->as_int();
    };
    Grant granted;
    granted.allow("NetResult", 1, rec2.id).with_os_capabilities(os_cap::Network);
    OutOfProcessResult g = host.mount("granted", ZEN_SO_NETPROBE, std::move(granted));
    REQUIRE_MESSAGE(g.ok, g.error);
    CHECK(host.containment("granted").find("network: granted") != std::string::npos);

    bus.send(g.id, Message(ping(endpoint.port()), WeaveId{}, rec2.id));
    REQUIRE(host.run_until([&] { return !rec2.weave->handled_names.empty(); }, 2000));
    CHECK(granted_code != ENETUNREACH); // it CAN reach the network
    CHECK(granted_code == 0);           // specifically: it connected and moved a byte

    // BOTH ENDS, as in C-2: the child said it wrote, and this end says it arrived. One
    // half alone would leave "the connection opened but carries nothing" unexamined,
    // and a handshake is not a data path.
    CHECK(endpoint.accept_token(2000) == kNetProbeToken);
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

    // SILENCE IS THE WITNESS. The bomb replies on every path where it did NOT
    // die, so a reply arriving means containment failed — and the sentinel says
    // WHICH failure, rather than leaving "it answered" to be interpreted.
    //
    // READ IT BEFORE THE REQUIRE BELOW, DELIBERATELY. `REQUIRE(quarantined)`
    // aborts the case, so a diagnostic placed after it is computed only on the
    // runs that did not need it — and never on the one failure it exists to
    // explain. The interesting failure is exactly "the bomb lived, so nothing
    // was quarantined", and that is the run that must say why.
    const std::int64_t sentinel =
        rec.weave->handled_values.empty() ? 0 : rec.weave->handled_values.front();
    INFO("bomb reply sentinel = "
         << sentinel
         << "  (0 = no reply, which is the expected kill; " << kBombAllocRefused
         << " = the kernel refused the allocation, so NO pressure was ever applied and this is "
            "not evidence of containment; "
         << kBombSurvived << " = the full page walk completed and containment did not act)");
    CHECK(sentinel != kBombAllocRefused);
    CHECK(sentinel != kBombSurvived);

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

    // ...and it survived the WHOLE walk. Merely receiving a reply is too weak a
    // control: the fixture also replies when the kernel refused the allocation
    // outright, and that run would prove the opposite of what this case exists to
    // prove — that ~200 MiB genuinely fits under a 512 MiB cap, so the 64 MiB cap
    // next door is the cause of the kill rather than the allocation size itself.
    REQUIRE_FALSE(rec.weave->handled_values.empty());
    const std::int64_t sentinel = rec.weave->handled_values.back();
    INFO("negative-control sentinel = " << sentinel << " (expected " << kBombSurvived
                                        << " = every page written; " << kBombAllocRefused
                                        << " would mean the allocation never happened)");
    CHECK(sentinel == kBombSurvived);
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

TEST_CASE("C-2: the descriptor sweep keeps exactly its allow-list, and refuses a malformed one") {
    // The mechanism itself, asked directly — the end-to-end case above proves the
    // BOUNDARY, this proves the TOOL. Two properties, and the second is the one that
    // has no other witness: the sweep walks the gaps BETWEEN allow-list entries, so an
    // unsorted list would leave a whole range unswept while returning success. That is
    // a silent hole in a function whose entire value is having none, so a malformed
    // list is refused up front — before a single descriptor is touched, which is why
    // these three calls are safe to make in the test process itself.
    const int unsorted[] = {3, 1};
    CHECK(close_inherited_descriptors(unsorted, 2) == -1);
    const int duplicated[] = {1, 1};
    CHECK(close_inherited_descriptors(duplicated, 2) == -1);
    const int negative[] = {-1, 3};
    CHECK(close_inherited_descriptors(negative, 2) == -1);

    // And the sweep itself, in a throwaway fork-child so the test process keeps its own
    // descriptors. The child's ONLY surviving descriptor is its report pipe, which is
    // also the allow-list — so the answer arrives over the very mechanism under test.
    //
    // BOTH implementations are run, not merely whichever one this kernel selects. A
    // fallback that executes only on hosts nobody tests on is a claim with no witness;
    // the enumeration path exists precisely for kernels older than close_range(2), which
    // is exactly the population least likely to run this suite.
    struct Sweep {
        char rc = 0;     ///< the sweep returned 0
        char before = 0; ///< the victim was open BEFORE (so `after` is a change, not a fact)
        char after = 0;  ///< ...and is open after
        char kept = 0;   ///< the allow-listed descriptor survived
    };
    const auto probe_sweep = [](int (*sweep)(const int*, std::size_t)) {
        int report[2] = {-1, -1};
        REQUIRE(::pipe(report) == 0);
        const int victim = ::dup(report[0]); // an ordinary descriptor, deliberately unlisted
        REQUIRE(victim >= 0);
        REQUIRE(victim != report[1]);
        const int keep[] = {report[1]};

        const pid_t pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            Sweep s;
            s.before = ::fcntl(victim, F_GETFD) != -1 ? 1 : 0;
            s.rc = sweep(keep, 1) == 0 ? 1 : 0;
            s.after = ::fcntl(victim, F_GETFD) != -1 ? 1 : 0;
            s.kept = ::fcntl(report[1], F_GETFD) != -1 ? 1 : 0;
            (void)!::write(report[1], &s, sizeof(s));
            ::_exit(0);
        }
        Sweep got;
        const bool complete = ::read(report[0], &got, sizeof(got)) == sizeof(got);
        int status = 0;
        (void)::waitpid(pid, &status, 0);
        ::close(victim);
        ::close(report[0]);
        ::close(report[1]);
        REQUIRE(complete);
        return got;
    };

    const Sweep policy = probe_sweep(&close_inherited_descriptors);
    CHECK(policy.rc == 1);
    CHECK(policy.before == 1);
    CHECK(policy.after == 0);
    CHECK(policy.kept == 1);

    const Sweep enumerated = probe_sweep(&close_descriptors_by_enumeration);
    CHECK(enumerated.rc == 1);
    CHECK(enumerated.before == 1);
    CHECK(enumerated.after == 0);
    CHECK(enumerated.kept == 1);

    // close_range(2) is an ENVIRONMENT FACT, so it is reported rather than demanded:
    // absent, the policy above still passed, which is the whole point of having two.
    const Sweep ranged = probe_sweep(&close_descriptors_by_close_range);
    MESSAGE("close_range(2) on this kernel: "
            << std::string(ranged.rc == 1 ? "present (the policy's preferred mechanism)"
                                          : "ABSENT -- the policy fell back to enumeration"));
    if (ranged.rc == 1) {
        CHECK(ranged.before == 1);
        CHECK(ranged.after == 0);
        CHECK(ranged.kept == 1);
    }
}

TEST_CASE("C-2: the child inherits NO ambient host descriptor, and the netns is still real") {
    // COLD-2's attack, reconstructed here rather than quoted. The host builds a real
    // connected loopback TCP pair through its own network stack, a real pipe and a real
    // file, parks one end of each at a known descriptor number WITHOUT FD_CLOEXEC, and
    // then mounts a weave with Network withheld. Before C2 the child received all three,
    // could write to them, and the bytes arrived back on the host side — while
    // containment() reported `network: contained (confirmed: child netns distinct from
    // host)` over that same byte.
    //
    // THE TWO FACTS ARE ASSERTED SEPARATELY, and that separation is the point. A test
    // that observes only ENETUNREACH proves the namespace and says nothing about
    // inheritance; COLD-2's host produced ENETUNREACH and a working escape at the same
    // time. So each result carries the namespace verdict AND the descriptor inventory,
    // and this case fails if either half regresses.
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Network},
                            "C-2: ambient descriptors removed at exec while the netns holds");

    // ---- the host's own ambient capabilities, built for real ----------------------
    //
    // Each is parked with F_DUPFD (never F_DUPFD_CLOEXEC), so the copy the child could
    // inherit has FD_CLOEXEC CLEAR — deliberately the hostile case, exactly as COLD-2
    // left it. The originals and the host-side ends are marked CLOEXEC so the ONE
    // subject of each assertion is the parked number and nothing else.
    const auto park = [](int fd) {
        const int parked = ::fcntl(fd, F_DUPFD, 20); // lowest free >= 20, CLOEXEC clear
        REQUIRE(parked >= 20);
        REQUIRE(parked < 63); // must land inside the bitmap the probe reports
        REQUIRE((::fcntl(parked, F_GETFD) & FD_CLOEXEC) == 0);
        return parked;
    };
    const auto cloexec = [](int fd) { REQUIRE(::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0); };

    // A genuinely connected TCP pair over loopback: the host end stays here, the client
    // end is what the child would inherit.
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    cloexec(listener);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // any free port
    REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(listener, 1) == 0);
    socklen_t addr_len = sizeof(addr);
    REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len) == 0);
    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(client >= 0);
    cloexec(client);
    REQUIRE(::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    const int host_end = ::accept(listener, nullptr, nullptr);
    REQUIRE(host_end >= 0);
    cloexec(host_end);

    int pipe_fds[2] = {-1, -1};
    REQUIRE(::pipe(pipe_fds) == 0);
    cloexec(pipe_fds[0]);
    cloexec(pipe_fds[1]);

    const std::string file_path = "/tmp/zen_c2_ambient_file.txt";
    const int file = ::open(file_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    REQUIRE(file >= 0);

    const int parked_socket = park(client);       // a CONNECTED socket, ready to move bytes
    const int parked_pipe = park(pipe_fds[1]);    // a non-network descriptor: the write end
    const int parked_file = park(file);           // a non-network descriptor: an ordinary file

    // ---- the mount: Network withheld, filesystem None, resources contained --------
    struct Reading {
        std::int64_t open_low = -1;
        std::int64_t open_high = -1;
        std::int64_t parked_write = -1;
        std::int64_t fresh_connect = -1;
    };
    std::vector<Reading> readings;
    Registered rec = register_probe(bus, {fdresult_schema()});
    rec.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
        Reading r;
        r.open_low = in.payload.get("open_low")->as_int();
        r.open_high = in.payload.get("open_high")->as_int();
        r.parked_write = in.payload.get("parked_write")->as_int();
        r.fresh_connect = in.payload.get("fresh_connect")->as_int();
        readings.push_back(r);
    };
    Grant grant;
    grant.allow("FdResult", 1, rec.id); // it may REPORT freely: a sandbox is not a muzzle
    OutOfProcessResult mounted = host.mount("c2", ZEN_SO_FDPROBE, std::move(grant));
    REQUIRE_MESSAGE(mounted.ok, mounted.error);
    CHECK(host.containment("c2").find("network: contained") != std::string::npos);

    // One Ping per parked descriptor; `seq` names the fd the child must try to USE.
    const int parked[] = {parked_socket, parked_pipe, parked_file};
    for (int fd : parked) {
        bus.send(mounted.id, Message(ping(fd), WeaveId{}, rec.id));
    }
    REQUIRE(host.run_until([&] { return readings.size() == 3; }, 4000));

    // The intentional set, stated as a number rather than described: stdin, stdout,
    // stderr (kept deliberately — the child shares the host's console) and fd 3, the
    // weave-host protocol transport. Anything else in the child's table is inherited.
    const std::int64_t kIntendedFds = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3);
    for (std::size_t i = 0; i < readings.size(); ++i) {
        const Reading& r = readings[i];
        const int fd = parked[i];
        CAPTURE(i);
        CAPTURE(fd);

        // (a) THE NAMESPACE IS STILL REAL. Asserted every time, so descriptor hygiene
        //     can never be mistaken for containment if the netns were ever lost.
        CHECK(r.fresh_connect == ENETUNREACH);

        // (b) THE INHERITED DESCRIPTOR IS GONE — not merely renumbered. Absent from the
        //     low bitmap AND from the high range, so "it moved to fd 900" fails here.
        CHECK((r.open_low & (static_cast<std::int64_t>(1) << fd)) == 0);
        CHECK(r.open_high == 0);

        // (c) AND IT CANNOT MOVE BYTES. Presence and usability are different questions;
        //     COLD-2's payload write returned 20, this one must fail.
        CHECK(r.parked_write == EBADF);

        // (d) THE INSTRUMENT WORKS. If the probe simply could not see open descriptors,
        //     (b) would pass vacuously — so the transport it is provably using right now
        //     must be visible in the very same bitmap.
        CHECK((r.open_low & (static_cast<std::int64_t>(1) << 3)) != 0);

        // (e) AUTOMATIC FUTURE PROTECTION. Not "the three fds this test parked" but
        //     "nothing beyond the intended set", so a descriptor some future host
        //     composition happens to hold open before spawn fails here without anyone
        //     remembering to extend this case.
        CHECK((r.open_low & ~kIntendedFds) == 0);
    }

    // (f) THE HOST'S OWN SIDE HEARD NOTHING. The end-to-end half of COLD-2: its host
    //     read "COLD2-ESCAPE-PAYLOAD" off this very socket. Both ends are checked, so a
    //     leak that somehow wrote without the child noticing still fails.
    // errno is captured on the SAME line as the syscall, never read from a later
    // assertion: doctest's own reporting writes to stdout between assertions, and under
    // `-s` that write resets errno — which showed up here as two failures that appeared
    // only when successes were printed. An errno read one assertion late is a flake.
    char sink[64];
    errno = 0;
    const ssize_t from_socket = ::recv(host_end, sink, sizeof(sink), MSG_DONTWAIT);
    const int socket_errno = errno;
    CHECK(from_socket < 0);
    CHECK((socket_errno == EAGAIN || socket_errno == EWOULDBLOCK));
    REQUIRE(::fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) == 0);
    errno = 0;
    const ssize_t from_pipe = ::read(pipe_fds[0], sink, sizeof(sink));
    const int pipe_errno = errno;
    CHECK(from_pipe < 0);
    CHECK((pipe_errno == EAGAIN || pipe_errno == EWOULDBLOCK));

    host.unmount("c2");
    for (int fd : parked) {
        ::close(fd);
    }
    ::close(host_end);
    ::close(listener);
    ::close(client);
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    ::close(file);
    (void)::unlink(file_path.c_str());
}

TEST_CASE("C-2a: the child's environment is the one Zen authored, not the host's ambient one") {
    // C2 closed the descriptor half of the exec boundary; this is the other half. The
    // child used to receive `environ` wholesale, so a weave at FsAccess::None with no
    // network was still handed the host's HOME, PATH, session-bus and compositor
    // addresses, whatever tokens the embedding process held -- and any LD_*, which the
    // loader acts on BEFORE any Zen code in the child runs.
    //
    // The load-bearing assertion is `count`, not any named lookup. A test that only
    // asked "is ZEN_C2A_AMBIENT_SECRET absent?" would pass forever while every other
    // host variable kept crossing, and would say nothing about a variable introduced
    // next year. Asserting the COMPLETE set against the authored one is what makes an
    // unknown future variable fail here with nobody remembering to add it.
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), {Capability::Network},
                            "C-2a: the child's environment is authored, not inherited");

    // Plant ambient state in THIS process, the way an embedding host really would. Each
    // is chosen to be inert if it did cross: a fake secret, a loader search path that
    // does not exist, a preload of a file that does not exist (the loader warns and
    // continues -- deliberately NOT a real preload, which would be an arbitrary-code
    // experiment), and a sanitizer option equal to its own default.
    struct Planted {
        const char* name;
        const char* value;
    };
    const Planted planted[] = {
        {"ZEN_C2A_AMBIENT_SECRET", "should-not-cross"},
        {"LD_LIBRARY_PATH", "/zen-c2a-nonexistent-lib-dir"},
        {"LD_PRELOAD", "/zen-c2a-nonexistent-preload.so"},
        {"ASAN_OPTIONS", "verbosity=0"},
    };
    for (const Planted& p : planted) {
        REQUIRE(::setenv(p.name, p.value, 1) == 0);
    }
    // Proof the planting worked: without this, everything below could pass because the
    // variables were never set rather than because they did not cross.
    for (const Planted& p : planted) {
        const char* here = std::getenv(p.name);
        REQUIRE(here != nullptr);
        CHECK(std::string(here) == p.value);
    }

    std::int64_t count = -1;
    std::int64_t ld_count = -1;
    std::int64_t secret_present = -1;
    std::string names = "<no reply>";
    Registered rec = register_probe(bus, {envresult_schema()});
    rec.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
        count = in.payload.get("count")->as_int();
        ld_count = in.payload.get("ld_count")->as_int();
        secret_present = in.payload.get("secret_present")->as_int();
        names = std::string(in.payload.get("names")->as_text());
    };
    Grant grant;
    grant.allow("EnvResult", 1, rec.id); // it may REPORT freely: a sandbox is not a muzzle
    OutOfProcessResult mounted = host.mount("c2a", ZEN_SO_ENVPROBE, std::move(grant));
    REQUIRE_MESSAGE(mounted.ok, mounted.error);

    // The full supported child path had to work to get here and to get a reply back:
    // execve, dynamic-loader startup, dlopen of the weave, the ABI check, create(), the
    // handshake (manifest + policy + snapshot), and now an ordinary message round trip
    // -- all under the authored environment. A successful execve alone would prove none
    // of that.
    bus.send(mounted.id, Message(ping(1), WeaveId{}, rec.id));
    REQUIRE(host.run_until([&] { return !rec.weave->handled_names.empty(); }, 4000));
    CHECK(rec.weave->handled_names.back() == "EnvResult");

    MESSAGE("child environment (" << count << " entries): " << (names.empty() ? "<empty>" : names));

    // THE COMPLETE SET. build_child_environment() authors nothing today, so the child's
    // environment is empty; if a future phase authors a variable, this number changes
    // deliberately here and in that builder, together.
    CHECK(count == 0);
    CHECK(names.empty());

    // The named questions, kept separate so a failure says WHICH boundary moved.
    CHECK(secret_present == 0); // the planted ambient secret is not visible to the child
    CHECK(ld_count == 0);       // no loader variable crossed -- LD_* is capability-bearing

    host.unmount("c2a");
    for (const Planted& p : planted) {
        REQUIRE(::unsetenv(p.name) == 0); // do not leave planted state to the next case
    }
}

TEST_CASE("C-2a: an authored environment refuses malformed entries rather than half-applying") {
    // The refusal path the spawn depends on (there is no fallback to `environ`, so a
    // malformed authored environment must be a REFUSAL, not a smaller environment).
    // Cheap and pure, so every posture is testable without a spawn.
    ChildEnvironment empty = build_child_environment();
    CHECK(empty.ok());
    CHECK(empty.size() == 0);
    REQUIRE(empty.data() != nullptr);
    CHECK(empty.data()[0] == nullptr); // an empty environment is still NULL-terminated

    ChildEnvironment one;
    one.set("ZEN_EXAMPLE", "value");
    CHECK(one.ok());
    CHECK(one.size() == 1);
    CHECK(std::string(one.data()[0]) == "ZEN_EXAMPLE=value");
    CHECK(one.data()[1] == nullptr);

    // An empty value is legitimate -- "set but empty" is a real, distinct state.
    ChildEnvironment blank;
    blank.set("ZEN_EXAMPLE", "");
    CHECK(blank.ok());
    CHECK(std::string(blank.data()[0]) == "ZEN_EXAMPLE=");

    // Each refusal poisons the WHOLE environment: an environment missing something Zen
    // meant to author is not the authored one, and a caller that cannot see the
    // difference would spawn against a silently different set.
    ChildEnvironment named;
    named.set("", "value");
    CHECK_FALSE(named.ok());

    ChildEnvironment equals;
    equals.set("ZEN=EXAMPLE", "value");
    CHECK_FALSE(equals.ok());

    ChildEnvironment nul_name;
    nul_name.set(std::string_view("ZEN\0X", 5), "value");
    CHECK_FALSE(nul_name.ok());

    ChildEnvironment nul_value;
    nul_value.set("ZEN_EXAMPLE", std::string_view("a\0b", 3));
    CHECK_FALSE(nul_value.ok());

    ChildEnvironment duplicated;
    duplicated.set("ZEN_EXAMPLE", "first");
    duplicated.set("ZEN_EXAMPLE", "second");
    CHECK_FALSE(duplicated.ok()); // which one the child would see is a lookup-order detail
    CHECK(duplicated.size() == 1);

    // A name that merely SHARES A PREFIX is not a duplicate -- the check must compare
    // the name, not a substring, or authoring ZEN_A and ZEN_AB would refuse.
    ChildEnvironment prefixed;
    prefixed.set("ZEN_A", "1");
    prefixed.set("ZEN_AB", "2");
    CHECK(prefixed.ok());
    CHECK(prefixed.size() == 2);
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

TEST_CASE("BL-0: a child's vocabulary is claimed by the MOUNT, and released by unmount") {
    // THE LIFETIME §13 ASKS FOR, PINNED. The narrowest object whose life
    // truthfully means "these bytes may still need these schemas" is the mount,
    // not the child process: a child that dies is respawned under the same Link
    // and re-uses the accept-set cached at handshake without reconstructing it,
    // and the channel's unread bytes belong to the Link too. So the mount claims,
    // and unmount is the whole release path.
    //
    // Read through the BUS, because that is the registry the child's emissions
    // are actually gated against.
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    CHECK(bus.resolve_schema("Ping", 1) == nullptr);

    OutOfProcessResult a = host.mount("m1", ZEN_SO_WEAVE, Grant{});
    REQUIRE_MESSAGE(a.ok, a.error);
    CHECK(bus.resolve_schema("Ping", 1) != nullptr);

    // A second mount of the same artifact: two mounts, one canonical definition.
    OutOfProcessResult b = host.mount("m2", ZEN_SO_WEAVE, Grant{});
    REQUIRE_MESSAGE(b.ok, b.error);
    const Schema* canonical = bus.resolve_schema("Ping", 1).get();

    host.unmount("m1");
    REQUIRE(bus.resolve_schema("Ping", 1) != nullptr); // m2 still needs it
    CHECK(bus.resolve_schema("Ping", 1).get() == canonical);

    host.unmount("m2");
    CHECK(bus.resolve_schema("Ping", 1) == nullptr);
}

TEST_CASE("BL-0: repeated mount/unmount does not accumulate vocabulary") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    for (int i = 0; i < 8; ++i) {
        OutOfProcessResult r = host.mount("cycle", ZEN_SO_WEAVE, Grant{});
        REQUIRE_MESSAGE(r.ok, r.error);
        REQUIRE(bus.resolve_schema("Ping", 1) != nullptr);
        host.unmount("cycle");
        REQUIRE(bus.resolve_schema("Ping", 1) == nullptr);
    }
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

// ---- R2F-C: consumed transport bytes are history, not live channel storage (LIFE-07) -----------
//
// COLD-1 F-18 named BOTH framers. The isolation Channel is the parent side of every out-of-process
// Weave link, so a long-lived host with a child that keeps up but never lets the socket run dry
// retained the whole session's byte volume: flush() clear()ed the outbox only on an EXACT drain,
// and kMaxBacklog measures the UNSENT residue, so nothing ever noticed. Measured pre-repair on this
// shape: +261 B per round, strictly linear, 524,160 bytes already sent and still retained after
// 2,000 rounds, failed() never set.
//
// These are deliberately INDEPENDENT of the BridgeChannel proofs in test_bridge.cpp: the two
// framers are separate source files with separate repairs, and one is not evidence for the other.

namespace {

struct ChannelPair { // a socketpair with a deliberately small in-flight window
    int producer = -1;
    int consumer = -1;
};

ChannelPair channel_pair(bool shrink = true) {
    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    if (shrink) {
        const int small = 2048; // the point is the RULE, not the volume
        (void)::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
        (void)::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    }
    return ChannelPair{sv[0], sv[1]};
}

constexpr std::size_t kChBodyLen = 200;
constexpr std::size_t kChFrameLen = 5 + kChBodyLen;

std::string ch_body(int i) {
    std::string p = "frame:" + std::to_string(i) + ":";
    p.resize(kChBodyLen, static_cast<char>('a' + (i % 26)));
    return p;
}

/// Where a byte offset falls in the uniform frame layout above. The outbox itself has no such
/// structure -- it is an undifferentiated byte stream, so a compaction boundary may land anywhere
/// and the move is the same either way. This only classifies what the run actually exercised.
enum class ChLanding { FrameEdge, InHeader, InPayload };
ChLanding ch_classify(std::size_t offset) {
    const std::size_t off = offset % kChFrameLen;
    if (off == 0) {
        return ChLanding::FrameEdge;
    }
    return off < 5 ? ChLanding::InHeader : ChLanding::InPayload;
}

} // namespace

TEST_CASE("R2F-C (isolation): a channel that is never idle still reclaims what it has already sent") {
    using P = ChannelStorageProbe;
    const ChannelPair fds = channel_pair();
    Channel ch(fds.producer);
    Channel peer(fds.consumer);

    int next = 0;
    std::size_t queued_bytes = 0; // counted here, so it survives every clear() and compaction
    const auto queue_one = [&]() {
        ch.queue(Op::Emit, ch_body(next++));
        queued_bytes += kChFrameLen;
    };

    // Phase 1 -- measure the socket's in-flight window: nothing has been drained yet, so everything
    // queued minus what is still unsent is exactly what the socket swallowed.
    for (int i = 0; i < 20000 && P::unsent(ch) == 0; ++i) {
        queue_one();
        ch.flush();
    }
    REQUIRE(P::unsent(ch) > 0);
    const std::size_t window = queued_bytes - P::unsent(ch);

    // Phase 2 -- a standing backlog LARGER than that window, so no later flush can empty the buffer.
    for (int i = 0; i < 20000 && P::unsent(ch) < window + 8192; ++i) {
        queue_one();
        ch.flush();
    }
    const std::size_t target = P::unsent(ch);
    REQUIRE(target > window);

    std::vector<Incoming> got;
    std::size_t compactions = 0;
    std::size_t bytes_moved = 0;
    std::size_t max_live = 0;
    int in_header = 0;
    int in_payload = 0;
    int on_edge = 0;
    int law_violations = 0;
    int idle_rounds = 0;
    constexpr int kRounds = 400;
    for (int r = 0; r < kRounds; ++r) {
        peer.poll(got);                  // the child keeps up: it drains everything available ...
        while (P::unsent(ch) < target) { // ... and the host tops the backlog straight back up
            queue_one();
        }
        const std::size_t live_before = P::live(ch);
        ch.flush();
        if (P::live(ch) < live_before) { // only a clear()/compaction can shrink the buffer
            ++compactions;
            bytes_moved += P::live(ch);  // after erase(0, out_pos_) the size IS the bytes moved
            switch (ch_classify(queued_bytes - P::unsent(ch))) {
            case ChLanding::InHeader:
                ++in_header;
                break;
            case ChLanding::InPayload:
                ++in_payload;
                break;
            case ChLanding::FrameEdge:
                ++on_edge;
                break;
            }
        }
        if (P::unsent(ch) == 0) {
            ++idle_rounds; // the exact-drain clear() would have been reachable after all
        }
        if (P::live(ch) > 2 * P::unsent(ch)) {
            ++law_violations; // THE LAW: live storage tracks the BACKLOG, never the history
        }
        max_live = std::max(max_live, P::live(ch));
    }

    MESSAGE("window " << window << " B; queued " << queued_bytes << " B over " << next
                      << " frames; live " << P::live(ch) << " B (high-water " << max_live
                      << " B, backlog target " << target << " B); " << compactions
                      << " compactions moved " << bytes_moved << " B; boundary landed in-header "
                      << in_header << ", in-payload " << in_payload << ", on-edge " << on_edge);

    CHECK(idle_rounds == 0);      // the buffer never once became empty -- the F-18 shape held
    CHECK(law_violations == 0);   // ... and live storage stayed bounded by twice the backlog anyway
    CHECK(compactions > 0);       // reclamation actually ran (guards a vacuously bounded pass)
    CHECK(bytes_moved <= queued_bytes); // amortized: a move never costs more than the bytes it drops
    CHECK(queued_bytes > 20 * max_live); // history dwarfs the high-water of live storage
    CHECK(in_header + in_payload > 0);   // a compaction really did split a frame and carry the rest
    CHECK_FALSE(ch.failed());
    CHECK_FALSE(peer.failed());

    // Every frame still arrives, exactly once, in order, byte for byte.
    for (int i = 0; i < 20000 && static_cast<int>(got.size()) < next; ++i) {
        ch.flush();
        peer.poll(got);
    }
    REQUIRE(static_cast<int>(got.size()) == next);
    std::size_t first_bad = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i].op != Op::Emit || got[i].payload != ch_body(static_cast<int>(i))) {
            first_bad = i;
            break;
        }
    }
    CHECK(first_bad == static_cast<std::size_t>(-1)); // index of the first corrupted/reordered frame
}

TEST_CASE("R2F-C (isolation): frames queued behind a half-sent one keep their order and bytes") {
    using P = ChannelStorageProbe;
    const ChannelPair fds = channel_pair();
    Channel ch(fds.producer);
    Channel peer(fds.consumer);

    int next = 0;
    std::size_t queued_bytes = 0;
    const auto queue_one = [&]() {
        ch.queue(Op::Emit, ch_body(next++));
        queued_bytes += kChFrameLen;
    };
    for (int i = 0; i < 20000 && P::unsent(ch) == 0; ++i) {
        queue_one();
        ch.flush();
    }
    REQUIRE(P::unsent(ch) > 0);
    const std::size_t window = queued_bytes - P::unsent(ch);
    for (int i = 0; i < 20000 && P::unsent(ch) < 4 * window + 4096; ++i) {
        queue_one();
        ch.flush();
    }
    // Three COMPLETE frames behind the half-sent one, none of them yet touched by the socket.
    const int first_untouched = next;
    for (int i = 0; i < 3; ++i) {
        queue_one();
    }
    REQUIRE(P::unsent(ch) > 3 * kChFrameLen);

    std::vector<Incoming> got;
    std::size_t compactions_with_unsent_data = 0;
    for (int i = 0; i < 20000 && static_cast<int>(got.size()) < next; ++i) {
        peer.poll(got);
        const std::size_t live_before = P::live(ch);
        ch.flush();
        if (P::live(ch) < live_before && P::unsent(ch) > 0) {
            ++compactions_with_unsent_data;
        }
    }
    MESSAGE("compactions carrying live data: " << compactions_with_unsent_data);
    CHECK(compactions_with_unsent_data > 0); // the reclamation under test actually ran

    REQUIRE(static_cast<int>(got.size()) == next);
    std::size_t first_bad = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i].op != Op::Emit || got[i].payload != ch_body(static_cast<int>(i))) {
            first_bad = i;
            break;
        }
    }
    CHECK(first_bad == static_cast<std::size_t>(-1));
    CHECK(got[static_cast<std::size_t>(first_untouched)].payload == ch_body(first_untouched));
    CHECK(got[static_cast<std::size_t>(first_untouched) + 1].payload == ch_body(first_untouched + 1));
    CHECK(got[static_cast<std::size_t>(first_untouched) + 2].payload == ch_body(first_untouched + 2));
}

TEST_CASE("R2F-C (isolation): reclamation moves the backlog, it does not shrink it") {
    using P = ChannelStorageProbe;
    const ChannelPair fds = channel_pair();
    Channel ch(fds.producer);
    Channel peer(fds.consumer);

    // kMaxBacklog is measured as `outbox_.size() - out_pos_`. A compaction subtracts the SAME
    // amount from both terms, so the number the cap reads is invariant -- which is what keeps a
    // child that will not drain contained exactly as before.
    int next = 0;
    for (int i = 0; i < 20000 && P::unsent(ch) == 0; ++i) {
        ch.queue(Op::Emit, ch_body(next++));
        ch.flush();
    }
    REQUIRE(P::unsent(ch) > 0);
    for (int i = 0; i < 200; ++i) {
        ch.queue(Op::Emit, ch_body(next++));
    }

    std::vector<Incoming> got;
    bool saw_compaction = false;
    for (int i = 0; i < 20000 && !saw_compaction; ++i) {
        peer.poll(got);
        const std::size_t live_before = P::live(ch);
        const std::size_t unsent_before = P::unsent(ch);
        if (unsent_before == 0) {
            break;
        }
        ch.flush();
        if (P::live(ch) < live_before && P::unsent(ch) > 0) {
            saw_compaction = true;
            CHECK(P::sent(ch) == 0);               // the offset moved to the front ...
            CHECK(P::live(ch) == P::unsent(ch));   // ... and the buffer is now exactly the backlog
            CHECK(P::unsent(ch) <= unsent_before); // the backlog never grew across the move
        }
    }
    REQUIRE(saw_compaction);

    // The caps still fire on a channel whose reclamation has already been exercised.
    const std::string mib(1024u * 1024u, 'z');
    for (int i = 0; i < 80 && !ch.failed(); ++i) {
        ch.queue(Op::Emit, mib);
    }
    CHECK(ch.failed()); // an undrained backlog is contained, exactly as before the repair

    const std::size_t live_when_failed = P::live(ch);
    ch.flush();
    CHECK(ch.failed());
    CHECK(ch.done());
    CHECK(P::live(ch) == live_when_failed); // flush() on a failed channel compacts nothing
    ch.queue(Op::Emit, "ignored");
    CHECK(P::live(ch) == live_when_failed); // and queue() is still a no-op
}

TEST_CASE("R2F-C (isolation): a failed channel neither sends nor reclaims") {
    using P = ChannelStorageProbe;
    const ChannelPair fds = channel_pair();
    Channel ch(fds.producer);
    Channel peer(fds.consumer);

    // Stage the state where a HEALTHY flush would visibly act: a small unsent remainder and a
    // socket with room for all of it, so an honest flush would send everything and empty the
    // buffer. Behind a 64 MiB backlog and a full socket the guard cannot be observed at all,
    // because flush() would have nothing it could do either way.
    int next = 0;
    for (int i = 0; i < 20000 && P::unsent(ch) == 0; ++i) {
        ch.queue(Op::Emit, ch_body(next++));
        ch.flush();
    }
    REQUIRE(P::unsent(ch) > 0);
    std::vector<Incoming> got;
    peer.poll(got); // the socket is now empty: room for the whole remainder
    const std::size_t delivered_before = got.size();

    // Channel has no fail() affordance; an over-length frame is the ordinary way in, and it
    // returns BEFORE touching the buffer, so the staged state survives intact.
    const std::string over(static_cast<std::size_t>(kMaxFrameLen) + 1u, 'x');
    ch.queue(Op::Emit, over);
    REQUIRE(ch.failed());
    const std::size_t live_when_failed = P::live(ch);
    const std::size_t sent_when_failed = P::sent(ch);
    REQUIRE(live_when_failed > 0);

    ch.flush();
    peer.poll(got);
    CHECK(ch.failed());                     // still failed ...
    CHECK(ch.done());                       // ... and still done
    CHECK(P::live(ch) == live_when_failed); // nothing cleared, nothing compacted ...
    CHECK(P::sent(ch) == sent_when_failed); // ... and nothing sent
    CHECK(got.size() == delivered_before);  // ... so the peer received nothing more
}

TEST_CASE("R2F-C (isolation): the RECEIVE buffer was never part of F-18") {
    // The finding named the outbox. Its sibling already reclaims decoded bytes unconditionally
    // (`inbox_.erase(0, pos)`), so a permanently incomplete suffix does NOT pin consumed history in
    // place. Measured, not assumed -- the evidence for "inspected, already correct".
    using P = ChannelStorageProbe;
    const ChannelPair fds = channel_pair(/*shrink=*/false); // raw pushes must never block the test
    Channel ch(fds.consumer);

    constexpr int kFrames = 400;
    std::string stream;
    for (int i = 0; i <= kFrames; ++i) {
        put_u32(stream, static_cast<std::uint32_t>(kChBodyLen));
        put_u8(stream, static_cast<std::uint8_t>(Op::Emit));
        stream += ch_body(i);
    }

    std::vector<Incoming> got;
    std::size_t max_inbox = 0;
    std::size_t pos = 0;
    for (int i = 0; i < kFrames; ++i) {
        // Round 1 pushes one frame plus three bytes; every later round pushes exactly one frame's
        // worth. So EVERY poll() completes one frame and is left holding a 3-byte partial header.
        // The suffix is a GENUINE prefix of the next frame; junk would merely desync the framer,
        // which is a different (and already covered) question.
        const std::size_t push = (i == 0) ? kChFrameLen + 3 : kChFrameLen;
        std::size_t off = 0;
        for (int spin = 0; off < push && spin < 10000; ++spin) {
            const ssize_t w = ::send(fds.producer, stream.data() + pos + off, push - off, 0);
            if (w > 0) {
                off += static_cast<std::size_t>(w); // the unshrunk socket holds the whole run
            }
        }
        pos += push;
        ch.poll(got);
        max_inbox = std::max(max_inbox, P::inbox(ch));
    }
    MESSAGE("pushed " << pos << " B through the framer; high-water live inbox " << max_inbox << " B");
    CHECK(static_cast<int>(got.size()) == kFrames);
    CHECK(P::inbox(ch) == 3);           // exactly the incomplete suffix, and nothing behind it
    CHECK(max_inbox < 2 * kChFrameLen); // bounded by framing state, never by the traffic volume
    CHECK(pos > 20 * max_inbox);
    CHECK_FALSE(ch.failed());
    ::close(fds.producer);
}

// Keep this LAST in the file: a positive tally so a green can never mean "every OS-enforcement case
// silently skipped." (Relies on doctest's default registration order; a --order-by=rand run would
// instead assert the count in a reporter hook.)
//
// EXACTLY 17, from this suite's own witnesses only (POP-02). Fourteen ZEN_REQUIRE_ENFORCEABLE guard
// sites, three of which sit in cases doctest re-enters once per leaf subcase — hence 17 executions,
// not 14. The number is exact rather than a floor because `>= 12` had three executions of slack:
// a genuine enforcement witness could be deleted and this suite stayed 32/32 green with 230
// assertions, nothing moved. When a new OS-enforcement proof is added, raise this deliberately.
//
// 15 -> 16 at C2: the exec-boundary descriptor case is a THIRTEENTH guard site. It is a genuine
// OS-enforcement witness — it asserts ENETUNREACH from inside the namespace alongside the
// descriptor inventory — so it belongs in this population, and raising the number on purpose is
// exactly the price this contract charges for adding one.
//
// 16 -> 17 at C2a: the exec-boundary ENVIRONMENT case is the fourteenth. It mounts under the
// enforced floor and reads the environment from inside it, so it needs the same gate. Its pure
// companion (the ChildEnvironment well-formedness case) deliberately does NOT take the gate — it
// spawns nothing and asserts nothing about the OS — which is why the population moved by one.
TEST_CASE("enforcement coverage: the OS-enforcement proofs actually executed, not silently skipped") {
    ZEN_ENFORCEMENT_POPULATION(17);
}

} // TEST_SUITE
