#include <doctest.h>

#include <zen/author/shape.hpp>
#include <zen/serialize.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace zen;
namespace au = zen::author;

namespace {

struct Inner {
    std::int64_t x;
    ZEN_SHAPE(Inner, 1, ZEN_FIELD(x));
};

// Exercises every kind: scalars, Bytes, a nested Message, a List of scalars, and
// a List of Messages.
struct Everything {
    std::int64_t i;
    double f;
    std::string t;
    bool b;
    zen::Bytes y;
    Inner nested;
    std::vector<std::int64_t> ints;
    std::vector<Inner> inners;
    ZEN_SHAPE(Everything, 1, ZEN_FIELD(i), ZEN_FIELD(f), ZEN_FIELD(t), ZEN_FIELD(b), ZEN_FIELD(y),
              ZEN_FIELD(nested), ZEN_FIELD(ints), ZEN_FIELD(inners));
};

struct Foo {
    std::int64_t a;
    std::string b;
    ZEN_SHAPE(Foo, 1, ZEN_FIELD(a), ZEN_FIELD(b));
};

// Two versions of the same logical shape — same name, different version.
namespace v1 {
struct Player {
    std::int64_t hp;
    ZEN_SHAPE(Player, 1, ZEN_FIELD(hp));
};
} // namespace v1
namespace v2 {
struct Player {
    std::int64_t hp;
    std::string name;
    ZEN_SHAPE(Player, 2, ZEN_FIELD(hp), ZEN_FIELD(name));
};
} // namespace v2

} // namespace

TEST_SUITE("author") {

TEST_CASE("a struct-derived schema is byte-identical to the hand-built one") {
    auto hand = SchemaBuilder("Foo", 1).field("a", Kind::Int).field("b", Kind::Text).build();
    auto derived = au::schema_of<Foo>();

    CHECK(derived->name() == "Foo");
    CHECK(derived->version() == 1);
    CHECK(derived->content_id() == hand->content_id()); // same identity, same door

    // They admit each other's values.
    Foo foo{7, "hi"};
    Value from_struct = au::to_value(foo);
    CHECK(admit(Value(from_struct), *hand).ok());

    Value hand_built(hand);
    hand_built.set("a", Cell::integer(7)).set("b", Cell::text("hi"));
    CHECK(admit(Value(hand_built), *derived).ok());

    // ... and the struct comes back out of the hand-built value unchanged.
    Foo back = au::from_value<Foo>(admit(std::move(hand_built), *derived).value());
    CHECK(back.a == 7);
    CHECK(back.b == "hi");
}

TEST_CASE("a differing struct under the same (name, version) is a SchemaConflict") {
    Registry reg;
    reg.register_schema(au::schema_of<Foo>());
    auto impostor = SchemaBuilder("Foo", 1).field("a", Kind::Float).build(); // different shape
    CHECK_THROWS_AS(reg.register_schema(impostor), zen::SchemaConflict);
}

TEST_CASE("every kind round-trips through the gated wire path") {
    Everything e;
    e.i = 9007199254740993LL; // 2^53 + 1
    e.f = 0.1;
    e.t = "h\xC3\xA9llo";
    e.b = true;
    e.y = zen::Bytes{0, 1, 2, 255};
    e.nested = Inner{42};
    e.ints = {1, 2, 3};
    e.inners = {Inner{10}, Inner{20}};

    std::string bytes = serialize(au::to_value(e));
    Unverified u = parse(bytes);
    REQUIRE(u.well_formed());
    Admission a = admit(u, au::schema_of<Everything>());
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    Everything back = au::from_value<Everything>(a.value());

    // Canonical-bytes equality is exact value equality.
    CHECK(serialize(au::to_value(back)) == bytes);
    CHECK(back.i == e.i);
    CHECK(back.nested.x == 42);
    REQUIRE(back.inners.size() == 2);
    CHECK(back.inners[1].x == 20);
    CHECK(back.y == e.y);
}

TEST_CASE("version is part of identity: v1 and v2 are distinct shapes") {
    auto p1 = au::schema_of<v1::Player>();
    auto p2 = au::schema_of<v2::Player>();
    CHECK(p1->name() == "Player");
    CHECK(p2->name() == "Player");
    CHECK(p1->version() == 1);
    CHECK(p2->version() == 2);
    CHECK(p1->content_id() != p2->content_id());
}

} // TEST_SUITE
