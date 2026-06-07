#include <doctest.h>

#include "fixtures.hpp"

#include <zen/serialize.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

using namespace zen;

namespace {

// A UTF-8 string with non-ASCII content, spelled in explicit bytes so the test
// does not depend on the source file's encoding: "héllo 世界 😀".
std::string utf8_sample() {
    return "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80";
}

// serialize (native binary) -> parse -> admit against the same schema. Requires
// success and returns the trusted, re-admitted value.
Value round_trip(const Value& v, std::shared_ptr<const Schema> door) {
    std::string bytes = serialize(v);
    Unverified u = parse(bytes);
    REQUIRE(u.well_formed());
    Admission a = admit(u, door);
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    return std::move(a).value();
}

// Canonical value-equality: two values are equal iff their native encodings are
// byte-identical (the format is canonical, so this is exact — and it treats all
// NaNs as equal, since encode normalizes them).
bool same_value(const Value& a, const Value& b) { return serialize(a) == serialize(b); }

} // namespace

TEST_SUITE("serialize") {

TEST_CASE("native output is binary with the ZN magic, not JSON") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    std::string bytes = serialize(v);
    REQUIRE(bytes.size() >= 3);
    CHECK(static_cast<unsigned char>(bytes[0]) == 0x5A); // 'Z'
    CHECK(static_cast<unsigned char>(bytes[1]) == 0x4E); // 'N'
    CHECK(static_cast<unsigned char>(bytes[2]) == 0x01); // format version
}

TEST_CASE("primitives round-trip losslessly, including large ints and special floats") {
    auto schema = SchemaBuilder("Prims", 1)
                      .field("i", Kind::Int)
                      .field("f", Kind::Float)
                      .field("t", Kind::Text)
                      .field("b", Kind::Bool)
                      .build();
    Value v(schema);
    const std::int64_t big = 9007199254740993LL; // 2^53 + 1
    v.set("i", Cell::integer(big));
    v.set("f", Cell::real(0.1));
    v.set("t", Cell::text(utf8_sample()));
    v.set("b", Cell::boolean(true));

    Value back = round_trip(v, schema);
    CHECK(back.get("i")->as_int() == big);
    CHECK(back.get("f")->as_float() == 0.1);
    CHECK(back.get("t")->as_text() == utf8_sample());
    CHECK(back.get("b")->as_bool() == true);
}

TEST_CASE("extreme ints round-trip") {
    auto schema = SchemaBuilder("I", 1).field("x", Kind::Int).build();
    for (std::int64_t n : {std::numeric_limits<std::int64_t>::min(),
                           std::numeric_limits<std::int64_t>::max(), std::int64_t{-1},
                           std::int64_t{0}, std::int64_t{1}}) {
        Value v(schema);
        v.set("x", Cell::integer(n));
        CHECK(round_trip(v, schema).get("x")->as_int() == n);
    }
}

TEST_CASE("NaN and infinities survive; signed zero is preserved and distinct") {
    auto schema = SchemaBuilder("F", 1).field("x", Kind::Float).build();
    auto rt = [&](double d) {
        Value v(schema);
        v.set("x", Cell::real(d));
        return round_trip(v, schema).get("x")->as_float();
    };
    CHECK(std::isnan(rt(std::numeric_limits<double>::quiet_NaN())));
    CHECK(rt(std::numeric_limits<double>::infinity()) == std::numeric_limits<double>::infinity());
    CHECK(rt(-std::numeric_limits<double>::infinity()) == -std::numeric_limits<double>::infinity());

    const double neg_zero = rt(-0.0);
    CHECK(neg_zero == 0.0);                      // compares equal to +0.0 ...
    CHECK(std::signbit(neg_zero));               // ... but the sign bit survived
    CHECK_FALSE(std::signbit(rt(0.0)));          // +0.0 stays +0.0
}

TEST_CASE("bytes round-trip raw (no base64 in native)") {
    Value v(fx::Blob());
    Bytes data;
    for (int i = 0; i < 256; ++i) {
        data.push_back(static_cast<std::uint8_t>(i));
    }
    v.set("data", Cell::bytes(data));
    Value back = round_trip(v, fx::Blob());
    CHECK(back.get("data")->as_bytes() == data);
}

