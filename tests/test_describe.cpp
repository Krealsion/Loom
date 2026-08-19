// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

// The self-description door (MSG-1): a participant asks a target which message
// shapes it accepts, by ordinary message, under ordinary authority.
//
// What these cases are actually about, and why each is here:
//   - the answer comes from the target's ENFORCED accept-set, never from a
//     Registry sweep and never from a second store;
//   - a stranger that never compiled against the described shapes can rebuild
//     them -- which requires the DEPENDENCY CLOSURE, not just the roots;
//   - roots and dependencies stay distinguishable on the wire;
//   - knowing a shape is accepted is not permission to send it.
// Portable (in-process, no OS boundary).

#include <zen/switchboard.hpp>
#include <zen/terminal/composer.hpp>
#include <zen/weave.hpp>

#include "switchboard_fixtures.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace loom;
namespace au = loom;

namespace {

// ---- a scalar target, shaped like the real one MSG-0 will compose for -------

struct StartTimer {
    std::string id;
    std::int64_t delay_ms = 0;
    bool repeat = false;
    ZEN_SHAPE(StartTimer, 1, ZEN_FIELD(id), ZEN_FIELD(delay_ms), ZEN_FIELD(repeat));
};
struct StopTimer {
    std::string id;
    ZEN_SHAPE(StopTimer, 1, ZEN_FIELD(id));
};
// The same NAME at a different version -- a distinct shape, and the wire must
// keep them apart.
struct StopTimerV2 {
    std::string id;
    static constexpr const char* zen_name = "StopTimer";
    static constexpr std::uint32_t zen_version = 2;
    using ZenSelf = StopTimerV2;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(id)); }
};
struct TimerState {
    std::int64_t started = 0;
    ZEN_SHAPE(TimerState, 1, ZEN_FIELD(started));
};
class Timerish : public au::WeaveBase<Timerish, TimerState, au::Accept<StartTimer, StopTimer>> {
public:
    void on(const StartTimer&, au::Mail&) { ++state_.started; }
    void on(const StopTimer&, au::Mail&) {}
    std::int64_t started() const { return state_.started; }
};

// ---- a NESTED target: the case that decides the reply payload ---------------
//
// Leaf is nested by Middle, which is nested by Outer; the target accepts Outer
// (and Listy) but accepts NEITHER Middle NOR Leaf. So an asker handed only the
// accepted roots has no way to learn what a `child` even is.

struct Leaf {
    std::int64_t n = 0;
    ZEN_SHAPE(Leaf, 1, ZEN_FIELD(n));
};
struct Middle {
    Leaf leaf;
    ZEN_SHAPE(Middle, 1, ZEN_FIELD(leaf));
};
struct Outer {
    Middle child;
    std::string tag;
    ZEN_SHAPE(Outer, 1, ZEN_FIELD(child), ZEN_FIELD(tag));
};
struct Listy {
    std::vector<Middle> many;
    ZEN_SHAPE(Listy, 1, ZEN_FIELD(many));
};
struct NestState {
    std::int64_t seen = 0;
    ZEN_SHAPE(NestState, 1, ZEN_FIELD(seen));
};
class Nested : public au::WeaveBase<Nested, NestState, au::Accept<Outer, Listy>> {
public:
    void on(const Outer&, au::Mail&) { ++state_.seen; }
    void on(const Listy&, au::Mail&) { ++state_.seen; }
};

// ---- the asker: an ordinary participant holding a role name and a grant -----

struct AskerState {
    std::int64_t answers = 0;
    ZEN_SHAPE(AskerState, 1, ZEN_FIELD(answers));
};

// The nudge that makes the asker act. A participant's send is one made from
// INSIDE its own delivery -- a host calling bus.send() directly is a root send
// and takes no grant check at all, so an authority case driven that way would
// prove nothing. The host root-sends this; everything the asker then does is an
// ordinary participant send against its own grant.
struct Kick {
    ZEN_SHAPE(Kick, 1);
};

