#ifndef ZEN_ISOLATION_SANDBOX_HPP
#define ZEN_ISOLATION_SANDBOX_HPP

// B3: the capability detection-and-honesty lattice, and the native enforcement that
// projects a Shard's OS-capability grant onto a child process at spawn.
//
// The rule is absolute: NEVER report enforcement we did not impose. Detection
// PROBES what this host can actually enforce (it does not assume — it attempts the
// real unprivileged operation and observes the result). Enforcement is native (no
// portable sandbox-abstraction dependency) so that the claim "this is contained" is
// backed by a boundary we understand, not a library's promise. An unrecognized
// platform or an unavailable primitive is the floor: zero enforceable capabilities
// → the mount fails safe (refuses) unless dev-mode overrides it, visibly.
//
// Network is the first primitive because it is binary and coarse: there is no
// "safer network," so it has no gradient to muddy the lattice. Later primitives
// (seccomp, cgroups, filesystem) layer onto this proven detect → apply → know →
// refuse-or-proceed frame in their own phases.

#include <cstdint>
#include <string>
#include <vector>

#include <sys/types.h> // pid_t

namespace zen::isolation {

/// An OS-capability the host may or may not be able to enforce; each has its own
/// detection probe. Network is *hard* (binary); Filesystem is *graduated* (a level);
/// Resources is *quantitative* (a limit), enforced via cgroup-v2.
enum class Capability : std::uint8_t {
    Network,
    Filesystem,
    Resources,
};

const char* capability_name(Capability c) noexcept;

/// Per-capability enforcement status — never a bare bool. Either it is enforceable
/// here and we name the `mechanism`, or it is not and we record why in `detail`.
struct CapabilityStatus {
    Capability capability{Capability::Network};
    bool enforceable = false;
    std::string mechanism; ///< how, when enforceable: "user+net namespace (no interface)"
    std::string detail;    ///< why not, when !enforceable
};

/// What this host, right now, can actually enforce — the structured report the
/// honesty lattice is built on.
struct EnforcementReport {
    std::vector<CapabilityStatus> capabilities;

