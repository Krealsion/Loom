// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A WORKER THAT LEAVES A CHILD BEHIND, and nothing else.
//
// The run manager owns a worker's EXECUTION GROUP, not the process that leads it
// (src/runs/process.hpp), and only the worker itself can put a second process into that group --
// so a test that wants to ask what this manager says about an execution whose LEADER has ended
// while the group is still running has to be handed a worker that arranges exactly that. This is
// that worker: the C++ twin of tests/session/lifecycle/descendants.py, for the suite that runs
// without Python and on both ownership paths (a Windows job object, a POSIX process group).
//
// It stands in for the catalog's INTERPRETER, so the manager starts it with the arguments it
// would give Python (`-X utf8 -u -m loom_session.worker`) and it ignores them:
//
//   <this> sleep <seconds>    the child, and the only argument list it reads.
//   <this> <anything else>    the leader. It starts a copy of ITSELF sleeping -- started
//                             ordinarily, with no job and no process group of its own, so the
//                             child lands in the group its parent leads -- writes that child's
//                             process id to `child.pid` in the working directory (the manager
//                             makes that the run's own directory), and exits with a code of its
//                             own that nothing may overwrite.
//
// It starts ITSELF rather than a named program so that nothing has to be quoted, looked up on
// PATH, or assumed to exist on the other platform.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

/// Where this program is, asked of the operating system where it can be, so that the copy it
/// starts is this same program however it was invoked.
std::string own_path(const char* argv0) {
#ifdef _WIN32
    std::string path(32768, '\0');
    const DWORD n = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n != 0) {
        path.resize(n);
        return path;
    }
#endif
    return argv0 == nullptr ? std::string() : std::string(argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(std::atoi(argv[2])));
        return 0;
    }
    const std::string self = own_path(argc > 0 ? argv[0] : nullptr);
    const std::string seconds = std::to_string(ZEN_RUNS_LEADER_SECONDS);
    long child = 0;
#ifdef _WIN32
    std::string line = "\"" + self + "\" sleep " + seconds;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(self.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &si, &pi) == 0) {
        std::fprintf(stderr, "cannot start a child of my own: %lu\n",
                     static_cast<unsigned long>(GetLastError()));
        return 3;
    }
    child = static_cast<long>(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    const pid_t forked = ::fork();
    if (forked < 0) {
        std::fprintf(stderr, "cannot fork a child of my own\n");
        return 3;
    }
    if (forked == 0) {
        ::execl(self.c_str(), self.c_str(), "sleep", seconds.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    child = static_cast<long>(forked);
#endif
    // The pid file is the test's handle on that child, and it exists only once the child does.
    std::ofstream(ZEN_RUNS_LEADER_PIDFILE, std::ios::binary) << child;
    return ZEN_RUNS_LEADER_CODE;
}
