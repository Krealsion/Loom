#include <doctest.h>

#include "enforcement_gate.hpp"
#include "weavelib/net_protocol.hpp"
#include "weavelib/storage_protocol.hpp"
#include "switchboard_fixtures.hpp"

#include <zen/isolation/grant_record.hpp>
#include <zen/isolation/host.hpp>
#include <zen/switchboard.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// The policy phase (P1). Part A's core hooks, proven on their own:
//   - role-addressing: a send may name a *role* (a stable capability slot) instead
//     of a WeaveId; the role resolves to its singleton holder at delivery, and a
//     grant of "shape -> role" is a distinct authority from "shape -> WeaveId".
// Part B's StorageBroker proofs (scoping, floor-without-disk, reload-keeps-state)
// build on top of these and land in this same suite.

using namespace sbfx;
using namespace loom;
using loom::Disposition;
using loom::Ticket;

namespace {

const std::string kHostExe = ZEN_WEAVE_HOST_EXE;

loom::Bytes bytes_of(const std::string& s) { return loom::Bytes(s.begin(), s.end()); }

// A tiny loopback TCP echo listener on an ephemeral port, in a background thread (the
// host netns — so the granted broker can reach it, while a netns'd mod cannot). For the
// NetworkBroker mediation proof. Best-effort: port() is 0 if setup failed.
class LoopbackEcho {
public:
    LoopbackEcho() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        int one = 1;
        (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return;
        }
        port_ = ntohs(addr.sin_port);
        if (::listen(fd_, 8) != 0) {
            port_ = 0;
            return;
        }
        thread_ = std::thread([this] { run(); });
    }
    ~LoopbackEcho() {
        stop_.store(true);
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    LoopbackEcho(const LoopbackEcho&) = delete;
    LoopbackEcho& operator=(const LoopbackEcho&) = delete;

    std::int64_t port() const { return port_; }

private:
    void run() {
        while (!stop_.load()) {
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) {
                break; // shutdown/close makes accept fail -> exit the loop
            }
            std::uint8_t buf[4096];
            const ssize_t r = ::recv(c, buf, sizeof(buf), 0);
            if (r > 0) {
                (void)::send(c, buf, static_cast<std::size_t>(r), MSG_NOSIGNAL); // echo
            }
            ::close(c);
        }
    }
    int fd_ = -1;
    std::int64_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

// Register a ProbeWeave bound to a role (a singleton capability slot), via the
// role-binding register_weave overload. Like register_probe otherwise.
Registered register_probe_role(Switchboard& bus,
                               std::vector<std::shared_ptr<const Schema>> accept, std::string role,
                               Grant grant = Grant{}.allow_any()) {
    auto owned = std::make_unique<ProbeWeave>(std::move(accept));
    ProbeWeave* raw = owned.get();
    WeaveId id = bus.register_weave(std::move(owned), std::move(grant), std::move(role));
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
    // permits exactly that — Ping -> role "storage" — never a WeaveId.
    Registered sender =
        register_probe(bus, {tick_schema()}, 2, true, Grant{}.allow_to_role("Ping", 1, "storage"));
    sender.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        b.send_to_role("storage", Message(ping(5)));
    };
    bus.send(sender.id, Message(tick(1))); // host trigger (ungated root authority)
    bus.pump();
    REQUIRE(storage.weave->handled_names.size() == 1);
    CHECK(storage.weave->handled_names[0] == "Ping");
    CHECK(storage.weave->handled_values[0] == 5);
}

TEST_CASE("a role send the grant does not permit is denied, even when the role is held") {
    Switchboard bus;
    Registered storage = register_probe_role(bus, {ping_schema()}, "storage");
    Registered sender = register_probe(bus, {tick_schema()}, 2, true, Grant::nothing());
    sender.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        b.send_to_role("storage", Message(ping(5)));
    };
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });
    bus.send(sender.id, Message(tick(1)));
    bus.pump();
    CHECK(storage.weave->handled_names.empty()); // never delivered
    CHECK(tap_has(tap, EventKind::Refused, RefusalReason::CapabilityDenied, "Ping"));
}

