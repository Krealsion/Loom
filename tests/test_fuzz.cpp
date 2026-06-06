#include <doctest.h>

#include "fixtures.hpp"

#include <zen/serialize.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace zen;

// The security property: feeding malformed, random, or hostile bytes to the
// deserializer must never crash, never leak, and never let an unverified value
// escape the gate. Any value that passes admit() must genuinely conform — which
// we re-check with diagnose(). Run under ASan/UBSan, this also asserts no
// undefined behavior on any input.

namespace {

// A door zoo: primitives, lists, nested messages, optionals.
const std::vector<std::shared_ptr<const Schema>>& doors() {
    static const std::vector<std::shared_ptr<const Schema>> d = {
        fx::PlayerState(), fx::Squad(), fx::Inventory(), fx::Blob(), fx::Greeting()};
    return d;
}

// Assert the one true invariant for a single (bytes, door) attempt: no crash,
// and if admitted, the value genuinely conforms.
void probe(const std::string& bytes, const std::shared_ptr<const Schema>& door) {
    Unverified u = parse(bytes); // noexcept; must not crash on anything
    Admission a = admit(u, door, Report::Full);
    if (a.ok()) {
        // Nothing unverified may escape: a passed value must re-validate clean.
        CHECK(diagnose(a.value(), *door).empty());
    } else {
        // A refusal must carry at least one machine-readable reason.
        CHECK_FALSE(a.errors().empty());
    }
}

} // namespace

TEST_SUITE("fuzz") {

TEST_CASE("random byte soup never crashes and never escapes the gate") {
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int> len_dist(0, 512);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    for (int iter = 0; iter < 4000; ++iter) {
        std::string bytes;
        const int n = len_dist(rng);
        bytes.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            bytes.push_back(static_cast<char>(byte_dist(rng)));
        }
        for (const auto& door : doors()) {
            probe(bytes, door);
        }
    }
}

TEST_CASE("random JSON-alphabet soup exercises the parser more deeply") {
    // Restrict to characters that look like JSON so the parser gets further in
    // before failing, reaching the decoders with odd-but-plausible structures.
    static const std::string alphabet =
        "{}[]\":,0123456789.-+eEtruefalsn \t\n\\/uABCDEF abc hp name items data to";
    std::mt19937_64 rng(0x1234567);
    std::uniform_int_distribution<int> len_dist(0, 400);
    std::uniform_int_distribution<std::size_t> ch_dist(0, alphabet.size() - 1);

    for (int iter = 0; iter < 4000; ++iter) {
        std::string bytes;
        const int n = len_dist(rng);
        for (int i = 0; i < n; ++i) {
            bytes.push_back(alphabet[ch_dist(rng)]);
        }
        for (const auto& door : doors()) {
            probe(bytes, door);
        }
    }
}

TEST_CASE("bit-flipped and truncated valid messages stay safe") {
    // Start from genuinely valid serializations and corrupt them.
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

    const std::vector<std::string> seeds = {serialize(squad), serialize(player)};

    std::mt19937_64 rng(0xDEADBEEF);
    for (const std::string& seed : seeds) {
        std::uniform_int_distribution<std::size_t> pos_dist(0, seed.size() - 1);
        std::uniform_int_distribution<int> byte_dist(0, 255);
        for (int iter = 0; iter < 6000; ++iter) {
            std::string mutated = seed;
            // Flip one to three bytes.
            const int flips = 1 + (iter % 3);
            for (int k = 0; k < flips; ++k) {
                mutated[pos_dist(rng)] = static_cast<char>(byte_dist(rng));
            }
            // Sometimes truncate.
            if ((iter & 7) == 0 && !mutated.empty()) {
                mutated.resize(pos_dist(rng));
            }
            for (const auto& door : doors()) {
                probe(mutated, door);
            }
        }
    }
}

TEST_CASE("pathologically deep nesting is rejected, not stack-overflowed") {
    for (int depth : {64, 200, 5000, 100000}) {
        std::string deep(static_cast<std::size_t>(depth), '[');
        Unverified u = parse(deep);
        CHECK_FALSE(u.well_formed());
        Admission a = admit(u, fx::PlayerState());
        CHECK_FALSE(a.ok());
        CHECK(a.first_error().kind == ErrorKind::MalformedBytes);
    }
    // Deeply nested *valid* objects must also be bounded by the depth guard.
    std::string nested = "{\"zen\":1,\"schema\":\"X\",\"version\":1,\"fields\":";
    for (int i = 0; i < 1000; ++i) {
        nested += "[";
    }
    Unverified u = parse(nested);
    CHECK_FALSE(u.well_formed());
}

TEST_CASE("admitting against a registry is equally safe on hostile input") {
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
            // It must conform to whatever it claimed (now resolved via registry).
            auto door = reg.lookup(u.claimed_name(), u.claimed_version());
            REQUIRE(door != nullptr);
            CHECK(diagnose(a.value(), *door).empty());
        }
    }
}

} // TEST_SUITE