// It accepts the answer shape the way any consumer would. The answer is not a
// ZEN_SHAPE (its fields are lists of zen.SchemaDesc), so the accept-set is
// declared through a raw loom::Weave rather than Accept<...> -- which is also
// what a stranger written against the installed package does when it wants the
// Value rather than a struct.
class Asker final : public loom::Weave {
public:
    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return {schema_of<Kick>(), accepted_shapes_schema()};
    }
    void handle(const Message& in, Bus& bus) override {
        if (in.payload.schema().name() == "Kick") {
            if (errand_) {
                errand_(bus);
            }
            return;
        }
        last_ = in.payload;
        ++answers_;
    }
    Value snapshot() const override { return to_value(AskerState{answers_}); }
    Value policy() const override {
        Value v(lifecycle_policy_schema());
        v.set("max_reloads", Cell::integer(4));
        v.set("revive_from_last_good", Cell::boolean(true));
        return v;
    }
    void revive(const Value&) override {}

    bool answered() const { return last_.has_value(); }
    const Value& answer() const { return *last_; }
    void forget() { last_.reset(); }

    void set_self(WeaveId id) { self_ = id; }

    /// Ask, as a participant: the send happens inside this weave's own delivery.
    void ask_role(Switchboard& bus, std::string role) {
        run(bus, [this, role](Bus& b) {
            b.send_to_role(role, Message(to_value(DescribeAccepted{}), self_, self_, 42));
        });
    }
    void ask_id(Switchboard& bus, WeaveId target) {
        run(bus, [this, target](Bus& b) {
            b.send(target, Message(to_value(DescribeAccepted{}), self_, self_, 42));
        });
    }
    /// Send anything else it has learned to send -- also as a participant.
    void send_learned(Switchboard& bus, WeaveId target, Value payload) {
        run(bus, [this, target, payload](Bus& b) {
            b.send(target, Message(payload, self_, self_, 9));
        });
    }

private:
    void run(Switchboard& bus, std::function<void(Bus&)> errand) {
        errand_ = std::move(errand);
        bus.send(self_, Message(to_value(Kick{})));  // root send, to start the turn
        bus.pump();
        errand_ = nullptr;
    }

    std::function<void(Bus&)> errand_;
    std::optional<Value> last_;
    WeaveId self_{};
    std::int64_t answers_ = 0;
};

// Register an asker with exactly the grant given -- no more.
Asker* mount_asker(Switchboard& bus, Grant grant, WeaveId& out_id) {
    auto a = std::make_unique<Asker>();
    Asker* raw = a.get();
    out_id = bus.register_weave(std::move(a), std::move(grant));
    raw->set_self(out_id);
    return raw;
}

Grant may_ask() {
    Grant g;
    g.allow_to_any(kDescribeAcceptedShapeName, 1);
    return g;
}

// The composer's two lookups, over a vocabulary this consumer LEARNED from a
// reply rather than compiled against. It resolves no references: a first
// composer has no received-message store to wire from.
class LearnedVocabulary final : public ComposeSource {
public:
    explicit LearnedVocabulary(const Registry& deps) : deps_(deps) {}
    std::shared_ptr<const Schema> resolve_schema(std::string_view name,
                                                 std::uint32_t version) const override {
        return deps_.lookup(name, version);
    }
    std::optional<Cell> resolve_ref(const Ref&, std::string* error) const override {
        if (error != nullptr) {
            *error = "this consumer holds no received messages to reference";
        }
        return std::nullopt;
    }

private:
    const Registry& deps_;
};

std::set<std::string> identities(const std::vector<std::shared_ptr<const Schema>>& v) {
    std::set<std::string> out;
    for (const auto& s : v) {
        out.insert(s->name() + " v" + std::to_string(s->version()));
    }
    return out;
}

std::set<std::string> identities_from_wire(const Value& answer, const char* section) {
    std::set<std::string> out;
    const Cell* list = answer.get(section);
    if (list == nullptr) {
        return out;
    }
    for (const Cell& c : list->as_list()) {
        const Value& d = *c.as_message();
        out.insert(d.get("name")->as_text() + " v" + std::to_string(d.get("version")->as_int()));
    }
    return out;
}

} // namespace