TEST_CASE("authorization precedes resolution: an unauthorized sender cannot probe a role's holder") {
    Switchboard bus;
    // No "storage" holder is registered. An unauthorized sender's role-send is
    // refused CapabilityDenied (authorization, decided first) — NOT NoSuchTarget —
    // so it cannot even learn whether the role is currently held.
    Registered sender = register_probe(bus, {tick_schema()}, 2, true, Grant::nothing());
    sender.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        b.send_to_role("storage", Message(ping(1)));
    };
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });
    bus.send(sender.id, Message(tick(1)));
    bus.pump();
    CHECK(tap_has(tap, EventKind::Refused, RefusalReason::CapabilityDenied, "Ping"));
    CHECK_FALSE(tap_has(tap, EventKind::Refused, RefusalReason::NoSuchTarget, "Ping"));
}

TEST_CASE("a WeaveId grant does not authorize a role send (the role wall is its own wall)") {
    Switchboard bus;
    Registered storage = register_probe_role(bus, {ping_schema()}, "storage");
    // Granted Ping -> the holder's *WeaveId* directly. A role-addressed send to the
    // very same holder is still denied: a direct-id rule never authorizes a role send.
    Registered sender =
        register_probe(bus, {tick_schema()}, 2, true, Grant{}.allow("Ping", 1, storage.id));
    sender.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        b.send_to_role("storage", Message(ping(5)));
    };
    bus.send(sender.id, Message(tick(1)));
    bus.pump();
    CHECK(storage.weave->handled_names.empty());
}

TEST_CASE("a role rule does not authorize a direct WeaveId send to the same holder") {
    Switchboard bus;
    Registered storage = register_probe_role(bus, {ping_schema()}, "storage");
    Registered sender =
        register_probe(bus, {tick_schema()}, 2, true, Grant{}.allow_to_role("Ping", 1, "storage"));
    const WeaveId holder = storage.id;
    sender.weave->on_handle = [holder](const Message&, Bus& b, ProbeWeave&) {
        b.send(holder, Message(ping(5))); // direct to the resolved id, bypassing the role
    };
    bus.send(sender.id, Message(tick(1)));
    bus.pump();
    CHECK(storage.weave->handled_names.empty());
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
    sender.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        b.send_to_role("storage", Message(ping(7)));
    };
    bus.send(sender.id, Message(tick(1)));
    bus.pump();
    REQUIRE(storage.weave->handled_names.size() == 1);

    // Hot-swap the holder's implementation: same WeaveId, same role binding.
    const std::string saved = bus.snapshot_bytes(storage.id);
    bus.swap_state(storage.id, saved);
    CHECK(bus.alive(storage.id));

    bus.send(sender.id, Message(tick(2)));
    bus.pump();
    CHECK(storage.weave->handled_names.size() == 2); // still routed after the reload
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

TEST_CASE("grant-record perms: a pre-planted looser-perm temp does not leak into the ledger (N-2)") {
    // Audit N-2: write_file_synced opens the temp with 0600, but open(O_CREAT,0600) IGNORES the
    // mode when the temp already EXISTS, and rename preserves the source mode — so a pre-planted
    // world-writable `<path>.tmp` would make this TCB ledger world-writable after the next
    // persist() (the re-test observed 0666). The fix fchmods the open fd to 0600 regardless of a
    // pre-existing file. Pin it: plant a 0666 temp, persist, confirm the live ledger is
    // owner-only (never group/other writable) and the temp did not leak.
    namespace fs = std::filesystem;
    const std::string path = "/tmp/zen_grant_record_perms_n2.json";
    const std::string tmp = path + ".tmp";
    std::remove(path.c_str());
    std::remove(tmp.c_str());

    { std::ofstream o(tmp); o << "stale-attacker-planted"; } // pre-plant the temp...
    fs::permissions(tmp,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                        fs::perms::group_write | fs::perms::others_read | fs::perms::others_write,
                    fs::perm_options::replace); // ...as 0666, explicitly (no umask masking)
    REQUIRE((fs::status(tmp).permissions() & fs::perms::others_write) != fs::perms::none);

    {
        GrantRecord rec;
        rec.load(path);
        rec.record("deadbeefcafef00d", GrantDelta{/*network=*/true, /*filesystem=*/""}); // persists
    }

    const fs::perms p = fs::status(path).permissions();
    CHECK((p & fs::perms::owner_read) != fs::perms::none);
    CHECK((p & fs::perms::owner_write) != fs::perms::none);
    CHECK((p & fs::perms::group_write) == fs::perms::none);  // NOT group-writable
    CHECK((p & fs::perms::others_write) == fs::perms::none); // NOT world-writable (the N-2 hole)
    CHECK((p & fs::perms::others_read) == fs::perms::none);  // 0600, not the umask-masked 0644
    CHECK_FALSE(fs::exists(tmp));                            // temp renamed away, no leak

    std::remove(path.c_str());
}

