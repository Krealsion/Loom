// B3/B4 enforcement, native and Linux-specific. The detection probe and the
// enforcement share one mechanism (an unprivileged user namespace plus a network
// and/or mount namespace), so the report reflects what we can actually impose.
// Everything that runs between fork() and execve() here is async-signal-safe: raw
// syscalls and reads of precomputed strings only, no allocation.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // unshare(2) + CLONE_NEW* on glibc
#endif

#include <zen/isolation/sandbox.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/magic.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// mount_setattr (kernel 5.12+); glibc 2.35 (Ubuntu 22.04) lacks the wrapper, so we
// call it by number with a locally-defined attr struct.
#ifndef MOUNT_ATTR_RDONLY
#define MOUNT_ATTR_RDONLY 0x00000001
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif
#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442 // x86_64
#endif
namespace {
struct zen_mount_attr {
    unsigned long long attr_set, attr_clr, propagation, userns_fd;
};
} // namespace
#endif

namespace zen::isolation {

const char* capability_name(Capability c) noexcept {
    switch (c) {
        case Capability::Network:
            return "Network";
        case Capability::Filesystem:
            return "Filesystem";
        case Capability::Resources:
            return "Resources";
    }
    return "?";
}

const CapabilityStatus* EnforcementReport::find(Capability c) const noexcept {
    for (const CapabilityStatus& s : capabilities) {
        if (s.capability == c) {
            return &s;
        }
    }
    return nullptr;
}

bool EnforcementReport::enforceable(Capability c) const noexcept {
    const CapabilityStatus* s = find(c);
    return s != nullptr && s->enforceable;
}

#if defined(__linux__)

namespace {

// Async-signal-safe unsigned→decimal. Writes into `buf` (≥20 bytes), returns length.
std::size_t write_uint(char* buf, unsigned long v) noexcept {
    char tmp[20];
    std::size_t n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v > 0) {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    for (std::size_t i = 0; i < n; ++i) {
        buf[i] = tmp[n - 1 - i];
    }
    return n;
}

bool write_proc(const char* path, const char* data, std::size_t len) noexcept {
    const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const ssize_t w = ::write(fd, data, len);
    ::close(fd);
    return w == static_cast<ssize_t>(len);
}

bool write_single_map(const char* path, unsigned long id) noexcept {
    char line[40];
    std::size_t p = 0;
    line[p++] = '0';
    line[p++] = ' ';
    p += write_uint(line + p, id);
    line[p++] = ' ';
    line[p++] = '1';
    line[p++] = '\n';
    return write_proc(path, line, p);
}

bool ns_distinct(pid_t child, const char* kind) noexcept {
    struct stat self_ns {};
    struct stat child_ns {};
    char self_path[64];
    char child_path[64];
    (void)std::snprintf(self_path, sizeof(self_path), "/proc/self/ns/%s", kind);
    (void)std::snprintf(child_path, sizeof(child_path), "/proc/%d/ns/%s", static_cast<int>(child),
                        kind);
    if (::stat(self_path, &self_ns) != 0 || ::stat(child_path, &child_ns) != 0) {
        return false; // cannot compare → fail safe
    }
    return !(self_ns.st_dev == child_ns.st_dev && self_ns.st_ino == child_ns.st_ino);
}

// A minimal restricted view (private → tmpfs root → ro-bind /usr → pivot_root) used
// only to probe whether this host permits unprivileged mount isolation at all.
MountPlan build_probe_plan(const std::string& root) {
    MountPlan p;
    p.push_back({MountOp::Kind::MakeRPrivate, "", "", "", 0});
    p.push_back({MountOp::Kind::Mount, "tmpfs", root, "tmpfs", 0});
    p.push_back({MountOp::Kind::Mkdir, root + "/usr", "", "", 0});
    p.push_back({MountOp::Kind::Mkdir, root + "/oldroot", "", "", 0});
    p.push_back({MountOp::Kind::Mount, "/usr", root + "/usr", "", MS_BIND | MS_REC});
    p.push_back({MountOp::Kind::RemountRO, "", root + "/usr", "", 0});
    p.push_back({MountOp::Kind::PivotRoot, root, root + "/oldroot", "", 0});
    p.push_back({MountOp::Kind::Chdir, "/", "", "", 0});
    p.push_back(
        {MountOp::Kind::Umount, "/oldroot", "", "", static_cast<unsigned long>(MNT_DETACH)});
    return p;
}

// Fork a throwaway child that enters the requested isolation (parent writes its maps
// over a pipe handshake) and optionally runs a mount plan, reporting success via its
// exit code. The very mechanism enforcement uses, so a true result means real
// enforcement. Used only by detection.
bool probe_isolation(bool network, bool filesystem, const MountPlan* plan) noexcept {
    int c2p[2];
    int p2c[2];
    if (::pipe2(c2p, O_CLOEXEC) != 0) {
        return false;
    }
    if (::pipe2(p2c, O_CLOEXEC) != 0) {
        ::close(c2p[0]);
        ::close(c2p[1]);
        return false;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(c2p[0]);
        ::close(p2c[1]);
        if (unshare_isolation(network, filesystem) != 0) {
            ::_exit(11);
        }
        char one = 1;
        (void)!::write(c2p[1], &one, 1); // "I have unshared"
        char go = 0;
        (void)!::read(p2c[0], &go, 1); // wait for the parent to map us
        if (go != 1) {
            ::_exit(12);
        }
        if (filesystem && plan != nullptr && run_mount_plan(*plan) != 0) {
            ::_exit(13);
        }
        ::_exit(0);
    }
    ::close(c2p[1]);
    ::close(p2c[0]);
    if (pid < 0) {
        ::close(c2p[0]);
        ::close(p2c[1]);
        return false;
    }
    char one = 0;
    const bool unshared = (::read(c2p[0], &one, 1) == 1 && one == 1);
    const bool mapped = unshared && write_isolation_id_maps(pid);
    if (unshared) { // avoid SIGPIPE: only release a child that is alive and waiting
        const char go = mapped ? 1 : 2;
        (void)!::write(p2c[1], &go, 1);
    }
    ::close(c2p[0]);
    ::close(p2c[1]);
    int status = 0;
    (void)::waitpid(pid, &status, 0);
    return mapped && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

int unshare_isolation(bool network, bool filesystem) noexcept {
    int flags = CLONE_NEWUSER;
    if (network) {
        flags |= CLONE_NEWNET;
    }
    if (filesystem) {
        flags |= CLONE_NEWNS;
    }
    return (::unshare(flags) == 0) ? 0 : -1;
}

bool write_isolation_id_maps(pid_t child) noexcept {
    char path[64];
    (void)std::snprintf(path, sizeof(path), "/proc/%d/setgroups", static_cast<int>(child));
    (void)write_proc(path, "deny", 4); // best-effort; required before an unprivileged gid_map
    (void)std::snprintf(path, sizeof(path), "/proc/%d/uid_map", static_cast<int>(child));
    if (!write_single_map(path, static_cast<unsigned long>(::getuid()))) {
        return false;
    }
    (void)std::snprintf(path, sizeof(path), "/proc/%d/gid_map", static_cast<int>(child));
    return write_single_map(path, static_cast<unsigned long>(::getgid()));
}

int run_mount_plan(const MountPlan& plan) noexcept {
    for (const MountOp& op : plan) {
        long rc = 0;
        switch (op.kind) {
            case MountOp::Kind::MakeRPrivate:
                rc = ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
                break;
            case MountOp::Kind::Mkdir:
                rc = ::mkdir(op.a.c_str(), 0755);
                if (rc != 0 && errno == EEXIST) {
                    rc = 0; // an existing mountpoint is fine
                }
                break;
            case MountOp::Kind::Mount:
                rc = ::mount(op.a.c_str(), op.b.c_str(),
                             op.fstype.empty() ? nullptr : op.fstype.c_str(), op.flags, nullptr);
                break;
            case MountOp::Kind::RemountRO:
                rc = ::mount(nullptr, op.b.c_str(), nullptr, MS_REMOUNT | MS_BIND | MS_RDONLY,
                             nullptr);
                break;
            case MountOp::Kind::SetattrRecRO: {
                zen_mount_attr attr{};
                attr.attr_set = MOUNT_ATTR_RDONLY;
                const int fd = ::open(op.b.c_str(), O_PATH | O_CLOEXEC);
                if (fd < 0) {
                    rc = -1;
                    break;
                }
                rc = ::syscall(SYS_mount_setattr, fd, "", AT_EMPTY_PATH | AT_RECURSIVE, &attr,
                               sizeof(attr));
                ::close(fd);
                break;
            }
            case MountOp::Kind::PivotRoot:
                rc = ::syscall(SYS_pivot_root, op.a.c_str(), op.b.c_str());
                break;
            case MountOp::Kind::Chdir:
                rc = ::chdir(op.a.c_str());
                break;
            case MountOp::Kind::Umount:
                rc = ::umount2(op.a.c_str(), static_cast<int>(op.flags));
                break;
        }
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

bool child_netns_is_isolated(pid_t child) noexcept {
    return ns_distinct(child, "net");
}

bool child_mountns_is_isolated(pid_t child) noexcept {
    return ns_distinct(child, "mnt");
}

// ---- B5 cgroup-v2 (parent-side, ordinary libc) -----------------------------------

namespace {

bool g_cg_memory = false;
bool g_cg_pids = false;
bool g_cg_cpu = false;

std::string read_text(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_text(const std::string& path, const std::string& data) {
    std::ofstream f(path);
    if (!f) {
        return false;
    }
    f << data;
    return f.good();
}

long long parse_ll(const std::string& s) {
    try {
        return std::stoll(s);
    } catch (...) {
        return -2; // unparseable (e.g. "max") → never equals a concrete cap
    }
}

// Whitespace-delimited token match — so "cpu" does not match "cpuset" in a
// space-separated controllers list.
bool has_token(const std::string& s, const std::string& tok) {
    std::stringstream ss(s);
    std::string t;
    while (ss >> t) {
        if (t == tok) {
            return true;
        }
    }
    return false;
}

// The cgroup-v2 line of /proc/self/cgroup is "0::<relpath>".
std::string self_cgroup_v2_rel() {
    std::ifstream f("/proc/self/cgroup");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("0::", 0) == 0) {
            return line.substr(3);
        }
    }
    return {};
}

// Move every pid in cgroup.procs file `from` into cgroup.procs file `to` (one per write).
void drain_procs(const std::string& from, const std::string& to) {
    std::vector<std::string> pids;
    {
        std::ifstream f(from);
        std::string pid;
        while (std::getline(f, pid)) {
            if (!pid.empty()) {
                pids.push_back(pid);
            }
        }
    }
    for (const std::string& p : pids) {
        std::ofstream o(to);
        if (o) {
            o << p;
        }
    }
}

} // namespace

ResourceCaps cgroup_default_caps() {
    ResourceCaps c;
    long long ram = 0;
    {
        std::ifstream f("/proc/meminfo");
        std::string key;
        long long val = 0;
        std::string unit;
        while (f >> key >> val >> unit) {
            if (key == "MemTotal:") {
                ram = val * 1024; // kB → bytes
                break;
            }
        }
    }
    long long mem = ram > 0 ? ram / 8 : 512LL * 1024 * 1024; // a bounded fraction of host RAM
    const long long ceiling = 1024LL * 1024 * 1024;          // capped at 1 GiB
    const long long floor = 128LL * 1024 * 1024;             // but at least 128 MiB
    c.memory_max = mem > ceiling ? ceiling : (mem < floor ? floor : mem);
    c.pids_max = 512;   // generous for threads; stops a fork-bomb
    c.cpu_weight = 100; // a fair default share
    return c;
}

const std::string& cgroup_base() {
    static const std::string base = []() -> std::string {
        struct statfs sfs {};
        if (::statfs("/sys/fs/cgroup", &sfs) != 0 ||
            sfs.f_type != static_cast<__fsword_t>(CGROUP2_SUPER_MAGIC)) {
            return {}; // not a unified cgroup-v2 mount
        }
        const std::string rel = self_cgroup_v2_rel();
        if (rel.empty()) {
            return {};
        }
        const std::string b = "/sys/fs/cgroup" + (rel == "/" ? std::string{} : rel);
        // Delegation test: can we create children here?
        const std::string sup = b + "/zen-supervisor";
        if (::mkdir(sup.c_str(), 0755) != 0 && errno != EEXIST) {
            return {}; // root cgroup / no delegation → not enforceable
        }
        // no-internal-processes: drain our base's processes into the supervisor so the
        // base may enable controllers for its (Shard-leaf) children.
        drain_procs(b + "/cgroup.procs", sup + "/cgroup.procs");
        const std::string ctrls = read_text(b + "/cgroup.controllers");
        std::string want;
        if (has_token(ctrls, "memory")) {
            want += "+memory ";
        }
        if (has_token(ctrls, "pids")) {
            want += "+pids ";
        }
        if (has_token(ctrls, "cpu")) {
            want += "+cpu";
        }
        if (want.empty() || !write_text(b + "/cgroup.subtree_control", want)) {
            ::rmdir(sup.c_str());
            return {};
        }
        const std::string enabled = read_text(b + "/cgroup.subtree_control");
        g_cg_memory = has_token(enabled, "memory");
        g_cg_pids = has_token(enabled, "pids");
        g_cg_cpu = has_token(enabled, "cpu");
        return b;
    }();
    return base;
}

bool cgroup_memory_available() noexcept {
    (void)cgroup_base();
    return g_cg_memory;
}
bool cgroup_pids_available() noexcept {
    (void)cgroup_base();
    return g_cg_pids;
}
bool cgroup_cpu_available() noexcept {
    (void)cgroup_base();
    return g_cg_cpu;
}

bool cgroup_create_leaf(const std::string& name, const ResourceCaps& caps) {
    const std::string& b = cgroup_base();
    if (b.empty()) {
        return false;
    }
    const std::string leaf = b + "/" + name;
    if (::mkdir(leaf.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }
    if (g_cg_memory && caps.memory_max >= 0) {
        if (!write_text(leaf + "/memory.max", std::to_string(caps.memory_max))) {
            return false;
        }
        write_text(leaf + "/memory.swap.max", "0"); // swap must not escape the memory cap
    }
    if (g_cg_pids && caps.pids_max >= 0) {
        if (!write_text(leaf + "/pids.max", std::to_string(caps.pids_max))) {
            return false;
        }
    }
    if (g_cg_cpu && caps.cpu_weight > 0) {
        // cpu.weight is a *fair-share* weight (shares under contention), not a hard cap.
        if (!write_text(leaf + "/cpu.weight", std::to_string(caps.cpu_weight))) {
            return false;
        }
    }
    return true;
}

bool cgroup_move_pid(const std::string& name, pid_t pid) {
    const std::string& b = cgroup_base();
    if (b.empty()) {
        return false;
    }
    return write_text(b + "/" + name + "/cgroup.procs", std::to_string(static_cast<long long>(pid)));
}

bool cgroup_confirm(const std::string& name, pid_t pid, const ResourceCaps& caps) {
    const std::string& b = cgroup_base();
    if (b.empty()) {
        return false;
    }
    const std::string leaf = b + "/" + name;
    const std::string pc = read_text("/proc/" + std::to_string(static_cast<long long>(pid)) +
                                     "/cgroup");
    if (pc.find("/" + name) == std::string::npos) {
        return false; // pid is not in the expected leaf
    }
    if (g_cg_memory && caps.memory_max >= 0 &&
        parse_ll(read_text(leaf + "/memory.max")) != caps.memory_max) {
        return false;
    }
    if (g_cg_pids && caps.pids_max >= 0 &&
        parse_ll(read_text(leaf + "/pids.max")) != caps.pids_max) {
        return false;
    }
    if (g_cg_cpu && caps.cpu_weight > 0 &&
        parse_ll(read_text(leaf + "/cpu.weight")) != caps.cpu_weight) {
        return false;
    }
    return true;
}

void cgroup_remove_leaf(const std::string& name) {
    const std::string& b = cgroup_base();
    if (!b.empty()) {
        ::rmdir((b + "/" + name).c_str());
    }
}

#else // !__linux__

int unshare_isolation(bool, bool) noexcept {
    return -1;
}
bool write_isolation_id_maps(pid_t) noexcept {
    return false;
}
int run_mount_plan(const MountPlan&) noexcept {
    return -1;
}
bool child_netns_is_isolated(pid_t) noexcept {
    return false;
}
bool child_mountns_is_isolated(pid_t) noexcept {
    return false;
}
ResourceCaps cgroup_default_caps() {
    return {};
}
const std::string& cgroup_base() {
    static const std::string empty;
    return empty;
}
bool cgroup_memory_available() noexcept {
    return false;
}
bool cgroup_pids_available() noexcept {
    return false;
}
bool cgroup_cpu_available() noexcept {
    return false;
}
bool cgroup_create_leaf(const std::string&, const ResourceCaps&) {
    return false;
}
bool cgroup_move_pid(const std::string&, pid_t) {
    return false;
}
bool cgroup_confirm(const std::string&, pid_t, const ResourceCaps&) {
    return false;
}
void cgroup_remove_leaf(const std::string&) {}

#endif

const EnforcementReport& detect_enforcement() {
    static const EnforcementReport report = [] {
        EnforcementReport r;

        CapabilityStatus net;
        net.capability = Capability::Network;
        CapabilityStatus fs;
        fs.capability = Capability::Filesystem;
        CapabilityStatus res;
        res.capability = Capability::Resources;

#if defined(__linux__)
        // Each probe uses the very mechanism enforcement uses (parent-mapped child),
        // reporting via its exit code. Nothing persists.
        if (probe_isolation(true, false, nullptr)) {
            net.enforceable = true;
            net.mechanism = "user+net namespace (no interface)";
        } else {
            net.detail = "unprivileged user+net namespace unavailable on this host";
        }
        {
            char tmpl[] = "/tmp/zen-fsprobe-XXXXXX";
            char* root = ::mkdtemp(tmpl);
            if (root == nullptr) {
                fs.detail = "filesystem probe could not create a root";
            } else {
                const MountPlan plan = build_probe_plan(root);
                if (probe_isolation(false, true, &plan)) {
                    fs.enforceable = true;
                    fs.mechanism = "user+mount namespace (pivot_root allow-list)";
                } else {
                    fs.detail = "unprivileged mount-namespace view unavailable on this host";
                }
                ::rmdir(root); // best-effort: the probe's mounts were private to its child
            }
        }
        // Resources: establishing the cgroup base IS the probe (it sets up the
        // supervisor hierarchy this host will use, once). Empty base → not enforceable
        // (no cgroup-v2 delegation — e.g. running outside a delegated systemd scope).
        if (!cgroup_base().empty() && (cgroup_memory_available() || cgroup_pids_available())) {
            res.enforceable = true;
            res.mechanism = std::string("cgroup-v2 leaf (") +
                            (cgroup_memory_available() ? "memory " : "") +
                            (cgroup_pids_available() ? "pids " : "") +
                            (cgroup_cpu_available() ? "cpu" : "") + ")";
        } else {
            res.detail = "no cgroup-v2 delegated subtree (run the host under a delegated scope)";
        }
#else
        net.detail = "no native enforcement backend for this platform";
        fs.detail = "no native enforcement backend for this platform";
        res.detail = "no native enforcement backend for this platform";
#endif
        r.capabilities.push_back(std::move(net));
        r.capabilities.push_back(std::move(fs));
        r.capabilities.push_back(std::move(res));
        return r;
    }();
    return report;
}

} // namespace zen::isolation
