#include <doctest.h>

#include "fixtures.hpp"

#include <zen/schema.hpp>

#include <stdexcept>

using namespace zen;

TEST_SUITE("schema") {

TEST_CASE("a built schema reports its name, version, and fields") {
    auto s = SchemaBuilder("Move", 3).field("dx", Kind::Float).field("dy", Kind::Float).build();
    CHECK(s->name() == "Move");
    CHECK(s->version() == 3);
    REQUIRE(s->fields().size() == 2);
    CHECK(s->fields()[0].name == "dx");
    CHECK(s->fields()[1].name == "dy");
    CHECK(s->find("dx") != nullptr);
    CHECK(s->find("dy") != nullptr);
    CHECK(s->find("dz") == nullptr);
}

TEST_CASE("content identity is stable across separately-built identical schemas") {
    auto a = SchemaBuilder("PlayerState", 1).field("hp", Kind::Int).field("name", Kind::Text).build();
    auto b = SchemaBuilder("PlayerState", 1).field("hp", Kind::Int).field("name", Kind::Text).build();
    CHECK(a->content_id() == b->content_id());
    CHECK(same_identity(*a, *b));
}

TEST_CASE("content identity changes with name, version, field name, kind, order, or requiredness") {
    auto base = SchemaBuilder("S", 1).field("a", Kind::Int).field("b", Kind::Text).build();

    auto diff_name = SchemaBuilder("T", 1).field("a", Kind::Int).field("b", Kind::Text).build();
    auto diff_ver = SchemaBuilder("S", 2).field("a", Kind::Int).field("b", Kind::Text).build();
    auto diff_fname = SchemaBuilder("S", 1).field("a", Kind::Int).field("c", Kind::Text).build();
    auto diff_kind = SchemaBuilder("S", 1).field("a", Kind::Float).field("b", Kind::Text).build();
    auto diff_order = SchemaBuilder("S", 1).field("b", Kind::Text).field("a", Kind::Int).build();
    auto diff_req =
        SchemaBuilder("S", 1).field("a", Kind::Int).field("b", Kind::Text, false).build();

    CHECK(base->content_id() != diff_name->content_id());
    CHECK(base->content_id() != diff_ver->content_id());
    CHECK(base->content_id() != diff_fname->content_id());
    CHECK(base->content_id() != diff_kind->content_id());
    CHECK(base->content_id() != diff_order->content_id());
    CHECK(base->content_id() != diff_req->content_id());
}

TEST_CASE("nested-message structure participates in identity") {
    auto inner1 = SchemaBuilder("Inner", 1).field("x", Kind::Int).build();
    auto inner2 = SchemaBuilder("Inner", 1).field("x", Kind::Text).build(); // different shape

    auto outer1 = SchemaBuilder("Outer", 1).message("in", inner1).build();
    auto outer2 = SchemaBuilder("Outer", 1).message("in", inner2).build();
    CHECK(outer1->content_id() != outer2->content_id());
}

TEST_CASE("malformed type references are rejected at construction") {
    // A Message field with no schema.
    CHECK_THROWS_AS((void)Schema("Bad", 1, {Field{"m", TypeRef{Kind::Message, nullptr, nullptr}, true}}),
                    std::invalid_argument);
    // A List field with no element.
    CHECK_THROWS_AS((void)Schema("Bad", 1, {Field{"l", TypeRef{Kind::List, nullptr, nullptr}, true}}),
                    std::invalid_argument);
    // type_of refuses non-primitive kinds.
    CHECK_THROWS_AS((void)type_of(Kind::Message), std::invalid_argument);
    CHECK_THROWS_AS((void)type_of(Kind::List), std::invalid_argument);
    // type_message refuses a null schema.
    CHECK_THROWS_AS((void)type_message(nullptr), std::invalid_argument);
}

TEST_CASE("duplicate and empty field names are rejected") {
    CHECK_THROWS_AS((void)SchemaBuilder("Dup", 1).field("a", Kind::Int).field("a", Kind::Text).build(),
                    std::invalid_argument);
    CHECK_THROWS_AS((void)SchemaBuilder("Empty", 1).field("", Kind::Int).build(),
                    std::invalid_argument);
}

TEST_CASE("list element types nest and contribute to identity") {
    auto list_of_int = SchemaBuilder("L", 1).list("xs", type_of(Kind::Int)).build();
    auto list_of_text = SchemaBuilder("L", 1).list("xs", type_of(Kind::Text)).build();
    auto list_of_list = SchemaBuilder("L", 1).list("xs", type_list(type_of(Kind::Int))).build();
    CHECK(list_of_int->content_id() != list_of_text->content_id());
    CHECK(list_of_int->content_id() != list_of_list->content_id());
}

} // TEST_SUITE