    const CapabilityStatus* find(Capability c) const noexcept;
    bool enforceable(Capability c) const noexcept;
};

/// Probe the host once and cache it: for each capability, attempt the real
/// unprivileged operation in a throwaway child process and observe whether it
/// works. The probe is the same mechanism the enforcement uses, so a green probe
/// means real enforcement, not an assumption.
const EnforcementReport& detect_enforcement();

/// One step of a restricted-view construction (B4). Precomputed in the parent (where
/// allocation is fine) and executed by the fork-child with raw syscalls only, so the
/// strings here are read — never built — inside the fork→exec window.
struct MountOp {
    enum class Kind {
        MakeRPrivate, ///< mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL) — the reverse-leak guard
        Mkdir,        ///< mkdir(a, 0755); pre-existing is fine
        Mount,        ///< mount(a=src, b=dst, fstype, flags, NULL)  (tmpfs/bind)
        RemountRO,    ///< mount(NULL, b=dst, NULL, MS_REMOUNT|MS_BIND|MS_RDONLY, NULL)
        SetattrRecRO, ///< recursive read-only on b=dst via mount_setattr(AT_RECURSIVE)
        PivotRoot,    ///< pivot_root(a=new_root, b=put_old)
        Chdir,        ///< chdir(a)
        Umount,       ///< umount2(a, flags)
    };
    Kind kind{Kind::Mkdir};
    std::string a;      ///< src / path / new_root / target
    std::string b;      ///< dst / put_old
    std::string fstype; ///< "tmpfs" for a Mount; empty for bind
    unsigned long flags = 0;
};
using MountPlan = std::vector<MountOp>;

/// Child-side, async-signal-safe: unshare CLONE_NEWUSER plus the requested extra
/// namespaces — CLONE_NEWNET (a no-interface netns, B3) and/or CLONE_NEWNS (a private
/// mount namespace, B4). 0 on success, -1 on failure. The uid/gid maps are written by
/// the PARENT afterwards (see write_isolation_id_maps) — this host refuses a child's
/// self-map (EPERM), the standard container constraint — so the child must then wait
/// for the parent to map it before doing anything that needs CAP_SYS_ADMIN.
int unshare_isolation(bool network, bool filesystem) noexcept;

/// Parent-side: write `child`'s setgroups-deny + single-line uid/gid maps (its uid/gid
/// to root in the new user namespace), so the child gains CAP_SYS_ADMIN there for the
/// mount ops. Call it AFTER the child has unshared (synchronise with a pipe) and
/// BEFORE signalling the child to proceed. Ordinary libc is fine here. true on success.
bool write_isolation_id_maps(pid_t child) noexcept;

/// Child-side, async-signal-safe: execute a precomputed mount plan (build the
/// restricted view, then pivot_root into it). Only raw syscalls and reads of the
/// plan's strings — no allocation. 0 on success, -1 on the first failing op. Must run
/// AFTER enter_isolation_namespaces (which grants CAP_SYS_ADMIN in the userns).
int run_mount_plan(const MountPlan& plan) noexcept;

/// Host-side **positive confirmation** that `child` is in a namespace of the given
/// kind ("net" or "mnt") distinct from this process — a hard-to-fool check (different
/// /proc/<pid>/ns/<kind> inode) needing no protocol change. Turns "contained" from
/// *inferred* into *verified*. False if they share a namespace OR the comparison
/// cannot be made (child gone / unreadable); the caller must then fail safe. Runs in
/// the parent, outside the fork/exec window, so ordinary libc is fine.
bool child_netns_is_isolated(pid_t child) noexcept;
bool child_mountns_is_isolated(pid_t child) noexcept;

// ---- B5: cgroup-v2 resource containment (all parent-side, ordinary libc) ---------

/// Concrete resource caps to apply to a Shard's cgroup leaf, resolved from the grant
/// against the host-computed defaults. -1 means "max" (unbounded for that dimension).
struct ResourceCaps {
    long long memory_max = -1; ///< bytes; -1 = "max"
    long long pids_max = -1;   ///< -1 = "max" (no fork-bomb stop)
    long long cpu_weight = 0;  ///< 0 = leave default; else 1..10000 (cgroup cpu.weight)
};

/// Conservative defaults computed from this host, NOT a config knob: memory = a bounded
/// fraction of RAM capped at a ceiling (so one Shard can't OOM the host); pids = a fixed
/// fork-bomb-stopping number; cpu_weight = a fair share.
ResourceCaps cgroup_default_caps();

/// Process-global, idempotent: ensure the supervisor hierarchy (drain our delegated
/// base into a `zen-supervisor` leaf, enable `+memory +pids`) and return the base path
/// where per-Shard leaves are created. Empty string if cgroup-v2 resource containment
/// is not enforceable here (no v2, no delegation, no controllers) → caller fails safe.
const std::string& cgroup_base();
bool cgroup_memory_available() noexcept; ///< memory controller enabled for leaves
bool cgroup_pids_available() noexcept;   ///< pids controller enabled for leaves
bool cgroup_cpu_available() noexcept;    ///< cpu controller enabled for leaves (cpu.weight)

/// Per-Shard leaf lifecycle. `name` is a bare leaf name unique to the Shard.
bool cgroup_create_leaf(const std::string& name, const ResourceCaps& caps);
bool cgroup_move_pid(const std::string& name, pid_t pid);          ///< move pid into the leaf
bool cgroup_confirm(const std::string& name, pid_t pid, const ResourceCaps& caps); ///< pid in leaf + limits readback
void cgroup_remove_leaf(const std::string& name);                 ///< rmdir (after procs reaped)

} // namespace zen::isolation

#endif // ZEN_ISOLATION_SANDBOX_HPP