TEST_SUITE("describe") {

// ---- the door itself --------------------------------------------------------

TEST_CASE("every woven weave carries the self-description door, beside the four poke doors") {
    Timerish t;
    const std::set<std::string> doors = identities(t.accepted_schemas());
    CHECK(doors.count("zen.DescribeAccepted v1") == 1);
    CHECK(doors.count("zen.PokeDescribe v1") == 1);
    CHECK(doors.count("zen.PokeRead v1") == 1);
    CHECK(doors.count("zen.PokeWrite v1") == 1);
    CHECK(doors.count("zen.PokeResetState v1") == 1);
    CHECK(doors.count("StartTimer v1") == 1);
    CHECK(doors.count("StopTimer v1") == 1);
    CHECK(t.accepted_schemas().size() == 7);
}

TEST_CASE("the request is fieldless and the answer's grammar is versioned") {
    CHECK(std::string(DescribeAccepted::zen_name) == "zen.DescribeAccepted");
    CHECK(DescribeAccepted::zen_version == 1u);
    CHECK(schema_of<DescribeAccepted>()->fields().empty());

    const auto answer = accepted_shapes_schema();
    CHECK(answer->name() == "zen.AcceptedShapes");
    CHECK(answer->version() == 1u);
    REQUIRE(answer->fields().size() == 2);
    CHECK(answer->fields()[0].name == "referenced");
    CHECK(answer->fields()[0].required == false);
    CHECK(answer->fields()[1].name == "accepted");
    CHECK(answer->fields()[1].required == true);
}

TEST_CASE("the answer is the ENFORCED accept-set: the same vector the Switchboard holds") {
    Switchboard bus;
    const WeaveId target = mount<Timerish>(bus);
    WeaveId asker_id{};
    Asker* asker = mount_asker(bus, may_ask(), asker_id);

    asker->ask_id(bus, target);
    REQUIRE(asker->answered());

    // What the bus will actually match a delivery against, asked of the host:
    const std::set<std::string> enforced = identities(bus.accepted_schemas(target));
    const std::set<std::string> described = identities_from_wire(asker->answer(), "accepted");
    CHECK(described == enforced);
    CHECK(described.count("StartTimer v1") == 1);
    CHECK(described.count("zen.DescribeAccepted v1") == 1);
}

TEST_CASE("the target answers for itself: the role resolves to the holder that replied") {
    Switchboard bus;
    auto t = std::make_unique<Timerish>();
    Timerish* raw = t.get();
    Grant g = emit_default_grant(*raw);
    allow_poke_answers(g);
    allow_describe_answers(g);
    const WeaveId target = bus.register_weave(std::move(t), std::move(g), std::string("timer"));
    raw->zen_set_self(target);

    WeaveId asker_id{};
    Grant ask = may_ask();
    ask.allow_to_role(kDescribeAcceptedShapeName, 1, "timer");
    Asker* asker = mount_asker(bus, std::move(ask), asker_id);

    asker->ask_role(bus, "timer");
    REQUIRE(asker->answered());
    CHECK(identities_from_wire(asker->answer(), "accepted").count("StartTimer v1") == 1);
}

TEST_CASE("the answer includes the request's own shape, and that terminates") {
    Switchboard bus;
    const WeaveId target = mount<Timerish>(bus);
    WeaveId asker_id{};
    Asker* asker = mount_asker(bus, may_ask(), asker_id);
    asker->ask_id(bus, target);
    REQUIRE(asker->answered());

    // Truthful: the target does accept it.
    CHECK(identities_from_wire(asker->answer(), "accepted").count("zen.DescribeAccepted v1") == 1);
    // And it recurses nowhere: the request is fieldless, so it names no
    // dependency, so describing it cannot describe anything that describes it.
    CHECK(schema_of<DescribeAccepted>()->fields().empty());
    CHECK(identities_from_wire(asker->answer(), "referenced").count("zen.AcceptedShapes v1") == 0);
    CHECK(identities_from_wire(asker->answer(), "accepted").count("zen.AcceptedShapes v1") == 0);
}

// ---- the stranger's reconstruction: the point of the whole seam --------------

TEST_CASE("a consumer that never compiled against the shapes rebuilds them from the answer") {
    Switchboard bus;
    const WeaveId target = mount<Timerish>(bus);
    WeaveId asker_id{};
    Asker* asker = mount_asker(bus, may_ask(), asker_id);
    asker->ask_id(bus, target);
    REQUIRE(asker->answered());

    // A registry the consumer owns. Nothing was handed to it but the reply.
    Registry deps;
    decode_accepted_referenced(asker->answer(), deps);
    const std::vector<std::shared_ptr<const Schema>> roots =
        decode_accepted_roots(asker->answer(), deps);

    const Schema* start = nullptr;
    for (const auto& r : roots) {
        if (r->name() == "StartTimer") {
            start = r.get();
        }
    }
    REQUIRE(start != nullptr);
    REQUIRE(start->fields().size() == 3);
    CHECK(start->fields()[0].name == "id");
    CHECK(start->fields()[0].type.kind == Kind::Text);
    CHECK(start->fields()[1].name == "delay_ms");
    CHECK(start->fields()[1].type.kind == Kind::Int);
    CHECK(start->fields()[2].name == "repeat");
    CHECK(start->fields()[2].type.kind == Kind::Bool);
    // Reconstruction is exact, not approximate: same content identity.
    CHECK(start->content_id() == schema_of<StartTimer>()->content_id());
}

TEST_CASE("the scalar handoff MSG-0 needs: describe -> choose -> compose -> assemble") {
    Switchboard bus;
    const WeaveId target = mount<Timerish>(bus);
    WeaveId asker_id{};
    Asker* asker = mount_asker(bus, may_ask(), asker_id);
    asker->ask_id(bus, target);
    REQUIRE(asker->answered());

    Registry deps;
    decode_accepted_referenced(asker->answer(), deps);
    for (const auto& r : decode_accepted_roots(asker->answer(), deps)) {
        deps.register_schema(r);
    }

    // From here on the consumer uses only the installed composer, over the
    // vocabulary it just learned -- it never saw StartTimer's C++ type.
    LearnedVocabulary src(deps);

    REQUIRE(deps.lookup("StartTimer", 1) != nullptr); // it was in the answer, or nothing follows
    const ShapeDesc desc = describe_schema(*deps.lookup("StartTimer", 1));
    CHECK(desc.name == "StartTimer");
    REQUIRE(desc.fields.size() == 3);
    CHECK(desc.fields[0].type == "Text");
    CHECK(desc.fields[1].type == "Int");
    CHECK(desc.fields[2].type == "Bool");

    const std::vector<Arg> args{
        Arg{std::string("id"), FieldValue{std::string("demo")}},
        Arg{std::string("delay_ms"), FieldValue{std::int64_t{1000}}},
        Arg{std::string("repeat"), FieldValue{false}}};
    Composition c = compose_message(src, "StartTimer", 1, args);
    REQUIRE(c.status == Composition::Status::Ready);
    Value msg = assemble(c);
    CHECK(msg.schema().name() == "StartTimer");
    CHECK(msg.get("id")->as_text() == "demo");
    CHECK(msg.get("delay_ms")->as_int() == 1000);
    CHECK(msg.get("repeat")->as_bool() == false);
}

// ---- dependency closure: the load-bearing half ------------------------------

TEST_CASE("a nested root is UNDECODABLE from the accepted roots alone") {
    // The measurement that chooses the payload. Without the closure section a
    // consumer holding every accepted descriptor still cannot rebuild one.
    Registry empty;
    CHECK_THROWS_AS(decode_schema(encode_schema(*schema_of<Outer>()), empty), std::runtime_error);
    CHECK_THROWS_AS(decode_schema(encode_schema(*schema_of<Listy>()), empty), std::runtime_error);
    // ... while a flat one is fine, which is why the section is optional.
    CHECK_NOTHROW(decode_schema(encode_schema(*schema_of<StartTimer>()), empty));
}

TEST_CASE("the answer carries the closure, in an order one forward pass can resolve") {
    Switchboard bus;
    const WeaveId target = mount<Nested>(bus);
    WeaveId asker_id{};
    Asker* asker = mount_asker(bus, may_ask(), asker_id);
    asker->ask_id(bus, target);
    REQUIRE(asker->answered());

    // Post-order: a schema's own references precede it. Leaf before Middle.
    // Read defensively -- `referenced` is optional, so a reply that dropped the
    // closure must fail as a WRONG ANSWER here rather than as a null dereference.
    const Cell* refs_cell = asker->answer().get("referenced");
    REQUIRE(refs_cell != nullptr); // a nesting target MUST carry its closure
    const Cell::Array& refs = refs_cell->as_list();
    REQUIRE(refs.size() == 2);
    CHECK(refs[0].as_message()->get("name")->as_text() == "Leaf");
    CHECK(refs[1].as_message()->get("name")->as_text() == "Middle");

    Registry deps;
    decode_accepted_referenced(asker->answer(), deps);
    const std::vector<std::shared_ptr<const Schema>> roots =
        decode_accepted_roots(asker->answer(), deps);

    const Schema* outer = nullptr;
    const Schema* listy = nullptr;
    for (const auto& r : roots) {
        if (r->name() == "Outer") {
            outer = r.get();
        }
        if (r->name() == "Listy") {
            listy = r.get();
        }
    }
    REQUIRE(outer != nullptr);
    REQUIRE(listy != nullptr);

    // Message(Middle) survived, and resolves to a Middle the consumer rebuilt.
    REQUIRE(outer->fields()[0].type.kind == Kind::Message);
    REQUIRE(outer->fields()[0].type.message != nullptr);
    CHECK(outer->fields()[0].type.message->name() == "Middle");
    CHECK(outer->fields()[0].type.message->fields()[0].name == "leaf");
    CHECK(outer->fields()[0].type.message->fields()[0].type.message->name() == "Leaf");
    // List<Message(Middle)> survived too.
    REQUIRE(listy->fields()[0].type.kind == Kind::List);
    REQUIRE(listy->fields()[0].type.element != nullptr);
    CHECK(listy->fields()[0].type.element->kind == Kind::Message);
    CHECK(listy->fields()[0].type.element->message->name() == "Middle");
    // Exact, not approximate.
    CHECK(outer->content_id() == schema_of<Outer>()->content_id());
    CHECK(listy->content_id() == schema_of<Listy>()->content_id());
}

TEST_CASE("a shared dependency is carried once, and a deeper list nesting survives") {
    // Outer and Listy both reach Middle -> Leaf; the closure is deduplicated by
    // (name, version) rather than repeated per root.
    std::vector<std::shared_ptr<const Schema>> refs;
    collect_referenced(*schema_of<Outer>(), refs);
    collect_referenced(*schema_of<Listy>(), refs);
    REQUIRE(refs.size() == 2);
    CHECK(refs[0]->name() == "Leaf");
    CHECK(refs[1]->name() == "Middle");

    // List<List<Message(Leaf)>> -- a type the wire's flat token stream must keep.
    auto deep = SchemaBuilder("Deep", 1)
                    .list("rows", type_list(type_message(schema_of<Leaf>())))
                    .build();
    const Value answer = encode_accepted_shapes({deep});
    Registry deps;
    decode_accepted_referenced(answer, deps);
    const auto roots = decode_accepted_roots(answer, deps);
    REQUIRE(roots.size() == 1);
    CHECK(roots[0]->content_id() == deep->content_id());
    CHECK(roots[0]->fields()[0].type.kind == Kind::List);
    CHECK(roots[0]->fields()[0].type.element->kind == Kind::List);
    CHECK(roots[0]->fields()[0].type.element->element->kind == Kind::Message);
    CHECK(roots[0]->fields()[0].type.element->element->message->name() == "Leaf");
}

TEST_CASE("roots stay distinguishable from dependencies, including when a shape is both") {
    Switchboard bus;
    const WeaveId target = mount<Nested>(bus);
    WeaveId asker_id{};
    Asker* asker = mount_asker(bus, may_ask(), asker_id);
    asker->ask_id(bus, target);
    REQUIRE(asker->answered());

    const std::set<std::string> roots = identities_from_wire(asker->answer(), "accepted");
    const std::set<std::string> deps = identities_from_wire(asker->answer(), "referenced");
    // Sendable to this target.
    CHECK(roots.count("Outer v1") == 1);
    CHECK(roots.count("Listy v1") == 1);
    // Structural only -- the target does NOT accept these.
    CHECK(deps.count("Middle v1") == 1);
    CHECK(deps.count("Leaf v1") == 1);
    CHECK(roots.count("Middle v1") == 0);
    CHECK(roots.count("Leaf v1") == 0);
    CHECK(bus.accepted_schemas(target).size() == roots.size());

    // A shape that is genuinely both appears in both, and re-registration on the
    // consumer side is an identical no-op rather than a conflict.
    const Value both = encode_accepted_shapes({schema_of<Outer>(), schema_of<Middle>()});
    CHECK(identities_from_wire(both, "accepted").count("Middle v1") == 1);
    CHECK(identities_from_wire(both, "referenced").count("Middle v1") == 1);
    Registry r;
    CHECK_NOTHROW(decode_accepted_referenced(both, r));
    CHECK_NOTHROW(decode_accepted_roots(both, r));
}

TEST_CASE("a flat accept-set emits no closure section, and absence decodes as empty") {
    const Value flat = encode_accepted_shapes({schema_of<StartTimer>()});
    CHECK(flat.get("referenced") == nullptr);
    Registry deps;
    CHECK_NOTHROW(decode_accepted_referenced(flat, deps));
    CHECK(decode_accepted_roots(flat, deps).size() == 1);
}

// ---- authority: ask, know, and still not be allowed to speak ----------------

TEST_CASE("an unauthorized asker is refused by the ordinary gate and learns nothing") {
    Switchboard bus;
    std::vector<sbfx::TapRecord> tap;  // host-side observation only
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(sbfx::to_record(e)); });
    const WeaveId target = mount<Timerish>(bus);

    WeaveId asker_id{};
    Asker* asker = mount_asker(bus, Grant{}, asker_id); // no rule for the request
    asker->ask_id(bus, target);

    CHECK_FALSE(asker->answered());
    bool denied = false;
    for (const auto& e : tap) {
        if (e.kind == EventKind::Refused && e.schema == kDescribeAcceptedShapeName) {
            denied = e.reason == RefusalReason::CapabilityDenied;
        }
    }
    CHECK(denied);
}

