// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_ISOLATION_HOST_HPP
#define ZEN_ISOLATION_HOST_HPP

// Out-of-process Weave hosting. IsolationHost spawns a child zen-weave-host per
// Weave, bridges it to the bus through a proxy that *is* a Weave (so the
// Switchboard is unchanged), and supervises it: crash detection → bounded reload
// from a host-owned snapshot → quarantine. Everything is single-threaded — the
// only async is the child's reply *timing* — so the bus's FIFO and reentrancy
// guarantees hold. The Switchboard must outlive the host.
//
// Honest containment (B3): an out-of-process Weave is *isolated* (process boundary:
// crash-contained, cannot touch host memory) and, when its grant withholds the
// Network capability and this host can enforce it, also *network-sandboxed* (the
// child runs with no network interface, so connect() fails at the syscall level).
// containment() is generated from what was ACTUALLY imposed per Weave — it never
// claims enforcement it did not apply. When the safe floor cannot be enforced the
// mount fails safe (refuses) unless dev-mode is on, in which case it proceeds with
// the Weave visibly marked uncontained.

#include <zen/isolation/channel.hpp>
#include <zen/isolation/grant_record.hpp>
#include <zen/isolation/sandbox.hpp>
#include <zen/kernel/schema_codec.hpp>
#include <zen/registry.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/value.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h> // pid_t

namespace loom {

class OutOfProcessWeave;

struct OutOfProcessResult {
    bool ok = false;
    loom::WeaveId id{};
    std::string error;
};

/// One capability's resolved outcome on a mounted child — recorded so containment()
/// reports the truth *per capability* and crash-recovery reapplies identically. B3
/// resolves exactly one (Network); B4 resolves more here, unchanged in shape (each
/// Link holds a vector of these, and containment() iterates them).
struct CapabilityResolution {
    enum class Outcome {
        Enforced,    ///< the safe floor was imposed AND positively confirmed
        Granted,     ///< intentionally granted — real power, not contained
        Uncontained, ///< requested but unenforceable here; dev-mode let it run, visibly
    };
    Capability capability{Capability::Network};
    Outcome outcome{Outcome::Granted};
    bool confirmed = false;  ///< Enforced only: positively verified (e.g. distinct netns inode)
    std::string note;        ///< extra honest detail (e.g. the filesystem level name)
};

class IsolationHost {
public:
    /// `weave_host_exe` is the path to the zen-weave-host child executable.
    IsolationHost(loom::Switchboard& bus, std::string weave_host_exe);
    ~IsolationHost();

    IsolationHost(const IsolationHost&) = delete;
    IsolationHost& operator=(const IsolationHost&) = delete;

    /// Mount a Weave out-of-process from `so_path` under `name`, with `grant`.
    /// Spawns a child, handshakes (reconstructs its schemas, caches its initial
    /// snapshot and policy), and registers the proxy on the bus.
    ///
    /// B3: if the grant withholds os_cap::Network, the child is launched into a
    /// no-interface network namespace (OS-enforced). If that cannot be enforced on
    /// this host, the mount refuses (fail-safe) unless dev-mode is on — then it
    /// proceeds with the Weave marked network-uncontained.
    OutOfProcessResult mount(const std::string& name, const std::string& so_path,
                             loom::Grant grant, const std::string& role = "");

    /// Mount the StorageBroker out-of-process at the TCB tier: host-granted
    /// FsAccess::WriteScoped to `storage_root` ONLY (the persistent-scoped-write
    /// extension — disk, but contained to that one dir; it can't reach the host home),
    /// permitted to reply StorageValue to any mod, and registered under role
    /// "storage" so floored mods reach it by role-addressing. `storage_root` is
    /// created if absent. Storage is **session-scoped** (keyed by the ephemeral
    /// sender); persistent-across-restart awaits the first-class-identity phase.
    OutOfProcessResult mount_broker(const std::string& name, const std::string& so_path,
                                    const std::string& storage_root);

    /// Mount the NetworkBroker out-of-process at the TCB tier: host-granted
    /// `os_cap::Network` (so it runs in the host netns, with the real network),
    /// `FsAccess::None`, bounded resources, permitted to reply NetResponse to any mod, and
    /// registered under role "net". A **higher-trust** broker than the StorageBroker:
    /// network is binary, so the OS gives it the *whole* host network — its per-destination
    /// scoping is **software** (its own allow-list), not OS-enforced. No mod reaches it
    /// without a recorded `net` grant delta (the floor denies the net role).
    OutOfProcessResult mount_net_broker(const std::string& name, const std::string& so_path);

    /// Mount an untrusted mod from `so_path` under `name` on the **floor**: minimal
    /// authority (no network, FsAccess::None, bounded resources) plus a single
    /// send-rule to the storage broker role — enough to persist and retrieve on
    /// messages alone, with zero disk access. The mod's identity is its .so
    /// content-hash; any delta the host has recorded for that identity (see
    /// record_grant_delta) is applied on top of the floor. The mod's *ask* is read
    /// (declared_ask) but never consulted for the grant: a declaration is not a grant.
    OutOfProcessResult mount_mod(const std::string& name, const std::string& so_path);

    /// Point the host's grant-record at a per-install JSON file (loading it). The
    /// floor-factory consults it for deltas; record_grant_delta writes to it.
    void set_grant_record_path(const std::string& path);

    /// Record (replace) a capability delta for a Weave identity (its .so
    /// content-hash, from so_content_hash) and persist it. This is the host holding
    /// the pen — the stand-in for the consent UX (deferred). The host, never a Weave,
    /// decides; only a delta recorded here raises a mod above the floor.
    void record_grant_delta(const std::string& content_hash, GrantDelta delta);

