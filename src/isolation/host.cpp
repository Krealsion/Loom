// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/isolation/host.hpp>

#include <zen/kernel/schema_codec.hpp> // manifest_schema, decode_schema (shared encode/decode)
#include <zen/serialize.hpp>           // parse, admit, serialize
#include <zen/value.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace loom {

namespace {

constexpr int kChildFd = 3;            // the child reads its socket from this fd
constexpr int kHandshakeTimeoutMs = 5000;

// THE INTENTIONAL CHILD DESCRIPTOR SET (C-2). Derived from what the child actually
// needs across execve, not guessed:
//
//   0,1,2      stdin/stdout/stderr — DELIBERATELY PRESERVED, and therefore an
//              intentional ambient capability, not an accident: a contained child
//              shares the host's console. It is what carries a crashing child's
//              sanitizer report and its libc/loader diagnostics, and closing it would
//              also arm the classic trap where the child's next open() silently
//              becomes fd 0. containment() and reference/capabilities.md both say so
//              in words, because a reader must not infer "the child gets nothing".
//   kChildFd   the weave-host protocol transport (the socketpair end dup2'd here and
//              named to the child as argv[1]).
//
// Nothing else. The spawn-synchronisation pipes are closed by the child before the
// mount plan runs and never cross exec; sv[0]/sv[1] are closed just above the sweep;
// the .so and the executable are opened by the loader AFTER exec. The list must stay
// strictly ascending — close_inherited_descriptors sweeps the gaps between entries.
constexpr int kChildKeepFds[] = {0, 1, 2, kChildFd};
static_assert(kChildFd > 2, "kChildKeepFds must remain strictly ascending");

// Child exit codes on the pre-exec path. 127 is the generic "the sandbox could not be
// entered"; the hygiene failure gets its own so the parent's refusal can name it
// instead of reporting a bare failed handshake.
constexpr int kExitPreExecFailed = 127;
constexpr int kExitFdHygieneFailed = 126;

// Reap `pid` if it has already exited, waiting briefly for it. Returns its exit code,
// or -1 if it did not exit within the budget (or died by signal). Used ONLY on the
// handshake-failure path, to turn "the child said nothing" into a reason.
int reap_exit_code(pid_t pid, int attempts) noexcept {
    for (int i = 0; i < attempts; ++i) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (r < 0) {
            return -1; // already reaped, or not ours
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return -1; // still running: an unresponsive child, not a pre-exec refusal
}

// Bounded, blocking wait for the child's first Hello frame. The child has nothing
// to do but handshake, so a short wait is enough; a child that never speaks is a
// failed spawn, not a host hang.
bool wait_for_hello(Channel& ch, Incoming& out, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::vector<Incoming> frames;
    for (;;) {
        ch.poll(frames);
        for (auto& f : frames) {
            if (f.op == Op::Hello) {
                out = std::move(f);
                return true;
            }
        }
        frames.clear();
        if (ch.done()) {
            return false; // EOF/error before any Hello: the child died at startup
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{};
        pfd.fd = ch.fd();
        pfd.events = POLLIN;
        const int r = ::poll(&pfd, 1, static_cast<int>(left));
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false; // timeout or poll error
        }
        // readable: loop back to drain frames
    }
}

// Process-global unique suffix for per-Weave cgroup leaf names (hosts share the base).
std::atomic<unsigned long long> g_leaf_counter{0};

// ---- B4 restricted-view plan (built in the parent; run by the fork-child) --------

std::string dirname_of(const std::string& path) {
    const std::size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    return pos == 0 ? std::string("/") : path.substr(0, pos);
}

// Emit Mkdir ops for `root` + each '/'-prefix of the absolute path `abs`, so a later
// bind has its mountpoint. `abs` begins with '/'; `root` already exists.
void mkdir_p_ops(MountPlan& p, const std::string& root, const std::string& abs) {
    for (std::size_t i = 1; i <= abs.size(); ++i) {
        if (i == abs.size() || abs[i] == '/') {
            p.push_back({MountOp::Kind::Mkdir, root + abs.substr(0, i), "", "", 0});
        }
    }
}

// Bind `src` (a host dir) read-only (recursively) into the view at root + src.
void bind_ro_ops(MountPlan& p, const std::string& root, const std::string& src) {
    mkdir_p_ops(p, root, src);
    p.push_back({MountOp::Kind::Mount, src, root + src, "", MS_BIND | MS_REC});
    p.push_back({MountOp::Kind::SetattrRecRO, "", root + src, "", 0}); // recursive read-only
}

// mkdir -p a real host directory (mode 0700), creating missing parents. Idempotent;
// returns false if a component exists as a non-directory or cannot be created. Used
// for the broker's persistent storage_root (the writable bind target).
bool ensure_host_dir(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::size_t pos = 0;
    while (pos < path.size()) {
        std::size_t slash = path.find('/', pos);
        if (slash == std::string::npos) {
            slash = path.size();
        }
        const std::string acc = path.substr(0, slash);
        if (!acc.empty() && acc != "/") {
            struct stat st {};
            if (::stat(acc.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode)) {
                    return false;
                }
            } else if (::mkdir(acc.c_str(), 0700) != 0) {
                return false;
            }
        }
        pos = slash + 1;
    }
    return true;
}

// The allow-list view for `level`: private-first, a tmpfs root, the loader closure
// and the exe/.so dirs bound read-only, a scratch tmpfs for the write levels, then
// pivot_root into it. The view is built purely by ADDITION, so what a child can see is
// exactly what this function binds -- nothing of the host home crosses, and neither
// does any path outside the list below. Say that rather than "secrets are absent": the
// loader closure includes the whole of /etc, and the exe/.so dirs are bound whole, so a
// secret is absent from this view only if it is in neither. The enumeration lives in
// docs/reference/capabilities.md and is written out there for the same reason.
MountPlan build_view_plan(loom::FsAccess level, const std::string& scoped_path,
                          const std::string& exe_path, const std::string& so_path,
                          const std::string& root) {
    MountPlan p;
    p.push_back({MountOp::Kind::MakeRPrivate, "", "", "", 0}); // reverse-leak guard, FIRST
    p.push_back({MountOp::Kind::Mount, "tmpfs", root, "tmpfs", 0});
    p.push_back({MountOp::Kind::Mkdir, root + "/oldroot", "", "", 0});

    for (const char* d : {"/usr", "/lib", "/lib64", "/bin", "/etc"}) {
        struct stat st {};
        if (::stat(d, &st) == 0 && S_ISDIR(st.st_mode)) {
            bind_ro_ops(p, root, d); // the dynamic loader's closure
        }
    }
    const std::string exe_dir = dirname_of(exe_path);
    bind_ro_ops(p, root, exe_dir); // so execve(exe) resolves inside the view
    const std::string so_dir = dirname_of(so_path);
    const bool so_under_exe = so_dir == exe_dir ||
                              so_dir.compare(0, exe_dir.size() + 1, exe_dir + "/") == 0;
    if (!so_dir.empty() && !so_under_exe) {
        bind_ro_ops(p, root, so_dir); // the .so, if it lives elsewhere
    }
    if (level == loom::FsAccess::ReadOnly && !scoped_path.empty()) {
        bind_ro_ops(p, root, scoped_path); // the granted tree, read-only
    }
    if (level == loom::FsAccess::WriteScoped && !scoped_path.empty()) {
        // Persistent-scoped write (TCB-only — the StorageBroker): bind the host's
        // persistent storage dir writable at /scratch instead of the ephemeral tmpfs.
        // Still least-privilege — only this one dir is reachable, never the host home —
        // but the data survives. A mod is FsAccess::None and never gets a scoped_path.
        p.push_back({MountOp::Kind::Mkdir, root + "/scratch", "", "", 0});
        p.push_back({MountOp::Kind::Mount, scoped_path, root + "/scratch", "", MS_BIND});
    } else if (level == loom::FsAccess::WriteScoped || level == loom::FsAccess::WriteNoExec) {
        const unsigned long f = (level == loom::FsAccess::WriteNoExec) ? MS_NOEXEC : 0;
        p.push_back({MountOp::Kind::Mkdir, root + "/scratch", "", "", 0});
        p.push_back({MountOp::Kind::Mount, "tmpfs", root + "/scratch", "tmpfs", f});
    }

    // The view root itself is now read-only (its mountpoints were created above), so a
    // Weave cannot write — or plant-and-exec — at "/"; only the scratch submount (if
    // any) stays writable, with its own flags. Without this the read-only/noexec intent
    // would leak through the writable root tmpfs.
    p.push_back({MountOp::Kind::Mount, root, root, "", MS_REMOUNT | MS_RDONLY});

    p.push_back({MountOp::Kind::PivotRoot, root, root + "/oldroot", "", 0});
    p.push_back({MountOp::Kind::Chdir, "/", "", "", 0});
    p.push_back(
        {MountOp::Kind::Umount, "/oldroot", "", "", static_cast<unsigned long>(MNT_DETACH)});
    return p;
}

// One honest sentence for a resolved capability — each capability describes its OWN
// boundary precisely (honest over flattering). B4 extends this with its capabilities.
std::string describe_resolution(const CapabilityResolution& r) {
    using Outcome = CapabilityResolution::Outcome;
    if (r.capability == Capability::Network) {
        switch (r.outcome) {
            case Outcome::Enforced:
                // THREE different facts, kept apart on purpose (C-2, C-2a). The namespace
                // decides what a FRESH socket can do; the descriptor sweep decides which
                // ALREADY-OPEN descriptors exist at all; the authored environment decides
                // what the child is TOLD. COLD-2 found the first true while the second was
                // false, and this sentence used to describe only the first — so a reader
                // had no way to learn that an inherited, connected host socket walked
                // straight through the containment being claimed.
                return std::string(
                           "network: contained — private user+net namespace, no external "
                           "interface, so new outbound connections fail at the syscall level") +
                       (r.confirmed ? " (confirmed: child netns distinct from host)" : "") +
                       "; ambient host descriptors do not cross the boundary: every "
                       "descriptor is closed immediately before execve except the control "
                       "fd and stdin/stdout/stderr, which are deliberately kept (the child "
                       "shares the host's console); nor does ambient host environment — the "
                       "child's environment is authored by Zen and is currently EMPTY, so "
                       "no host token, path, session address or LD_* variable crosses. So "
                       "'contained' means no external reachability, no inherited reach and "
                       "no inherited environment, and still not no-IPC: the control fd is a "
                       "channel to this host, by design";
            case Outcome::Granted:
                return "network: granted — full host network by the grant, NOT OS-scoped (no "
                       "namespace limits its reach); any per-destination limit is the holder's "
                       "own software policy (e.g. a broker's allow-list), not the kernel — a "
                       "higher-trust posture than an OS-contained capability";
            case Outcome::Uncontained:
                return "network: NOT CONTAINED — requested but unenforceable on this host, "
                       "running under dev-mode override";
        }
    }
    if (r.capability == Capability::Filesystem) {
        switch (r.outcome) {
            case Outcome::Enforced:
                return std::string("filesystem: contained at level ") + r.note +
                       " — private mount namespace, allow-list view (pivot_root into a minimal "
                       "read-only root; the host home and its secrets are absent, not merely "
                       "hidden)" +
                       (r.confirmed ? " (confirmed: child mountns distinct from host)" : "") +
                       "; honest scope: a writable mount is noexec only at WriteNoExec and even "
                       "then blocks native execve, not code run by an interpreter already in the "
                       "view; PIDs are not namespaced, so /proc is deliberately not mounted";
            case Outcome::Granted:
                return "filesystem: WriteAnywhere — unrestricted host filesystem by the grant "
                       "(not contained)";
            case Outcome::Uncontained:
                return "filesystem: NOT CONTAINED — requested but unenforceable on this host, "
                       "running under dev-mode override";
        }
    }
    if (r.capability == Capability::Resources) {
        switch (r.outcome) {
            case Outcome::Enforced:
                // Rendered by the pure, delegation-qualified helper (sandbox.cpp) so the
                // fork-bomb-stop claim mirrors the memory clause: it is asserted only where
                // the pids controller is delegated (F-20's pids mirror). cgroup_pids_available()
                // is the same source of truth cgroup_create_leaf gates pids.max on.
                return resource_attestation(r.note, cgroup_pids_available(), r.confirmed);
            case Outcome::Granted:
                // Resources never resolve to Granted (there is no wholesale opt-out; where the
                // pids controller is delegated, no grant licenses a fork bomb). Kept only for
                // switch-exhaustiveness; defensive if ever hit.
                return "resources: (unexpected) — no wholesale opt-out";
            case Outcome::Uncontained:
                return "resources: NOT CONTAINED — requested but unenforceable on this host, "
                       "running under dev-mode override";
        }
    }
    return std::string(capability_name(r.capability)) + ": (unrecognized)";
}

} // namespace