TEST_CASE("so_content_hash is a truncated SHA-256 (NIST vectors); a content change changes the key") {
    // Audit F-1: the grant key is now a cryptographic digest (SHA-256 truncated to 128
    // bits), not FNV-1a — because this key ALONE decides a mod's authority above the
    // floor, and FNV's ~2^32 birthday resistance was cheap to forge a second build onto
    // an existing grant. Pin it to FIPS 180-4 known-answer vectors so the in-tree
    // SHA-256 is proven correct, not merely trusted, then confirm the security property:
    // any content change changes the key (a rebuilt mod re-floors — the honest default).
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "zen_f1_kat";
    fs::create_directories(dir);
    const auto hash_of = [&](const std::string& name, const std::string& content) {
        const fs::path p = dir / name;
        {
            std::ofstream o(p, std::ios::binary | std::ios::trunc);
            o.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        return so_content_hash(p.string());
    };

    // FIPS 180-4 example messages, SHA-256 truncated to the first 128 bits (32 hex).
    CHECK(hash_of("abc", "abc") == "ba7816bf8f01cfea414140de5dae2223");
    CHECK(hash_of("two_block", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039"); // spans the pad boundary: exercises 2 blocks

    // The defining property: one flipped byte yields a different key.
    const std::string a = hash_of("v1", "the-mods-bytes-and-then-some-v1");
    const std::string b = hash_of("v2", "the-mods-bytes-and-then-some-v2");
    CHECK(a != b);
    CHECK(a.size() == 32); // 128 bits, lowercase hex

    fs::remove_all(dir);
}

TEST_CASE("ask-is-not-a-grant: a mod that asks for the world still lands on the floor") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(),
                            ZEN_FLOOR_CAPS,
                            "ask-is-not-a-grant: a woven mod's ask still lands on the floor");
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
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(),
                            ZEN_FLOOR_CAPS,
                            "host-holds-the-pen: authority above the floor comes from a recorded delta grant");
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
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(),
                            ZEN_FLOOR_CAPS,
                            "the out-of-process role-send (kEmitToRole) seam reaches the role holder");
    // An in-process Weave holds the "storage" role and accepts StoragePut — a stand-in
    // for the broker; this stage proves only that the wire seam carries a role-send to
    // its holder with the sender stamped host-side.
    Registered holder =
        register_probe_role(bus, {loom::schema_of<storage::StoragePut>()}, "storage");
    loom::WeaveId got_sender{};
    holder.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) { got_sender = in.sender; };

    // Mount the storage-client mod out-of-process on the floor (FsAccess::None).
    OutOfProcessResult mod = host.mount_mod("client", ZEN_SO_STORAGE_CLIENT);
    REQUIRE_MESSAGE(mod.ok, mod.error);

    // Drive a DoPut -> the mod's handler calls Mail::send_to_role("storage", StoragePut).
    bus.send(mod.id, Message(loom::to_value(storage::DoPut{"save", loom::Bytes{'x'}})));
    REQUIRE(host.run_until([&] { return !holder.weave->handled_names.empty(); }, 2000));

    CHECK(holder.weave->handled_names.back() == "StoragePut");
    CHECK(got_sender == mod.id); // stamped from the connection (link.id), never the wire
}

