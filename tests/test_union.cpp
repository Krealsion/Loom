// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The union and the open field: a shape whose value is exactly one of its cases, a field that
// holds a value of any registered shape, and the one budget a value is admitted under. Each rule
// of docs/reference/values-and-admission.md#unions-and-the-open-field has a case here, through
// the gate, both codecs, the authoring layer, the manifest's codec, the composer and the bus.

#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/gate.hpp>
#include <zen/kernel/schema_codec.hpp>
#include <zen/registry.hpp>
#include <zen/serialize.hpp>
#include <zen/terminal/composer.hpp>
#include <zen/weave/shape.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace loom;

namespace union_fx {

// A pointer moment: a move, a button, or nothing to say.
inline std::shared_ptr<const Schema> moved() {
    static const auto s =
        SchemaBuilder("Moved", 1).field("x", Kind::Int).field("y", Kind::Int).build();
    return s;
}
inline std::shared_ptr<const Schema> moment() {
    static const auto s = make_union("Moment", 1,
                                     {case_of("moved", type_message(moved())),
                                      case_of("button", type_of(Kind::Int)), case_bare("idle")});
    return s;
}
// Records that carry a moment, and a list of them.
inline std::shared_ptr<const Schema> sample() {
    static const auto s = SchemaBuilder("Sample", 1)
                              .field("at", Kind::Int)
                              .message("moment", moment())
                              .list("trail", type_message(moment()), /*required=*/false)
                              .build();
    return s;
}
// A record holding a value of any registered shape.
inline std::shared_ptr<const Schema> carry() {
    static const auto s = SchemaBuilder("Carry", 1)
                              .field("label", Kind::Text)
                              .add(Field{"item", type_any(), true, true})
                              .build();
    return s;
}
inline std::shared_ptr<const Schema> big_list() {
    static const auto s = SchemaBuilder("Many", 1).list("n", type_of(Kind::Int)).build();
    return s;
}

inline Value moved_value(std::int64_t x, std::int64_t y) {
    Value v(moved());
    v.set("x", Cell::integer(x)).set("y", Cell::integer(y));
    return v;
}
inline Value moment_moved(std::int64_t x, std::int64_t y) {
    Value v(moment());
    v.set("moved", Cell::message(moved_value(x, y)));
    return v;
}
inline Value moment_idle() {
    Value v(moment());
    v.choose("idle");
    return v;
}
inline Value sample_of(Value m) {
    Value v(sample());
    v.set("at", Cell::integer(7)).set("moment", Cell::message(std::move(m)));
    return v;
}

// The authoring layer's forms.
struct Press {
    std::int64_t key = 0;
    ZEN_SHAPE(Press, 1, ZEN_FIELD(key));
};
struct Release {
    std::int64_t key = 0;
    ZEN_SHAPE(Release, 1, ZEN_FIELD(key));
};
struct Stroke {
    std::variant<Press, Release> is;
    ZEN_UNION(Stroke, 1, is);
};
struct Pick {
    std::variant<std::int64_t, std::int64_t, std::monostate> is;
    ZEN_UNION(Pick, 1, is, "left", "right", "none");
};
enum class Outcome { delivered, refused, handler_failed };
ZEN_ENUM(Outcome, 1, delivered, refused, handler_failed);
struct Digest {
    Outcome outcome = Outcome::delivered;
    std::vector<Stroke> strokes;
    std::optional<Pick> pick;
    loom::Any extra;
    ZEN_SHAPE(Digest, 1, ZEN_FIELD(outcome), ZEN_FIELD(strokes), ZEN_FIELD(pick),
              ZEN_FIELD(extra));
};

} // namespace union_fx

using namespace union_fx;

