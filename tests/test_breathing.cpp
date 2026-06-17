#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace zen;
using namespace zen::sb;
using namespace sbfx;

// The milestone: the kernel is alive. Two cooperative Shards exchange a gated
// directed message and a reply, a publish reaches only its accepters, one Shard
// dies and revives through native bytes, and an observer witnesses it all.

TEST_SUITE("breathing") {

TEST_CASE("a directed reply, a selective publish, and a death/revival under policy") {
    Switchboard bus;
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    Registered responder = register_probe(bus, {ping_schema()});
    Registered collector = register_probe(bus, {pong_schema(), greet_schema()});

    // The responder answers whoever asked, by sending — replies are ordinary sends.
    responder.shard->on_handle = [rid = responder.id](const Message& in, Bus& bus_, ProbeShard&) {
        const std::int64_t seq = in.payload.get("seq")->as_int();
        bus_.send(in.reply_to, Message(pong(seq), rid));
    };

    // 1) A directed, gated send, with a reply routed back as a send.
    bus.send(responder.id, Message(ping(42), /*sender=*/ShardId{}, /*reply_to=*/collector.id));
    bus.pump();
    REQUIRE(responder.shard->handled_values.size() == 1);
    REQUIRE(collector.shard->handled_names.size() == 1);
    CHECK(collector.shard->handled_names[0] == "Pong");
    CHECK(collector.shard->handled_values[0] == 42);

    // 2) A publish reaches the accepter, not the non-accepter.
    const std::size_t recipients = bus.publish(Message(greet("hello, shards")));
    CHECK(recipients == 1);
    bus.pump();
    CHECK(responder.shard->handled_values.size() == 1); // unchanged: doesn't accept Greet
    REQUIRE(collector.shard->handled_names.size() == 2);
    CHECK(collector.shard->handled_names[1] == "Greet");

    // 3) The responder dies and revives, its state round-tripping through native bytes.
    REQUIRE(responder.shard->count == 1);
    const std::string saved = bus.snapshot_bytes(responder.id); // Counter{count:1}
    bus.kill(responder.id);
    CHECK_FALSE(bus.alive(responder.id));

    // While dead, a directed send is refused (and observed).
    Ticket dead = bus.send(responder.id, Message(ping(7)));
    bus.pump();
    CHECK(bus.outcome(dead).refusal.reason == RefusalReason::TargetUnavailable);

    // Corrupt the in-memory self, then revive from the saved snapshot through the gate.
    responder.shard->count = 555;
    ReviveOutcome ro = bus.reload(responder.id, saved);
    CHECK(ro.revived);
    CHECK_FALSE(ro.from_last_known_good);
    CHECK(bus.alive(responder.id));
    CHECK(responder.shard->count == 1); // state restored, gated on the way in

    // 4) The observer witnessed the deliveries and the death/revival.
    int delivered = 0;
    int died = 0;
    int revived = 0;
    for (const TapRecord& r : tap) {
        if (r.kind == EventKind::Delivered) {
            ++delivered;
        } else if (r.kind == EventKind::Died) {
            ++died;
        } else if (r.kind == EventKind::Revived) {
            ++revived;
        }
    }
    CHECK(delivered == 3); // ping->responder, pong->collector, greet->collector
    CHECK(died == 1);
    CHECK(revived == 1);
}

} // TEST_SUITE
