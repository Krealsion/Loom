#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/gate.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace loom;
using namespace loom;
using namespace sbfx;

namespace {
// Shorthand; the real helper lives in the fixtures.
Registered reg(Switchboard& bus, std::vector<std::shared_ptr<const Schema>> accept,
               std::int64_t max_reloads = 2, bool revive_from_last_good = true) {
    return register_probe(bus, std::move(accept), max_reloads, revive_from_last_good);
}
} // namespace

TEST_SUITE("switchboard") {

TEST_CASE("registration records the accept-set and is queryable") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema(), greet_schema()});
    REQUIRE(bus.list_weaves().size() == 1);
    CHECK(bus.list_weaves()[0] == r.id);
    CHECK(bus.accepted_schemas(r.id).size() == 2);
    CHECK(bus.weave(r.id) != nullptr);
    CHECK(bus.alive(r.id));
}

TEST_CASE("a directed send to an accepting target is delivered and gated") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    Ticket t = bus.send(r.id, Message(ping(7)));
    CHECK(bus.outcome(t).disposition == Disposition::Pending); // deferred until pump
    bus.pump();
    CHECK(bus.outcome(t).disposition == Disposition::Delivered);
    REQUIRE(r.weave->handled_values.size() == 1);
    CHECK(r.weave->handled_values[0] == 7);
}

TEST_CASE("a send whose shape the target does not accept is refused, handler untouched") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    Ticket t = bus.send(r.id, Message(greet("hi")));
    bus.pump();
    CHECK(bus.outcome(t).disposition == Disposition::Refused);
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::NotAccepted);
    CHECK(r.weave->handled_names.empty());
}

TEST_CASE("a directed send to an unknown target is refused") {
    Switchboard bus;
    Ticket t = bus.send(WeaveId{9999}, Message(ping(1)));
    bus.pump();
    CHECK(bus.outcome(t).disposition == Disposition::Refused);
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::NoSuchTarget);
}

TEST_CASE("the delivery journal is a bounded ring: recent outcomes survive, ancient ones are evicted") {
    // Audit F-6: journal_ used to retain one outcome per message EVER enqueued — a
    // linear leak in lifetime throughput, and a bus is precisely the component that
    // runs for weeks. It is now a fixed-capacity ring of kJournalCapacity slots.
    // Flood far past the bound and show the journal keeps a recent window intact (no
    // live correlation lost) while the footprint stays bounded (an out-of-window ticket
    // has been evicted — the journal did NOT keep one slot per message sent).
    Switchboard bus;
    constexpr std::uint64_t cap = Switchboard::kJournalCapacity;
    const std::uint64_t flood = cap * 3; // three windows' worth of traffic

    Ticket first{}, recent{}, edge_in{}, edge_out{};
    for (std::uint64_t i = 0; i < flood; ++i) {
        Ticket t = bus.send(WeaveId{9999}, Message(ping(static_cast<std::int64_t>(i)))); // no such target
        const std::uint64_t seq = i + 1; // seqs start at 1
        if (seq == 1) first = t;                     // ancient: 3x the window old by the end
        if (seq == flood - cap) edge_out = t;        // one slot past the retained window
        if (seq == flood - cap + 1) edge_in = t;     // the oldest slot still inside the window
        recent = t;
        bus.pump(); // drain fully each time — the leak was in the journal, not the queue
    }

    // In-window tickets still report their true fate — correlation is preserved.
    CHECK(bus.outcome(recent).disposition == Disposition::Refused);
    CHECK(bus.outcome(recent).refusal.reason == RefusalReason::NoSuchTarget);
    CHECK(bus.outcome(edge_in).disposition == Disposition::Refused); // oldest retained: kept

    // Out-of-window tickets have been evicted — the observable proof the ring is
    // bounded (it did not retain all `flood` outcomes). An evicted seq reads Pending,
    // exactly as an unknown seq does.
    CHECK(bus.outcome(edge_out).disposition == Disposition::Pending); // one past the window
    CHECK(bus.outcome(first).disposition == Disposition::Pending);    // ancient
}