// ---- the proxy: a Weave, on the bus, backed by a child process ---------------
//
// To the Switchboard this is an ordinary Weave. handle() serializes and ships the
// message to the child and returns at once (fire-and-continue — a slow or hung
// child never blocks the bus). snapshot()/policy() return the host-owned cached
// values refreshed from the child's proactive Snapshot frames. revive() (re)spawns
// the child if dead and ships the state.
class OutOfProcessWeave final : public loom::Weave {
public:
    OutOfProcessWeave(IsolationHost* host, IsolationHost::Link* link) : host_(host), link_(link) {}

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return link_->accept;
    }
    void handle(const loom::Message& in, loom::Bus& /*bus*/) override {
        host_->ship_deliver(*link_, in);
    }
    loom::Value snapshot() const override { return *link_->snapshot_value; }
    loom::Value policy() const override { return *link_->policy_value; }
    void revive(const loom::Value& state) override { host_->respawn_and_revive(*link_, state); }

private:
    IsolationHost* host_;
    IsolationHost::Link* link_;
};

// ---- IsolationHost -----------------------------------------------------------

IsolationHost::IsolationHost(loom::Switchboard& bus, std::string weave_host_exe)
    : bus_(bus), exe_(std::move(weave_host_exe)), enforcement_(detect_enforcement()) {}

