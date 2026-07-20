#include <doctest.h>

#include <zen/weave.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/switchboard.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace loom;
using namespace loom;
namespace au = loom;

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

// Accepts Ping, replies Pong, counts handled messages as its state. The maker
// writes only the handler and (optionally) the policy.
class Responder : public au::WeaveBase<Responder, CounterState, au::Accept<Ping>, au::Emit<Pong>> {
public:
    void on(const Ping& p, au::Mail& mail) {
        ++state_.count;
        mail.reply(Pong{p.seq});
    }
    au::LifecyclePolicy policy_config() const { return {2, true}; }

    std::int64_t count() const { return state_.count; }
    void set_count(std::int64_t c) { state_.count = c; }
};

class Collector : public au::WeaveBase<Collector, CollectorState, au::Accept<Pong>> {
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
class Router : public au::WeaveBase<Router, RouteLog, au::Accept<Alpha, Beta, Gamma>> {
public:
    std::vector<std::string> trail;
    void on(const Alpha& m, au::Mail&) { ++state_.alpha; trail.push_back("alpha:" + std::to_string(m.v)); }
    void on(const Beta& m, au::Mail&) { ++state_.beta; trail.push_back("beta:" + std::to_string(m.v)); }
    void on(const Gamma& m, au::Mail&) { ++state_.gamma; trail.push_back("gamma:" + std::to_string(m.v)); }
};

// A shape NO Weave below declares as Emit<...> — used to drive an undeclared emit.
struct Rogue {
    std::int64_t v;
    ZEN_SHAPE(Rogue, 1, ZEN_FIELD(v));
};

// A trusted-mount Weave whose handler emits its declared Pong (permitted by the auto-grant) AND an
// undeclared Rogue (which the auto-grant must deny). The emit-enforcement gate is intentionally NOT
// compile-time, so a handler CAN attempt an undeclared send — that is what makes the runtime denial
// testable.
class Leaker : public au::WeaveBase<Leaker, CounterState, au::Accept<Ping>, au::Emit<Pong>> {
public:
    WeaveId target{};
    void on(const Ping&, au::Mail& mail) {
        mail.send(target, Pong{1});  // declared in Emit<...> -> permitted
        mail.send(target, Rogue{2}); // NOT declared -> must be CapabilityDenied
    }
};

// The carve-out pair: a maker whose handler emits an UNDECLARED standard reply
// (zen.Refused — empty Emit<> on purpose), and a sink that accepts it.
class StandardLeaker
    : public au::WeaveBase<StandardLeaker, CounterState, au::Accept<Ping>, au::Emit<>> {
public:
    WeaveId target{};
    void on(const Ping&, au::Mail& mail) { mail.send(target, au::Refused{"undeclared"}); }
};
class RefusedSink
    : public au::WeaveBase<RefusedSink, CounterState, au::Accept<au::Refused>, au::Emit<>> {
public:
    void on(const au::Refused&, au::Mail&) { ++state_.count; }
};

} // namespace