TEST_CASE("publish reaches every accepter in registration order; non-accepters get nothing") {
    Switchboard bus;
    Registered a = reg(bus, {ping_schema()});
    Registered b = reg(bus, {ping_schema()});
    Registered c = reg(bus, {greet_schema()});

    std::size_t recipients = bus.publish(Message(ping(5)));
    CHECK(recipients == 2);
    bus.pump();
    CHECK(a.weave->handled_values.size() == 1);
    CHECK(b.weave->handled_values.size() == 1);
    CHECK(c.weave->handled_values.empty());
}

TEST_CASE("publish with zero accepters is legal: recipient count 0, no delivery") {
    Switchboard bus;
    reg(bus, {ping_schema()});
    std::size_t recipients = bus.publish(Message(tick(1)));
    CHECK(recipients == 0);
    bus.pump();
    CHECK(bus.pending() == 0);
}

TEST_CASE("a fixed sequence of sends is delivered FIFO, reproducibly") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    for (std::int64_t i = 1; i <= 5; ++i) {
        bus.send(r.id, Message(ping(i)));
    }
    bus.pump();
    CHECK(r.weave->handled_values == std::vector<std::int64_t>{1, 2, 3, 4, 5});
}

TEST_CASE("a handler that sends during handling causes a later delivery, never a nested one") {
    Switchboard bus;
    Registered a = reg(bus, {ping_schema()});
    Registered b = reg(bus, {pong_schema()});

    struct Reentry {
        int depth = 0;
        int max = 0;
    } re;
    auto track = [&re] {
        ++re.depth;
        re.max = std::max(re.max, re.depth);
        --re.depth;
    };

    a.weave->on_handle = [&re, bid = b.id](const Message&, Bus& bus_, ProbeWeave&) {
        ++re.depth;
        re.max = std::max(re.max, re.depth);
        bus_.send(bid, Message(pong(99))); // enqueues; must not deliver now
        --re.depth;
    };
    b.weave->on_handle = [&track](const Message&, Bus&, ProbeWeave&) { track(); };

    bus.send(a.id, Message(ping(1)));
    bus.pump();

    CHECK(re.max == 1); // delivery never nested
    REQUIRE(b.weave->handled_values.size() == 1);
    CHECK(b.weave->handled_values[0] == 99); // the during-handle send was delivered, later
}

TEST_CASE("the live delivery path funnels through the same gate as persistence") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()}); // pure recorder: handler enqueues nothing

    const auto g0 = gate_invocations();
    Ticket t = bus.send(r.id, Message(ping(1)));
    bus.pump();
    const auto g1 = gate_invocations();
    CHECK(g1 == g0 + 1); // exactly one validator call for one delivery
    CHECK(bus.outcome(t).disposition == Disposition::Delivered);

    // The persistence (bytes) path advances the very same global counter.
    std::string bytes = bus.snapshot_bytes(r.id);
    const auto g2 = gate_invocations();
    ReviveOutcome ro = bus.reload(r.id, bytes);
    const auto g3 = gate_invocations();
    CHECK(ro.revived);
    CHECK(g3 > g2);
}

TEST_CASE("an observer taps deliveries and refusals without being a recipient") {
    Switchboard bus;
    Registered r = reg(bus, {ping_schema()});
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    bus.send(r.id, Message(ping(1)));          // delivered
    bus.send(r.id, Message(greet("x")));       // refused: NotAccepted
    bus.send(r.id, Message(malformed_ping())); // refused: GateRefused (MissingField)
    bus.pump();

    REQUIRE(tap.size() == 3);
    CHECK(tap[0].kind == EventKind::Delivered);
    CHECK(tap[1].kind == EventKind::Refused);
    CHECK(tap[1].reason == RefusalReason::NotAccepted);
    CHECK(tap[2].kind == EventKind::Refused);
    CHECK(tap[2].reason == RefusalReason::GateRefused);
    CHECK(tap[2].error_kind == ErrorKind::MissingField);
}

TEST_CASE("two weaves declaring the same (name,version) with different shapes conflict") {
    Switchboard bus;
    reg(bus, {ping_schema()});
    auto impostor = SchemaBuilder("Ping", 1).field("seq", Kind::Text).build(); // different shape
    auto bad = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{impostor});
    CHECK_THROWS_AS(bus.register_weave(std::move(bad)), loom::SchemaConflict);
}

} // TEST_SUITE