TEST_CASE("lists, nested messages, and lists of messages round-trip") {
    Value v(fx::Inventory());
    v.set("owner", Cell::text("me"));
    v.set("items", Cell::list({Cell::text("sword"), Cell::text("shield")}));
    v.set("counts", Cell::list({Cell::integer(1), Cell::integer(2), Cell::integer(3)}));
    Value back = round_trip(v, fx::Inventory());
    CHECK(back.get("items")->as_list().size() == 2);
    CHECK(back.get("counts")->as_list()[2].as_int() == 3);

    Value leader(fx::PlayerState());
    leader.set("hp", Cell::integer(50)).set("name", Cell::text("Cap"));
    Value m1(fx::PlayerState());
    m1.set("hp", Cell::integer(10)).set("name", Cell::text("A"));
    Value squad(fx::Squad());
    squad.set("name", Cell::text("Alpha"));
    squad.set("leader", Cell::message(std::move(leader)));
    squad.set("members", Cell::list({Cell::message(std::move(m1))}));

    Value back2 = round_trip(squad, fx::Squad());
    CHECK(back2.get("leader")->as_message()->get("name")->as_text() == "Cap");
    CHECK(back2.get("members")->as_list()[0].as_message()->get("hp")->as_int() == 10);
}

TEST_CASE("the remaining fixtures round-trip too") {
    Value mv(fx::Move());
    mv.set("dx", Cell::real(1.5)).set("dy", Cell::real(-2.5));
    Value mv_back = round_trip(mv, fx::Move());
    CHECK(mv_back.get("dx")->as_float() == 1.5);
    CHECK(mv_back.get("dy")->as_float() == -2.5);

    Value sc(fx::SetColor());
    sc.set("r", Cell::integer(10)).set("g", Cell::integer(20)).set("b", Cell::integer(30));
    sc.set("named", Cell::boolean(false));
    Value sc_back = round_trip(sc, fx::SetColor());
    CHECK(sc_back.get("g")->as_int() == 20);
    CHECK(sc_back.get("named")->as_bool() == false);
}

TEST_CASE("optional-absent fields round-trip as absent") {
    Value g(fx::Greeting());
    g.set("to", Cell::text("world")); // 'note' optional, omitted
    Value back = round_trip(g, fx::Greeting());
    CHECK(back.has("to"));
    CHECK_FALSE(back.has("note"));
}

TEST_CASE("the native header carries a challengeable schema identity") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    Unverified u = parse(serialize(v));
    REQUIRE(u.well_formed());
    CHECK(u.claimed_name() == "PlayerState");
    CHECK(u.claimed_version() == 1);
}

// ---- Canonicality ---------------------------------------------------------

TEST_CASE("encoding is canonical: stable, twin-identical, and round-trip-stable") {
    Value a(fx::PlayerState());
    a.set("hp", Cell::integer(30)).set("name", Cell::text("Ami"));

    // Stable across calls.
    CHECK(serialize(a) == serialize(a));

    // Identical for a separately-built, structurally-equal value (and a twin schema).
    auto twin_schema =
        SchemaBuilder("PlayerState", 1).field("hp", Kind::Int).field("name", Kind::Text).build();
    Value b(twin_schema);
    b.set("hp", Cell::integer(30)).set("name", Cell::text("Ami"));
    CHECK(serialize(a) == serialize(b));

    // A decoded value re-serializes to the very same bytes (content-addressable).
    std::string bytes = serialize(a);
    Value back = round_trip(a, fx::PlayerState());
    CHECK(serialize(back) == bytes);
}

TEST_CASE("canonicality holds through nesting and lists") {
    auto build = [] {
        Value leader(fx::PlayerState());
        leader.set("hp", Cell::integer(7)).set("name", Cell::text("Cap"));
        Value squad(fx::Squad());
        squad.set("name", Cell::text("Alpha"));
        squad.set("leader", Cell::message(std::move(leader)));
        squad.set("members", Cell::list({}));
        return squad;
    };
    CHECK(serialize(build()) == serialize(build()));
    CHECK(serialize(round_trip(build(), fx::Squad())) == serialize(build()));
}

// ---- Cross-format equivalence ---------------------------------------------

