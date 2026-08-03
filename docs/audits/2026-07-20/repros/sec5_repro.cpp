// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <string>
#include <cstdio>

// mirror of the membership test at sandbox.cpp:500
static bool member(const std::string& proc_cgroup, const std::string& name) {
    return proc_cgroup.find("/" + name) != std::string::npos;
}

int main() {
    // realistic single unified cgroup-v2 line for a pid actually moved into leaf zen-weave-10
    std::string pc_in_10 = "0::/user.slice/user-1000.slice/user@1000.service/zen.scope/zen-weave-10\n";
    // pid actually in its own leaf zen-weave-1
    std::string pc_in_1  = "0::/user.slice/user-1000.slice/user@1000.service/zen.scope/zen-weave-1\n";

    printf("pid(in zen-weave-1)  vs name=zen-weave-1  -> %s (expect PASS)\n",  member(pc_in_1,  "zen-weave-1")  ? "PASS":"fail");
    printf("pid(in zen-weave-10) vs name=zen-weave-10 -> %s (expect PASS)\n",  member(pc_in_10, "zen-weave-10") ? "PASS":"fail");
    printf("pid(in zen-weave-10) vs name=zen-weave-1  -> %s (bug=FALSE-PASS)\n", member(pc_in_10, "zen-weave-1")  ? "FALSE-PASS":"correctly-fails");
    printf("pid(in zen-weave-1)  vs name=zen-weave-10 -> %s (expect fail)\n",   member(pc_in_1,  "zen-weave-10") ? "FALSE-PASS":"correctly-fails");
    return 0;
}
