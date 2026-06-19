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
#include <spawn.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace zen::isolation {

namespace {

constexpr int kChildFd = 3;            // the child reads its socket from this fd
constexpr int kHandshakeTimeoutMs = 5000;

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

// Process-global unique suffix for per-Shard cgroup leaf names (hosts share the base).
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

// The allow-list view for `level`: private-first, a tmpfs root, the loader closure
// and the exe/.so dirs bound read-only, a scratch tmpfs for the write levels, then
// pivot_root into it. Nothing of the host home is bound, so secrets are absent.
MountPlan build_view_plan(zen::sb::FsAccess level, const std::string& scoped_path,
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
    if (level == zen::sb::FsAccess::ReadOnly && !scoped_path.empty()) {
        bind_ro_ops(p, root, scoped_path); // the granted tree, read-only
    }
    if (level == zen::sb::FsAccess::WriteScoped || level == zen::sb::FsAccess::WriteNoExec) {
        const unsigned long f = (level == zen::sb::FsAccess::WriteNoExec) ? MS_NOEXEC : 0;
        p.push_back({MountOp::Kind::Mkdir, root + "/scratch", "", "", 0});
        p.push_back({MountOp::Kind::Mount, "tmpfs", root + "/scratch", "tmpfs", f});
    }

    // The view root itself is now read-only (its mountpoints were created above), so a
    // Shard cannot write — or plant-and-exec — at "/"; only the scratch submount (if
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
                return std::string(
                           "network: contained — private user+net namespace, no external "
                           "interface, so new outbound connections fail at the syscall level") +
                       (r.confirmed ? " (confirmed: child netns distinct from host)" : "") +
                       "; the inherited host-control fd and filesystem-path sockets remain by "
                       "design, so 'contained' means no external reachability, not no-IPC";
            case Outcome::Granted:
                return "network: granted — child shares the host network by the grant (not "
                       "contained)";
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
                return std::string("resources: contained (cgroup-v2: ") + r.note + ")" +
                       (r.confirmed ? " (confirmed: pid in leaf, limits read back)" : "") +
                       "; honest scope: a memory cap OOM-kills within the cgroup (the host "
                       "survives and reloads-then-quarantines), pids.max stops a fork-bomb; "
                       "cpu.weight is a fair-share weight (set-and-confirmed where the cpu "
                       "controller is delegated, absent otherwise), not a hard cap";
            case Outcome::Granted:
                return "resources: unlimited — no limits, by grant (not contained)";
            case Outcome::Uncontained:
                return "resources: NOT CONTAINED — requested but unenforceable on this host, "
                       "running under dev-mode override";
        }
    }
    return std::string(capability_name(r.capability)) + ": (unrecognized)";
}

} // namespace

// ---- the proxy: a Shard, on the bus, backed by a child process ---------------
//
// To the Switchboard this is an ordinary Shard. handle() serializes and ships the
// message to the child and returns at once (fire-and-continue — a slow or hung
// child never blocks the bus). snapshot()/policy() return the host-owned cached
// values refreshed from the child's proactive Snapshot frames. revive() (re)spawns
// the child if dead and ships the state.
class OutOfProcessShard final : public zen::sb::Shard {
public:
    OutOfProcessShard(IsolationHost* host, IsolationHost::Link* link) : host_(host), link_(link) {}

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return link_->accept;
    }
    void handle(const zen::sb::Message& in, zen::sb::Bus& /*bus*/) override {
        host_->ship_deliver(*link_, in);
    }
    zen::Value snapshot() const override { return *link_->snapshot_value; }
    zen::Value policy() const override { return *link_->policy_value; }
    void revive(const zen::Value& state) override { host_->respawn_and_revive(*link_, state); }

private:
    IsolationHost* host_;
    IsolationHost::Link* link_;
};

// ---- IsolationHost -----------------------------------------------------------

