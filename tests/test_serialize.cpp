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

// Serialize -> bytes -> parse -> admit against the same schema. Requires success
// and returns the trusted, re-admitted value.
Value round_trip(const Value& v, std::shared_ptr<const Schema> door) {
    std::string bytes = serialize(v);
    Unverified u = parse(bytes);
    REQUIRE(u.well_formed());
    Admission a = admit(u, door);
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    return std::move(a).value();
}

} // namespace

TEST_SUITE("serialize") {

TEST_CASE("primitives round-trip losslessly, including large ints and special floats") {
    auto schema = SchemaBuilder("Prims", 1)
                      .field("i", Kind::Int)
                      .field("f", Kind::Float)
                      .field("t", Kind::Text)
                      .field("b", Kind::Bool)
                      .build();
    Value v(schema);
    const std::int64_t big = 9007199254740993LL; // 2^53 + 1, lost by a JSON double
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

TEST_CASE("NaN and infinities survive the wire") {
    auto schema = SchemaBuilder("F", 1).field("x", Kind::Float).build();

    Value nan(schema);
    nan.set("x", Cell::real(std::numeric_limits<double>::quiet_NaN()));
    CHECK(std::isnan(round_trip(nan, schema).get("x")->as_float()));

    Value pinf(schema);
    pinf.set("x", Cell::real(std::numeric_limits<double>::infinity()));
    CHECK(round_trip(pinf, schema).get("x")->as_float() == std::numeric_limits<double>::infinity());

    Value ninf(schema);
    ninf.set("x", Cell::real(-std::numeric_limits<double>::infinity()));
    CHECK(round_trip(ninf, schema).get("x")->as_float() == -std::numeric_limits<double>::infinity());
}

TEST_CASE("bytes round-trip through base64") {
    Value v(fx::Blob());
    Bytes data;
    for (int i = 0; i < 256; ++i) {
        data.push_back(static_cast<std::uint8_t>(i));
    }
    v.set("data", Cell::bytes(data));
    Value back = round_trip(v, fx::Blob());
    CHECK(back.get("data")->as_bytes() == data);
}

TEST_CASE("lists of primitives round-trip") {
    Value v(fx::Inventory());
    v.set("owner", Cell::text("me"));
    v.set("items", Cell::list({Cell::text("sword"), Cell::text("shield")}));
    v.set("counts", Cell::list({Cell::integer(1), Cell::integer(2), Cell::integer(3)}));
    Value back = round_trip(v, fx::Inventory());
    REQUIRE(back.get("items")->as_list().size() == 2);
    CHECK(back.get("items")->as_list()[1].as_text() == "shield");
    CHECK(back.get("counts")->as_list()[2].as_int() == 3);
}

TEST_CASE("nested messages and lists of messages round-trip") {
    Value leader(fx::PlayerState());
    leader.set("hp", Cell::integer(50)).set("name", Cell::text("Cap"));
    Value m1(fx::PlayerState());
    m1.set("hp", Cell::integer(10)).set("name", Cell::text("A"));

    Value squad(fx::Squad());
    squad.set("name", Cell::text("Alpha"));
    squad.set("leader", Cell::message(std::move(leader)));
    squad.set("members", Cell::list({Cell::message(std::move(m1))}));

    Value back = round_trip(squad, fx::Squad());
    CHECK(back.get("leader")->as_message()->get("name")->as_text() == "Cap");
    REQUIRE(back.get("members")->as_list().size() == 1);
    CHECK(back.get("members")->as_list()[0].as_message()->get("hp")->as_int() == 10);
}

TEST_CASE("the serialized form carries a challengeable schema identity") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    std::string bytes = serialize(v);
    Unverified u = parse(bytes);
    CHECK(u.well_formed());
    CHECK(u.claimed_name() == "PlayerState");
    CHECK(u.claimed_version() == 1);
}

TEST_CASE("deserialized bytes are refused at the wrong door") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    Unverified u = parse(serialize(v));
    Admission a = admit(u, fx::Move()); // different schema
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::SchemaMismatch);
}

TEST_CASE("same name+version but a different shape is caught by content id") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    Unverified u = parse(serialize(v));
    // An impostor door with the same key but a different shape.
    auto impostor = SchemaBuilder("PlayerState", 1).field("hp", Kind::Float).build();
    Admission a = admit(u, impostor);
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::SchemaMismatch);
}

TEST_CASE("a payload missing a required field is refused after the round trip") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)); // no name
    Unverified u = parse(serialize(v));
    Admission a = admit(u, fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MissingField);
    CHECK(a.first_error().path == "name");
}

TEST_CASE("resolving the claim against a registry") {
    Registry reg;
    reg.register_schema(fx::PlayerState());

    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(42)).set("name", Cell::text("Ami"));
    Unverified u = parse(serialize(v));

    Admission ok = admit(u, reg);
    REQUIRE(ok.ok());
    CHECK(ok.value().get("hp")->as_int() == 42);

    // A registry that has never heard of this schema refuses cleanly.
    Registry empty;
    Admission unknown = admit(u, empty);
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.first_error().kind == ErrorKind::UnknownSchema);
}

TEST_CASE("non-JSON and structurally-wrong envelopes are refused, not crashed") {
    for (const char* bad : {"", "not json", "{", "[]", "{\"zen\":1}", "null", "123",
                            "{\"zen\":2,\"schema\":\"X\",\"version\":1,\"fields\":{}}"}) {
        Unverified u = parse(bad);
        CHECK_FALSE(u.well_formed());
        Admission a = admit(u, fx::PlayerState());
        CHECK_FALSE(a.ok());
        CHECK(a.first_error().kind == ErrorKind::MalformedBytes);
    }
}

TEST_CASE("a field with corrupted bytes is refused with a precise diagnosis") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    std::string good = serialize(v);
    // Tamper the real bytes: turn hp's integer-string "1" into a JSON object.
    const std::string needle = "\"hp\":\"1\"";
    auto pos = good.find(needle);
    REQUIRE(pos != std::string::npos);
    good.replace(pos, needle.size(), "\"hp\":{}");

    Unverified tampered = parse(good);
    REQUIRE(tampered.well_formed());
    Admission a = admit(tampered, fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().path == "hp");
    CHECK(a.first_error().kind == ErrorKind::TypeMismatch);
}

TEST_CASE("a malformed-UTF-8 text field is refused after the round trip") {
    // Build bytes by hand with an invalid UTF-8 byte inside a Text field.
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    std::string good = serialize(v);
    // Replace name's "Ami" with a lone continuation byte (invalid UTF-8) via \u? No —
    // inject a raw 0x80 byte through a JSON \u escape that decodes to a bad sequence is
    // not possible; instead replace the quoted value directly with a raw bad byte.
    const std::string needle = "\"name\":\"Ami\"";
    auto pos = good.find(needle);
    REQUIRE(pos != std::string::npos);
    good.replace(pos, needle.size(), std::string("\"name\":\"\x80\""));
    Unverified u = parse(good);
    // The raw control/continuation byte 0x80 is >= 0x20, so JSON parsing accepts it;
    // the Text decoder then rejects it as invalid UTF-8.
    REQUIRE(u.well_formed());
    Admission a = admit(u, fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedField);
    CHECK(a.first_error().path == "name");
}

} // TEST_SUITE