IsolationHost::~IsolationHost() {
    std::vector<std::string> names;
    names.reserve(links_.size());
    for (const auto& entry : links_) {
        names.push_back(entry.first);
    }
    for (const std::string& n : names) {
        unmount(n);
    }
}

bool IsolationHost::spawn_and_handshake(Link& link, std::string* manifest, std::string* policy,
                                        std::string* snapshot, std::string& error) {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        error = "socketpair failed";
        return false;
    }
    // BOTH ends get CLOEXEC, so neither is inherited by any *other* child this host
    // (or its embedder) may spawn. The child end reaches its own exec by an explicit
    // act instead: dup2 onto kChildFd clears CLOEXEC on the copy, and in the rare case
    // where socketpair already handed back kChildFd itself the child clears the flag
    // by hand. Loom's own descriptors being CLOEXEC is defence in depth, never the
    // boundary — the boundary is the sweep below, because the embedding host owns
    // descriptors Loom never created (C-2).
    (void)::fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    (void)::fcntl(sv[1], F_SETFD, FD_CLOEXEC);

    const std::string fd_arg = std::to_string(kChildFd);
    char* argv[] = {const_cast<char*>(exe_.c_str()), const_cast<char*>(fd_arg.c_str()),
                    const_cast<char*>(link.so_path.c_str()), nullptr};

    // The exec boundary's SECOND authority surface (C-2a). Built here, in the parent,
    // where allocation is fine — exactly as the mount plan is — so the fork-child does
    // nothing but hand the finished array to execve. `environ` is never consulted: a
    // variable reaches the child because Zen authored it, not because this process
    // happened to hold it.
    ChildEnvironment child_env = build_child_environment();
    if (!child_env.ok()) {
        ::close(sv[0]);
        ::close(sv[1]);
        error = "child environment could not be constructed (the authored environment was "
                "malformed), so the spawn refused rather than fall back to this host's "
                "ambient environment";
        return false;
    }
    char* const* envp = child_env.data(); // allocates: parent-side, before the fork

    const bool sandbox_net = network_sandboxed(link);
    const bool sandbox_fs = filesystem_sandboxed(link);
    const bool sandbox_res = resources_contained(link);
    const bool needs_userns = sandbox_net || sandbox_fs;

    if (sandbox_fs && !link.fs_root.empty()) {
        (void)::mkdir(link.fs_root.c_str(), 0700); // (re)create the view-root mountpoint for spawn
    }
    if (sandbox_res && !cgroup_create_leaf(link.cg_leaf, link.cg_caps)) {
        // Fail-safe before spawning: the resource leaf must exist with its limits.
        ::close(sv[0]);
        ::close(sv[1]);
        error = "resource sandbox: could not create/limit the cgroup leaf";
        return false;
    }

    // ONE spawn path, and therefore ONE exec boundary (C-2). It used to be two: a
    // fork/execve for the sandboxed case (posix_spawn cannot unshare) and a posix_spawn
    // for the granted/dev-mode case. Two boundaries meant two descriptor policies, and
    // posix_spawn's file actions cannot express "close everything except these" without
    // enumerating — so the second path could only ever have had the weaker one. The
    // guarantee this phase makes is unconditional (a network-GRANTED weave has no more
    // business inheriting the host's open database handle than a contained one does),
    // so the guarantee gets a single place to live.
    //
    // B3/B4: the child unshares into a network and/or mount namespace. This host refuses
    // a child's self-map (EPERM), so the PARENT writes the child's uid/gid maps over a
    // pipe handshake: the child unshares and signals, the parent maps it and releases it,
    // then the child builds its restricted view and execs. Everything the child does is
    // async-signal-safe (the mount plan was precomputed); the parent's map-writing uses
    // ordinary libc. When nothing is sandboxed the same handshake runs and simply has no
    // privileged work to do in the middle of it.
    int sync_c2p[2];
    int sync_p2c[2];
    if (::pipe(sync_c2p) != 0 || ::pipe(sync_p2c) != 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        error = "sandbox sync pipe failed";
        return false;
    }
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(sync_c2p[0]);
        ::close(sync_p2c[1]);
        // force_entry_failure_ simulates a *surprise* entry failure: the child dies
        // before exec exactly as a true unshare()/mount()/cgroup failure would, so the
        // handshake fails and the mount refuses — strict and dev mode alike.
        if (force_entry_failure_) {
            ::_exit(kExitPreExecFailed);
        }
        if (needs_userns && unshare_isolation(sandbox_net, sandbox_fs) != 0) {
            ::_exit(kExitPreExecFailed);
        }
        char one = 1;
        (void)!::write(sync_c2p[1], &one, 1); // "I have unshared"
        char go = 0;
        (void)!::read(sync_p2c[0], &go, 1); // wait to be mapped by the parent
        ::close(sync_c2p[1]);
        ::close(sync_p2c[0]);
        if (go != 1 || (sandbox_fs && run_mount_plan(link.fs_plan) != 0)) {
            ::_exit(kExitPreExecFailed); // not mapped, or the view could not be built
        }
        // Place the protocol transport at the fd the child is told to read. dup2 clears
        // CLOEXEC on the copy; where socketpair already handed back kChildFd there is no
        // copy to make, so the flag the parent set is cleared by hand instead.
        if (sv[1] != kChildFd) {
            if (::dup2(sv[1], kChildFd) < 0) {
                ::_exit(kExitPreExecFailed);
            }
            ::close(sv[1]);
        } else if (::fcntl(kChildFd, F_SETFD, 0) != 0) {
            ::_exit(kExitPreExecFailed);
        }
        if (sv[0] != kChildFd) {
            ::close(sv[0]);
        }
        // THE DESCRIPTOR BOUNDARY, the first of the exec boundary's two authority
        // surfaces (the second, the authored environment, was built above and is handed
        // to execve below). Everything the embedding host happened to hold open — its
        // Bridge sockets, its files, its pipes, its terminal — stops existing for this
        // child here, one syscall before it becomes zen-weave-host. The explicit closes
        // above are subsumed by this and kept anyway: they state the intent, and the
        // sweep enforces it over descriptors this code never knew about.
        if (close_inherited_descriptors(kChildKeepFds,
                                        sizeof(kChildKeepFds) / sizeof(kChildKeepFds[0])) != 0) {
            // Fail SAFE, loudly. stderr is in the allow-list and still open, and a raw
            // write of a fixed string is async-signal-safe; the parent turns the exit
            // code below into the refusal the operator reads.
            static const char msg[] =
                "[zen-isolation] refusing to exec zen-weave-host: the exec-boundary "
                "descriptor sweep could not be established\n";
            (void)!::write(2, msg, sizeof(msg) - 1);
            ::_exit(kExitFdHygieneFailed);
        }
        ::execve(exe_.c_str(), argv, envp); // the AUTHORED environment, never `environ`
        ::_exit(kExitPreExecFailed);        // exec failed
    }
    ::close(sync_c2p[1]);
    ::close(sync_p2c[0]);
    if (pid < 0) {
        ::close(sync_c2p[0]);
        ::close(sync_p2c[1]);
        ::close(sv[0]);
        ::close(sv[1]);
        error = "fork (child spawn) failed";
        return false;
    }
    char one = 0;
    const bool signaled = (::read(sync_c2p[0], &one, 1) == 1 && one == 1);
    // At the sync point (child unshared, blocked on "go") the parent does the
    // privileged setup the child cannot: write its id maps (if it made a userns) and
    // move it into its cgroup leaf — so the whole subtree it execs/spawns runs under
    // the limits before it can consume anything.
    bool prepared = signaled;
    if (prepared && needs_userns) {
        prepared = write_isolation_id_maps(pid);
    }
    if (prepared && sandbox_res) {
        prepared = cgroup_move_pid(link.cg_leaf, pid);
    }
    if (signaled) {
        // Only write when the child is alive and waiting; otherwise its read end is
        // gone and the write would raise SIGPIPE (e.g. the forced-failure path).
        const char go = prepared ? 1 : 2;
        (void)!::write(sync_p2c[1], &go, 1); // release the child (or tell it to die)
    }
    ::close(sync_c2p[0]);
    ::close(sync_p2c[1]);
    if (!prepared) {
        ::close(sv[0]);
        ::close(sv[1]);
        link.pid = pid; // let teardown reap the child that is now exiting
        error = "could not prepare the sandboxed child (id-map or cgroup move failed)";
        return false;
    }
    ::close(sv[1]); // the parent never uses the child end
    link.pid = pid;
    link.channel = std::make_unique<Channel>(sv[0]);

    Incoming hello;
    if (!wait_for_hello(*link.channel, hello, kHandshakeTimeoutMs)) {
        error = "child did not complete the handshake";
        // A silent child is alive and says nothing; a child that refused before exec is
        // already gone and carries its reason in its exit code. Distinguish them, so a
        // descriptor-hygiene refusal reads as itself rather than as a mystery timeout.
        const int code = reap_exit_code(link.pid, 50); // ~100 ms, only on this path
        if (code >= 0) {
            link.pid = -1; // reaped here; teardown must not wait on it again
        }
        if (code == kExitFdHygieneFailed) {
            error = "descriptor hygiene could not be established at the exec boundary "
                    "(no close_range(2) on this kernel and no bounded RLIMIT_NOFILE to "
                    "enumerate), so the spawn refused rather than exec a child still "
                    "holding this host's descriptors";
        }
        return false; // caller tears down
    }

    Cursor cursor(hello.payload);
    std::string_view m;
    std::string_view p;
    std::string_view s;
    if (!cursor.bytes(m) || !cursor.bytes(p) || !cursor.bytes(s)) {
        error = "malformed Hello frame";
        return false;
    }
    if (manifest != nullptr) {
        *manifest = std::string(m);
    }
    if (policy != nullptr) {
        *policy = std::string(p);
    }
    if (snapshot != nullptr) {
        *snapshot = std::string(s);
    }

    // Part 3: positively confirm the sandbox actually took before any "contained"
    // claim rests on it — the child is provably in a network namespace distinct from
    // the host (inferred → verified). A failure here fails safe: the caller tears the
    // child down and the mount refuses, so there is no path where a Weave runs while
    // its status claims contained-but-the-namespace-was-not-entered. This also closes
    // the probe-passed-but-real-entry-failed window for free.
    if (sandbox_net && !child_netns_is_isolated(link.pid)) {
        error = "network sandbox not confirmed: child shares the host network namespace";
        return false;
    }
    if (sandbox_fs && !child_mountns_is_isolated(link.pid)) {
        error = "filesystem sandbox not confirmed: child shares the host mount namespace";
        return false;
    }
    if (sandbox_res && !cgroup_confirm(link.cg_leaf, link.pid, link.cg_caps)) {
        error = "resource sandbox not confirmed: child not in its cgroup leaf or limits not applied";
        return false;
    }
    for (CapabilityResolution& r : link.resolutions) {
        if (r.outcome != CapabilityResolution::Outcome::Enforced) {
            continue;
        }
        if ((r.capability == Capability::Network && sandbox_net) ||
            (r.capability == Capability::Filesystem && sandbox_fs) ||
            (r.capability == Capability::Resources && sandbox_res)) {
            r.confirmed = true;
        }
    }
    return true;
}