    /// Dev-mode (default off = strict): converts a fail-safe refusal — when a
    /// requested capability cannot be enforced on this host — into a loud warning,
    /// proceeding with the Weave visibly marked uncontained for that capability. A
    /// deployment-level choice (dev box vs prod); the one knob B3 introduces.
    void set_dev_mode(bool on) noexcept { dev_mode_ = on; }
    bool dev_mode() const noexcept { return dev_mode_; }

    /// The enforcement this host detected it can impose.
    const EnforcementReport& enforcement() const noexcept { return enforcement_; }
    /// Test seam: force a detection report (e.g. Network unenforceable) so both the
    /// enforced and the fail-safe/dev-mode branches are exercisable on any host.
    void override_enforcement_for_test(EnforcementReport report) {
        enforcement_ = std::move(report);
    }
    /// Test seam, distinct from override_enforcement_for_test (which forges the
    /// detection *verdict*): force the *real* sandbox entry to fail at the next
    /// spawn, as if `unshare()` failed in the child. Proves that a surprise failure
    /// of an *intended* enforcement fails safe (refuses) in both strict and dev mode.
    void force_entry_failure_for_test(bool on) noexcept { force_entry_failure_ = on; }

    /// One host-loop iteration, single-threaded: flush + drain child I/O
    /// (re-enqueue child output gated, refresh cached snapshots, note deaths) →
    /// pump the bus (proxies fire-and-continue) → supervise (reap dead children,
    /// drive bounded reload-then-quarantine). Pure in-process systems can keep
    /// calling Switchboard::pump() alone; this composes with it.
    void step();

    /// Step until `predicate()` holds or `max_steps` is reached. Sleeps briefly
    /// between steps so child processes make progress. Returns predicate's result.
    template <class Pred>
    bool run_until(Pred predicate, int max_steps);

    void unmount(const std::string& name);

    /// Reload a mounted Weave's implementation IN PLACE: re-spawn its child (from the
    /// same .so) and re-revive from the host-owned snapshot, keeping the SAME WeaveId,
    /// grant, and role — so routing (role-addressing included) and reload-stable
    /// send-rules survive. A broker's on-disk data is durable independently of this.
    /// Returns false if `name` is not mounted or the respawn failed.
    bool reload(const std::string& name);

    bool is_mounted(const std::string& name) const;
    bool quarantined(const std::string& name) const;
    /// The honest containment level of a hosted Weave.
    std::string containment(const std::string& name) const;

    /// The capability ask the Weave's manifest declared, or nullopt if it asked for
    /// nothing (or is not mounted). This is the host reading the *advice* — what the
    /// Weave wanted — distinct from what it was granted; the gap between this and
    /// containment() is the advice-vs-authority wall made observable.
    std::optional<loom::CapabilityAsk> declared_ask(const std::string& name) const;

private:
    struct Link {
        std::string name;
        std::string so_path;
        loom::WeaveId id{};
        std::unique_ptr<Channel> channel; // null when no live child
        pid_t pid = -1;
        std::vector<std::shared_ptr<const Schema>> accept;
        std::shared_ptr<const Schema> state_schema;
        std::optional<Value> snapshot_value; // last good admitted snapshot (host-owned)
        std::string snapshot_bytes;          // its canonical bytes, for revival
        std::optional<Value> policy_value;
        std::optional<loom::CapabilityAsk> requested_caps; // the manifest's ask (advice)
        OutOfProcessWeave* proxy = nullptr;
        std::vector<CapabilityResolution> resolutions; // per-capability, resolved at mount
        MountPlan fs_plan;     // precomputed restricted-view plan (empty if fs not sandboxed)
        std::string fs_root;   // the mkdtemp'd new-root dir (for teardown), empty otherwise
        std::string cg_leaf;   // the per-Weave cgroup leaf name (empty if resources not contained)
        ResourceCaps cg_caps;  // the resolved resource caps applied to the leaf
        bool dead = false;
        bool death_signaled = false;
        bool quarantined = false;
    };

    friend class OutOfProcessWeave;

    // Proxy-facing operations.
    void ship_deliver(Link& link, const loom::Message& in);
    void respawn_and_revive(Link& link, const Value& state);

    // Spawn a child for `link`, wait for its Hello; optionally return its bytes.
    bool spawn_and_handshake(Link& link, std::string* manifest, std::string* policy,
                             std::string* snapshot, std::string& error);
    void reconstruct_and_cache(Link& link, const std::string& manifest, const std::string& policy,
                               const std::string& snapshot);
    void handle_child_frame(Link& link, const Incoming& frame);
    void on_child_death(Link& link);
    void recover(Link& link);
    void teardown_child(Link& link);

    // Is a capability resolved to Enforced for this link (→ sandboxed spawn)?
    static bool network_sandboxed(const Link& link);
    static bool filesystem_sandboxed(const Link& link);
    static bool resources_contained(const Link& link);

    loom::Switchboard& bus_;
    std::string exe_;
    loom::Registry registry_; // reconstructed child schemas (decode deps + resolution)
    std::map<std::string, std::unique_ptr<Link>> links_;
    EnforcementReport enforcement_;    // what this host can actually impose (detected once)
    GrantRecord grant_record_;         // per-install ledger of grant deltas above the floor
    bool dev_mode_ = false;            // strict by default
    bool force_entry_failure_ = false; // test seam: simulate a real sandbox-entry failure
};

template <class Pred>
bool IsolationHost::run_until(Pred predicate, int max_steps) {
    for (int i = 0; i < max_steps; ++i) {
        if (predicate()) {
            return true;
        }
        step();
        // Yield briefly so child processes make progress between iterations
        // (their replies/deaths are asynchronous to the host loop).
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

} // namespace loom

#endif // ZEN_ISOLATION_HOST_HPP