TEST_CASE("KNOWING A DOOR EXISTS IS NOT PERMISSION TO WALK THROUGH IT") {
    Switchboard bus;
    std::vector<sbfx::TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(sbfx::to_record(e)); });
    auto t = std::make_unique<Timerish>();
    Timerish* target_raw = t.get();
    Grant g = emit_default_grant(*target_raw);
    allow_poke_answers(g);
    allow_describe_answers(g);
    const WeaveId target = bus.register_weave(std::move(t), std::move(g));
    target_raw->zen_set_self(target);

    // Authorized to ASK. Not authorized to send anything it might discover.
    WeaveId asker_id{};
    Asker* asker = mount_asker(bus, may_ask(), asker_id);
    asker->ask_id(bus, target);
    REQUIRE(asker->answered());
    CHECK(identities_from_wire(asker->answer(), "accepted").count("StartTimer v1") == 1);

    // It now knows the shape exactly. Sending it is a different question.
    Registry deps;
    decode_accepted_referenced(asker->answer(), deps);
    for (const auto& r : decode_accepted_roots(asker->answer(), deps)) {
        deps.register_schema(r);
    }
    Value discovered(deps.lookup("StartTimer", 1));
    discovered.set("id", Cell::text("demo"));
    discovered.set("delay_ms", Cell::integer(1000));
    discovered.set("repeat", Cell::boolean(false));
    tap.clear();
    asker->send_learned(bus, target, std::move(discovered));

    CHECK(target_raw->started() == 0); // never delivered
    bool denied = false;
    for (const auto& e : tap) {
        if (e.kind == EventKind::Refused && e.schema == "StartTimer") {
            denied = e.reason == RefusalReason::CapabilityDenied;
        }
    }
    CHECK(denied);
}

