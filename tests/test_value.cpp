#include <doctest.h>

#include "fixtures.hpp"

#include <zen/value.hpp>

#include <optional>
#include <stdexcept>

using namespace zen;

TEST_SUITE("value") {

TEST_CASE("cells carry exactly one kind") {
    CHECK(Cell::integer(7).kind() == Kind::Int);
    CHECK(Cell::real(1.5).kind() == Kind::Float);
    CHECK(Cell::text("hi").kind() == Kind::Text);
    CHECK(Cell::boolean(true).kind() == Kind::Bool);
    CHECK(Cell::bytes({1, 2, 3}).kind() == Kind::Bytes);
    CHECK(Cell::list({}).kind() == Kind::List);

    CHECK(Cell::integer(7).as_int() == 7);
    CHECK(Cell::text("hi").as_text() == "hi");
    CHECK(Cell::boolean(true).as_bool() == true);
}

TEST_CASE("a value requires a schema") {
    CHECK_THROWS_AS(Value(nullptr), std::invalid_argument);
}

TEST_CASE("set/get round-trips field data and chains") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(30)).set("name", Cell::text("Ami"));
    REQUIRE(v.has("hp"));
    REQUIRE(v.has("name"));
    CHECK(v.get("hp")->as_int() == 30);
    CHECK(v.get("name")->as_text() == "Ami");
    CHECK(v.get("absent") == nullptr);
}

TEST_CASE("setting a field the schema does not declare is refused") {
    Value v(fx::PlayerState());
    CHECK_THROWS_AS(v.set("mana", Cell::integer(5)), std::out_of_range);
}

TEST_CASE("absent fields read back as null, present positionally") {
    Value v(fx::PlayerState());
    CHECK(v.field_count() == 2);
    CHECK(v.at(0) == nullptr);
    v.set("hp", Cell::integer(1));
    CHECK(v.at(0) != nullptr);
    CHECK(v.at(0)->as_int() == 1);
    CHECK(v.at(1) == nullptr);
    CHECK(v.at(99) == nullptr);
}

TEST_CASE("blind construction walks a runtime-discovered schema") {
    // The console has no compiled knowledge of the schema; it reads the shape.
    auto schema = fx::SetColor();
    int fields_seen = 0;
    auto source = [&](const Field& f) -> std::optional<Cell> {
        ++fields_seen;
        switch (f.type.kind) {
        case Kind::Int:
            return Cell::integer(128);
        case Kind::Bool:
            return Cell::boolean(true);
        default:
            return std::nullopt;
        }
    };
    Value v = construct_blind(schema, source);
    CHECK(fields_seen == 4);
    CHECK(v.get("r")->as_int() == 128);
    CHECK(v.get("named")->as_bool() == true);
}

TEST_CASE("blind construction may leave optional fields absent") {
    auto source = [](const Field& f) -> std::optional<Cell> {
        if (f.name == "to") {
            return Cell::text("world");
        }
        return std::nullopt; // skip the optional 'note'
    };
    Value v = construct_blind(fx::Greeting(), source);
    CHECK(v.has("to"));
    CHECK_FALSE(v.has("note"));
}

TEST_CASE("values move without copying their payload") {
    Value v(fx::PlayerState());
    v.set("name", Cell::text("Ami"));
    Value moved = std::move(v);
    CHECK(moved.get("name")->as_text() == "Ami");
}

} // TEST_SUITE