void IsolationHost::reconstruct_and_cache(Link& link, const std::string& manifest,
                                          const std::string& policy, const std::string& snapshot) {
    // Manifest -> the child's accept-set + state schema, re-admitted through the
    // gate against the kernel's meta-schema and reconstructed exactly as a loaded
    // library's manifest is.
    loom::Unverified um = loom::parse(manifest);
    loom::Admission am = loom::admit(um, loom::manifest_schema());
    if (!am.ok()) {
        throw std::runtime_error("manifest refused: " + am.first_error().message());
    }
    const loom::Value& mv = am.value();

    std::vector<std::shared_ptr<const Schema>> accept;
    for (const loom::Cell& c : mv.get("accepted")->as_list()) {
        accept.push_back(loom::decode_schema(*c.as_message(), registry_));
    }
    auto state = loom::decode_schema(*mv.get("state")->as_message(), registry_);
    // One transaction for the child's whole vocabulary (BL-0): cross-mount
    // agreement on (name, version) as before, but a disagreement about the last
    // door now leaves none of the earlier ones claimed — and the claim belongs to
    // the mount, so a handshake refused below releases it on the way out.
    std::vector<std::shared_ptr<const Schema>> vocabulary = accept;
    vocabulary.push_back(state);
    registry_.claim(link.schemas, vocabulary);
    link.accept = std::move(accept);
    link.state_schema = state;

    // The ask: requested-capabilities is *advice* (conformance data the host reads
    // to know what to surface), not authority. Decode and record it; the grant is
    // decided by the floor-factory + grant-record, never by this declaration. Absent
    // (the floor case) leaves requested_caps empty.
    if (const loom::Cell* rq = mv.get("requests")) {
        link.requested_caps = loom::decode_capability_ask(*rq->as_message());
    }

    // Policy -> cached, validated against the fixed lifecycle grammar.
    loom::Unverified up = loom::parse(policy);
    loom::Admission ap = loom::admit(up, loom::lifecycle_policy_schema());
    if (!ap.ok()) {
        throw std::runtime_error("policy refused: " + ap.first_error().message());
    }
    link.policy_value = std::move(ap).value();

    // Initial snapshot -> cached as host-owned last-known-good (value + bytes).
    loom::Unverified us = loom::parse(snapshot);
    loom::Admission as = loom::admit(us, link.state_schema);
    if (!as.ok()) {
        throw std::runtime_error("snapshot refused: " + as.first_error().message());
    }
    link.snapshot_value = std::move(as).value();
    link.snapshot_bytes = snapshot;
}

