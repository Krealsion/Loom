#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace loom;
using namespace loom;
using namespace sbfx;

// The adversarial Harness: each dummy Weave probes the boundary, and the
// boundary holds. Every refusal is observable via the tap.

TEST_SUITE("harness") {

TEST_CASE("a handler emitting a malformed payload: refused, recipient untouched, observed") {
    Switchboard bus;
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    Registered recorder = register_probe(bus, {ping_schema()});
    Registered saboteur = register_probe(bus, {ping_schema()});
    saboteur.weave->on_handle = [rid = recorder.id](const Message&, Bus& bus_, ProbeWeave&) {
        bus_.send(rid, Message(malformed_ping())); // a Ping missing its required 'seq'
    };

    bus.send(saboteur.id, Message(ping(1))); // valid trigger: saboteur handles, then sabotages
    bus.pump();

    CHECK(saboteur.weave->handled_values.size() == 1); // the valid trigger got through
    CHECK(recorder.weave->handled_names.empty());       // the malformed delivery never landed

    bool observed = false;
    for (const TapRecord& r : tap) {
        if (r.kind == EventKind::Refused && r.target == recorder.id &&
            r.reason == RefusalReason::GateRefused && r.error_kind == ErrorKind::MissingField) {
            observed = true;
        }
    }
    CHECK(observed);
}

TEST_CASE("a directed send to a non-accepting target is refused") {
    Switchboard bus;
    Registered r = register_probe(bus, {ping_schema()});
    Ticket t = bus.send(r.id, Message(greet("hi")));
    bus.pump();
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::NotAccepted);
    CHECK(r.weave->handled_names.empty());
}

TEST_CASE("a publish with zero accepters delivers nothing and errs nothing") {
    Switchboard bus;
    register_probe(bus, {ping_schema()});
    CHECK(bus.publish(Message(tick(0))) == 0);
    bus.pump();
    CHECK(bus.pending() == 0);
}

TEST_CASE("revive from corrupt state falls back to last-known-good when policy allows") {
    Switchboard bus;
    Registered r = register_probe(bus, {ping_schema()}, /*max_reloads=*/3, /*revive_lkg=*/true);

    // Advance state, then revive from a good snapshot so last-known-good = count 3.
    for (int i = 0; i < 3; ++i) {
        bus.send(r.id, Message(ping(1)));
    }
    bus.pump();
    CHECK(r.weave->count == 3);
    std::string good = bus.snapshot_bytes(r.id);
    ReviveOutcome ok = bus.reload(r.id, good);
    REQUIRE(ok.revived);
    CHECK_FALSE(ok.from_last_known_good);

    // Now a corrupt candidate arrives; the self returns as its last-known-good.
    r.weave->count = 999; // simulate a damaged in-memory self
    ReviveOutcome fb = bus.reload(r.id, "not zen bytes at all");
    CHECK(fb.revived);
    CHECK(fb.from_last_known_good);
    CHECK(fb.refusal.reason == RefusalReason::GateRefused);
    CHECK(r.weave->count == 3); // restored
}

TEST_CASE("revive from corrupt state is refused outright when policy forbids fallback") {
    Switchboard bus;
    Registered r = register_probe(bus, {ping_schema()}, /*max_reloads=*/3, /*revive_lkg=*/false);
    r.weave->count = 42;
    ReviveOutcome out = bus.reload(r.id, "garbage");
    CHECK_FALSE(out.revived);
    CHECK(out.refusal.reason == RefusalReason::GateRefused);
    CHECK(r.weave->count == 42); // untouched
}

TEST_CASE("a weave that has spent its reloads stays gone") {
    Switchboard bus;
    Registered r = register_probe(bus, {ping_schema()}, /*max_reloads=*/0, /*revive_lkg=*/true);
    ReviveOutcome out = bus.reload(r.id, "garbage");
    CHECK_FALSE(out.revived);
    CHECK(out.reloads_exhausted);
}

TEST_CASE("a flooder's many messages are all delivered FIFO and the loop terminates") {
    Switchboard bus;
    Registered sink = register_probe(bus, {tick_schema()});
    Registered flooder = register_probe(bus, {ping_schema()});

    constexpr int kN = 1000;
    flooder.weave->on_handle = [sid = sink.id](const Message&, Bus& bus_, ProbeWeave&) {
        for (int i = 0; i < kN; ++i) {
            bus_.send(sid, Message(tick(i)));
        }
    };

    bus.send(flooder.id, Message(ping(0)));
    bus.pump(); // delivers the trigger, which enqueues kN ticks, which all drain

    REQUIRE(sink.weave->handled_values.size() == static_cast<std::size_t>(kN));
    for (int i = 0; i < kN; ++i) {
        CHECK(sink.weave->handled_values[static_cast<std::size_t>(i)] == i);
    }
    CHECK(bus.pending() == 0);
}

} // TEST_SUITE