TEST_SUITE("union") {

// ---- the shape --------------------------------------------------------------------------------

TEST_CASE("a union is a shape of its own form: a record's id is unchanged, and the same fields "
          "as a union are another identity") {
    const auto as_record = SchemaBuilder("Moment", 1)
                               .message("moved", moved())
                               .field("button", Kind::Int)
                               .build();
    const auto as_union = make_union(
        "Moment", 1, {case_of("moved", type_message(moved())), case_of("button", type_of(Kind::Int))});
    CHECK(as_union->is_union());
    CHECK_FALSE(as_record->is_union());
    CHECK(as_record->content_id() != as_union->content_id());
    CHECK_FALSE(same_structure(*as_record, *as_union));
    // A record's id folds what it always folded: the hand-written constructor agrees.
    CHECK(Schema("Moment", 1, as_record->fields()).content_id() == as_record->content_id());
    // A bare case is another identity than a carrying one of the same name.
    const auto bare = make_union("E", 1, {case_bare("a")});
    const auto carrying = make_union("E", 1, {case_of("a", type_of(Kind::Bool))});
    CHECK(bare->content_id() != carrying->content_id());
}

TEST_CASE("what no shape may be is refused when it is built") {
    CHECK_THROWS_AS(make_union("Empty", 1, {}), std::invalid_argument);
    Field optional_case = case_of("a", type_of(Kind::Int));
    optional_case.required = false;
    CHECK_THROWS_AS(make_union("U", 1, {optional_case}), std::invalid_argument);
    Field bare_field = case_bare("a");
    CHECK_THROWS_AS(make_schema("R", 1, {bare_field}), std::invalid_argument);
    // A field's kind says which form it names.
    CHECK(type_message(moment()).kind == Kind::Union);
    CHECK(type_message(moved()).kind == Kind::Message);
    CHECK_THROWS_AS(make_schema("R", 1, {Field{"m", TypeRef{Kind::Message, moment(), nullptr}, true, true}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(make_schema("R", 1, {Field{"m", TypeRef{Kind::Union, moved(), nullptr}, true, true}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(type_of(Kind::Union), std::invalid_argument);
}

TEST_CASE("the component closure walks a union's cases, and an open field names no shape") {
    std::vector<std::shared_ptr<const Schema>> out;
    collect_referenced(*sample(), out);
    REQUIRE(out.size() == 2);
    CHECK(out[0]->name() == "Moved"); // post-order: a component before what nests it
    CHECK(out[1]->name() == "Moment");
    out.clear();
    collect_referenced(*carry(), out);
    CHECK(out.empty());
    // Whether a shape can hold an open field's value is known when it is built, however deep.
    CHECK(carry()->reaches_open());
    CHECK_FALSE(sample()->reaches_open());
    const auto outer =
        SchemaBuilder("Outer", 1).list("carried", type_message(carry()), /*required=*/false).build();
    CHECK(outer->reaches_open());
    CHECK(make_union("Either", 1, {case_of("c", type_message(outer)), case_bare("none")})
              ->reaches_open());
    CHECK_FALSE(make_union("Bare", 1, {case_bare("none")})->reaches_open());
}

// ---- the value --------------------------------------------------------------------------------

TEST_CASE("a union value holds one case at most: setting or choosing replaces the one held") {
    Value v(moment());
    CHECK_FALSE(v.chosen().has_value());
    v.set("button", Cell::integer(2));
    REQUIRE(v.chosen().has_value());
    CHECK(*v.chosen() == 1);
    CHECK(v.has("button"));
    CHECK(v.get("button")->as_int() == 2);
    v.choose("idle");
    CHECK(*v.chosen() == 2);
    CHECK(v.has("idle"));
    CHECK_FALSE(v.has("button"));
    CHECK(v.get("button") == nullptr);
    CHECK(v.get("idle") == nullptr); // a bare case carries nothing to get
    CHECK_THROWS_AS(v.set("idle", Cell::boolean(true)), std::invalid_argument);
    CHECK_THROWS_AS(v.choose("button"), std::invalid_argument);
    CHECK_THROWS_AS(v.choose("nope"), std::out_of_range);
    Value r(moved());
    CHECK_THROWS_AS(r.choose("x"), std::out_of_range);
    CHECK(Cell::message(moment_idle()).kind() == Kind::Union);
    CHECK(Cell::message(moved_value(1, 2)).kind() == Kind::Message);
}

// ---- the gate ---------------------------------------------------------------------------------

TEST_CASE("the gate admits a union holding one case, and refuses one holding none") {
    CHECK(admit(moment_moved(1, 2), *moment()).ok());
    CHECK(admit(moment_idle(), *moment()).ok());
    Admission none = admit(Value(moment()), *moment());
    REQUIRE_FALSE(none.ok());
    CHECK(none.first_error().kind == ErrorKind::NoCase);
    CHECK(none.first_error().path.empty());
    // Nested: the union's own id first, then its one case, named on the path.
    Value nested_none = sample_of(Value(moment()));
    Admission n = admit(std::move(nested_none), *sample());
    REQUIRE_FALSE(n.ok());
    CHECK(n.first_error().kind == ErrorKind::NoCase);
    CHECK(n.first_error().path == "moment");
    Value wrong(moment());
    Value bad_move(moved());
    bad_move.set("x", Cell::text("left")).set("y", Cell::integer(0));
    wrong.set("moved", Cell::message(std::move(bad_move)));
    Admission w = admit(sample_of(std::move(wrong)), *sample());
    REQUIRE_FALSE(w.ok());
    CHECK(w.first_error().kind == ErrorKind::TypeMismatch);
    CHECK(w.first_error().path == "moment.moved.x");
}

TEST_CASE("a field of one form refuses a value of the other, and another union's value") {
    Value s(sample());
    s.set("at", Cell::integer(1)).set("moment", Cell::message(moved_value(1, 1)));
    Admission a = admit(std::move(s), *sample());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::TypeMismatch);
    const auto other = make_union("Moment", 2, {case_bare("idle")});
    Value o(other);
    o.choose("idle");
    Admission b = admit(sample_of(std::move(o)), *sample());
    REQUIRE_FALSE(b.ok());
    CHECK(b.first_error().kind == ErrorKind::SchemaMismatch);
}

TEST_CASE("one value, one budget: the live gate refuses a value larger than a decode admits, "
          "counted a visit") {
    // Many v1 { n: List<Int> }: one cell for the record's field, one an element.
    Cell::Array at_bound(kMaxDecodedCells - 1, Cell::integer(0));
    Value fits(big_list());
    fits.set("n", Cell::list(at_bound));
    CHECK(admit(fits, *big_list()).ok());
    // The same bytes re-admit: the boundaries count alike.
    CHECK(admit(parse(serialize(fits)), big_list()).ok());
    Cell::Array past = at_bound;
    past.push_back(Cell::integer(0));
    Value over(big_list());
    over.set("n", Cell::list(std::move(past)));
    Admission a = admit(std::move(over), *big_list());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::TooLarge);
    // Shared parts are counted each visit: one value of 2 cells placed in a list 40,000 times
    // costs 120,001 cells to walk, though it is held once.
    const auto pairs = SchemaBuilder("Pairs", 1).list("p", type_message(moved())).build();
    const Cell shared = Cell::message(moved_value(1, 2));
    Value many(pairs);
    many.set("p", Cell::list(Cell::Array(40000, shared)));
    Admission m = admit(std::move(many), *pairs);
    REQUIRE_FALSE(m.ok());
    CHECK(m.first_error().kind == ErrorKind::TooLarge);
    // A union entered costs one cell: kMaxDecodedCells - 1 bare moments, each a union, fit
    // exactly when the list's elements are counted with their entries.
    const auto idles = SchemaBuilder("Idles", 1).list("m", type_message(moment())).build();
    Value u(idles);
    u.set("m", Cell::list(Cell::Array((kMaxDecodedCells - 1) / 2, Cell::message(moment_idle()))));
    CHECK(admit(std::move(u), *idles).ok());
}

TEST_CASE("an open field holds a value of a registered shape, checked against the registry") {
    Registry reg;
    reg.register_schema(moved());
    Value c(carry());
    c.set("label", Cell::text("a")).set("item", Cell::message(moved_value(3, 4)));
    // Without a registry there is nothing to check the shape against: refused, never trusted.
    Admission no_reg = admit(c, *carry());
    REQUIRE_FALSE(no_reg.ok());
    CHECK(no_reg.first_error().kind == ErrorKind::UnknownSchema);
    CHECK(no_reg.first_error().path == "item");
    CHECK(admit(c, *carry(), reg).ok());
    // An unregistered shape is refused.
    Value d(carry());
    d.set("label", Cell::text("b")).set("item", Cell::message(Value(SchemaBuilder("Stranger", 1).build())));
    Admission un = admit(std::move(d), *carry(), reg);
    REQUIRE_FALSE(un.ok());
    CHECK(un.first_error().kind == ErrorKind::UnknownSchema);
    // Another definition of a registered name and version is refused.
    const auto impostor = SchemaBuilder("Moved", 1).field("x", Kind::Text).build();
    Value i(impostor);
    i.set("x", Cell::text("x"));
    Value e(carry());
    e.set("label", Cell::text("c")).set("item", Cell::message(std::move(i)));
    Admission im = admit(std::move(e), *carry(), reg);
    REQUIRE_FALSE(im.ok());
    CHECK(im.first_error().kind == ErrorKind::SchemaMismatch);
    // The value is checked against the registered shape, at its path.
    Value half(moved());
    half.set("x", Cell::integer(1));
    Value f(carry());
    f.set("label", Cell::text("d")).set("item", Cell::message(std::move(half)));
    Admission h = admit(std::move(f), *carry(), reg);
    REQUIRE_FALSE(h.ok());
    CHECK(h.first_error().kind == ErrorKind::MissingField);
    CHECK(h.first_error().path == "item.y");
    // A scalar in an open field is the wrong kind.
    Value g(carry());
    g.set("label", Cell::text("e")).set("item", Cell::integer(1));
    Admission k = admit(std::move(g), *carry(), reg);
    REQUIRE_FALSE(k.ok());
    CHECK(k.first_error().kind == ErrorKind::TypeMismatch);
}

// ---- the native codec -------------------------------------------------------------------------

TEST_CASE("native bytes: a union is its case's index and that case's value, one form a value") {
    for (Value m : {moment_moved(-3, 9), moment_idle()}) {
        Value s = sample_of(m);
        Cell::Array trail{Cell::message(moment_idle()), Cell::message(moment_moved(1, 1))};
        s.set("trail", Cell::list(std::move(trail)));
        const std::string bytes = serialize(s);
        Admission a = admit(parse(bytes), sample());
        REQUIRE(a.ok());
        CHECK(serialize(a.value()) == bytes);
    }
    // An enumeration's value is one byte of body.
    const auto e = make_union("E", 1, {case_bare("a"), case_bare("b")});
    Value b(e);
    b.choose("b");
    const std::string eb = serialize(b);
    const std::string header = serialize(Value(SchemaBuilder("E", 1).build()));
    CHECK(eb.size() == header.size() + 1);
    CHECK(static_cast<unsigned char>(eb.back()) == 1);
    CHECK_THROWS_AS(serialize(Value(e)), std::invalid_argument); // no case: not a value to write
}

TEST_CASE("native bytes: an index past the last case is refused, never skipped") {
    const auto e = make_union("E", 1, {case_bare("a"), case_bare("b")});
    Value b(e);
    b.choose("b");
    std::string bytes = serialize(b);
    bytes.back() = static_cast<char>(2); // the third case of two
    Admission a = admit(parse(bytes), e);
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedField);
    // A record's body under a union's identity is refused, not read as a case.
    const auto rec = SchemaBuilder("E", 1).field("a", Kind::Int).field("b", Kind::Int).build();
    Value r(rec);
    r.set("a", Cell::integer(5)).set("b", Cell::integer(6));
    std::string forged = serialize(r);
    const std::string real_header = serialize(b).substr(0, serialize(b).size() - 1);
    forged = real_header + forged.substr(real_header.size());
    CHECK_FALSE(admit(parse(forged), e).ok());
}

TEST_CASE("native bytes: an open field carries its shape's identity and is resolved on the way "
          "in") {
    Registry reg;
    reg.register_schema(moved());
    reg.register_schema(moment());
    for (Value item : {moved_value(5, 6), moment_moved(1, 2)}) {
        Value c(carry());
        c.set("label", Cell::text("x")).set("item", Cell::message(item));
        const std::string bytes = serialize(c);
        Admission a = admit(parse(bytes), carry(), reg);
        REQUIRE(a.ok());
        CHECK(a.value().get("item")->as_message()->schema().name() == item.schema().name());
        CHECK(serialize(a.value()) == bytes);
        Admission none = admit(parse(bytes), carry());
        REQUIRE_FALSE(none.ok());
        CHECK(none.first_error().kind == ErrorKind::UnknownSchema);
    }
    // Written against another definition of the name and version: refused by its content id.
    const auto other = SchemaBuilder("Moved", 1).field("x", Kind::Int).build();
    Value o(other);
    o.set("x", Cell::integer(1));
    Value c(carry());
    c.set("label", Cell::text("y")).set("item", Cell::message(std::move(o)));
    Admission a = admit(parse(serialize(c)), carry(), reg);
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::SchemaMismatch);
    // Unregistered: refused by name.
    Registry empty;
    Admission u = admit(parse(serialize(c)), carry(), empty);
    REQUIRE_FALSE(u.ok());
    CHECK(u.first_error().kind == ErrorKind::UnknownSchema);
}

// ---- compat JSON ------------------------------------------------------------------------------

TEST_CASE("compat JSON: a union is one member naming its case, or a bare case's name") {
    Value s = sample_of(moment_moved(2, 3));
    s.set("trail", Cell::list({Cell::message(moment_idle())}));
    const std::string text = compat::serialize(s);
    CHECK(text.find("\"moment\":{\"moved\":{\"x\":\"2\",\"y\":\"3\"}}") != std::string::npos);
    CHECK(text.find("\"trail\":[\"idle\"]") != std::string::npos);
    Admission a = admit(compat::parse(text), sample());
    REQUIRE(a.ok());
    CHECK(serialize(a.value()) == serialize(s));
    // A union at the door: its fields are its case.
    const std::string top = compat::serialize(moment_idle());
    CHECK(top.find("\"fields\":\"idle\"") != std::string::npos);
    CHECK(admit(compat::parse(top), moment()).ok());
}

TEST_CASE("compat JSON: no case, several cases, an undeclared case and a case in the wrong form "
          "are each refused by name") {
    const std::string head = "{\"zen\":1,\"schema\":\"Moment\",\"version\":1,\"fields\":";
    auto refused = [&](const std::string& body) {
        Admission a = admit(compat::parse(head + body + "}"), moment());
        REQUIRE_FALSE(a.ok());
        return a.first_error();
    };
    CHECK(refused("{}").kind == ErrorKind::NoCase);
    const Error several = refused("{\"button\":\"1\",\"idle\":true}");
    CHECK(several.kind == ErrorKind::SeveralCases);
    CHECK(refused("{\"jump\":\"1\"}").kind == ErrorKind::UnknownField);
    CHECK(refused("\"jump\"").kind == ErrorKind::UnknownField);
    CHECK(refused("{\"idle\":true}").kind == ErrorKind::TypeMismatch);
    CHECK(refused("\"button\"").kind == ErrorKind::MissingField);
    CHECK(refused("[]").kind == ErrorKind::MalformedBytes); // neither an object nor a name
    // A record's fields are an object, never a case's name.
    Admission r = admit(compat::parse("{\"zen\":1,\"schema\":\"Moved\",\"version\":1,\"fields\":\"x\"}"),
                        moved());
    CHECK_FALSE(r.ok());
}

TEST_CASE("compat JSON: an open field names its shape and is resolved on the way in") {
    Registry reg;
    reg.register_schema(moved());
    Value c(carry());
    c.set("label", Cell::text("x")).set("item", Cell::message(moved_value(5, 6)));
    const std::string text = compat::serialize(c);
    CHECK(text.find("\"item\":{\"schema\":\"Moved\",\"version\":1,\"content_id\":") !=
          std::string::npos);
    Admission a = admit(compat::parse(text), carry(), reg);
    REQUIRE(a.ok());
    CHECK(serialize(a.value()) == serialize(c));
    Admission none = admit(compat::parse(text), carry());
    REQUIRE_FALSE(none.ok());
    CHECK(none.first_error().kind == ErrorKind::UnknownSchema);
    const std::string head = "{\"zen\":1,\"schema\":\"Carry\",\"version\":1,\"fields\":{\"label\":"
                             "\"x\",\"item\":";
    auto refused = [&](const std::string& item) {
        Admission b = admit(compat::parse(head + item + "}}"), carry(), reg);
        REQUIRE_FALSE(b.ok());
        return b.first_error().kind;
    };
    CHECK(refused("{\"schema\":\"Moved\",\"version\":1,\"fields\":{\"x\":\"1\"},\"extra\":1}") ==
          ErrorKind::UnknownField);
    CHECK(refused("{\"schema\":\"Gone\",\"version\":1,\"fields\":{}}") == ErrorKind::UnknownSchema);
    CHECK(refused("{\"schema\":\"Moved\",\"version\":1,\"content_id\":\"0x1\",\"fields\":{}}") ==
          ErrorKind::SchemaMismatch);
    CHECK(refused("{\"version\":1,\"fields\":{}}") == ErrorKind::MalformedField);
    CHECK(refused("\"Moved\"") == ErrorKind::TypeMismatch);
}

TEST_CASE("the decode budget counts a union entered as one cell, in both encodings") {
    const auto idles = SchemaBuilder("Idles", 1).list("m", type_message(moment())).build();
    // One cell for the record's field, then a cell an element and a cell a union entered.
    const std::size_t fit = (kMaxDecodedCells - 1) / 2;
    Value v(idles);
    v.set("m", Cell::list(Cell::Array(fit, Cell::message(moment_idle()))));
    CHECK(admit(parse(serialize(v)), idles).ok());
    CHECK(admit(compat::parse(compat::serialize(v)), idles).ok());
    Value w(idles);
    w.set("m", Cell::list(Cell::Array(fit + 1, Cell::message(moment_idle()))));
    Admission a = admit(parse(serialize(w)), idles);
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedBytes);
}

// ---- the authoring layer ------------------------------------------------------------------------

TEST_CASE("ZEN_UNION: a struct with one variant is a union shape, its cases named by their shapes "
          "or by the line") {
    const auto stroke = schema_of<Stroke>();
    REQUIRE(stroke->is_union());
    REQUIRE(stroke->fields().size() == 2);
    CHECK(stroke->fields()[0].name == "Press");
    CHECK(stroke->fields()[1].name == "Release");
    CHECK(stroke->fields()[1].type.kind == Kind::Message);
    const auto pick = schema_of<Pick>();
    REQUIRE(pick->fields().size() == 3);
    CHECK(pick->fields()[1].name == "right");
    CHECK(pick->fields()[1].type.kind == Kind::Int);
    CHECK_FALSE(pick->fields()[2].carries);
    CHECK(schema_of<Digest>()->find("strokes")->type.element->kind == Kind::Union);
}

TEST_CASE("ZEN_ENUM: an enum is an enumeration, a union whose cases carry nothing") {
    const auto e = schema_of<Outcome>();
    REQUIRE(e->is_union());
    REQUIRE(e->fields().size() == 3);
    CHECK(e->fields()[2].name == "handler_failed");
    CHECK_FALSE(e->fields()[0].carries);
    Value v = to_value(Outcome::refused);
    CHECK(*v.chosen() == 1);
    CHECK(from_value<Outcome>(v) == Outcome::refused);
    // An enumerator the line does not list holds no case, which the gate refuses.
    Admission a = admit(to_value(static_cast<Outcome>(9)), *e);
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::NoCase);
}

TEST_CASE("a struct holding unions, an enumeration and an open field round-trips through the "
          "gate and the bytes") {
    Registry reg;
    reg.register_schema(schema_of<Press>());
    Digest r;
    r.outcome = Outcome::handler_failed;
    r.strokes = {Stroke{Press{4}}, Stroke{Release{4}}};
    r.pick = Pick{std::variant<std::int64_t, std::int64_t, std::monostate>{std::in_place_index<1>, 8}};
    r.extra = Any::of(Press{11});
    Value v = to_value(r);
    Admission a = admit(parse(serialize(v)), schema_of<Digest>(), reg);
    REQUIRE(a.ok());
    const Digest back = from_value<Digest>(a.value());
    CHECK(back.outcome == Outcome::handler_failed);
    REQUIRE(back.strokes.size() == 2);
    CHECK(std::get<Release>(back.strokes[1].is).key == 4);
    REQUIRE(back.pick.has_value());
    CHECK(back.pick->is.index() == 1);
    CHECK(std::get<1>(back.pick->is) == 8);
    CHECK(back.extra.holds<Press>());
    CHECK_FALSE(back.extra.holds<Release>());
    CHECK(back.extra.as<Press>().key == 11);
    CHECK_THROWS_AS(back.extra.as<Release>(), std::invalid_argument);
    // An empty Any leaves its field absent, and the gate says so.
    Digest empty;
    Admission m = admit(to_value(empty), *schema_of<Digest>(), reg);
    REQUIRE_FALSE(m.ok());
    CHECK(m.first_error().kind == ErrorKind::MissingField);
    CHECK(m.first_error().path == "extra");
}

// ---- the manifest's codec, the describe door, and the composer --------------------------------

TEST_CASE("a shape's description says a union, its bare cases and an open field, and rebuilds "
          "the same identity") {
    Registry deps;
    deps.register_schema(moved());
    for (const auto& s : {moment(), carry()}) {
        Value desc = encode_schema(*s);
        Admission a = admit(parse(serialize(desc)), schema_desc_schema());
        REQUIRE(a.ok());
        const auto rebuilt = decode_schema(a.value(), deps);
        CHECK(same_identity(*rebuilt, *s));
        deps.register_schema(rebuilt);
    }
    // A token naming a record as a union is refused.
    Value forged(schema_desc_schema());
    forged.set("name", Cell::text("Bad")).set("version", Cell::integer(1));
    forged.set("union", Cell::boolean(false));
    Value fd(field_desc_schema());
    fd.set("name", Cell::text("m")).set("required", Cell::boolean(true));
    fd.set("carries", Cell::boolean(true));
    fd.set("type", Cell::list({Cell::message(make_type_token(Kind::Union, moved()))}));
    forged.set("fields", Cell::list({Cell::message(std::move(fd))}));
    CHECK_THROWS_AS(decode_schema(forged, deps), std::runtime_error);
    CHECK(manifest_schema()->version() == 6);
}

namespace {
class ComposeFrom final : public ComposeSource {
public:
    std::shared_ptr<const Schema> resolve_schema(std::string_view name,
                                                 std::uint32_t version) const override {
        return name == "Moment" && version == 1 ? moment() : nullptr;
    }
    std::optional<Cell> resolve_ref(const Ref&, std::string* error) const override {
        *error = "no references here";
        return std::nullopt;
    }
};
} // namespace

TEST_CASE("the composer composes a union from one case, and a console describes one as cases") {
    const ComposeFrom src;
    const ShapeDesc d = describe_schema(*moment());
    CHECK(d.is_union);
    REQUIRE(d.fields.size() == 3);
    CHECK(d.fields[2].type == "nothing");
    Composition c = compose_message(src, "Moment", 1, {Arg{"button", FieldValue{std::int64_t{3}}}});
    REQUIRE(c.status == Composition::Status::Ready);
    Value v = assemble(c);
    CHECK(v.get("button")->as_int() == 3);
    CHECK(admit(std::move(v), *moment()).ok());
    Composition idle = compose_message(src, "Moment", 1, {Arg{std::nullopt, FieldValue{std::string("idle")}}});
    REQUIRE(idle.status == Composition::Status::Ready);
    CHECK(*assemble(idle).chosen() == 2);
    Composition two = compose_message(src, "Moment", 1,
                                      {Arg{"button", FieldValue{std::int64_t{3}}},
                                       Arg{std::nullopt, FieldValue{std::string("idle")}}});
    CHECK(two.status == Composition::Status::Error);
    CHECK(compose_message(src, "Moment", 1, {}).status == Composition::Status::NeedsInput);
    CHECK(compose_message(src, "Moment", 1, {Arg{"idle", FieldValue{true}}}).status ==
          Composition::Status::Error);
}

// ---- the bus ----------------------------------------------------------------------------------

TEST_CASE("the bus admits an open field against its registry, and a weave nests only a shape it "
          "may itself send") {
    using namespace sbfx;
    Switchboard bus;
    std::vector<BusEvent> events;
    bus.add_observer([&](const BusEvent& e) {
        BusEvent copy = e;
        copy.payload = nullptr;
        events.push_back(copy);
    });
    auto receiver = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{carry()});
    receiver->declared_emits = {moved()}; // what makes Moved a registered shape on this bus
    ProbeWeave* receiver_raw = receiver.get();
    const WeaveId receiver_id = bus.register_weave(std::move(receiver), Grant{}.allow_any());

    Value carried(carry());
    carried.set("label", Cell::text("ok")).set("item", Cell::message(moved_value(1, 2)));

    // A host send is not a weave's speech, and the gate still checks the open field.
    bus.send(receiver_id, Message(carried));
    Value stranger(carry());
    stranger.set("label", Cell::text("?"))
        .set("item", Cell::message(Value(SchemaBuilder("Unheard", 1).build())));
    bus.send(receiver_id, Message(stranger));
    bus.drain_until_idle();
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == EventKind::Delivered);
    CHECK(events[1].kind == EventKind::Refused);
    CHECK(events[1].refusal.reason == RefusalReason::GateRefused);
    CHECK(events[1].refusal.error.kind == ErrorKind::UnknownSchema);
    CHECK(receiver_raw->handled_names == std::vector<std::string>{"Carry"});

    // A weave that may send Carry and not Moved may not carry a Moved inside it.
    auto narrow = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()});
    narrow->on_handle = [&](const Message&, Bus& gated, ProbeWeave&) {
        gated.send(receiver_id, Message(carried));
    };
    const WeaveId narrow_id =
        bus.register_weave(std::move(narrow), Grant{}.allow_to_any("Carry", 1));
    auto wide = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{ping_schema()});
    wide->on_handle = [&](const Message&, Bus& gated, ProbeWeave&) {
        gated.send(receiver_id, Message(carried));
    };
    const WeaveId wide_id = bus.register_weave(
        std::move(wide), Grant{}.allow_to_any("Carry", 1).allow_to_any("Moved", 1));
    events.clear();
    bus.send(narrow_id, Message(ping(1)));
    bus.send(wide_id, Message(ping(2)));
    bus.drain_until_idle();
    // Both pings are delivered, then the two Carry sends their handlers queued, in order.
    REQUIRE(events.size() == 4);
    CHECK(events[2].kind == EventKind::Refused);
    CHECK(events[2].sender == narrow_id);
    CHECK(events[2].refusal.reason == RefusalReason::CapabilityDenied);
    CHECK(events[3].kind == EventKind::Delivered);
    CHECK(events[3].sender == wide_id);
    CHECK(receiver_raw->handled_names.size() == 2);
}

} // TEST_SUITE