TEST_SUITE("weave") {

TEST_CASE("typed handlers dispatch the right struct and reply with plain structs") {
    Switchboard bus;
    WeaveId responder = au::mount<Responder>(bus);
    WeaveId collector = au::mount<Collector>(bus);

    bus.send(responder, Message(au::to_value(Ping{42}), /*sender=*/WeaveId{}, /*reply_to=*/collector));
    bus.pump();

    auto* c = static_cast<Collector*>(bus.weave(collector));
    REQUIRE(c != nullptr);
    CHECK(c->received() == 1);
    CHECK(c->last() == 42);
}

TEST_CASE("each accepted shape routes to its own handler and to no other") {
    Switchboard bus;
    WeaveId rid = au::mount<Router>(bus);

    // Deliver one of each accepted shape, with distinguishable payloads.
    bus.send(rid, Message(au::to_value(Alpha{10})));
    bus.send(rid, Message(au::to_value(Beta{20})));
    bus.send(rid, Message(au::to_value(Gamma{30})));
    bus.pump();

    auto* r = static_cast<Router*>(bus.weave(rid));
    REQUIRE(r != nullptr);
    // Each shape landed in exactly its own handler, in delivery order — no shape
    // leaked into another handler, none was silently dropped.
    REQUIRE(r->trail.size() == 3);
    CHECK(r->trail[0] == "alpha:10");
    CHECK(r->trail[1] == "beta:20");
    CHECK(r->trail[2] == "gamma:30");
}

TEST_CASE("a Weave's declared Emit<...> matches what it actually emits") {
    Switchboard bus;
    WeaveId responder = au::mount<Responder>(bus);
    WeaveId collector = au::mount<Collector>(bus);

    // The bus tap carries each delivery's sender, so we can collect the set of
    // shapes a given Weave actually put on the wire (a refused emit still counts).
    std::set<std::string> from_responder;
    std::set<std::string> from_collector;
    bus.add_observer([&](const BusEvent& e) {
        if (e.kind != EventKind::Delivered && e.kind != EventKind::Refused) {
            return;
        }
        if (e.sender == responder) {
            from_responder.insert(e.schema_name);
        } else if (e.sender == collector) {
            from_collector.insert(e.schema_name);
        }
    });

    bus.send(responder, Message(au::to_value(Ping{1}), WeaveId{}, collector));
    bus.send(responder, Message(au::to_value(Ping{2}), WeaveId{}, collector));
    bus.pump();

    auto declared_of = [](auto* weave) {
        std::set<std::string> names;
        for (const auto& s : weave->emitted_schemas()) {
            names.insert(s->name());
        }
        return names;
    };
    auto* r = static_cast<Responder*>(bus.weave(responder));
    auto* c = static_cast<Collector*>(bus.weave(collector));

    // Responder declares Emit<Pong> and emits exactly Pong — no more, no less.
    CHECK(from_responder == std::set<std::string>{"Pong"});
    CHECK(from_responder == declared_of(r));
    // Collector declares no Emit<...> and emits nothing.
    CHECK(from_collector.empty());
    CHECK(declared_of(c).empty());
}

TEST_CASE("the mount<> auto-grant denies an emit the Weave did not declare") {
    // mount<> derives the grant from the declared Emit<...> set (allow_to_any per shape) PLUS the
    // four poke-answer shapes (allow_poke_answers — the construction layer's answering machinery;
    // that carve-out is pinned as known in the next case). So a handler that sends an UNDECLARED
    // shape outside that set is CapabilityDenied — emit-denial holds on the auto-grant path too,
    // not just the explicit-grant path (the latter is pinned in test_capabilities).
    Switchboard bus;
    WeaveId sink = au::mount<Collector>(bus); // accepts Pong (Rogue isn't accepted, but denial is first)
    WeaveId leaker = au::mount<Leaker>(bus);
    static_cast<Leaker*>(bus.weave(leaker))->target = sink;

    bool rogue_denied = false;
    bool pong_ok = false;
    bus.add_observer([&](const BusEvent& e) {
        if (e.sender != leaker) {
            return; // the trigger into the Weave is ungated; only its outbound sends are gated
        }
        if (e.schema_name == "Rogue" && e.kind == EventKind::Refused &&
            e.refusal.reason == RefusalReason::CapabilityDenied) {
            rogue_denied = true;
        }
        if (e.schema_name == "Pong" && e.kind == EventKind::Delivered) {
            pong_ok = true;
        }
    });

    bus.send(leaker, Message(au::to_value(Ping{1}))); // host root trigger (ungated)
    bus.pump();

    CHECK(pong_ok);      // the DECLARED emit went through the auto-grant
    CHECK(rogue_denied); // the UNDECLARED emit was CapabilityDenied on the auto-grant path
}

TEST_CASE("the known carve-out, pinned: an UNDECLARED standard-reply emit is deliverable under "
          "mount<>") {
    // mount<>'s ride-along allow_poke_answers grant covers the standard replies
    // (zen.Ack/zen.Refused/zen.Result + zen.PokeStructure) for every trusted
    // weave, because the construction layer answers pokes with them. Since the
    // same shapes are now the universal reply vocabulary, a maker's own
    // UNDECLARED emit of one rides that grant — deliverable with an empty
    // Emit<>. This is a KNOWN carve-out from "the silhouette is the grant",
    // recorded here so it is never latent: makers who reply with a standard
    // shape still declare it in Emit<...> (the standing rule in
    // standard_shapes.hpp), and the reserved Mail emit-gate would close this
    // for maker sends the day it lands — at which point this pin flips.
    Switchboard bus;
    WeaveId sink = au::mount<RefusedSink>(bus);
    WeaveId leaker = au::mount<StandardLeaker>(bus);
    static_cast<StandardLeaker*>(bus.weave(leaker))->target = sink;

    bool refused_delivered = false;
    bus.add_observer([&](const BusEvent& e) {
        if (e.sender == leaker && e.schema_name == "zen.Refused" &&
            e.kind == EventKind::Delivered) {
            refused_delivered = true;
        }
    });

    bus.send(leaker, Message(au::to_value(Ping{1}))); // host root trigger (ungated)
    bus.pump();

    CHECK(refused_delivered); // undeclared, yet delivered: the carve-out, on the record
    CHECK(static_cast<RefusedSink*>(bus.weave(sink))->snapshot().get("count")->as_int() == 1);
}

TEST_CASE("the accept-set is the typed handlers plus the universal poke doors; emit-set stays "
          "the maker's declaration") {
    Switchboard bus;
    WeaveId responder = au::mount<Responder>(bus);

    // The maker's doors (from Accept<...>) plus the four substrate poke doors
    // every woven Weave answers (the inspect-the-structure floor, always on).
    auto acc = bus.accepted_schemas(responder);
    std::set<std::string> door_names;
    for (const auto& s : acc) {
        door_names.insert(s->name());
    }
    CHECK(door_names == std::set<std::string>{"Ping", "zen.PokeDescribe", "zen.PokeRead",
                                              "zen.PokeWrite", "zen.PokeResetState"});
    REQUIRE(acc.size() == 5);
    CHECK(acc[0]->name() == "Ping");
    CHECK(acc[0]->content_id() == au::schema_of<Ping>()->content_id());

    // emitted_schemas() remains the MAKER's declared Emit<...> alone — the
    // construction layer's poke answers are substrate machinery, granted
    // separately by mount() (allow_poke_answers), never smuggled into the
    // maker's declaration.
    auto* r = static_cast<Responder*>(bus.weave(responder));
    auto emitted = r->emitted_schemas();
    REQUIRE(emitted.size() == 1);
    CHECK(emitted[0]->name() == "Pong");
}

TEST_CASE("snapshot/revive are derived; state round-trips through the gate") {
    Switchboard bus;
    WeaveId responder = au::mount<Responder>(bus);
    WeaveId collector = au::mount<Collector>(bus);

    for (int i = 0; i < 3; ++i) {
        bus.send(responder, Message(au::to_value(Ping{1}), WeaveId{}, collector));
    }
    bus.pump();

    auto* r = static_cast<Responder*>(bus.weave(responder));
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
    WeaveId responder = au::mount<Responder>(bus);
    // Responder declares max_reloads = 2; a third corrupt reload (after two good
    // ones) is exhausted.
    std::string snap = bus.snapshot_bytes(responder);
    CHECK(bus.reload(responder, snap).revived);
    CHECK(bus.reload(responder, snap).revived);
    ReviveOutcome third = bus.reload(responder, "garbage");
    CHECK_FALSE(third.revived);
    CHECK(third.reloads_exhausted);
}

// ---- the Loomstd lifecycle vocabulary (weave/lifecycle.hpp) -----------------
// This case lives in the PORTABLE suite on purpose. The letter protocol is
// Loomstd-tier — universal, not kernel-tier — and a header that claims to be
// portable while only ever being compiled behind `if(NOT WIN32)` is a claim
// nothing checks. Compiling and running it on every platform is what makes the
// tier placement true rather than asserted. (The parts that swap real .so files
// stay Linux-gated in the `manager` suite, where they belong.)

TEST_CASE("a letter item round-trips, and reading one goes through the gate or not at all") {
    // bequeath_item writes; claim_item reads. claim_item's whole job is that
    // inherited mail is UNTRUSTED INPUT: it admits through the one validator
    // before a field is touched, so a corrupt or wrong-shaped item is a clean
    // nothing rather than a misread.
    const loom::Bytes item = loom::bequeath_item(Ping{41});
    const std::optional<Ping> read = loom::claim_item<Ping>(item);
    REQUIRE(read.has_value());
    CHECK(read->seq == 41);

    // Garbage is refused, not guessed at.
    CHECK_FALSE(loom::claim_item<Ping>(loom::Bytes{0x00, 0x01, 0x02, 0x03}).has_value());
    CHECK_FALSE(loom::claim_item<Ping>(loom::Bytes{}).has_value());
    // A well-formed item of a DIFFERENT shape is refused too — the gate compares
    // the claim against the schema the READER asked for, not the one it carries.
    CHECK_FALSE(loom::claim_item<Ping>(loom::bequeath_item(loom::Refused{"other"})).has_value());

    // The bound is published, so it is checkable rather than folklore.
    static_assert(loom::kMaxBequestItems > 0, "a letter must be able to say something");
    CHECK(std::string(loom::kManagerRole) == "zen.manager");
}

} // TEST_SUITE
