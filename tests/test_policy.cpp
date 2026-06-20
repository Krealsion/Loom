#include <doctest.h>

#include "shardlib/storage_protocol.hpp"
#include "switchboard_fixtures.hpp"

#include <zen/isolation/grant_record.hpp>
#include <zen/isolation/host.hpp>
#include <zen/switchboard.hpp>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// The policy phase (P1). Part A's core hooks, proven on their own:
//   - role-addressing: a send may name a *role* (a stable capability slot) instead
//     of a ShardId; the role resolves to its singleton holder at delivery, and a
//     grant of "shape -> role" is a distinct authority from "shape -> ShardId".
// Part B's StorageBroker proofs (scoping, floor-without-disk, reload-keeps-state)
// build on top of these and land in this same suite.

using namespace sbfx;
using namespace zen::isolation;
using zen::sb::Disposition;
using zen::sb::Ticket;

namespace {

const std::string kHostExe = ZEN_SHARD_HOST_EXE;

// Register a ProbeShard bound to a role (a singleton capability slot), via the
// role-binding register_shard overload. Like register_probe otherwise.
Registered register_probe_role(Switchboard& bus,
                               std::vector<std::shared_ptr<const Schema>> accept, std::string role,
                               Grant grant = Grant{}.allow_any()) {
    auto owned = std::make_unique<ProbeShard>(std::move(accept));
    ProbeShard* raw = owned.get();
    ShardId id = bus.register_shard(std::move(owned), std::move(grant), std::move(role));
    return {id, raw};
}

bool tap_has(const std::vector<TapRecord>& tap, EventKind kind, RefusalReason reason,
             const std::string& schema) {
    for (const auto& r : tap) {
        if (r.kind == kind && r.reason == reason && r.schema == schema) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_SUITE("policy") {

// ---- Role-addressing (Part A) --------------------------------------------

TEST_CASE("a send addressed to a role reaches the role's holder, authorized by a role rule") {
    Switchboard bus;
    Registered storage = register_probe_role(bus, {ping_schema()}, "storage");
    // The sender accepts Tick; on Tick it sends a Ping to role "storage". Its grant
    // permits exactly that — Ping -> role "storage" — never a ShardId.
    Registered sender =
        register_probe(bus, {tick_schema()}, 2, true, Grant{}.allow_to_role("Ping", 1, "storage"));
    sender.shard->on_handle = [](const Message&, Bus& b, ProbeShard&) {
        b.send_to_role("storage", Message(ping(5)));
    };
    bus.send(sender.id, Message(tick(1))); // host trigger (ungated root authority)
    bus.pump();
    REQUIRE(storage.shard->handled_names.size() == 1);
    CHECK(storage.shard->handled_names[0] == "Ping");
    CHECK(storage.shard->handled_values[0] == 5);
}

TEST_CASE("a role send the grant does not permit is denied, even when the role is held") {
    Switchboard bus;
    Registered storage = register_probe_role(bus, {ping_schema()}, "storage");
    Registered sender = register_probe(bus, {tick_schema()}, 2, true, Grant::nothing());
    sender.shard->on_handle = [](const Message&, Bus& b, ProbeShard&) {
        b.send_to_role("storage", Message(ping(5)));
    };
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });
    bus.send(sender.id, Message(tick(1)));
    bus.pump();
    CHECK(storage.shard->handled_names.empty()); // never delivered
    CHECK(tap_has(tap, EventKind::Refused, RefusalReason::CapabilityDenied, "Ping"));
}

TEST_CASE("authorization precedes resolution: an unauthorized sender cannot probe a role's holder") {
    Switchboard bus;
    // No "storage" holder is registered. An unauthorized sender's role-send is
    // refused CapabilityDenied (authorization, decided first) — NOT NoSuchTarget —
    // so it cannot even learn whether the role is currently held.
    Registered sender = register_probe(bus, {tick_schema()}, 2, true, Grant::nothing());
    sender.shard->on_handle = [](const Message&, Bus& b, ProbeShard&) {
        b.send_to_role("storage", Message(ping(1)));
    };
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });
    bus.send(sender.id, Message(tick(1)));
    bus.pump();
    CHECK(tap_has(tap, EventKind::Refused, RefusalReason::CapabilityDenied, "Ping"));
    CHECK_FALSE(tap_has(tap, EventKind::Refused, RefusalReason::NoSuchTarget, "Ping"));
}

