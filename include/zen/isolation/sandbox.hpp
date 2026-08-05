// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_ISOLATION_SANDBOX_HPP
#define ZEN_ISOLATION_SANDBOX_HPP

// B3: the capability detection-and-honesty lattice, and the native enforcement that
// projects a Weave's OS-capability grant onto a child process at spawn.
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

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h> // pid_t

namespace loom {

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

/// Child-side, async-signal-safe: THE EXEC-BOUNDARY DESCRIPTOR POLICY (C-2).
///
/// Close every descriptor this process holds EXCEPT the ones named in `keep`, which
/// must be strictly ascending and non-negative (the caller writes it as a literal, so
/// this is a precondition — a malformed list is refused, never partially applied).
/// Run it immediately before `execve`: whatever the embedding host happened to hold
/// open — sockets, files, pipes, terminals, a Bridge connection — stops existing for
/// the child at that instant, so the child begins execution owning exactly what Zen
/// decided to give it and nothing it merely inherited.
///
/// This is a *boundary*, not a hardening pass. A network namespace makes a FRESH
/// socket fail; it says nothing about an ALREADY-CONNECTED descriptor that crossed
/// `execve`, and COLD-2 demonstrated exactly that gap: a host socket parked without
/// `FD_CLOEXEC` arrived usable inside a network-denied child and moved bytes back.
/// `FD_CLOEXEC` at each creation site is worth having (and Loom's own sockets now set
/// it), but it can never be the boundary: the embedding host owns descriptors Loom
/// never created and cannot annotate.
///
/// Mechanism, stated honestly. `close_range(2)` (Linux 5.9+) is used first, called by
/// syscall number rather than through the glibc 2.34+ wrapper so no new toolchain
/// floor is introduced; one call per gap covers every descriptor number up to
/// UINT_MAX, so nothing can hide at a high fd. Where the kernel lacks it, the
/// fallback closes every number below the `RLIMIT_NOFILE` HARD limit — sound because
/// a descriptor can never exceed the soft limit in force when it was opened, which
/// can never exceed the hard limit. `/proc/self/fd` is deliberately NOT used: the
/// restricted view does not mount `/proc`, so at the exec point it is not there.
///
/// 0 on success. -1 when neither mechanism can be applied (no `close_range` AND an
/// unbounded hard limit, so there is nothing sound to enumerate) or the allow-list is
/// malformed — and a -1 here must REFUSE the spawn. Reporting containment over a
/// child that kept the host's descriptors is the one thing this lattice forbids.
int close_inherited_descriptors(const int* keep, std::size_t keep_count) noexcept;

/// The two mechanisms behind it, named separately so BOTH can be exercised rather than
/// only whichever one the running kernel happens to select — the same reason
/// `resource_note`/`resource_attestation` are pure. A fallback that only runs on hosts
/// nobody tests on is a claim with no witness, and this project does not make those.
/// Nothing in the runtime chooses between them: `close_inherited_descriptors` is the
/// policy, these are its implementations, and there is no knob that weakens it.
///
/// `..._by_close_range` returns -1 with `errno == ENOSYS` where the kernel lacks the
/// syscall (and on every non-Linux host); `..._by_enumeration` is plain POSIX and
/// refuses only an unbounded `RLIMIT_NOFILE`. Same allow-list contract as above.
int close_descriptors_by_close_range(const int* keep, std::size_t keep_count) noexcept;
int close_descriptors_by_enumeration(const int* keep, std::size_t keep_count) noexcept;

/// THE EXEC-BOUNDARY ENVIRONMENT POLICY (C-2a), and the second of the boundary's two
/// authority surfaces — the first being the descriptor allow-list above.
///
/// The environment a child receives is AUTHORED, never inherited. `execve(exe, argv,
/// environ)` handed a weave with `FsAccess::None` the embedding host's whole ambient
/// state: session-bus and compositor socket addresses, `HOME`/`USER`, `PATH`, whatever
/// tokens or keys the host process happened to hold, and — read before Zen's own code
/// runs — any `LD_*` the host had set. None of that is a capability Zen decided to
/// grant; it crossed because the host possessed it.
///
/// Built in the PARENT (allocation is fine there) and handed to the fork-child as a
/// ready `char* const*`, exactly as the mount plan is precomputed for the same reason.
/// Default deny is structural rather than filtered: the builder starts empty and the
/// only way in is an explicit `set`, so a variable a future host introduces is absent
/// without anyone maintaining a list. There is no blacklist — a blacklist of
/// secret-looking names leaves every unlisted variable's authority intact.
class ChildEnvironment {
public:
    /// Author one variable. Refuses — and marks the WHOLE environment invalid, because
    /// a partially-authored environment is not the one that was authored — an empty
    /// name, a name containing '=' or a NUL, a value containing a NUL, or a duplicate
    /// name (which would make "what the child sees" depend on lookup order).
    void set(std::string_view name, std::string_view value);