TEST_CASE("asking one target's vocabulary confers nothing toward another target") {
    Switchboard bus;
    std::vector<sbfx::TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(sbfx::to_record(e)); });
    const WeaveId a = mount<Timerish>(bus);
    const WeaveId b = mount<Nested>(bus);

    WeaveId asker_id{};
    Grant only_a;
    only_a.allow(kDescribeAcceptedShapeName, 1, a); // this target and no other
    Asker* asker = mount_asker(bus, std::move(only_a), asker_id);

    asker->ask_id(bus, a);
    CHECK(asker->answered());

    tap.clear();
    asker->ask_id(bus, b);
    bool denied = false;
    for (const auto& e : tap) {
        if (e.kind == EventKind::Refused && e.schema == kDescribeAcceptedShapeName) {
            denied = e.reason == RefusalReason::CapabilityDenied;
        }
    }
    CHECK(denied);
}

TEST_CASE("the answer is a gated send like any other: an ungranted target stays silent") {
    Switchboard bus;
    std::vector<sbfx::TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(sbfx::to_record(e)); });
    auto t = std::make_unique<Timerish>();
    Timerish* raw = t.get();
    // mount_granted with a grant that does NOT allow the answer.
    const WeaveId target = bus.register_weave(std::move(t), Grant{});
    raw->zen_set_self(target);

    WeaveId asker_id{};
    Asker* asker = mount_asker(bus, may_ask(), asker_id);
    asker->ask_id(bus, target);

    CHECK_FALSE(asker->answered());
    bool denied = false;
    for (const auto& e : tap) {
        if (e.kind == EventKind::Refused && e.schema == kAcceptedShapesShapeName) {
            denied = e.reason == RefusalReason::CapabilityDenied;
        }
    }
    CHECK(denied);
}