TEST_CASE("a ShardId grant does not authorize a role send (the role wall is its own wall)") {
    Switchboard bus;
    Registered storage = register_probe_role(bus, {ping_schema()}, "storage");
    // Granted Ping -> the holder's *ShardId* directly. A role-addressed send to the
    // very same holder is still denied: a direct-id rule never authorizes a role send.
    Registered sender =
        register_probe(bus, {tick_schema()}, 2, true, Grant{}.allow("Ping", 1, storage.id));
    sender.shard->on_handle = [](const Message&, Bus& b, ProbeShard&) {
        b.send_to_role("storage", Message(ping(5)));
    };
    bus.send(sender.id, Message(tick(1)));
    bus.pump();
    CHECK(storage.shard->handled_names.empty());
}

TEST_CASE("a role rule does not authorize a direct ShardId send to the same holder") {
    Switchboard bus;
    Registered storage = register_probe_role(bus, {ping_schema()}, "storage");
    Registered sender =
        register_probe(bus, {tick_schema()}, 2, true, Grant{}.allow_to_role("Ping", 1, "storage"));
    const ShardId holder = storage.id;
    sender.shard->on_handle = [holder](const Message&, Bus& b, ProbeShard&) {
        b.send(holder, Message(ping(5))); // direct to the resolved id, bypassing the role
    };
    bus.send(sender.id, Message(tick(1)));
    bus.pump();
    CHECK(storage.shard->handled_names.empty());
}

TEST_CASE("a role with no live holder degrades to NoSuchTarget — unavailable, not a hole") {
    Switchboard bus;
    // No "storage" holder registered; a (host, ungated) role-send simply finds none.
    Ticket t = bus.send_to_role("storage", Message(ping(1)));
    bus.pump();
    CHECK(bus.outcome(t).disposition == Disposition::Refused);
    CHECK(bus.outcome(t).refusal.reason == RefusalReason::NoSuchTarget);
}

TEST_CASE("a role binding survives the holder reloading; the role rule keeps routing") {
    Switchboard bus;
    Registered storage = register_probe_role(bus, {ping_schema()}, "storage");
    Registered sender =
        register_probe(bus, {tick_schema()}, 2, true, Grant{}.allow_to_role("Ping", 1, "storage"));
    sender.shard->on_handle = [](const Message&, Bus& b, ProbeShard&) {
        b.send_to_role("storage", Message(ping(7)));
    };
    bus.send(sender.id, Message(tick(1)));
    bus.pump();
    REQUIRE(storage.shard->handled_names.size() == 1);

    // Hot-swap the holder's implementation: same ShardId, same role binding.
    const std::string saved = bus.snapshot_bytes(storage.id);
    bus.swap_state(storage.id, saved);
    CHECK(bus.alive(storage.id));

    bus.send(sender.id, Message(tick(2)));
    bus.pump();
    CHECK(storage.shard->handled_names.size() == 2); // still routed after the reload
}

TEST_CASE("a role is a singleton in this phase: binding a role already held is refused") {
    Switchboard bus;
    Registered first = register_probe_role(bus, {ping_schema()}, "storage");
    (void)first;
    CHECK_THROWS_AS(register_probe_role(bus, {ping_schema()}, "storage"), std::invalid_argument);
}

// ---- The ask, the floor, and the host's pen (Part A) ----------------------

TEST_CASE("the grant-record persists deltas across reload, keyed by content-hash") {
    const std::string path = "/tmp/zen_grant_record_unit.json";
    std::remove(path.c_str());
    const std::string id = "deadbeefcafef00d";
    {
        GrantRecord rec;
        rec.load(path); // missing file -> empty: everyone floors
        CHECK_FALSE(rec.lookup(id).network);
        rec.record(id, GrantDelta{/*network=*/true, /*filesystem=*/"read-only"}); // persists
    }
    {
        GrantRecord rec; // a fresh record, loaded from the same file on disk
        rec.load(path);
        const GrantDelta d = rec.lookup(id);
        CHECK(d.network);
        CHECK(d.filesystem == "read-only");
        CHECK_FALSE(rec.lookup("an-unknown-id").network); // an unknown identity -> the floor
    }
    std::remove(path.c_str());
}

