// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A WORKER THAT LEAVES A CHILD BEHIND, and nothing else. The run manager owns a worker's
// EXECUTION GROUP, not its leader (src/runs/process.hpp), and only the worker can put a second
// process into that group, so asking what the manager says of a group whose leader has ended
// needs this worker: the C++ twin of tests/session/lifecycle/descendants.py, for the suite that
// runs without Python, on both ownership paths (a Windows job object, a POSIX process group).

// It stands in for the catalog's INTERPRETER and ignores the arguments it would give Python.
// `<this> sleep <seconds>` is the child. Anything else is the leader: it starts a copy of
// ITSELF sleeping, ordinarily, so the child lands in the group its parent leads; writes that
// child's pid to `child.pid` in its working directory (the run's own); and exits with a code of
// its own that nothing may overwrite. Starting itself means nothing is quoted or looked up.

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