// ---- the finite-declaration law ---------------------------------------------

TEST_CASE("a self-describing weave cannot also be wildcard-accepting") {
    Switchboard bus;
    CHECK_THROWS_AS(bus.register_weave(std::make_unique<Timerish>(), Grant{},
                                       AcceptMode::AnyRegistered),
                    std::invalid_argument);
    // The same weave registers fine under the ordinary listed mode.
    CHECK_NOTHROW(bus.register_weave(std::make_unique<Timerish>(), Grant{}, AcceptMode::Listed));
}

TEST_CASE("a wildcard weave that does NOT describe itself is unaffected") {
    // What the console and the bridge's operator proxy actually are: a raw
    // loom::Weave carrying no substrate doors at all.
    Switchboard bus;
    CHECK_NOTHROW(
        bus.register_weave(std::make_unique<Asker>(), Grant{}, AcceptMode::AnyRegistered));
}

// ---- snapshot semantics ------------------------------------------------------

TEST_CASE("the answer describes the holder that answered, and goes stale on replacement") {
    Switchboard bus;
    auto first = std::make_unique<Timerish>();
    Timerish* raw = first.get();
    Grant g = emit_default_grant(*raw);
    allow_poke_answers(g);
    allow_describe_answers(g);
    const WeaveId id = bus.register_weave(std::move(first), std::move(g), std::string("timer"));
    raw->zen_set_self(id);

    WeaveId asker_id{};
    Grant ask = may_ask();
    ask.allow_to_role(kDescribeAcceptedShapeName, 1, "timer");
    Asker* asker = mount_asker(bus, std::move(ask), asker_id);
    asker->ask_role(bus, "timer");
    REQUIRE(asker->answered());
    const std::set<std::string> before = identities_from_wire(asker->answer(), "accepted");
    CHECK(before.count("StartTimer v1") == 1);
    CHECK(before.count("Outer v1") == 0);

    // The office changes hands. The old answer is not corrected, invalidated or
    // resent -- it remains exactly what it always was: a snapshot.
    bus.unregister_weave(id);
    auto second = std::make_unique<Nested>();
    Nested* raw2 = second.get();
    Grant g2 = emit_default_grant(*raw2);
    allow_poke_answers(g2);
    allow_describe_answers(g2);
    const WeaveId id2 = bus.register_weave(std::move(second), std::move(g2), std::string("timer"));
    raw2->zen_set_self(id2);

    CHECK(identities_from_wire(asker->answer(), "accepted") == before); // unchanged, and now stale

    asker->ask_role(bus, "timer");
    const std::set<std::string> after = identities_from_wire(asker->answer(), "accepted");
    CHECK(after.count("Outer v1") == 1);
    CHECK(after.count("StartTimer v1") == 0);
}

