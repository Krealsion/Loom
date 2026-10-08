// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include <zen/weave/shape.hpp>
#include <zen/serialize.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace loom;
namespace au = loom;

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
    loom::Bytes y;
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

// A std::optional member of every kind beside one required member.
struct Maybe {
    std::int64_t id = 0;
    std::optional<std::int64_t> i;
    std::optional<double> f;
    std::optional<std::string> t;
    std::optional<bool> b;
    std::optional<loom::Bytes> y;
    std::optional<Inner> nested;
    std::optional<std::vector<std::int64_t>> ints;
    std::optional<std::vector<Inner>> inners;
    ZEN_SHAPE(Maybe, 1, ZEN_FIELD(id), ZEN_FIELD(i), ZEN_FIELD(f), ZEN_FIELD(t), ZEN_FIELD(b),
              ZEN_FIELD(y), ZEN_FIELD(nested), ZEN_FIELD(ints), ZEN_FIELD(inners));
};

// A keyed entry whose value is one of three kinds, each an optional field; listed in a shape that
// nests it twice, so absence is carried inside list elements and inside a nested message.
struct Entry {
    std::string key;
    std::optional<std::int64_t> number;
    std::optional<bool> flag;
    std::optional<std::string> text;
    ZEN_SHAPE(Entry, 1, ZEN_FIELD(key), ZEN_FIELD(number), ZEN_FIELD(flag), ZEN_FIELD(text));
};
struct Entries {
    std::vector<Entry> entries;
    std::optional<Entry> pinned;
    ZEN_SHAPE(Entries, 1, ZEN_FIELD(entries), ZEN_FIELD(pinned));
};

struct Defaulted {
    std::optional<std::int64_t> limit = 5;
    ZEN_SHAPE(Defaulted, 1, ZEN_FIELD(limit));
};

const char* const kMaybeOptional[] = {"i", "f", "t", "b", "y", "nested", "ints", "inners"};

Value through_native(const Value& v, std::shared_ptr<const Schema> door) {
    Admission a = admit(parse(serialize(v)), std::move(door));
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    return a.value();
}

Value through_compat(const Value& v, std::shared_ptr<const Schema> door) {
    Admission a = admit(compat::parse(compat::serialize(v)), std::move(door));
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    return a.value();
}

} // namespace

TEST_SUITE("weave_shape") {

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
    CHECK_THROWS_AS(reg.register_schema(impostor), loom::SchemaConflict);
}

TEST_CASE("every kind round-trips through the gated wire path") {
    Everything e;
    e.i = 9007199254740993LL; // 2^53 + 1
    e.f = 0.1;
    e.t = "h\xC3\xA9llo";
    e.b = true;
    e.y = loom::Bytes{0, 1, 2, 255};
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

TEST_CASE("a struct-derived value's bytes are its hand-built twin's") {
    auto hand = SchemaBuilder("Foo", 1).field("a", Kind::Int).field("b", Kind::Text).build();
    Value hand_built(hand);
    hand_built.set("a", Cell::integer(7)).set("b", Cell::text("hi"));
    CHECK(serialize(au::to_value(Foo{7, "hi"})) == serialize(hand_built));
    CHECK(compat::serialize(au::to_value(Foo{7, "hi"})) == compat::serialize(hand_built));
}

TEST_CASE("a std::optional member is an optional field of its type, and its schema is the "
          "hand-built twin's") {
    auto inner = au::schema_of<Inner>();
    auto hand = SchemaBuilder("Maybe", 1)
                    .field("id", Kind::Int)
                    .field("i", Kind::Int, /*required=*/false)
                    .field("f", Kind::Float, false)
                    .field("t", Kind::Text, false)
                    .field("b", Kind::Bool, false)
                    .field("y", Kind::Bytes, false)
                    .message("nested", inner, false)
                    .list("ints", type_of(Kind::Int), false)
                    .list("inners", type_message(inner), false)
                    .build();
    auto derived = au::schema_of<Maybe>();
    CHECK(derived->content_id() == hand->content_id());
    CHECK(same_identity(*derived, *hand));
    REQUIRE(derived->fields().size() == 9);
    CHECK(derived->fields()[0].required);
    for (std::size_t n = 1; n < derived->fields().size(); ++n) {
        CHECK_FALSE(derived->fields()[n].required);
    }
    CHECK(derived->find("nested")->type.kind == Kind::Message);
    CHECK(derived->find("inners")->type.element->kind == Kind::Message);

    // Requiredness is identity: the all-required twin is another shape.
    auto all_required = SchemaBuilder("Maybe", 1)
                            .field("id", Kind::Int)
                            .field("i", Kind::Int)
                            .field("f", Kind::Float)
                            .field("t", Kind::Text)
                            .field("b", Kind::Bool)
                            .field("y", Kind::Bytes)
                            .message("nested", inner)
                            .list("ints", type_of(Kind::Int))
                            .list("inners", type_message(inner))
                            .build();
    CHECK(derived->content_id() != all_required->content_id());
}

TEST_CASE("std::nullopt is the field absent, and both encodings carry it back as std::nullopt") {
    Maybe none;
    none.id = 1;
    const Value v = au::to_value(none);
    for (const char* name : kMaybeOptional) {
        CHECK_MESSAGE(!v.has(name), name);
    }
    const std::string text = compat::serialize(v);
    CHECK(text.find("\"i\"") == std::string::npos);
    CHECK(text.find("null") == std::string::npos);

    for (const Value& back : {through_native(v, au::schema_of<Maybe>()),
                              through_compat(v, au::schema_of<Maybe>())}) {
        const Maybe m = au::from_value<Maybe>(back);
        CHECK(m.id == 1);
        CHECK_FALSE(m.i.has_value());
        CHECK_FALSE(m.f.has_value());
        CHECK_FALSE(m.t.has_value());
        CHECK_FALSE(m.b.has_value());
        CHECK_FALSE(m.y.has_value());
        CHECK_FALSE(m.nested.has_value());
        CHECK_FALSE(m.ints.has_value());
        CHECK_FALSE(m.inners.has_value());
    }
}

TEST_CASE("a JSON null is not absence: the compat decoder refuses it for an optional field of "
          "every kind") {
    for (const char* name : kMaybeOptional) {
        const std::string text =
            std::string(R"({"zen":1,"schema":"Maybe","version":1,"fields":{"id":"1",")") + name +
            R"(":null}})";
        Admission a = admit(compat::parse(text), au::schema_of<Maybe>());
        REQUIRE_FALSE_MESSAGE(a.ok(), name);
        CHECK_MESSAGE(a.first_error().kind == ErrorKind::TypeMismatch, name);
        CHECK_MESSAGE(a.first_error().path == name, name);
    }
}