TEST_CASE("a forged role-send reply_to cannot redirect a broker's reply (confused-deputy closed)") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(), ZEN_FLOOR_CAPS,
                            "a forged role-send reply_to cannot redirect a broker's reply");
    const std::string root = "/tmp/zen_storage_forge";
    std::filesystem::remove_all(root);
    OutOfProcessResult broker = host.mount_broker("broker", ZEN_SO_STORAGE_BROKER, root);
    REQUIRE_MESSAGE(broker.ok, broker.error);
    // A MALICIOUS floored mod: on DoForge it bypasses Mail and emits a role-send StorageGet whose
    // wire reply_to is forged to point at the victim below.
    OutOfProcessResult attacker = host.mount_mod("attacker", ZEN_SO_FORGE_CLIENT);
    REQUIRE_MESSAGE(attacker.ok, attacker.error);

    // An in-process VICTIM that WOULD accept a StorageValue — the forged target. If the host
    // honored the wire reply_to, the broker's reply would land HERE; it must not.
    Registered victim = register_probe(bus, {loom::schema_of<storage::StorageValue>()});

    std::int64_t delivered_to = -1;
    bus.add_observer([&](const BusEvent& e) {
        if (e.kind == EventKind::Delivered && e.schema_name == "StorageValue") {
            delivered_to = static_cast<std::int64_t>(e.target.value);
        }
    });

    // The attacker forges StorageGet{reply_to = victim}. The broker replies StorageValue to its
    // in_.reply_to, which the host forced to the STAMPED sender (the attacker), not the wire value.
    bus.send(attacker.id, Message(loom::to_value(
                              storage::DoForge{"k", static_cast<std::int64_t>(victim.id.value)})));
    REQUIRE(host.run_until([&] { return delivered_to != -1; }, 4000));

    CHECK(delivered_to == static_cast<std::int64_t>(attacker.id.value)); // -> requester (stamped sender)
    CHECK(delivered_to != static_cast<std::int64_t>(victim.id.value));   // never the forged target
    CHECK(victim.weave->handled_names.empty());                          // the victim got nothing
    std::filesystem::remove_all(root);
}

TEST_CASE("scoping: each mod reads only its own data; B can never read A's (negative control)") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(),
                            ZEN_FLOOR_CAPS,
                            "storage keyspace scoping by stamped sender (mod-vs-mod negative control)");
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
            const loom::Bytes& v = e.payload->get("value")->as_bytes();
            got[e.target.value] = std::string(v.begin(), v.end());
        }
    });

    // Both mods write the SAME key "save" with different secrets, then read it back.
    bus.send(a.id, Message(loom::to_value(storage::DoPut{"save", bytes_of("secretA")})));
    bus.send(a.id, Message(loom::to_value(storage::DoGet{"save"})));
    bus.send(b.id, Message(loom::to_value(storage::DoPut{"save", bytes_of("secretB")})));
    bus.send(b.id, Message(loom::to_value(storage::DoGet{"save"})));
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
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(),
                            ZEN_FLOOR_CAPS,
                            "floor-without-disk: a floored mod persists only via the broker");
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
            const loom::Bytes& v = e.payload->get("value")->as_bytes();
            probe = std::string(v.begin(), v.end());
            have = true;
        }
    });
    // Probe: the mod attempts a DIRECT file open (must fail at the syscall level) and
    // carries the errno back THROUGH the broker — proving "no disk of my own" and
    // "persists via messages alone" in one round-trip.
    bus.send(mod.id, Message(loom::to_value(storage::Probe{1})));
    bus.send(mod.id, Message(loom::to_value(storage::DoGet{"__probe__"})));
    REQUIRE(host.run_until([&] { return have; }, 4000));

    REQUIRE_FALSE(probe.empty());      // the broker round-trip succeeded (persisted via messages)
    CHECK(std::stol(probe) != 0);      // the direct open failed at the syscall level (no disk)

    std::filesystem::remove_all(root);
}