TEST_CASE("native and compat decode to the same value") {
    Value v(fx::Squad());
    {
        Value leader(fx::PlayerState());
        leader.set("hp", Cell::integer(50)).set("name", Cell::text(utf8_sample()));
        Value m(fx::PlayerState());
        m.set("hp", Cell::integer(-3)).set("name", Cell::text("B"));
        v.set("name", Cell::text("Alpha"));
        v.set("leader", Cell::message(std::move(leader)));
        v.set("members", Cell::list({Cell::message(std::move(m))}));
    }

    Value via_binary = admit(parse(serialize(v)), fx::Squad()).value();
    Value via_json = admit(compat::parse(compat::serialize(v)), fx::Squad()).value();

    CHECK(same_value(via_binary, v));
    CHECK(same_value(via_json, v));
    CHECK(same_value(via_binary, via_json));
}

// ---- Strictness (native) --------------------------------------------------

TEST_CASE("content_id is mandatory in native: a header missing it is rejected") {
    auto empty = SchemaBuilder("Empty", 1).build(); // zero fields => empty body
    std::string bytes = serialize(Value(empty));
    // Drop the final byte so the 8-byte content id can no longer be read in full.
    bytes.pop_back();
    Unverified u = parse(bytes);
    CHECK_FALSE(u.well_formed());
    CHECK(admit(u, empty).first_error().kind == ErrorKind::MalformedBytes);
}

TEST_CASE("a mismatched content_id is SchemaMismatch") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    std::string bytes = serialize(v);
    // content_id sits after magic(3) + nameLen(2) + name(11) + schemaVersion(4) = offset 20.
    const std::size_t cid_off = 3 + 2 + std::string("PlayerState").size() + 4;
    REQUIRE(bytes.size() > cid_off);
    bytes[cid_off] = static_cast<char>(bytes[cid_off] ^ 0xFF);
    Admission a = admit(parse(bytes), fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::SchemaMismatch);
}

TEST_CASE("a Bool byte outside {0,1} is rejected") {
    auto schema = SchemaBuilder("B", 1).field("b", Kind::Bool).build();
    Value v(schema);
    v.set("b", Cell::boolean(true));
    std::string bytes = serialize(v); // the bool byte is the last byte
    bytes.back() = static_cast<char>(0x02);
    Admission a = admit(parse(bytes), schema);
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedField);
    CHECK(a.first_error().path == "b");
}

TEST_CASE("a non-minimal varint is rejected") {
    auto schema = SchemaBuilder("I", 1).field("n", Kind::Int).build();
    Value v(schema);
    v.set("n", Cell::integer(1)); // zigzag(1)=2 => single byte 0x02 at the end
    std::string bytes = serialize(v);
    bytes.pop_back();          // remove the 0x02
    bytes.push_back('\x82');   // 0x82: continuation, low bits = 2
    bytes.push_back('\x00');   // 0x00: terminator -> non-minimal encoding of 2
    Admission a = admit(parse(bytes), schema);
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedField);
    CHECK(a.first_error().path == "n");
}

TEST_CASE("trailing bytes after the value are rejected") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    std::string bytes = serialize(v);
    bytes.push_back('\x00'); // one junk byte past the value
    Admission a = admit(parse(bytes), fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedBytes);
}

TEST_CASE("a non-Zen byte string is refused, not crashed") {
    for (const char* junk : {"", "hello", "{\"zen\":1}", "ZN"}) {
        Unverified u = parse(junk);
        CHECK_FALSE(u.well_formed());
        CHECK_FALSE(admit(u, fx::PlayerState()).ok());
    }
}

TEST_CASE("the wrong door is refused even with valid bytes") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    Admission a = admit(parse(serialize(v)), fx::Move());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::SchemaMismatch);
}

TEST_CASE("a payload missing a required field is refused via the gate") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)); // no name
    Admission a = admit(parse(serialize(v)), fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MissingField);
    CHECK(a.first_error().path == "name");
}

TEST_CASE("resolving the claim against a registry (native)") {
    Registry reg;
    reg.register_schema(fx::PlayerState());
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(42)).set("name", Cell::text("Ami"));
    Unverified u = parse(serialize(v));
    CHECK(admit(u, reg).value().get("hp")->as_int() == 42);

    Registry empty;
    CHECK(admit(u, empty).first_error().kind == ErrorKind::UnknownSchema);
}

} // TEST_SUITE