    /// False if any `set` was refused. The caller must then REFUSE THE SPAWN; falling
    /// back to `environ` would hand over exactly what this exists to withhold.
    bool ok() const noexcept { return ok_; }
    std::size_t size() const noexcept { return entries_.size(); }

    /// The NUL-terminated array `execve` wants (never null; an empty environment is a
    /// one-element array holding only the terminator). Call it in the PARENT: it
    /// allocates, and the child must not. Valid until the next `set`.
    char* const* data();

private:
    std::vector<std::string> entries_; ///< "NAME=VALUE", in authored order
    std::vector<char*> pointers_;      ///< rebuilt by data(); back() is always nullptr
    bool ok_ = true;
};

/// The authored environment for `zen-weave-host`. Every entry it contains exists
/// because Zen put it there for a measured reason; see reference/capabilities.md for
/// the current set and why it is what it is.
ChildEnvironment build_child_environment();

/// Host-side **positive confirmation** that `child` is in a namespace of the given
/// kind ("net" or "mnt") distinct from this process — a hard-to-fool check (different
/// /proc/<pid>/ns/<kind> inode) needing no protocol change. Turns "contained" from
/// *inferred* into *verified*. False if they share a namespace OR the comparison
/// cannot be made (child gone / unreadable); the caller must then fail safe. Runs in
/// the parent, outside the fork/exec window, so ordinary libc is fine.
bool child_netns_is_isolated(pid_t child) noexcept;
bool child_mountns_is_isolated(pid_t child) noexcept;

// ---- B5: cgroup-v2 resource containment (all parent-side, ordinary libc) ---------

/// Concrete resource caps to apply to a Weave's cgroup leaf, resolved from the grant
/// against the host-computed defaults. -1 means "max" (unbounded for that dimension).
struct ResourceCaps {
    long long memory_max = -1; ///< bytes; -1 = "max"
    long long pids_max = -1;   ///< -1 = "max" (no fork-bomb stop)
    long long cpu_weight = 0;  ///< 0 = leave default; else 1..10000 (cgroup cpu.weight)
};

/// Conservative defaults computed from this host, NOT a config knob: memory = a bounded
/// fraction of RAM capped at a ceiling (so one Weave can't OOM the host); pids = a fixed
/// fork-bomb-stopping number; cpu_weight = a fair share.
ResourceCaps cgroup_default_caps();

/// Process-global, idempotent: ensure the supervisor hierarchy (drain our delegated
/// base into a `zen-supervisor` leaf, enable `+memory +pids`) and return the base path
/// where per-Weave leaves are created. Empty string if cgroup-v2 resource containment
/// is not enforceable here (no v2, no delegation, no controllers) → caller fails safe.
const std::string& cgroup_base();
bool cgroup_memory_available() noexcept; ///< memory controller enabled for leaves
bool cgroup_pids_available() noexcept;   ///< pids controller enabled for leaves
bool cgroup_cpu_available() noexcept;    ///< cpu controller enabled for leaves (cpu.weight)

/// The honest one-line resource note for a leaf with these caps. It must state only
/// what cgroup_create_leaf will TRULY impose: memory.max and pids.max are each written
/// only where their controller is delegated, so where `memory_enforceable` /
/// `pids_enforceable` is false the note says that dimension is UNCAPPED rather than claim
/// a computed-but-never-set cap. This is the lattice's one absolute rule — never report
/// enforcement we did not impose (audit F-20: a pids-only host reported `memory<=…` while
/// memory ran uncapped; its mirror — a memory-only host claiming `pids<=…` — is closed the
/// same way). Pure and portable, so the honesty is unit-testable without a live cgroup.
std::string resource_note(const ResourceCaps& caps, bool memory_enforceable,
                          bool pids_enforceable);

/// The honest one-line resources ATTESTATION (the note plus the "honest scope" sentence)
/// rendered into containment(). Pure and portable like resource_note: the fork-bomb-stop
/// claim is delegation-qualified on `pids_enforceable` (the pids controller is what imposes
/// pids.max), so every posture — including memory-only, which no live cgroup on a
/// memory+pids host can produce — is unit-testable without a live cgroup. `confirmed` is
/// the leaf's positive readback (of the delegated controllers only).
std::string resource_attestation(const std::string& note, bool pids_enforceable, bool confirmed);

/// Per-Weave leaf lifecycle. `name` is a bare leaf name unique to the Weave.
bool cgroup_create_leaf(const std::string& name, const ResourceCaps& caps);
bool cgroup_move_pid(const std::string& name, pid_t pid);          ///< move pid into the leaf
bool cgroup_confirm(const std::string& name, pid_t pid, const ResourceCaps& caps); ///< pid in leaf + limits readback
void cgroup_remove_leaf(const std::string& name);                 ///< rmdir (after procs reaped)

} // namespace loom

#endif // ZEN_ISOLATION_SANDBOX_HPP