// ---- versions ----------------------------------------------------------------

TEST_CASE("the described identity carries its VERSION, and versions are not interchangeable") {
    const Value answer =
        encode_accepted_shapes({schema_of<StopTimer>(), schema_of<StopTimerV2>()});
    const std::set<std::string> roots = identities_from_wire(answer, "accepted");
    CHECK(roots.count("StopTimer v1") == 1);
    CHECK(roots.count("StopTimer v2") == 1);

    Registry deps;
    const auto rebuilt = decode_accepted_roots(answer, deps);
    REQUIRE(rebuilt.size() == 2);
    CHECK(rebuilt[1]->version() == 2u);
}

// ---- required / optional fidelity --------------------------------------------

TEST_CASE("required-ness, declaration order and field names survive the wire") {
    auto mixed = SchemaBuilder("Mixed", 1)
                     .field("first", Kind::Text)
                     .field("second", Kind::Int, /*required=*/false)
                     .field("third", Kind::Bool)
                     .build();
    const Value answer = encode_accepted_shapes({mixed});
    Registry deps;
    const auto roots = decode_accepted_roots(answer, deps);
    REQUIRE(roots.size() == 1);
    REQUIRE(roots[0]->fields().size() == 3);
    CHECK(roots[0]->fields()[0].name == "first");
    CHECK(roots[0]->fields()[0].required == true);
    CHECK(roots[0]->fields()[1].name == "second");
    CHECK(roots[0]->fields()[1].required == false);
    CHECK(roots[0]->fields()[2].name == "third");
    CHECK(roots[0]->fields()[2].required == true);
    CHECK(roots[0]->content_id() == mixed->content_id());
}

