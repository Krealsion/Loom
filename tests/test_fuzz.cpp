#include <doctest.h>

#include "fixtures.hpp"

#include <zen/gate.hpp>
#include <zen/serialize.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace zen;

// The security property: feeding malformed, random, or hostile bytes to a
// decoder must never crash, overread, leak, allocate beyond the input, or let an
// unverified value escape the gate. Any value that passes admit() must genuinely
// conform — re-checked with diagnose(). Run under ASan/UBSan, this also asserts
// no undefined behavior on any input.

namespace {

const std::vector<std::shared_ptr<const Schema>>& doors() {
    static const std::vector<std::shared_ptr<const Schema>> d = {
        fx::PlayerState(), fx::Squad(), fx::Inventory(), fx::Blob(), fx::Greeting()};
    return d;
}

void probe_native(const std::string& bytes, const std::shared_ptr<const Schema>& door) {
    Unverified u = parse(bytes); // noexcept; must not crash on anything
    Admission a = admit(u, door, Report::Full);
    if (a.ok()) {
        CHECK(diagnose(a.value(), *door).empty()); // nothing unverified escapes
    } else {
        CHECK_FALSE(a.errors().empty());
    }
}

void probe_compat(const std::string& bytes, const std::shared_ptr<const Schema>& door) {
    Unverified u = compat::parse(bytes);
    Admission a = admit(u, door, Report::Full);
    if (a.ok()) {
        CHECK(diagnose(a.value(), *door).empty());
    } else {
        CHECK_FALSE(a.errors().empty());
    }
}

// A valid native header for `s` (so the identity check passes and the body
// decoder is exercised on whatever bytes we append).
std::string native_header(const std::shared_ptr<const Schema>& s) {
    std::string h;
    h.push_back('\x5A');
    h.push_back('\x4E');
    h.push_back('\x01');
    const auto nlen = static_cast<std::uint16_t>(s->name().size());
    h.push_back(static_cast<char>(nlen & 0xFF));
    h.push_back(static_cast<char>((nlen >> 8) & 0xFF));
    h += s->name();
    const std::uint32_t v = s->version();
    for (int i = 0; i < 4; ++i) {
        h.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
    const std::uint64_t cid = s->content_id();
    for (int i = 0; i < 8; ++i) {
        h.push_back(static_cast<char>((cid >> (8 * i)) & 0xFF));
    }
    return h;
}

} // namespace

TEST_SUITE("fuzz") {

TEST_CASE("random byte soup never crashes the native decoder or escapes the gate") {
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int> len_dist(0, 512);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (int iter = 0; iter < 4000; ++iter) {
        std::string bytes;
        const int n = len_dist(rng);
        for (int i = 0; i < n; ++i) {
            bytes.push_back(static_cast<char>(byte_dist(rng)));
        }
        for (const auto& door : doors()) {
            probe_native(bytes, door);
        }
    }
}

TEST_CASE("valid-header + random body drives the body decoder on garbage") {
    std::mt19937_64 rng(0x5EED);
    std::uniform_int_distribution<int> len_dist(0, 256);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (int iter = 0; iter < 4000; ++iter) {
        for (const auto& door : doors()) {
            std::string bytes = native_header(door);
            const int n = len_dist(rng);
            for (int i = 0; i < n; ++i) {
                bytes.push_back(static_cast<char>(byte_dist(rng)));
            }
            probe_native(bytes, door);
        }
    }
}

TEST_CASE("bit-flipped and truncated valid native messages stay safe") {
    Value leader(fx::PlayerState());
    leader.set("hp", Cell::integer(50)).set("name", Cell::text("Cap"));
    Value m(fx::PlayerState());
    m.set("hp", Cell::integer(10)).set("name", Cell::text("A"));
    Value squad(fx::Squad());
    squad.set("name", Cell::text("Alpha"));
    squad.set("leader", Cell::message(std::move(leader)));
    squad.set("members", Cell::list({Cell::message(std::move(m))}));

    Value player(fx::PlayerState());
    player.set("hp", Cell::integer(7)).set("name", Cell::text("Ami"));

    Value inv(fx::Inventory());
    inv.set("owner", Cell::text("me"));
    inv.set("items", Cell::list({Cell::text("a"), Cell::text("b")}));
    inv.set("counts", Cell::list({Cell::integer(1), Cell::integer(2)}));

    const std::vector<std::pair<std::string, std::shared_ptr<const Schema>>> seeds = {
        {serialize(squad), fx::Squad()},
        {serialize(player), fx::PlayerState()},
        {serialize(inv), fx::Inventory()}};

    std::mt19937_64 rng(0xDEADBEEF);
    for (const auto& [seed, door] : seeds) {
        std::uniform_int_distribution<std::size_t> pos_dist(0, seed.size() - 1);
        std::uniform_int_distribution<int> byte_dist(0, 255);
        for (int iter = 0; iter < 6000; ++iter) {
            std::string mutated = seed;
            const int flips = 1 + (iter % 3);
            for (int k = 0; k < flips; ++k) {
                mutated[pos_dist(rng)] = static_cast<char>(byte_dist(rng));
            }
            if ((iter & 7) == 0) {
                mutated.resize(pos_dist(rng));
            }
            probe_native(mutated, door);
        }
    }
}

TEST_CASE("a length that lies beyond the input is rejected, not over-read") {
    // PlayerState body: mask 0x02 (only 'name' present), then a huge Text length.
    std::string bytes = native_header(fx::PlayerState());
    bytes.push_back('\x02');                  // presence: field 1 (name) present
    for (int i = 0; i < 4; ++i) {             // varint length ~ 0xFFFFFFF (huge)
        bytes.push_back('\xFF');
    }
    bytes.push_back('\x0F');
    Admission a = admit(parse(bytes), fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedField);
}

TEST_CASE("a list count beyond the cap is rejected, not allocated") {
    // Inventory body: mask 0x02 (only 'items' present), then an enormous count.
    std::string bytes = native_header(fx::Inventory());
    bytes.push_back('\x02');                  // presence: field 1 (items) present
    for (int i = 0; i < 9; ++i) {             // a ~64-bit varint count
        bytes.push_back('\xFF');
    }
    bytes.push_back('\x01');
    Admission a = admit(parse(bytes), fx::Inventory());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedField);
}

TEST_CASE("pathological binary nesting is bounded by the depth cap, not the stack") {
    // A deeply List-nested schema used as the door; a body of all-0x01 counts
    // would recurse forever but the depth cap stops it cleanly.
    TypeRef t = type_of(Kind::Int);
    for (int i = 0; i < 200; ++i) {
        t = type_list(t);
    }
    auto deep = make_schema("Deep", 1, {Field{"x", t, true}});

    std::string bytes = native_header(deep);
    bytes.push_back('\x01');           // presence: field 0 present
    for (int i = 0; i < 200; ++i) {    // count=1 at each level
        bytes.push_back('\x01');
    }
    Unverified u = parse(bytes);
    REQUIRE(u.well_formed());
    Admission a = admit(u, deep);
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedBytes);
}