TEST_CASE("ask-is-not-a-grant: a mod that asks for the world still lands on the floor") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Network) ||
        !host.enforcement().enforceable(Capability::Filesystem) ||
        !host.enforcement().enforceable(Capability::Resources)) {
        WARN("the floor is not fully OS-enforceable here; skipping the on-the-floor check");
        return;
    }
    // The host's grant-record is empty: no delta exists for this mod.
    OutOfProcessResult r = host.mount_mod("greedy", ZEN_SO_MOD_STORAGE);
    REQUIRE_MESSAGE(r.ok, r.error);

    // The host READ the ask — it surfaced network and filesystem write...
    auto ask = host.declared_ask("greedy");
    REQUIRE(ask.has_value());
    CHECK(ask->network);
    CHECK(ask->filesystem == "write-scoped");

    // ...and still the floor holds: the ask granted nothing it asked for.
    CHECK(host.containment("greedy").find("network: contained") != std::string::npos);
    CHECK(host.containment("greedy").find("filesystem: contained") != std::string::npos);
}

TEST_CASE("the host holds the pen: a recorded delta — not the ask — raises a mod above the floor") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Network) ||
        !host.enforcement().enforceable(Capability::Filesystem) ||
        !host.enforcement().enforceable(Capability::Resources)) {
        WARN("the floor is not fully OS-enforceable here; skipping the delta-grant check");
        return;
    }
    const std::string path = "/tmp/zen_grant_record_pen.json";
    std::remove(path.c_str());
    host.set_grant_record_path(path);
    // The host writes a network delta for this mod's identity — the pen, not the ask.
    host.record_grant_delta(so_content_hash(ZEN_SO_MOD_STORAGE), GrantDelta{/*network=*/true, ""});

    OutOfProcessResult r = host.mount_mod("blessed", ZEN_SO_MOD_STORAGE);
    REQUIRE_MESSAGE(r.ok, r.error);
    // The SAME mod, the SAME ask as above — now network is GRANTED, because the host
    // recorded a delta. The declaration never changed; the host's pen did.
    CHECK(host.containment("blessed").find("network: granted") != std::string::npos);
    std::remove(path.c_str());
}

// ---- Part B: the StorageBroker (the powerbox, end to end) -----------------

TEST_CASE("out-of-process role-send reaches the role holder, sender stamped (kEmitToRole seam)") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Network) ||
        !host.enforcement().enforceable(Capability::Filesystem) ||
        !host.enforcement().enforceable(Capability::Resources)) {
        WARN("the floor is not fully OS-enforceable here; skipping the kEmitToRole seam check");
        return;
    }
    // An in-process Shard holds the "storage" role and accepts StoragePut — a stand-in
    // for the broker; this stage proves only that the wire seam carries a role-send to
    // its holder with the sender stamped host-side.
    Registered holder =
        register_probe_role(bus, {zen::author::schema_of<storage::StoragePut>()}, "storage");
    zen::sb::ShardId got_sender{};
    holder.shard->on_handle = [&](const Message& in, Bus&, ProbeShard&) { got_sender = in.sender; };

    // Mount the storage-client mod out-of-process on the floor (FsAccess::None).
    OutOfProcessResult mod = host.mount_mod("client", ZEN_SO_STORAGE_CLIENT);
    REQUIRE_MESSAGE(mod.ok, mod.error);

    // Drive a DoPut -> the mod's handler calls Mail::send_to_role("storage", StoragePut).
    bus.send(mod.id, Message(zen::author::to_value(storage::DoPut{"save", zen::Bytes{'x'}})));
    REQUIRE(host.run_until([&] { return !holder.shard->handled_names.empty(); }, 2000));

    CHECK(holder.shard->handled_names.back() == "StoragePut");
    CHECK(got_sender == mod.id); // stamped from the connection (link.id), never the wire
}

} // TEST_SUITE
