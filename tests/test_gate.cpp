#include <doctest.h>

#include "fixtures.hpp"

#include <zen/gate.hpp>

using namespace zen;

namespace {

Value good_move() {
    Value v(fx::Move());
    v.set("dx", Cell::real(1.0)).set("dy", Cell::real(-2.0));
    return v;
}

Value good_player(std::int64_t hp = 30) {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(hp)).set("name", Cell::text("Ami"));
    return v;
}

} // namespace

TEST_SUITE("gate") {

TEST_CASE("a well-formed value is admitted and yields the value") {
    Admission a = admit(good_move(), *fx::Move());
    REQUIRE(a.ok());
    CHECK(a.value().get("dx")->as_float() == doctest::Approx(1.0));
    // moving the trusted value out
    Value out = std::move(a).value();
    CHECK(out.get("dy")->as_float() == doctest::Approx(-2.0));
}

TEST_CASE("a missing required field is refused with a precise error") {
    Value v(fx::Move());
    v.set("dx", Cell::real(1.0)); // dy never set
    Admission a = admit(std::move(v), *fx::Move());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MissingField);
    CHECK(a.first_error().path == "dy");
}

TEST_CASE("a value claiming the wrong schema is refused") {
    Value spawn(fx::Spawn());
    spawn.set("kind", Cell::text("dragon"));
    Admission a = admit(std::move(spawn), *fx::Move());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::SchemaMismatch);
}

TEST_CASE("a wrong-typed field is refused with TypeMismatch") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::text("?!")).set("name", Cell::text("Ami")); // hp should be Int
    Admission a = admit(std::move(v), *fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::TypeMismatch);
    CHECK(a.first_error().path == "hp");
    CHECK(a.first_error().expected == "Int");
}

TEST_CASE("accessing value() on a refusal throws; first_error() on success throws") {
    Admission good = admit(good_move(), *fx::Move());
    CHECK_THROWS(good.first_error());
    Value v(fx::Move());
    Admission bad = admit(std::move(v), *fx::Move());
    CHECK_THROWS(bad.value());
}

TEST_CASE("fast path stops at the first error; full report collects all") {
    Value v(fx::SetColor()); // r,g,b Int + named Bool, all required, all absent
    Admission fast = admit(Value(v), *fx::SetColor(), Report::FirstError);
    REQUIRE_FALSE(fast.ok());
    CHECK(fast.errors().size() == 1);

    auto all = diagnose(v, *fx::SetColor());
    CHECK(all.size() == 4);
}

TEST_CASE("nested messages and lists of messages validate recursively") {
    Value leader(fx::PlayerState());
    leader.set("hp", Cell::integer(50)).set("name", Cell::text("Cap"));

    Value m1(fx::PlayerState());
    m1.set("hp", Cell::integer(10)).set("name", Cell::text("A"));
    Value m2(fx::PlayerState());
    m2.set("hp", Cell::integer(20)).set("name", Cell::text("B"));

    Value squad(fx::Squad());
    squad.set("name", Cell::text("Alpha"));
    squad.set("leader", Cell::message(std::move(leader)));
    squad.set("members", Cell::list({Cell::message(std::move(m1)), Cell::message(std::move(m2))}));

    CHECK(admit(Value(squad), *fx::Squad()).ok());

    // Corrupt one nested member: drop its required 'name'.
    Value bad_member(fx::PlayerState());
    bad_member.set("hp", Cell::integer(1));
    Value bad_squad(fx::Squad());
    bad_squad.set("name", Cell::text("Alpha"));
    bad_squad.set("leader", Cell::message(good_player()));
    bad_squad.set("members", Cell::list({Cell::message(std::move(bad_member))}));

    Admission a = admit(std::move(bad_squad), *fx::Squad());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MissingField);
    CHECK(a.first_error().path == "members[0].name");
}

TEST_CASE("a list element of the wrong type is refused at its index") {
    Value inv(fx::Inventory());
    inv.set("owner", Cell::text("me"));
    inv.set("items", Cell::list({Cell::text("sword"), Cell::integer(7)})); // 7 is not Text
    inv.set("counts", Cell::list({}));
    Admission a = admit(std::move(inv), *fx::Inventory());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::TypeMismatch);
    CHECK(a.first_error().path == "items[1]");
}

TEST_CASE("a null nested message is refused") {
    Value squad(fx::Squad());
    squad.set("name", Cell::text("Alpha"));
    squad.set("leader", Cell::message(std::shared_ptr<Value>{}));
    squad.set("members", Cell::list({}));
    Admission a = admit(std::move(squad), *fx::Squad());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::NullMessage);
    CHECK(a.first_error().path == "leader");
}

TEST_CASE("optional fields may be absent") {
    Value g(fx::Greeting());
    g.set("to", Cell::text("world"));
    CHECK(admit(std::move(g), *fx::Greeting()).ok());
}

TEST_CASE("the gate invocation counter advances once per admit") {
    auto before = gate_invocations();
    (void)admit(good_player(), *fx::PlayerState());
    CHECK(gate_invocations() == before + 1);
}

} // TEST_SUITE