std::optional<loom::CapabilityAsk>
IsolationHost::declared_ask(const std::string& name) const {
    auto it = links_.find(name);
    if (it == links_.end()) {
        return std::nullopt;
    }
    return it->second->requested_caps;
}

OutOfProcessResult IsolationHost::mount_mod(const std::string& name, const std::string& so_path) {
    std::string hash;
    try {
        hash = so_content_hash(so_path);
    } catch (const std::exception& e) {
        return {false, {}, e.what()};
    }
    // The floor: minimal authority (no network, FsAccess::None, bounded resources,
    // empty sends) plus the one reach that makes a mod useful on messages alone — a
    // send-rule to the storage broker role. Least-privilege: storage shapes only.
    loom::Grant grant;
    grant.allow_to_role(kStoragePut, kStorageProtocolVersion, kStorageRole);
    grant.allow_to_role(kStorageGet, kStorageProtocolVersion, kStorageRole);

    // Apply any host-recorded delta for this identity. The *ask* is NOT consulted
    // here: only what the host has written into the grant-record raises a Weave above
    // the floor. A declaration is never a grant.
    const GrantDelta delta = grant_record_.lookup(hash);
    if (delta.network) {
        grant.with_os_capabilities(loom::os_cap::Network);
    }
    if (!delta.filesystem.empty()) {
        loom::FsAccess level = loom::FsAccess::None;
        if (delta.filesystem == "read-only") {
            level = loom::FsAccess::ReadOnly;
        } else if (delta.filesystem == "write-scoped") {
            level = loom::FsAccess::WriteScoped;
        } else if (delta.filesystem == "write-no-exec") {
            level = loom::FsAccess::WriteNoExec;
        } else if (delta.filesystem == "write-anywhere") {
            level = loom::FsAccess::WriteAnywhere;
        }
        grant.with_filesystem(level);
    }
    // Roles above the floor: the floor already grants the storage role to all; the delta
    // may add others. v1 wires the well-known "net" role explicitly (a general
    // role->protocol registry is a deferred refinement). Network is dangerous, so —
    // unlike storage — NO mod reaches the net broker without this recorded delta, and
    // even then it gets only the role send-rule, never os_cap::Network: it stays
    // OS-network-denied and reaches the network solely through the broker.
    for (const std::string& role : delta.roles) {
        if (role == kNetRole) {
            grant.allow_to_role(kNetRequest, kNetProtocolVersion, kNetRole);
        }
        // kStorageRole is already granted by the floor; a role the host doesn't know how
        // to wire is ignored (the deferred role->protocol registry).
    }
    return mount(name, so_path, std::move(grant));
}

void IsolationHost::set_grant_record_path(const std::string& path) { grant_record_.load(path); }

void IsolationHost::record_grant_delta(const std::string& content_hash, GrantDelta delta) {
    grant_record_.record(content_hash, std::move(delta));
}

OutOfProcessResult IsolationHost::mount_broker(const std::string& name, const std::string& so_path,
                                               const std::string& storage_root) {
    if (storage_root.empty() || !ensure_host_dir(storage_root)) {
        return {false, {}, "storage root unavailable: '" + storage_root + "'"};
    }
    // TCB-tier grant: WriteScoped to storage_root ONLY (the persistent bind — disk, but
    // contained to that one dir), plus the authority to reply StorageValue to any mod.
    // Registered under role "storage" so floored mods reach it by role-addressing.
    loom::Grant grant;
    grant.with_filesystem(loom::FsAccess::WriteScoped, storage_root);
    grant.allow_to_any(kStorageValue, kStorageProtocolVersion);
    return mount(name, so_path, std::move(grant), kStorageRole);
}

