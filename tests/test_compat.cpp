#include <doctest.h>

#include "fixtures.hpp"

#include <zen/gate.hpp>
#include <zen/serialize.hpp>

#include <string>

using namespace zen;

namespace {

Value compat_round_trip(const Value& v, std::shared_ptr<const Schema> door) {
    std::string text = compat::serialize(v);
    Unverified u = compat::parse(text);
    REQUIRE(u.well_formed());
    Admission a = admit(u, door);
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    return std::move(a).value();
}

} // namespace

TEST_SUITE("compat") {

TEST_CASE("compat output is JSON text and round-trips through the same gate") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(30)).set("name", Cell::text("Ami"));
    std::string text = compat::serialize(v);
    CHECK(text.front() == '{');
    CHECK(text.find("\"schema\":\"PlayerState\"") != std::string::npos);

    Value back = compat_round_trip(v, fx::PlayerState());
    CHECK(back.get("hp")->as_int() == 30);
    CHECK(back.get("name")->as_text() == "Ami");
}

TEST_CASE("nested messages and lists round-trip in compat") {
    Value leader(fx::PlayerState());
    leader.set("hp", Cell::integer(50)).set("name", Cell::text("Cap"));
    Value squad(fx::Squad());
    squad.set("name", Cell::text("Alpha"));
    squad.set("leader", Cell::message(std::move(leader)));
    squad.set("members", Cell::list({}));
    Value back = compat_round_trip(squad, fx::Squad());
    CHECK(back.get("leader")->as_message()->get("name")->as_text() == "Cap");
}

TEST_CASE("content_id is optional in compat: a JSON envelope without it still admits") {
    std::string text =
        R"({"zen":1,"schema":"PlayerState","version":1,"fields":{"hp":"1","name":"Ami"}})";
    Admission a = admit(compat::parse(text), fx::PlayerState());
    REQUIRE(a.ok());
    CHECK(a.value().get("hp")->as_int() == 1);
}

// The strictness regression: this used to be silently dropped.
TEST_CASE("an unknown field in the payload is now rejected (UnknownField)") {
    std::string text = R"({"zen":1,"schema":"PlayerState","version":1,)"
                       R"("fields":{"hp":"1","name":"Ami","ghost":"x"}})";
    Unverified u = compat::parse(text);
    REQUIRE(u.well_formed());
    Admission a = admit(u, fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::UnknownField);
    CHECK(a.first_error().path == "ghost");
}

TEST_CASE("an unknown field inside a nested message is rejected at its path") {
    std::string text = R"({"zen":1,"schema":"Squad","version":1,"fields":{)"
                       R"("name":"Alpha","leader":{"hp":"1","name":"Cap","x":"y"},"members":[]}})";
    Unverified u = compat::parse(text);
    REQUIRE(u.well_formed());
    Admission a = admit(u, fx::Squad(), Report::Full);
    REQUIRE_FALSE(a.ok());
    bool found = false;
    for (const Error& e : a.errors()) {
        if (e.kind == ErrorKind::UnknownField && e.path == "leader.x") {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("an unknown envelope member is rejected") {
    std::string text = R"({"zen":1,"schema":"PlayerState","version":1,)"
                       R"("fields":{"hp":"1","name":"Ami"},"surprise":7})";
    Unverified u = compat::parse(text);
    CHECK_FALSE(u.well_formed());
    CHECK(admit(u, fx::PlayerState()).first_error().kind == ErrorKind::MalformedBytes);
}

TEST_CASE("a wrong-typed JSON field is refused with a precise diagnosis") {
    std::string text =
        R"({"zen":1,"schema":"PlayerState","version":1,"fields":{"hp":{},"name":"Ami"}})";
    Admission a = admit(compat::parse(text), fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().path == "hp");
    CHECK(a.first_error().kind == ErrorKind::TypeMismatch);
}

TEST_CASE("invalid UTF-8 in a JSON text field is refused") {
    std::string text =
        std::string(R"({"zen":1,"schema":"PlayerState","version":1,"fields":{"hp":"1","name":")") +
        "\x80" + R"("}})";
    Unverified u = compat::parse(text);
    REQUIRE(u.well_formed());
    Admission a = admit(u, fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedField);
    CHECK(a.first_error().path == "name");
}

TEST_CASE("non-JSON and structurally-wrong envelopes are refused, not crashed") {
    for (const char* bad : {"", "not json", "{", "[]", "{\"zen\":1}", "null", "123",
                            R"({"zen":2,"schema":"X","version":1,"fields":{}})"}) {
        Unverified u = compat::parse(bad);
        CHECK_FALSE(u.well_formed());
        CHECK(admit(u, fx::PlayerState()).first_error().kind == ErrorKind::MalformedBytes);
    }
}

TEST_CASE("compat path funnels through the one gate") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(7)).set("name", Cell::text("Ami"));
    Unverified u = compat::parse(compat::serialize(v));
    auto before = gate_invocations();
    Admission a = admit(u, fx::PlayerState());
    REQUIRE(a.ok());
    CHECK(gate_invocations() == before + 1);
}

} // TEST_SUITE
