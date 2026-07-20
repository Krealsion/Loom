// SEC-2 live reproduction probe.
// Must be run while the process sits inside a cgroup whose parent enabled +pids
// but NOT +memory in subtree_control (a "pids-without-memory" base). We then call
// the REAL libzen-isolation functions and show that:
//   * detect_enforcement() reports Resources ENFORCEABLE (pids present),
//   * cgroup_default_caps().memory_max is a POSITIVE number (a claimed cap),
//   * the leaf gets created and cgroup_confirm() returns TRUE,
//   * yet the leaf's memory.max is NEVER written (memory is UNCAPPED),
//   * so the host.cpp note "memory<=NMiB ... (confirmed)" is a false claim.
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "zen/isolation/sandbox.hpp"

static std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s.empty() ? std::string("<absent/empty>") : s;
}

int main() {
    using namespace loom;

    printf("self pid = %d\n", (int)getpid());
    printf("self cgroup line: %s\n", slurp("/proc/self/cgroup").c_str());

    const EnforcementReport& rep = detect_enforcement();
    const CapabilityStatus* res = rep.find(Capability::Resources);
    printf("\n[detect_enforcement] Resources.enforceable = %s ; mechanism='%s' ; detail='%s'\n",
           res && res->enforceable ? "TRUE" : "false",
           res ? res->mechanism.c_str() : "", res ? res->detail.c_str() : "");

    const std::string& base = cgroup_base();
    printf("\ncgroup_base = '%s'\n", base.c_str());
    printf("cgroup_memory_available = %s\n", cgroup_memory_available() ? "TRUE" : "false");
    printf("cgroup_pids_available   = %s\n", cgroup_pids_available() ? "TRUE" : "false");
    printf("cgroup_cpu_available    = %s\n", cgroup_cpu_available() ? "TRUE" : "false");

    ResourceCaps caps = cgroup_default_caps();
    printf("\ncgroup_default_caps.memory_max = %lld bytes (%lld MiB)\n",
           caps.memory_max, caps.memory_max / (1024 * 1024));
    printf("cgroup_default_caps.pids_max   = %lld\n", caps.pids_max);

    // Reconstruct EXACTLY the host.cpp:760-763 note the mount() path would emit,
    // given a default grant (no unlimited_memory, no explicit memory_bytes -> caps
    // stay at the host defaults).
    std::string note =
        (caps.memory_max < 0 ? std::string("memory unlimited-by-grant")
                             : "memory<=" + std::to_string(caps.memory_max / (1024 * 1024)) + "MiB") +
        ", pids<=" + std::to_string(caps.pids_max);
    printf("\n[host.cpp note that mount() would build] : \"%s\"\n", note.c_str());

    // Now do what host.cpp does at spawn: create the leaf, move the pid in, confirm.
    const std::string leaf = "sec2-probe-leaf";
    bool created = cgroup_create_leaf(leaf, caps);
    bool moved = cgroup_move_pid(leaf, getpid());
    bool confirmed = cgroup_confirm(leaf, getpid(), caps);

    printf("\ncgroup_create_leaf = %s ; move_pid = %s ; cgroup_confirm = %s\n",
           created ? "true" : "false", moved ? "true" : "false",
           confirmed ? "TRUE" : "false");

    // What actually landed in the leaf?
    printf("\nleaf memory.max on disk = '%s'   <-- the ACTUAL memory cap\n",
           slurp(base + "/" + leaf + "/memory.max").c_str());
    printf("leaf pids.max   on disk = '%s'\n",
           slurp(base + "/" + leaf + "/pids.max").c_str());

    printf("\n================ VERDICT ================\n");
    if (res && res->enforceable && caps.memory_max > 0 && confirmed &&
        slurp(base + "/" + leaf + "/memory.max") == "<absent/empty>") {
        printf("HONESTY VIOLATION REPRODUCED: note claims 'memory<=%lldMiB ... (confirmed)'\n",
               caps.memory_max / (1024 * 1024));
        printf("but leaf/memory.max is absent -> memory is UNCAPPED. Enforcement claimed, not imposed.\n");
    } else {
        printf("not reproduced under these conditions.\n");
    }
    return 0;
}