TEST_CASE("native admit against a registry is equally safe on hostile input") {
    Registry reg;
    for (const auto& d : doors()) {
        reg.register_schema(d);
    }
    std::mt19937_64 rng(0xABCDEF);
    std::uniform_int_distribution<int> len_dist(0, 256);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (int iter = 0; iter < 4000; ++iter) {
        std::string bytes;
        const int n = len_dist(rng);
        for (int i = 0; i < n; ++i) {
            bytes.push_back(static_cast<char>(byte_dist(rng)));
        }
        Unverified u = parse(bytes);
        Admission a = admit(u, reg, Report::Full);
        if (a.ok()) {
            auto door = reg.lookup(u.claimed_name(), u.claimed_version());
            REQUIRE(door != nullptr);
            CHECK(diagnose(a.value(), *door).empty());
        }
    }
}

// ---- Compat (JSON) decoder fuzz -------------------------------------------

TEST_CASE("random and JSON-shaped soup never crashes the compat decoder") {
    static const std::string alphabet =
        "{}[]\":,0123456789.-+eEtruefalsn \t\n\\/uABCDEF zen schema version fields hp name items";
    std::mt19937_64 rng(0x1234567);
    std::uniform_int_distribution<int> len_dist(0, 400);
    std::uniform_int_distribution<std::size_t> ch_dist(0, alphabet.size() - 1);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    for (int iter = 0; iter < 4000; ++iter) {
        std::string bytes;
        const int n = len_dist(rng);
        const bool jsonish = (iter % 2) == 0;
        for (int i = 0; i < n; ++i) {
            bytes.push_back(jsonish ? alphabet[ch_dist(rng)]
                                    : static_cast<char>(byte_dist(rng)));
        }
        for (const auto& door : doors()) {
            probe_compat(bytes, door);
        }
    }
}

TEST_CASE("pathologically deep JSON is rejected, not stack-overflowed (compat)") {
    for (int depth : {64, 200, 5000, 100000}) {
        std::string deep(static_cast<std::size_t>(depth), '[');
        Unverified u = compat::parse(deep);
        CHECK_FALSE(u.well_formed());
        CHECK(admit(u, fx::PlayerState()).first_error().kind == ErrorKind::MalformedBytes);
    }
}

} // TEST_SUITE
