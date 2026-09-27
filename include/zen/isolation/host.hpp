// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_ISOLATION_HOST_HPP
#define ZEN_ISOLATION_HOST_HPP

// Out-of-process weave hosting. IsolationHost spawns a zen-weave-host child per weave, bridges
// it to the bus through a proxy that is itself a weave, and supervises it: crash detection,
// bounded reload from a host-owned snapshot, then quarantine. Single-threaded, so the bus's FIFO
// and reentrancy guarantees hold. containment() reports only what was imposed; a safe floor that
// cannot be enforced refuses the mount unless dev mode lets it run, visibly uncontained.
// docs/reference/capabilities.md#os-containment-out-of-process-linuxwsl

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

/// One capability's resolved outcome on a mounted child, recorded so containment() reports each
/// capability truthfully and crash recovery reapplies it identically.
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

    /// Mount a weave out of process from `so_path` under `name`, with `grant`: spawn a child,
    /// handshake (reconstruct its schemas, cache its snapshot and policy), register the proxy.
    /// What the grant withholds is enforced in the child (a network namespace with no
    /// interface, a restricted filesystem view, a cgroup leaf); what this host cannot enforce
    /// refuses the mount unless dev mode is on, and then the weave is marked uncontained.
    OutOfProcessResult mount(const std::string& name, const std::string& so_path,
                             loom::Grant grant, const std::string& role = "");

    /// Mount the StorageBroker out of process with host-granted FsAccess::WriteScoped to
    /// `storage_root` only (created if absent), permission to reply StorageValue to any mod, and
    /// the role "storage", so floored mods reach it by role. Its storage is keyed by the
    /// sender's WeaveId, which a restart does not keep, so a mod's data lasts one session.
    OutOfProcessResult mount_broker(const std::string& name, const std::string& so_path,
                                    const std::string& storage_root);

    /// Mount the NetworkBroker out of process with host-granted `os_cap::Network` (the whole
    /// host network), FsAccess::None, bounded resources, permission to reply NetResponse to any
    /// mod, and the role "net". Its per-destination scoping is its own allow-list, not the
    /// OS's. No mod reaches it without a recorded `net` grant delta: the floor denies the role.
    OutOfProcessResult mount_net_broker(const std::string& name, const std::string& so_path);

    /// Mount an untrusted mod on the floor: no network, FsAccess::None, bounded resources, and
    /// one send rule to the storage broker's role. Its identity is its .so content hash, and a
    /// delta recorded for that identity (record_grant_delta) is applied on top. Its declared
    /// ask is read (declared_ask) but never used for the grant.
    OutOfProcessResult mount_mod(const std::string& name, const std::string& so_path);

    /// Point the host's grant-record at a per-install JSON file (loading it). The
    /// floor-factory consults it for deltas; record_grant_delta writes to it.
    void set_grant_record_path(const std::string& path);

    /// Record (replace) a capability delta for a weave identity (its .so content hash, from
    /// so_content_hash) and persist it. The host decides, never a weave: only a delta recorded
    /// here raises a mod above the floor.
    void record_grant_delta(const std::string& content_hash, GrantDelta delta);

    /// Dev mode (off by default): a mount whose requested containment cannot be enforced here
    /// proceeds with a loud warning and the weave visibly marked uncontained for that
    /// capability, instead of refusing. A deployment's choice, a dev box's or production's.
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

    /// One host-loop iteration: flush and drain child I/O (re-enqueue child output through the
    /// gate, refresh cached snapshots, note deaths), dispatch the bus, then supervise (reap dead
    /// children, reload or quarantine). The bus step is `Switchboard::drain_until_idle()`, so a
    /// perpetual in-process service on the same bus keeps it from returning; such a host drives
    /// `Switchboard::pump_pending()` itself.
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
        /// The child's declared emit-set (its manifest's `emits`), decoded the way its doors
        /// are, against this host's registry, and declared by the proxy on the bus so the one
        /// agreement wall reads it. Vocabulary only: the host still decides the child's grant.
        std::vector<std::shared_ptr<const Schema>> emits;
        std::shared_ptr<const Schema> state_schema;
        /// The mount's claim on this host's dependency registry (LIFE-08), scoped to the mount:
        /// a respawned child reuses the accept-set and state schema cached above, and everything
        /// that could still need these shapes decoded (unread channel bytes, the cached
        /// snapshot, the next handshake) is owned by this Link. `unmount` is the release path.
        SchemaClaimScope schemas;
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