IsolationHost::IsolationHost(zen::sb::Switchboard& bus, std::string shard_host_exe)
    : bus_(bus), exe_(std::move(shard_host_exe)), enforcement_(detect_enforcement()) {}

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
    // The parent end gets CLOEXEC so it is not inherited by *other* children we
    // later spawn; the child end is placed at a known fd via dup2 (which clears
    // CLOEXEC, so it survives this child's exec).
    (void)::fcntl(sv[0], F_SETFD, FD_CLOEXEC);

    const std::string fd_arg = std::to_string(kChildFd);
    char* argv[] = {const_cast<char*>(exe_.c_str()), const_cast<char*>(fd_arg.c_str()),
                    const_cast<char*>(link.so_path.c_str()), nullptr};

    const bool sandbox_net = network_sandboxed(link);
    const bool sandbox_fs = filesystem_sandboxed(link);
    const bool sandbox_res = resources_contained(link);
    const bool needs_userns = sandbox_net || sandbox_fs;
    const bool sandboxed = needs_userns || sandbox_res;

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

    pid_t pid = -1;
    if (sandboxed) {
        // B3/B4: launch the child into a network and/or mount namespace. posix_spawn
        // cannot unshare, so we fork. This host refuses a child's self-map (EPERM), so
        // the PARENT writes the child's uid/gid maps over a pipe handshake: the child
        // unshares and signals, the parent maps it and releases it, then the child
        // builds its restricted view and execs. Everything the child does is
        // async-signal-safe (the mount plan was precomputed); the parent's map-writing
        // uses ordinary libc. The socket dance mirrors the posix_spawn branch.
        int sync_c2p[2];
        int sync_p2c[2];
        if (::pipe(sync_c2p) != 0 || ::pipe(sync_p2c) != 0) {
            ::close(sv[0]);
            ::close(sv[1]);
            error = "sandbox sync pipe failed";
            return false;
        }
        pid = ::fork();
        if (pid == 0) {
            ::close(sync_c2p[0]);
            ::close(sync_p2c[1]);
            // force_entry_failure_ simulates a *surprise* entry failure: the child dies
            // before exec exactly as a true unshare()/mount()/cgroup failure would, so the
            // handshake fails and the mount refuses — strict and dev mode alike.
            if (force_entry_failure_) {
                ::_exit(127);
            }
            if (needs_userns && unshare_isolation(sandbox_net, sandbox_fs) != 0) {
                ::_exit(127);
            }
            char one = 1;
            (void)!::write(sync_c2p[1], &one, 1); // "I have unshared"
            char go = 0;
            (void)!::read(sync_p2c[0], &go, 1); // wait to be mapped by the parent
            ::close(sync_c2p[1]);
            ::close(sync_p2c[0]);
            if (go != 1 || (sandbox_fs && run_mount_plan(link.fs_plan) != 0)) {
                ::_exit(127); // not mapped, or the restricted view could not be built
            }
            if (sv[1] != kChildFd && ::dup2(sv[1], kChildFd) < 0) {
                ::_exit(127);
            }
            if (sv[0] != kChildFd) {
                ::close(sv[0]);
            }
            if (sv[1] != kChildFd) {
                ::close(sv[1]);
            }
            ::execve(exe_.c_str(), argv, environ);
            ::_exit(127); // exec failed
        }
        ::close(sync_c2p[1]);
        ::close(sync_p2c[0]);
        if (pid < 0) {
            ::close(sync_c2p[0]);
            ::close(sync_p2c[1]);
            ::close(sv[0]);
            ::close(sv[1]);
            error = "fork (sandboxed spawn) failed";
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
    } else {
        // Granted (or dev-mode uncontained): the original B2 spawn, unchanged.
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, sv[1], kChildFd);
        // Close the original ends in the child — but never the fd we just dup'd onto
        // (socketpair may hand back kChildFd itself, and closing it would leave the
        // child with no socket).
        if (sv[0] != kChildFd) {
            posix_spawn_file_actions_addclose(&fa, sv[0]);
        }
        if (sv[1] != kChildFd) {
            posix_spawn_file_actions_addclose(&fa, sv[1]);
        }
        const int rc = ::posix_spawn(&pid, exe_.c_str(), &fa, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) {
            ::close(sv[0]);
            ::close(sv[1]);
            error = "posix_spawn failed";
            return false;
        }
    }
    ::close(sv[1]); // the parent never uses the child end
    link.pid = pid;
    link.channel = std::make_unique<Channel>(sv[0]);

    Incoming hello;
    if (!wait_for_hello(*link.channel, hello, kHandshakeTimeoutMs)) {
        error = "child did not complete the handshake";
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
    // child down and the mount refuses, so there is no path where a Shard runs while
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
    zen::Unverified um = zen::parse(manifest);
    zen::Admission am = zen::admit(um, zen::kernel::manifest_schema());
    if (!am.ok()) {
        throw std::runtime_error("manifest refused: " + am.first_error().message());
    }
    const zen::Value& mv = am.value();

    std::vector<std::shared_ptr<const Schema>> accept;
    for (const zen::Cell& c : mv.get("accepted")->as_list()) {
        auto s = zen::kernel::decode_schema(*c.as_message(), registry_);
        registry_.register_schema(s); // cross-mount agreement on (name, version)
        accept.push_back(std::move(s));
    }
    auto state = zen::kernel::decode_schema(*mv.get("state")->as_message(), registry_);
    registry_.register_schema(state);
    link.accept = std::move(accept);
    link.state_schema = state;

    // Policy -> cached, validated against the fixed lifecycle grammar.
    zen::Unverified up = zen::parse(policy);
    zen::Admission ap = zen::admit(up, zen::sb::lifecycle_policy_schema());
    if (!ap.ok()) {
        throw std::runtime_error("policy refused: " + ap.first_error().message());
    }
    link.policy_value = std::move(ap).value();

    // Initial snapshot -> cached as host-owned last-known-good (value + bytes).
    zen::Unverified us = zen::parse(snapshot);
    zen::Admission as = zen::admit(us, link.state_schema);
    if (!as.ok()) {
        throw std::runtime_error("snapshot refused: " + as.first_error().message());
    }
    link.snapshot_value = std::move(as).value();
    link.snapshot_bytes = snapshot;
}

