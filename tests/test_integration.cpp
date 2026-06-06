#include <doctest.h>

#include "fixtures.hpp"

#include <zen/zen.hpp>

#include <optional>
#include <string>

using namespace zen;

// The three proof-of-concept scenes, reproduced as assertions over the public
// API only. No domain type lives in the library; every schema here is a test
// fixture handed to the kernel as data.

TEST_SUITE("integration") {

// ---------------------------------------------------------------------------
// Scene A: a message that knows what it is.
// ---------------------------------------------------------------------------
TEST_CASE("A: well-formed admitted; missing field refused; wrong claim refused") {
    // A well-formed Move crosses the bus.
    Value step(fx::Move());
    step.set("dx", Cell::real(1.0)).set("dy", Cell::real(-2.0));
    CHECK(admit(std::move(step), *fx::Move()).ok());

    // A Move that lost a field is refused.
    Value broken(fx::Move());
    broken.set("dx", Cell::real(1.0));
    Admission b = admit(std::move(broken), *fx::Move());
    REQUIRE_FALSE(b.ok());
    CHECK(b.first_error().kind == ErrorKind::MissingField);

    // A Spawn offered to a door that admits only Move is refused.
    Value liar(fx::Spawn());
    liar.set("kind", Cell::text("dragon"));
    Admission l = admit(std::move(liar), *fx::Move());
    REQUIRE_FALSE(l.ok());
    CHECK(l.first_error().kind == ErrorKind::SchemaMismatch);
}

// ---------------------------------------------------------------------------
// Scene B: the console builds a message for a Shard it has never seen.
// ---------------------------------------------------------------------------
TEST_CASE("B: a runtime-discovered schema is blind-constructed and admitted") {
    // The schema "arrives from a DLL": nothing was compiled to know it. We learn
    // it only through the registry, then build a value by walking its shape.
    Registry reg;
    reg.register_schema(fx::SetColor());

    std::shared_ptr<const Schema> discovered = reg.lookup("SetColor", 1);
    REQUIRE(discovered != nullptr);

    // The console has no compiled SetColor type; it reads fields off the shape.
    auto console_source = [](const Field& f) -> std::optional<Cell> {
        switch (f.type.kind) {
        case Kind::Int:
            return Cell::integer(128);
        case Kind::Bool:
            return Cell::boolean(true);
        default:
            return std::nullopt;
        }
    };
    Value built = construct_blind(discovered, console_source);

    Admission a = admit(std::move(built), *discovered);
    REQUIRE(a.ok());
    CHECK(a.value().get("r")->as_int() == 128);
    CHECK(a.value().get("named")->as_bool() == true);
}

// ---------------------------------------------------------------------------
// Scene C: the self sets the lock its own return must open.
// ---------------------------------------------------------------------------
namespace {

struct Shard {
    std::string name;
    std::shared_ptr<const Schema> state_schema; // the lock the self chose
    Value policy;                               // self-declared; kernel-checked
    Value last_known_good;                      // the safe self to revert to
    std::int64_t reloads_used = 0;
};

// Revive from a *serialized* candidate: it counts only if it round-trips through
// bytes and passes the gate against the self-chosen lock. The kernel never
// understands what the state means — it only judges shape.
bool revive_from_bytes(Shard& s, const std::string& candidate_bytes,
                       const Schema& policy_door) {
    // The policy is itself validated as a value against a schema the library does
    // not hard-code.
    REQUIRE(admit(Value(s.policy), policy_door).ok());
    const bool revive_lkg = s.policy.get("revive_from_last_good")->as_bool();
    const std::int64_t max = s.policy.get("max_reloads")->as_int();
    if (s.reloads_used >= max) {
        return false;
    }

    Unverified u = parse(candidate_bytes);
    Admission a = admit(u, s.state_schema); // the self-set lock, on persisted bytes
    if (a.ok()) {
        s.last_known_good = std::move(a).value();
        ++s.reloads_used;
        return true;
    }
    if (revive_lkg) {
        ++s.reloads_used; // wake as the last-known-good self
        return true;
    }
    return false;
}

} // namespace

TEST_CASE("C: a Shard is revived only through the same gate persistence uses") {
    // Value has no default constructor (it always carries a schema), so the
    // Shard is built whole, with its lock and selves already shaped.
    Shard ami{"Ami", fx::PlayerState(), Value(fx::ReloadPolicy()), Value(fx::PlayerState()), 0};
    ami.policy.set("max_reloads", Cell::integer(2));
    ami.policy.set("revive_from_last_good", Cell::boolean(true));
    ami.last_known_good.set("hp", Cell::integer(20)).set("name", Cell::text("Ami"));

    // The kernel validates the policy the self declared (shape only).
    CHECK(admit(Value(ami.policy), *fx::ReloadPolicy()).ok());

    // Death 1 — a clean save returns, having round-tripped through bytes.
    Value good(fx::PlayerState());
    good.set("hp", Cell::integer(30)).set("name", Cell::text("Ami"));
    CHECK(revive_from_bytes(ami, serialize(good), *fx::ReloadPolicy()));
    CHECK(ami.last_known_good.get("hp")->as_int() == 30);
    CHECK(ami.reloads_used == 1);

    // Death 2 — a corrupted save tries to return. It still CLAIMS PlayerState,
    // but hp is text, not a number. The gate cannot fix it, only refuse it.
    Value corrupt(fx::PlayerState());
    corrupt.set("name", Cell::text("Ami"));
    std::string corrupt_bytes = serialize(corrupt); // hp absent on the wire
    // Confirm directly that no trusted value escapes the gate.
    CHECK_FALSE(admit(parse(corrupt_bytes), fx::PlayerState()).ok());
    // The Shard falls back to last-known-good (the self permitted it).
    CHECK(revive_from_bytes(ami, corrupt_bytes, *fx::ReloadPolicy()));
    CHECK(ami.last_known_good.get("hp")->as_int() == 30); // unchanged: still the good self
    CHECK(ami.reloads_used == 2);

    // The grammar is open: a wholly different policy shape passes the identical
    // machinery, with no change to the kernel.
    Value eph(fx::EphemeralPolicy());
    eph.set("wipe_on_reload", Cell::boolean(true));
    CHECK(admit(std::move(eph), *fx::EphemeralPolicy()).ok());
}

// ---------------------------------------------------------------------------
// One gate, every boundary: the bus path and the persistence path reach the
// very same structural validator.
// ---------------------------------------------------------------------------
TEST_CASE("one gate: live admit and deserialized admit both invoke the single validator") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(7)).set("name", Cell::text("Ami"));
    std::string bytes = serialize(v);

    // Live (bus) path.
    auto before_live = gate_invocations();
    Admission live = admit(std::move(v), *fx::PlayerState());
    REQUIRE(live.ok());
    CHECK(gate_invocations() == before_live + 1);

    // Persistence path — same counter advances, proving the shared validator.
    auto before_persist = gate_invocations();
    Admission persisted = admit(parse(bytes), fx::PlayerState());
    REQUIRE(persisted.ok());
    CHECK(gate_invocations() == before_persist + 1);
}

} // TEST_SUITE