OutOfProcessResult IsolationHost::mount_net_broker(const std::string& name,
                                                   const std::string& so_path) {
    // TCB-tier grant: os_cap::Network (no netns — the real host network), FsAccess::None
    // (no disk), bounded resources, permitted to reply NetResponse to any mod, role "net".
    // Per-destination scoping is the broker's own software allow-list, NOT OS-enforced —
    // the higher-trust broker. No mod reaches "net" without a recorded delta.
    loom::Grant grant;
    grant.with_os_capabilities(loom::os_cap::Network);
    grant.allow_to_any(kNetResponse, kNetProtocolVersion);
    return mount(name, so_path, std::move(grant), kNetRole);
}

bool IsolationHost::reload(const std::string& name) {
    auto it = links_.find(name);
    if (it == links_.end() || !it->second->snapshot_value) {
        return false;
    }
    Link& link = *it->second;
    // Re-spawn a fresh child from the same .so and re-revive from the host-owned
    // snapshot. The bus registration (WeaveId, grant, role) is untouched, so routing
    // and role-addressing survive; a broker's on-disk data is durable regardless.
    respawn_and_revive(link, *link.snapshot_value);
    return link.channel != nullptr;
}

OutOfProcessResult IsolationHost::mount(const std::string& name, const std::string& so_path,
                                        loom::Grant grant, const std::string& role) {
    if (links_.count(name) != 0) {
        return {false, {}, "already mounted: " + name};
    }
    auto link = std::make_unique<Link>();
    link->name = name;
    link->so_path = so_path;

    // B3: resolve each OS-capability from the grant and what this host can actually
    // enforce, recording a per-capability outcome (one today — Network; B4 resolves
    // more here, same shape). The default grant withholds Network, so the default is a
    // sandboxed child — minimal authority, safe by default.
    {
        using Outcome = CapabilityResolution::Outcome;
        CapabilityResolution net;
        net.capability = Capability::Network;
        if (grant.has_os_capability(loom::os_cap::Network)) {
            net.outcome = Outcome::Granted; // real power, granted on purpose
        } else if (enforcement_.enforceable(Capability::Network)) {
            net.outcome = Outcome::Enforced; // no-interface namespace; confirmed post-spawn
        } else if (dev_mode_) {
            net.outcome = Outcome::Uncontained;
            std::fprintf(stderr,
                         "[zen-isolation] WARNING: '%s' mounted network-UNCONTAINED — this host "
                         "cannot enforce network isolation and dev-mode is on. The Weave can "
                         "reach the network despite withholding the Network grant.\n",
                         name.c_str());
        } else {
            return {false,
                    {},
                    "refused (fail-safe): cannot enforce network isolation for '" + name +
                        "' on this host (no unprivileged network namespace). Grant "
                        "os_cap::Network to allow the network intentionally, or enable dev-mode "
                        "to run it network-uncontained."};
        }
        link->resolutions.push_back(net);
    }
    {
        using Outcome = CapabilityResolution::Outcome;
        CapabilityResolution fs;
        fs.capability = Capability::Filesystem;
        const loom::FsAccess level = grant.filesystem();
        fs.note = loom::fs_access_name(level);
        if (level == loom::FsAccess::WriteScoped && !grant.filesystem_path().empty()) {
            // The persistent-scoped-write extension: a writable bind to one host dir
            // that SURVIVES (vs the ephemeral tmpfs). Honest about its limit — a broker
            // keying by the ephemeral sender is session-scoped, not save-across-restart.
            fs.note = "write-scoped (persistent storage bind; session-scoped: keyed by the "
                      "ephemeral sender, not save-across-restart)";
        }
        if (level == loom::FsAccess::WriteAnywhere) {
            fs.outcome = Outcome::Granted; // the opt-out: unrestricted host fs, by grant
        } else if (enforcement_.enforceable(Capability::Filesystem)) {
            fs.outcome = Outcome::Enforced; // None/ReadOnly/WriteScoped/WriteNoExec → mount-ns view
            char tmpl[] = "/tmp/zen-sb-XXXXXX";
            const char* root = ::mkdtemp(tmpl);
            if (root == nullptr) {
                return {false, {},
                        "filesystem sandbox: could not create a view root for '" + name + "'"};
            }
            link->fs_root = root;
            link->fs_plan =
                build_view_plan(level, grant.filesystem_path(), exe_, so_path, link->fs_root);
        } else if (dev_mode_) {
            fs.outcome = Outcome::Uncontained;
            std::fprintf(stderr,
                         "[zen-isolation] WARNING: '%s' mounted filesystem-UNCONTAINED — this host "
                         "cannot enforce filesystem isolation and dev-mode is on. The Weave can "
                         "read and write the host filesystem despite a restricted grant.\n",
                         name.c_str());
        } else {
            return {false, {},
                    "refused (fail-safe): cannot enforce filesystem isolation for '" + name +
                        "' on this host (no unprivileged mount namespace). Grant "
                        "FsAccess::WriteAnywhere to opt out intentionally, or enable dev-mode to "
                        "run it filesystem-uncontained."};
        }
        link->resolutions.push_back(fs);
    }
    {
        using Outcome = CapabilityResolution::Outcome;
        CapabilityResolution rc;
        rc.capability = Capability::Resources;
        const loom::ResourceLimits& lim = grant.resources();
        if (enforcement_.enforceable(Capability::Resources)) {
            // Always Enforced when any resource controller is delegated: a leaf bounded by
            // whatever controllers are present. There is no wholesale opt-out — no grant can
            // license a fork bomb where the pids controller is delegated. The memory cap may be
            // opted out of (unlimited_memory); memory, pids, and cpu are each imposed only where
            // their controller is delegated, and the note/attestation say so per dimension.
            // (cgroups unavailable -> Uncontained in dev-mode, else refuse — never Granted.)
            rc.outcome = Outcome::Enforced;
            ResourceCaps caps = cgroup_default_caps(); // conservative, computed from host
            if (lim.unlimited_memory) {
                caps.memory_max = -1; // uncap memory: create_leaf leaves memory.max at "max"
            } else if (lim.memory_bytes > 0) {
                caps.memory_max = lim.memory_bytes;
            }
            if (lim.pids > 0) {
                caps.pids_max = lim.pids; // raise the fork-bomb stop; never removes it
            }
            if (lim.cpu_weight > 0) {
                caps.cpu_weight = lim.cpu_weight;
            }
            link->cg_caps = caps;
            link->cg_leaf = "zen-weave-" + std::to_string(g_leaf_counter++);
            // Built from what a leaf will ACTUALLY impose, not from the computed caps: where a
            // controller is not delegated, create_leaf never writes that dimension's cap, so
            // the note must say it is uncapped, not claim a cap it never set (audit F-20 — the
            // honesty lattice's one absolute rule — applied to BOTH memory and pids).
            rc.note = resource_note(caps, cgroup_memory_available(), cgroup_pids_available());
        } else if (dev_mode_) {
            rc.outcome = Outcome::Uncontained;
            std::fprintf(stderr,
                         "[zen-isolation] WARNING: '%s' mounted resource-UNCONTAINED — this host "
                         "cannot enforce cgroup-v2 limits (no delegated subtree) and dev-mode is "
                         "on. The Weave can exhaust host memory/pids/cpu.\n",
                         name.c_str());
        } else {
            return {false, {},
                    "refused (fail-safe): cannot enforce resource limits for '" + name +
                        "' on this host (no cgroup-v2 delegation — run the host under a delegated "
                        "scope, or enable dev-mode to run it resource-uncontained)."};
        }
        link->resolutions.push_back(rc);
    }

    std::string manifest;
    std::string policy;
    std::string snapshot;
    std::string error;
    if (!spawn_and_handshake(*link, &manifest, &policy, &snapshot, error)) {
        teardown_child(*link);
        return {false, {}, "spawn/handshake failed: " + error};
    }
    try {
        reconstruct_and_cache(*link, manifest, policy, snapshot);
    } catch (const std::exception& e) {
        teardown_child(*link);
        return {false, {}, std::string("handshake refused by the gate: ") + e.what()};
    }

    auto proxy = std::make_unique<OutOfProcessWeave>(this, link.get());
    OutOfProcessWeave* raw = proxy.get();
    loom::WeaveId id;
    try {
        id = bus_.register_weave(std::move(proxy), std::move(grant), role);
    } catch (const std::exception& e) {
        teardown_child(*link);
        return {false, {}, std::string("register refused: ") + e.what()};
    }
    link->id = id;
    link->proxy = raw;
    links_.emplace(name, std::move(link));
    return {true, id, ""};
}