OutOfProcessResult IsolationHost::mount(const std::string& name, const std::string& so_path,
                                        zen::sb::Grant grant) {
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
        if (grant.has_os_capability(zen::sb::os_cap::Network)) {
            net.outcome = Outcome::Granted; // real power, granted on purpose
        } else if (enforcement_.enforceable(Capability::Network)) {
            net.outcome = Outcome::Enforced; // no-interface namespace; confirmed post-spawn
        } else if (dev_mode_) {
            net.outcome = Outcome::Uncontained;
            std::fprintf(stderr,
                         "[zen-isolation] WARNING: '%s' mounted network-UNCONTAINED — this host "
                         "cannot enforce network isolation and dev-mode is on. The Shard can "
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
        const zen::sb::FsAccess level = grant.filesystem();
        fs.note = zen::sb::fs_access_name(level);
        if (level == zen::sb::FsAccess::WriteAnywhere) {
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
                         "cannot enforce filesystem isolation and dev-mode is on. The Shard can "
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
        const zen::sb::ResourceLimits& lim = grant.resources();
        if (lim.unlimited) {
            rc.outcome = Outcome::Granted; // the opt-out: no limits, by grant
        } else if (enforcement_.enforceable(Capability::Resources)) {
            rc.outcome = Outcome::Enforced;
            ResourceCaps caps = cgroup_default_caps(); // conservative, computed from host
            if (lim.memory_bytes > 0) {
                caps.memory_max = lim.memory_bytes;
            }
            if (lim.pids > 0) {
                caps.pids_max = lim.pids;
            }
            if (lim.cpu_weight > 0) {
                caps.cpu_weight = lim.cpu_weight;
            }
            link->cg_caps = caps;
            link->cg_leaf = "zen-shard-" + std::to_string(g_leaf_counter++);
            rc.note = "memory<=" + std::to_string(caps.memory_max / (1024 * 1024)) +
                      "MiB, pids<=" + std::to_string(caps.pids_max);
        } else if (dev_mode_) {
            rc.outcome = Outcome::Uncontained;
            std::fprintf(stderr,
                         "[zen-isolation] WARNING: '%s' mounted resource-UNCONTAINED — this host "
                         "cannot enforce cgroup-v2 limits (no delegated subtree) and dev-mode is "
                         "on. The Shard can exhaust host memory/pids/cpu.\n",
                         name.c_str());
        } else {
            return {false, {},
                    "refused (fail-safe): cannot enforce resource limits for '" + name +
                        "' on this host (no cgroup-v2 delegation — run the host under a delegated "
                        "scope). Grant unlimited resources to opt out, or enable dev-mode to run "
                        "it resource-uncontained."};
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

    auto proxy = std::make_unique<OutOfProcessShard>(this, link.get());
    OutOfProcessShard* raw = proxy.get();
    zen::sb::ShardId id;
    try {
        id = bus_.register_shard(std::move(proxy), std::move(grant));
    } catch (const std::exception& e) {
        teardown_child(*link);
        return {false, {}, std::string("register refused: ") + e.what()};
    }
    link->id = id;
    link->proxy = raw;
    links_.emplace(name, std::move(link));
    return {true, id, ""};
}

void IsolationHost::ship_deliver(Link& link, const zen::sb::Message& in) {
    if (!link.channel) {
        return; // no live child (dead/quarantined); the delivery is dropped
    }
    const std::string bytes = zen::serialize(in.payload);
    std::string frame;
    put_u64(frame, in.sender.value);
    put_u64(frame, in.reply_to.value);
    put_u64(frame, in.correlation);
    frame.append(bytes);
    link.channel->queue(Op::Deliver, frame); // flushed on the next step()
}

void IsolationHost::respawn_and_revive(Link& link, const zen::Value& state) {
    if (link.channel) {
        teardown_child(link); // a stale child should not exist here; be safe
    }
    std::string error;
    if (!spawn_and_handshake(link, nullptr, nullptr, nullptr, error)) {
        teardown_child(link); // spawn failed; recover() detects via a null channel
        return;
    }
    const std::string bytes = zen::serialize(state);
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
        zen::Unverified u = zen::parse(payload);
        std::shared_ptr<const Schema> door = bus_.resolve_schema(u.claimed_name(), u.claimed_version());
        if (!door) {
            return; // a schema the system does not know -> drop (cannot be gated)
        }
        zen::Admission a = zen::admit(u, door);
        if (!a.ok()) {
            return; // gate-refused (malformed/hostile child output) -> drop
        }
        // The sender is stamped from the connection (link.id), never from the
        // payload — the child has no way to express a sender. send_as/publish_as
        // then authorize against this Shard's grant at delivery (CapabilityDenied
        // on a violation), identical to the in-process ShardBus path.
        zen::sb::Message msg(std::move(a).value(), zen::sb::ShardId{}, zen::sb::ShardId{reply_to},
                             correlation);
        if (kind == kEmitPublish) {
            (void)bus_.publish_as(link.id, std::move(msg));
        } else {
            (void)bus_.send_as(link.id, zen::sb::ShardId{target}, std::move(msg));
        }
        return;
    }

    if (frame.op == Op::Snapshot) {
        // A fresh post-handle/post-revive snapshot. Admit it host-side; on success
        // it becomes the host-owned last-known-good. A malformed snapshot is
        // ignored — the previous good one stands.
        zen::Unverified u = zen::parse(frame.payload);
        zen::Admission a = zen::admit(u, link.state_schema);
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
    // Shard stays dead: quarantine.
    teardown_child(link);
    const zen::sb::ReviveOutcome ro = bus_.reload(link.id, link.snapshot_bytes);
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
    std::unique_ptr<zen::sb::Shard> proxy = bus_.unregister_shard(link.id);
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

} // namespace zen::isolation