TEST_CASE("a present optional field holding its kind's zero is a value, never absence") {
    Maybe zero;
    zero.id = 1;
    zero.i = 0;
    zero.f = 0.0;
    zero.t = "";
    zero.b = false;
    zero.y = loom::Bytes{};
    zero.nested = Inner{0};
    zero.ints = std::vector<std::int64_t>{};
    zero.inners = std::vector<Inner>{};
    const Value v = au::to_value(zero);
    for (const char* name : kMaybeOptional) {
        CHECK_MESSAGE(v.has(name), name);
    }
    Maybe none;
    none.id = 1;
    CHECK(serialize(v) != serialize(au::to_value(none)));
    CHECK(compat::serialize(v) != compat::serialize(au::to_value(none)));

    for (const Value& back : {through_native(v, au::schema_of<Maybe>()),
                              through_compat(v, au::schema_of<Maybe>())}) {
        const Maybe m = au::from_value<Maybe>(back);
        CHECK(m.i == std::optional<std::int64_t>{0});
        CHECK(m.f == std::optional<double>{0.0});
        CHECK(m.t == std::optional<std::string>{""});
        CHECK(m.b == std::optional<bool>{false});
        CHECK(m.y == std::optional<loom::Bytes>{loom::Bytes{}});
        REQUIRE(m.nested.has_value());
        CHECK(m.nested->x == 0);
        CHECK(m.ints == std::optional<std::vector<std::int64_t>>{std::vector<std::int64_t>{}});
        REQUIRE(m.inners.has_value());
        CHECK(m.inners->empty());
        CHECK(serialize(au::to_value(m)) == serialize(v));
    }
}

TEST_CASE("absence is carried inside list elements and nested messages, element by element") {
    Entries e;
    e.entries = {Entry{"width", 80, std::nullopt, std::nullopt},
                 Entry{"wrap", std::nullopt, true, std::nullopt},
                 Entry{"title", std::nullopt, std::nullopt, "Notes"}};
    for (const Value& back : {through_native(au::to_value(e), au::schema_of<Entries>()),
                              through_compat(au::to_value(e), au::schema_of<Entries>())}) {
        const Entries got = au::from_value<Entries>(back);
        REQUIRE(got.entries.size() == 3);
        CHECK(got.entries[0].number == std::optional<std::int64_t>{80});
        CHECK_FALSE(got.entries[0].flag.has_value());
        CHECK_FALSE(got.entries[0].text.has_value());
        CHECK_FALSE(got.entries[1].number.has_value());
        CHECK(got.entries[1].flag == std::optional<bool>{true});
        CHECK_FALSE(got.entries[1].text.has_value());
        CHECK_FALSE(got.entries[2].number.has_value());
        CHECK_FALSE(got.entries[2].flag.has_value());
        CHECK(got.entries[2].text == std::optional<std::string>{"Notes"});
        CHECK_FALSE(got.pinned.has_value());
    }

    e.pinned = Entry{"pin", std::nullopt, false, std::nullopt};
    const Entries pinned =
        au::from_value<Entries>(through_compat(au::to_value(e), au::schema_of<Entries>()));
    REQUIRE(pinned.pinned.has_value());
    CHECK(pinned.pinned->key == "pin");
    CHECK(pinned.pinned->flag == std::optional<bool>{false});
    CHECK_FALSE(pinned.pinned->number.has_value());
}

TEST_CASE("an absent optional field is std::nullopt, whatever the member's initializer holds") {
    CHECK(au::to_value(Defaulted{}).has("limit")); // the initializer is a value the struct holds
    const Value absent(au::schema_of<Defaulted>());
    REQUIRE(admit(Value(absent), *au::schema_of<Defaulted>()).ok());
    CHECK_FALSE(au::from_value<Defaulted>(absent).limit.has_value());
}

TEST_CASE("the gate judges a present optional field as it judges a required one") {
    Value wrong(au::schema_of<Maybe>());
    wrong.set("id", Cell::integer(1)).set("i", Cell::text("seven"));
    Admission a = admit(Value(wrong), *au::schema_of<Maybe>());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::TypeMismatch);
    CHECK(a.first_error().path == "i");

    Value no_id(au::schema_of<Maybe>());
    no_id.set("i", Cell::integer(7));
    Admission b = admit(Value(no_id), *au::schema_of<Maybe>());
    REQUIRE_FALSE(b.ok());
    CHECK(b.first_error().kind == ErrorKind::MissingField);
    CHECK(b.first_error().path == "id");
}

} // TEST_SUITE