void IsolationHost::ship_deliver(Link& link, const loom::Message& in) {
    if (!link.channel) {
        return; // no live child (dead/quarantined); the delivery is dropped
    }
    const std::string bytes = loom::serialize(in.payload);
    std::string frame;
    put_u64(frame, in.sender.value);
    put_u64(frame, in.reply_to.value);
    put_u64(frame, in.correlation);
    frame.append(bytes);
    link.channel->queue(Op::Deliver, frame); // flushed on the next step()
}

void IsolationHost::respawn_and_revive(Link& link, const loom::Value& state) {
    if (link.channel) {
        teardown_child(link); // a stale child should not exist here; be safe
    }
    std::string error;
    if (!spawn_and_handshake(link, nullptr, nullptr, nullptr, error)) {
        teardown_child(link); // spawn failed; recover() detects via a null channel
        return;
    }
    const std::string bytes = loom::serialize(state);
    link.channel->queue(Op::Revive, bytes); // child applies it, then ships a Snapshot
}

void IsolationHost::handle_child_frame(Link& link, const Incoming& frame) {
    if (frame.op == Op::Emit) {
        Cursor cursor(frame.payload);
        std::uint8_t kind = 0;
        std::uint64_t target = 0;
        std::uint64_t reply_to = 0;
        std::uint64_t correlation = 0;
        if (!cursor.u8(kind) || !cursor.u64(target) || !cursor.u64(reply_to) ||
            !cursor.u64(correlation)) {
            return; // malformed Emit header -> drop
        }
        const std::string_view payload = cursor.rest();

        // Re-admit the child's output through the one gate, host-side, exactly as
        // the kernel does for a loaded library's emitted message.
        loom::Unverified u = loom::parse(payload);
        std::shared_ptr<const Schema> door = bus_.resolve_schema(u.claimed_name(), u.claimed_version());
        if (!door) {
            return; // a schema the system does not know -> drop (cannot be gated)
        }
        loom::Admission a = loom::admit(u, door);
        if (!a.ok()) {
            return; // gate-refused (malformed/hostile child output) -> drop
        }
        // The sender is stamped from the connection (link.id), never from the
        // payload — the child has no way to express a sender. send_as/publish_as
        // then authorize against this Weave's grant at delivery (CapabilityDenied
        // on a violation), identical to the in-process WeaveBus path.
        loom::Message msg(std::move(a).value(), loom::WeaveId{}, loom::WeaveId{reply_to},
                             correlation);
        if (kind == kEmitPublish) {
            (void)bus_.publish_as(link.id, std::move(msg));
        } else {
            (void)bus_.send_as(link.id, loom::WeaveId{target}, std::move(msg));
        }
        return;
    }

    if (frame.op == Op::EmitRole) {
        Cursor cursor(frame.payload);
        std::string_view role;
        std::uint64_t reply_to = 0;
        std::uint64_t correlation = 0;
        if (!cursor.bytes(role) || !cursor.u64(reply_to) || !cursor.u64(correlation)) {
            return; // malformed EmitRole header -> drop
        }
        const std::string_view payload = cursor.rest();

        loom::Unverified u = loom::parse(payload);
        std::shared_ptr<const Schema> door =
            bus_.resolve_schema(u.claimed_name(), u.claimed_version());
        if (!door) {
            return; // a schema the system does not know -> drop (cannot be gated)
        }
        loom::Admission a = loom::admit(u, door);
        if (!a.ok()) {
            return; // gate-refused (malformed/hostile child output) -> drop
        }
        // The sender is stamped from the connection (link.id), never from the wire —
        // the EmitRole frame carries no sender field. The reply address of a role-send
        // (a request to a broker) is ALWAYS the stamped sender: a child-supplied
        // reply_to is ignored, so a mod cannot make a broker reply to another
        // (guessable) WeaveId — a confused deputy that would reintroduce a sender-like
        // field a mod could fiddle with. send_as_to_role then authorizes by role at
        // delivery (Part A).
        (void)reply_to; // reserved in the frame; not trusted for routing a role-send reply
        loom::Message msg(std::move(a).value(), loom::WeaveId{}, link.id, correlation);
        (void)bus_.send_as_to_role(link.id, std::string(role), std::move(msg));
        return;
    }

    if (frame.op == Op::Snapshot) {
        // A fresh post-handle/post-revive snapshot. Admit it host-side; on success
        // it becomes the host-owned last-known-good. A malformed snapshot is
        // ignored — the previous good one stands.
        loom::Unverified u = loom::parse(frame.payload);
        loom::Admission a = loom::admit(u, link.state_schema);
        if (!a.ok()) {
            return;
        }
        link.snapshot_value = std::move(a).value();
        link.snapshot_bytes = frame.payload;
        return;
    }
    // Hello mid-stream or unknown ops: ignored.
}