TEST_CASE("reload-keeps-state: stored data survives a broker implementation reload; mods still route") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(),
                            ZEN_FLOOR_CAPS,
                            "reload keeps persisted state (the broker outlives a mod reload)");
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
            const loom::Bytes& v = e.payload->get("value")->as_bytes();
            got = std::string(v.begin(), v.end());
            have = true;
        }
    });

    bus.send(mod.id, Message(loom::to_value(storage::DoPut{"k", bytes_of("persisted")})));
    bus.send(mod.id, Message(loom::to_value(storage::DoGet{"k"})));
    REQUIRE(host.run_until([&] { return have; }, 4000));
    REQUIRE(got == "persisted");

    // Reload the broker's implementation in place: same WeaveId, role, grant; fresh
    // child; the on-disk data is durable.
    REQUIRE(host.reload("broker"));
    CHECK(host.is_mounted("broker"));

    // The mod (same id, same role send-rule) reads the SAME key again — still its data,
    // still correctly scoped, routed by role to the reloaded broker.
    have = false;
    got.clear();
    bus.send(mod.id, Message(loom::to_value(storage::DoGet{"k"})));
    REQUIRE(host.run_until([&] { return have; }, 4000));
    CHECK(got == "persisted");

    std::filesystem::remove_all(root);
}

TEST_CASE("broker-down degrades gracefully: a mod's storage send is NoSuchTarget, the mod stays None") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(),
                            ZEN_FLOOR_CAPS,
                            "broker-down degrades gracefully (NoSuchTarget, not a crash)");
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
    bus.send(mod.id, Message(loom::to_value(storage::DoPut{"k", bytes_of("x")})));
    REQUIRE(host.run_until([&] { return refused; }, 2000));
    // The mod's authorized storage send is simply undelivered — storage is *unavailable*,
    // not a disk leak. The mod is still FsAccess::None.
    CHECK(host.containment("mod").find("filesystem: contained at level none") != std::string::npos);
    std::filesystem::remove_all(root);
}

// ---- P2: the NetworkBroker (the powerbox generalized to a second capability) ----

TEST_CASE("floor denies net: a mod (even one that asks) cannot reach role net without a delta") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(),
                            ZEN_FLOOR_CAPS,
                            "the floor denies net (net is a recorded delta, granted to none)");
    // The broker is present (so NetRequest is a known schema), but a pure-floor mod has no
    // net role-rule — unlike storage, the floor grants the net role to no one.
    OutOfProcessResult broker = host.mount_net_broker("net", ZEN_SO_NET_BROKER);
    REQUIRE_MESSAGE(broker.ok, broker.error);
    OutOfProcessResult mod = host.mount_mod("nc", ZEN_SO_NET_CLIENT); // no delta -> pure floor
    REQUIRE_MESSAGE(mod.ok, mod.error);

    // The host READ the mod's ask (it requested network) — advice, surfaced...
    auto ask = host.declared_ask("nc");
    REQUIRE(ask.has_value());
    CHECK(ask->network);

    bool denied = false;
    bus.add_observer([&denied](const BusEvent& e) {
        if (e.kind == EventKind::Refused && e.schema_name == "NetRequest" &&
            e.refusal.reason == RefusalReason::CapabilityDenied) {
            denied = true;
        }
    });
    bus.send(mod.id, Message(loom::to_value(net::DoNet{"127.0.0.1", 9})));
    REQUIRE(host.run_until([&] { return denied; }, 2000));
    // ...yet the floor holds: CapabilityDenied to role net (ask is not a grant), and the
    // mod is OS-network-denied — net is a deliberate delta, not the floor.
    CHECK(host.containment("nc").find("network: contained") != std::string::npos);
}