// ---- no authority widening through a helper ---------------------------------

TEST_CASE("allow_describe_answers adds exactly one rule, for the answer shape only") {
    // The whole point of a substrate grant helper is that it is narrow. A
    // helper that quietly widened a weave's reach would hand every mount()ed
    // weave in two repositories authority nobody wrote down.
    Grant g;
    allow_describe_answers(g);
    const auto& rules = g.live().rules();
    REQUIRE(rules.size() == 1);
    CHECK(rules[0].any_shape == false);
    CHECK(rules[0].shape_name == kAcceptedShapesShapeName);
    CHECK(rules[0].shape_version == 1u);
    CHECK(rules[0].any_target == true); // to whoever asked -- the asker is not known in advance
    CHECK(rules[0].target_role.empty());

    // And mount()'s composite grant is still exactly Emit + poke answers + this
    // one: being answerable is not being permitted.
    Switchboard bus;
    Timerish probe;
    Grant mounted = emit_default_grant(probe);
    allow_poke_answers(mounted);
    allow_describe_answers(mounted);
    for (const auto& r : mounted.live().rules()) {
        CHECK(r.any_shape == false); // never an allow_any
    }
}

// ---- honest failure: no half-built vocabulary --------------------------------

TEST_CASE("a malformed answer fails whole; it never yields a partial vocabulary") {
    // A hand-built answer whose closure is mis-ordered (Middle before the Leaf
    // it nests). The encoder cannot produce this -- collect_referenced is
    // post-order by construction -- but a consumer must not half-trust one that
    // arrives anyway.
    Value bad(accepted_shapes_schema());
    bad.set("referenced", Cell::list({Cell::message(encode_schema(*schema_of<Middle>())),
                                      Cell::message(encode_schema(*schema_of<Leaf>()))}));
    bad.set("accepted", Cell::list({Cell::message(encode_schema(*schema_of<Outer>()))}));

    Registry deps;
    CHECK_THROWS_AS(decode_accepted_referenced(bad, deps), std::runtime_error);
    // Nothing partial was published as though it were complete: Middle never
    // resolved, so it never landed.
    CHECK(deps.lookup("Middle", 1) == nullptr);
    CHECK_THROWS_AS(decode_accepted_roots(bad, deps), std::runtime_error);

    // And a root whose dependency is simply absent refuses too, rather than
    // reconstructing a shape with a hole in it.
    Value missing(accepted_shapes_schema());
    missing.set("accepted", Cell::list({Cell::message(encode_schema(*schema_of<Outer>()))}));
    Registry empty;
    CHECK_THROWS_AS(decode_accepted_roots(missing, empty), std::runtime_error);
}

TEST_CASE("the answer encodes the WHOLE accept-set or nothing -- it never truncates") {
    // The one property a bound would quietly break. Encoding walks the schemas
    // the weave already holds and can neither fail for want of resolution nor
    // decide to stop early; a target with many doors says so.
    std::vector<std::shared_ptr<const Schema>> many;
    for (int i = 0; i < 200; ++i) {
        many.push_back(SchemaBuilder("Many" + std::to_string(i), 1)
                           .field("a", Kind::Text)
                           .build());
    }
    const Value answer = encode_accepted_shapes(many);
    CHECK(answer.get("accepted")->as_list().size() == 200);
    Registry deps;
    CHECK(decode_accepted_roots(answer, deps).size() == 200);
}

} // TEST_SUITE("describe")
