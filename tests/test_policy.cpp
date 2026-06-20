#include <doctest.h>

#include "shardlib/storage_protocol.hpp"
#include "switchboard_fixtures.hpp"

#include <zen/isolation/grant_record.hpp>
#include <zen/isolation/host.hpp>
#include <zen/switchboard.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
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

zen::Bytes bytes_of(const std::string& s) { return zen::Bytes(s.begin(), s.end()); }

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

TEST_CASE("scoping: each mod reads only its own data; B can never read A's (negative control)") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Network) ||
        !host.enforcement().enforceable(Capability::Filesystem) ||
        !host.enforcement().enforceable(Capability::Resources)) {
        WARN("the floor/broker are not fully OS-enforceable here; skipping the scoping proof");
        return;
    }
    const std::string root = "/tmp/zen_storage_scoping";
    std::filesystem::remove_all(root);

    // The broker holds the only disk capability (WriteScoped(root)); the two mods are
    // FsAccess::None and hold only the floor's storage role send-rules.
    OutOfProcessResult broker = host.mount_broker("broker", ZEN_SO_STORAGE_BROKER, root);
    REQUIRE_MESSAGE(broker.ok, broker.error);
    OutOfProcessResult a = host.mount_mod("modA", ZEN_SO_STORAGE_CLIENT);
    OutOfProcessResult b = host.mount_mod("modB", ZEN_SO_STORAGE_CLIENT);
    REQUIRE_MESSAGE(a.ok, a.error);
    REQUIRE_MESSAGE(b.ok, b.error);

    // Read each StorageValue reply off the bus tap, keyed by the mod it was delivered to.
    std::map<std::uint64_t, std::string> got;
    bus.add_observer([&got](const BusEvent& e) {
        if (e.kind == EventKind::Delivered && e.schema_name == "StorageValue" &&
            e.payload != nullptr) {
            const zen::Bytes& v = e.payload->get("value")->as_bytes();
            got[e.target.value] = std::string(v.begin(), v.end());
        }
    });

    // Both mods write the SAME key "save" with different secrets, then read it back.
    bus.send(a.id, Message(zen::author::to_value(storage::DoPut{"save", bytes_of("secretA")})));
    bus.send(a.id, Message(zen::author::to_value(storage::DoGet{"save"})));
    bus.send(b.id, Message(zen::author::to_value(storage::DoPut{"save", bytes_of("secretB")})));
    bus.send(b.id, Message(zen::author::to_value(storage::DoGet{"save"})));
    REQUIRE(host.run_until([&] { return got.count(a.id.value) && got.count(b.id.value); }, 4000));

    CHECK(got[a.id.value] == "secretA"); // A reads its own
    CHECK(got[b.id.value] == "secretB"); // B reads its own
    // The negative control: same key "save", yet B's value is its own — never A's. The
    // scoping is by the unforgeable stamped sender, not incidental.
    CHECK(got[b.id.value] != "secretA");

    std::filesystem::remove_all(root);
}

TEST_CASE("floor-without-disk: a None mod persists via the broker, but a direct open fails (syscall)") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Network) ||
        !host.enforcement().enforceable(Capability::Filesystem) ||
        !host.enforcement().enforceable(Capability::Resources)) {
        WARN("not fully OS-enforceable here; skipping the floor-without-disk proof");
        return;
    }
    const std::string root = "/tmp/zen_storage_floor";
    std::filesystem::remove_all(root);
    OutOfProcessResult broker = host.mount_broker("broker", ZEN_SO_STORAGE_BROKER, root);
    REQUIRE_MESSAGE(broker.ok, broker.error);
    OutOfProcessResult mod = host.mount_mod("mod", ZEN_SO_STORAGE_CLIENT);
    REQUIRE_MESSAGE(mod.ok, mod.error);

    // The mod is FsAccess::None — OS-enforced, no filesystem of its own.
    CHECK(host.containment("mod").find("filesystem: contained at level none") != std::string::npos);

    std::string probe;
    bool have = false;
    bus.add_observer([&](const BusEvent& e) {
        if (e.kind == EventKind::Delivered && e.schema_name == "StorageValue" &&
            e.payload != nullptr && e.target == mod.id) {
            const zen::Bytes& v = e.payload->get("value")->as_bytes();
            probe = std::string(v.begin(), v.end());
            have = true;
        }
    });
    // Probe: the mod attempts a DIRECT file open (must fail at the syscall level) and
    // carries the errno back THROUGH the broker — proving "no disk of my own" and
    // "persists via messages alone" in one round-trip.
    bus.send(mod.id, Message(zen::author::to_value(storage::Probe{1})));
    bus.send(mod.id, Message(zen::author::to_value(storage::DoGet{"__probe__"})));
    REQUIRE(host.run_until([&] { return have; }, 4000));

    REQUIRE_FALSE(probe.empty());      // the broker round-trip succeeded (persisted via messages)
    CHECK(std::stol(probe) != 0);      // the direct open failed at the syscall level (no disk)

    std::filesystem::remove_all(root);
}