TEST_CASE("mediation + negative control: a net-denied mod reaches the allowed host only via the broker") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(),
                            ZEN_FLOOR_CAPS,
                            "net mediation: a net-denied mod reaches loopback only via the broker");
    LoopbackEcho echo;
    REQUIRE(echo.port() > 0);

    OutOfProcessResult broker = host.mount_net_broker("net", ZEN_SO_NET_BROKER);
    REQUIRE_MESSAGE(broker.ok, broker.error);

    // Record a net delta for the client — the net ROLE only, NOT os_cap::Network.
    const std::string rec = "/tmp/zen_net_grant.json";
    std::remove(rec.c_str());
    host.set_grant_record_path(rec);
    GrantDelta delta;
    delta.roles = {"net"};
    host.record_grant_delta(so_content_hash(ZEN_SO_NET_CLIENT), delta);
    OutOfProcessResult mod = host.mount_mod("client", ZEN_SO_NET_CLIENT);
    REQUIRE_MESSAGE(mod.ok, mod.error);
    // The delta granted the role, not the OS capability: the mod is still network-DENIED.
    CHECK(host.containment("client").find("network: contained") != std::string::npos);

    bool ok = false;
    std::string echoed;
    bool got = false;
    bus.add_observer([&](const BusEvent& e) {
        if (e.kind == EventKind::Delivered && e.schema_name == "NetResponse" &&
            e.payload != nullptr && e.target == mod.id) {
            ok = e.payload->get("ok")->as_bool();
            const loom::Bytes& d = e.payload->get("data")->as_bytes();
            echoed = std::string(d.begin(), d.end());
            got = true;
        }
    });
    bus.send(mod.id, Message(loom::to_value(net::DoNet{"127.0.0.1", echo.port()})));
    REQUIRE(host.run_until([&] { return got; }, 5000));

    CHECK(ok); // reached the allowed loopback listener THROUGH the broker
    // The echoed bytes are the mod's OWN direct-connect errno (carried via the broker's
    // echo): nonzero proves the mod's direct connect failed at the syscall level
    // (ENETUNREACH — the B3 netns denial). Useful via the broker, powerless directly.
    REQUIRE_FALSE(echoed.empty());
    CHECK(std::stol(echoed) != 0);
    std::remove(rec.c_str());
}

TEST_CASE("allow-list scoping: the broker refuses a disallowed destination and never connects") {
    Switchboard bus;
    IsolationHost host(bus, kHostExe);
    ZEN_REQUIRE_ENFORCEABLE(host.enforcement(),
                            ZEN_FLOOR_CAPS,
                            "net allow-list scoping (the broker validates host:port)");
    OutOfProcessResult broker = host.mount_net_broker("net", ZEN_SO_NET_BROKER);
    REQUIRE_MESSAGE(broker.ok, broker.error);
    const std::string rec = "/tmp/zen_net_scope_grant.json";
    std::remove(rec.c_str());
    host.set_grant_record_path(rec);
    GrantDelta delta;
    delta.roles = {"net"};
    host.record_grant_delta(so_content_hash(ZEN_SO_NET_CLIENT), delta);
    OutOfProcessResult mod = host.mount_mod("client", ZEN_SO_NET_CLIENT);
    REQUIRE_MESSAGE(mod.ok, mod.error);

    bool ok = true;
    bool got = false;
    bus.add_observer([&](const BusEvent& e) {
        if (e.kind == EventKind::Delivered && e.schema_name == "NetResponse" &&
            e.payload != nullptr && e.target == mod.id) {
            ok = e.payload->get("ok")->as_bool();
            got = true;
        }
    });
    // 203.0.113.0/24 is TEST-NET-3 (documentation/unroutable) — not on the broker's
    // loopback-only allow-list. A fast ok=false is itself evidence the broker refused
    // BEFORE connecting (a passthrough would stall on the unroutable address).
    bus.send(mod.id, Message(loom::to_value(net::DoNet{"203.0.113.1", 80})));
    REQUIRE(host.run_until([&] { return got; }, 5000));
    CHECK_FALSE(ok); // refused by the broker's allow-list; the connection is never made
    std::remove(rec.c_str());
}

// Keep this LAST: a positive tally so a green policy run can never mean "every floor proof silently
// skipped." (Default doctest registration order; a --order-by=rand run would assert in a reporter.)
TEST_CASE("enforcement coverage: the floor proofs actually executed, not silently skipped") {
    if (!zenh::require_enforcement_strict() || zenh::degraded_run()) {
        MESSAGE("degraded run (opt-out set): the enforcement-coverage floor is relaxed");
        return;
    }
    // 11 full-floor OS-enforcement guard sites in this suite (incl. the forged-reply_to proof); on a
    // provisioned host all execute.
    MESSAGE("OS-enforcement cases executed: " << zenh::enforced_case_count());
    CHECK(zenh::enforced_case_count() >= 11);
}

} // TEST_SUITE
