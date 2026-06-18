#include <doctest.h>

#include <zen/author.hpp>
#include <zen/switchboard.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace zen;
using namespace zen::sb;
namespace au = zen::author;

namespace {

struct Ping {
    std::int64_t seq;
    ZEN_SHAPE(Ping, 1, ZEN_FIELD(seq));
};
struct Pong {
    std::int64_t seq;
    ZEN_SHAPE(Pong, 1, ZEN_FIELD(seq));
};
struct CounterState {
    std::int64_t count;
    ZEN_SHAPE(CounterState, 1, ZEN_FIELD(count));
};
struct CollectorState {
    std::int64_t count;
    std::int64_t last;
    ZEN_SHAPE(CollectorState, 1, ZEN_FIELD(count), ZEN_FIELD(last));
};

// Accepts Ping, replies Pong, counts handled messages as its state. The author
// writes only the handler and (optionally) the policy.
class Responder : public au::ShardBase<Responder, CounterState, au::Accept<Ping>, au::Emit<Pong>> {
public:
    void on(const Ping& p, au::Mail& mail) {
        ++state_.count;
        mail.reply(Pong{p.seq});
    }
    au::LifecyclePolicy policy_config() const { return {2, true}; }

    std::int64_t count() const { return state_.count; }
    void set_count(std::int64_t c) { state_.count = c; }
};

class Collector : public au::ShardBase<Collector, CollectorState, au::Accept<Pong>> {
public:
    void on(const Pong& p, au::Mail&) {
        ++state_.count;
        state_.last = p.seq;
    }
    std::int64_t received() const { return state_.count; }
    std::int64_t last() const { return state_.last; }
};

// Three distinct accepted shapes, one handler each, for the routing test.
struct Alpha {
    std::int64_t v;
    ZEN_SHAPE(Alpha, 1, ZEN_FIELD(v));
};
struct Beta {
    std::int64_t v;
    ZEN_SHAPE(Beta, 1, ZEN_FIELD(v));
};
struct Gamma {
    std::int64_t v;
    ZEN_SHAPE(Gamma, 1, ZEN_FIELD(v));
};
struct RouteLog {
    std::int64_t alpha;
    std::int64_t beta;
    std::int64_t gamma;
    ZEN_SHAPE(RouteLog, 1, ZEN_FIELD(alpha), ZEN_FIELD(beta), ZEN_FIELD(gamma));
};

// Records which handler fired and the value it saw, so a test can prove each
// shape lands in its own handler and in no other.
class Router : public au::ShardBase<Router, RouteLog, au::Accept<Alpha, Beta, Gamma>> {
public:
    std::vector<std::string> trail;
    void on(const Alpha& m, au::Mail&) { ++state_.alpha; trail.push_back("alpha:" + std::to_string(m.v)); }
    void on(const Beta& m, au::Mail&) { ++state_.beta; trail.push_back("beta:" + std::to_string(m.v)); }
    void on(const Gamma& m, au::Mail&) { ++state_.gamma; trail.push_back("gamma:" + std::to_string(m.v)); }
};

} // namespace

TEST_SUITE("author_shard") {

TEST_CASE("typed handlers dispatch the right struct and reply with plain structs") {
    Switchboard bus;
    ShardId responder = au::mount<Responder>(bus);
    ShardId collector = au::mount<Collector>(bus);

    bus.send(responder, Message(au::to_value(Ping{42}), /*sender=*/ShardId{}, /*reply_to=*/collector));
    bus.pump();

    auto* c = static_cast<Collector*>(bus.shard(collector));
    REQUIRE(c != nullptr);
    CHECK(c->received() == 1);
    CHECK(c->last() == 42);
}

TEST_CASE("each accepted shape routes to its own handler and to no other") {
    Switchboard bus;
    ShardId rid = au::mount<Router>(bus);

    // Deliver one of each accepted shape, with distinguishable payloads.
    bus.send(rid, Message(au::to_value(Alpha{10})));
    bus.send(rid, Message(au::to_value(Beta{20})));
    bus.send(rid, Message(au::to_value(Gamma{30})));
    bus.pump();

    auto* r = static_cast<Router*>(bus.shard(rid));
    REQUIRE(r != nullptr);
    // Each shape landed in exactly its own handler, in delivery order — no shape
    // leaked into another handler, none was silently dropped.
    REQUIRE(r->trail.size() == 3);
    CHECK(r->trail[0] == "alpha:10");
    CHECK(r->trail[1] == "beta:20");
    CHECK(r->trail[2] == "gamma:30");
}

TEST_CASE("the accept-set is derived from the typed handlers; emit-set is reported") {
    Switchboard bus;
    ShardId responder = au::mount<Responder>(bus);

    auto acc = bus.accepted_schemas(responder);
    REQUIRE(acc.size() == 1);
    CHECK(acc[0]->name() == "Ping");
    CHECK(acc[0]->content_id() == au::schema_of<Ping>()->content_id());

    auto* r = static_cast<Responder*>(bus.shard(responder));
    auto emitted = r->emitted_schemas();
    REQUIRE(emitted.size() == 1);
    CHECK(emitted[0]->name() == "Pong");
}

TEST_CASE("snapshot/revive are derived; state round-trips through the gate") {
    Switchboard bus;
    ShardId responder = au::mount<Responder>(bus);
    ShardId collector = au::mount<Collector>(bus);

    for (int i = 0; i < 3; ++i) {
        bus.send(responder, Message(au::to_value(Ping{1}), ShardId{}, collector));
    }
    bus.pump();

    auto* r = static_cast<Responder*>(bus.shard(responder));
    CHECK(r->count() == 3);

    // The bus snapshots via the derived snapshot(); revive via the derived revive().
    std::string snap = bus.snapshot_bytes(responder);
    r->set_count(99); // simulate drift
    ReviveOutcome ro = bus.reload(responder, snap);
    CHECK(ro.revived);
    CHECK(r->count() == 3); // restored from the gated snapshot
}

TEST_CASE("a derived policy reaches the bus") {
    Switchboard bus;
    ShardId responder = au::mount<Responder>(bus);
    // Responder declares max_reloads = 2; a third corrupt reload (after two good
    // ones) is exhausted.
    std::string snap = bus.snapshot_bytes(responder);
    CHECK(bus.reload(responder, snap).revived);
    CHECK(bus.reload(responder, snap).revived);
    ReviveOutcome third = bus.reload(responder, "garbage");
    CHECK_FALSE(third.revived);
    CHECK(third.reloads_exhausted);
}

} // TEST_SUITE
