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

#include <string>
#include <vector>

using namespace zen;
using namespace zen::sb;
using namespace zen::kernel;
using namespace zen::isolation;
using namespace sbfx;

namespace {
const std::string kHostExe = ZEN_SHARD_HOST_EXE;
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

TEST_CASE("honest containment status: isolated, not sandboxed") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);

    CHECK(host.containment("nope") == "not mounted");

    OutOfProcessResult r = host.mount("w", ZEN_SO_SHARD, Grant{});
    REQUIRE_MESSAGE(r.ok, r.error);

    const std::string status = host.containment("w");
    CHECK(status.find("isolated") != std::string::npos);
    CHECK(status.find("Not sandboxed") != std::string::npos);
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