TEST_CASE("reload-keeps-state: stored data survives a broker implementation reload; mods still route") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Network) ||
        !host.enforcement().enforceable(Capability::Filesystem) ||
        !host.enforcement().enforceable(Capability::Resources)) {
        WARN("not fully OS-enforceable here; skipping the reload-keeps-state proof");
        return;
    }
    const std::string root = "/tmp/zen_storage_reload";
    std::filesystem::remove_all(root);
    OutOfProcessResult broker = host.mount_broker("broker", ZEN_SO_STORAGE_BROKER, root);
    REQUIRE_MESSAGE(broker.ok, broker.error);
    OutOfProcessResult mod = host.mount_mod("mod", ZEN_SO_STORAGE_CLIENT);
    REQUIRE_MESSAGE(mod.ok, mod.error);

    std::string got;
    bool have = false;
    bus.add_observer([&](const BusEvent& e) {
        if (e.kind == EventKind::Delivered && e.schema_name == "StorageValue" &&
            e.payload != nullptr && e.target == mod.id) {
            const zen::Bytes& v = e.payload->get("value")->as_bytes();
            got = std::string(v.begin(), v.end());
            have = true;
        }
    });

    bus.send(mod.id, Message(zen::author::to_value(storage::DoPut{"k", bytes_of("persisted")})));
    bus.send(mod.id, Message(zen::author::to_value(storage::DoGet{"k"})));
    REQUIRE(host.run_until([&] { return have; }, 4000));
    REQUIRE(got == "persisted");

    // Reload the broker's implementation in place: same ShardId, role, grant; fresh
    // child; the on-disk data is durable.
    REQUIRE(host.reload("broker"));
    CHECK(host.is_mounted("broker"));

    // The mod (same id, same role send-rule) reads the SAME key again — still its data,
    // still correctly scoped, routed by role to the reloaded broker.
    have = false;
    got.clear();
    bus.send(mod.id, Message(zen::author::to_value(storage::DoGet{"k"})));
    REQUIRE(host.run_until([&] { return have; }, 4000));
    CHECK(got == "persisted");

    std::filesystem::remove_all(root);
}

TEST_CASE("broker-down degrades gracefully: a mod's storage send is NoSuchTarget, the mod stays None") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    if (!host.enforcement().enforceable(Capability::Network) ||
        !host.enforcement().enforceable(Capability::Filesystem) ||
        !host.enforcement().enforceable(Capability::Resources)) {
        WARN("not fully OS-enforceable here; skipping the broker-down check");
        return;
    }
    // Mount then UNMOUNT the broker, so the storage role has no live holder (the
    // crashed/quarantined/unmounted case). The storage schemas stay registered, so the
    // role-send still reaches delivery — and degrades there, not silently.
    const std::string root = "/tmp/zen_storage_down";
    std::filesystem::remove_all(root);
    OutOfProcessResult broker = host.mount_broker("broker", ZEN_SO_STORAGE_BROKER, root);
    REQUIRE_MESSAGE(broker.ok, broker.error);
    OutOfProcessResult mod = host.mount_mod("mod", ZEN_SO_STORAGE_CLIENT);
    REQUIRE_MESSAGE(mod.ok, mod.error);
    host.unmount("broker"); // the storage role now has no holder

    bool refused = false;
    bus.add_observer([&refused](const BusEvent& e) {
        if (e.kind == EventKind::Refused && e.schema_name == "StoragePut" &&
            e.refusal.reason == RefusalReason::NoSuchTarget) {
            refused = true;
        }
    });
    bus.send(mod.id, Message(zen::author::to_value(storage::DoPut{"k", bytes_of("x")})));
    REQUIRE(host.run_until([&] { return refused; }, 2000));
    // The mod's authorized storage send is simply undelivered — storage is *unavailable*,
    // not a disk leak. The mod is still FsAccess::None.
    CHECK(host.containment("mod").find("filesystem: contained at level none") != std::string::npos);
    std::filesystem::remove_all(root);
}

} // TEST_SUITE