void IsolationHost::recover(Link& link) {
    // The child is dead. Reap it, then drive bounded reload from the host-owned
    // snapshot. reload() checks/decrements the policy's max_reloads and, when the
    // budget allows, calls proxy->revive() — which respawns a fresh child and ships
    // the state. When the budget is exhausted, revive() is never called and the
    // Weave stays dead: quarantine.
    teardown_child(link);
    const loom::ReviveOutcome ro = bus_.reload(link.id, link.snapshot_bytes);
    if (ro.revived && link.channel) {
        link.dead = false;
        link.death_signaled = false;
    } else {
        link.quarantined = true;
        link.dead = true;
        teardown_child(link); // ensure no child process lingers
    }
}

void IsolationHost::teardown_child(Link& link) {
    if (link.channel && !link.channel->done()) {
        link.channel->queue(Op::Shutdown, "");
        link.channel->flush(); // best-effort clean stop
    }
    link.channel.reset(); // closes the fd

    if (link.pid > 0) {
        // Give a clean exit a brief chance (so the child's own sanitizer checks
        // run), then force it. A crashed child is already a zombie and reaps at
        // once.
        bool reaped = false;
        for (int i = 0; i < 200; ++i) {
            int status = 0;
            const pid_t r = ::waitpid(link.pid, &status, WNOHANG);
            if (r == link.pid || r < 0) {
                reaped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!reaped) {
            ::kill(link.pid, SIGKILL);
            int status = 0;
            (void)::waitpid(link.pid, &status, 0);
        }
        link.pid = -1;
    }

    // Remove the host-side (empty) view-root mountpoint; a respawn re-creates it
    // before launch, so this is safe to do on every teardown.
    if (!link.fs_root.empty()) {
        (void)::rmdir(link.fs_root.c_str());
    }
    // Remove the cgroup leaf now that its process is reaped (rmdir needs it empty); a
    // respawn re-creates it before launch.
    if (!link.cg_leaf.empty()) {
        cgroup_remove_leaf(link.cg_leaf);
    }
}

void IsolationHost::step() {
    // (1) flush queued output to each child, then drain its input (re-enqueue
    //     emitted messages gated, refresh cached snapshots).
    for (auto& entry : links_) {
        Link& link = *entry.second;
        if (!link.channel) {
            continue;
        }
        link.channel->flush();
        std::vector<Incoming> frames;
        link.channel->poll(frames);
        for (const Incoming& f : frames) {
            handle_child_frame(link, f);
        }
    }

    // (2) pump the bus — proxies fire-and-continue, so this never blocks on a child.
    bus_.pump();

    // (3) supervise: detect deaths, drive bounded reload-then-quarantine.
    for (auto& entry : links_) {
        Link& link = *entry.second;
        if (link.quarantined) {
            continue;
        }
        const bool dead_now = !link.channel || link.channel->done();
        if (dead_now && !link.death_signaled) {
            link.dead = true;
            link.death_signaled = true;
            bus_.kill(link.id); // mark dead on the bus + emit Died (once per death)
        }
        if (link.dead) {
            recover(link);
        }
    }
}

void IsolationHost::unmount(const std::string& name) {
    auto it = links_.find(name);
    if (it == links_.end()) {
        return;
    }
    Link& link = *it->second;
    // Drop the proxy from the bus first (so no further delivery lands on it), then
    // stop the child. The proxy holds a Link* so it must die before the Link.
    std::unique_ptr<loom::Weave> proxy = bus_.unregister_weave(link.id);
    teardown_child(link);
    proxy.reset();
    links_.erase(it);
}

bool IsolationHost::is_mounted(const std::string& name) const {
    return links_.count(name) != 0;
}

bool IsolationHost::quarantined(const std::string& name) const {
    auto it = links_.find(name);
    return it != links_.end() && it->second->quarantined;
}

bool IsolationHost::network_sandboxed(const Link& link) {
    for (const CapabilityResolution& r : link.resolutions) {
        if (r.capability == Capability::Network) {
            return r.outcome == CapabilityResolution::Outcome::Enforced;
        }
    }
    return false;
}

bool IsolationHost::filesystem_sandboxed(const Link& link) {
    for (const CapabilityResolution& r : link.resolutions) {
        if (r.capability == Capability::Filesystem) {
            return r.outcome == CapabilityResolution::Outcome::Enforced;
        }
    }
    return false;
}

bool IsolationHost::resources_contained(const Link& link) {
    for (const CapabilityResolution& r : link.resolutions) {
        if (r.capability == Capability::Resources) {
            return r.outcome == CapabilityResolution::Outcome::Enforced;
        }
    }
    return false;
}

std::string IsolationHost::containment(const std::string& name) const {
    auto it = links_.find(name);
    if (it == links_.end()) {
        return "not mounted";
    }
    const Link& link = *it->second;

    const std::string head =
        link.quarantined
            ? "isolated (process boundary); quarantined: dead after exhausting reloads."
            : "isolated (process boundary): crash-contained, cannot corrupt host memory.";

    // Generated from what was ACTUALLY imposed, iterated per capability — never a
    // hardcoded single-capability claim. B4's second capability needs no change here.
    std::string body;
    for (const CapabilityResolution& r : link.resolutions) {
        body += " " + describe_resolution(r) + ".";
    }
    return head + body + " syscalls: not enforced (seccomp is a separate, later decision).";
}

} // namespace loom
