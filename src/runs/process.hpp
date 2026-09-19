// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_RUNS_PROCESS_HPP
#define ZEN_RUNS_PROCESS_HPP

// ONE WORKER PROCESS, STARTED AND WATCHED WITHOUT BLOCKING THE HOST.
//
// The run manager starts each run's worker as a separate operating-system process and then only
// ASKS about it -- `poll` never waits -- because the manager lives on the host's single bus thread
// and a manager that blocked on a child would stop the session for every client.
//
// WHAT THE CHILD GETS, EXACTLY, because the exec boundary is where ambient authority leaks
// (docs/reference/capabilities.md, "the exec boundary"): its standard input is the null device,
// its standard output and error go to one file in the run's directory, its working directory is
// the run's directory, and its environment is the host's plus the variables the manager names.
// NOTHING ELSE IS INHERITED: on Windows the handle list is explicit (PROC_THREAD_ATTRIBUTE_
// HANDLE_LIST), and on POSIX every descriptor above the three standard ones is closed before the
// program starts -- so a worker never holds the host's listener or a client's socket open.
//
// WHAT IT IS NOT: a sandbox. The worker runs as the same user with the same filesystem and network
// reach as the host. Its bus authority is bounded by its session's grant; its OS authority is not
// bounded here at all, and the docs say so.
//
// ENDING IT. `terminate` ends the whole process tree the worker started: a Windows job object
// (which also ends every worker when the host process itself ends), a POSIX process group.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace loom::runs {

struct SpawnSpec {
    std::string program;                                     ///< an absolute path, or a name on PATH
    std::vector<std::string> args;                           ///< argv[1..]
    std::string cwd;                                         ///< the child's working directory
    std::vector<std::pair<std::string, std::string>> env;    ///< set (or override) these
    std::string output;                                      ///< stdout+stderr land here (truncated)
};

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;

    /// Start it. False, with the operating system's reason, when it could not be started at all.
    bool spawn(const SpawnSpec& spec, std::string* why);

    bool started() const noexcept { return pid_ != 0; }
    std::int64_t pid() const noexcept { return pid_; }

    /// Has it ended? Never waits. Once it has, `exit_code` is its code (on POSIX a signal ends it
    /// as 128 + the signal) and every later call answers the same.
    bool ended();
    int exit_code() const noexcept { return exit_code_; }

    /// End it and everything it started, now. Returns false when there was nothing to end.
    bool terminate();

    /// Look a program up the way a shell would, for the one name the manager searches for when
    /// the catalog names no interpreter. Empty when it is not found.
    static std::string find_on_path(const std::string& name);

private:
    /// End and let go of the worker (the destructor's work, shared with move assignment).
    void release() noexcept;

    std::int64_t pid_ = 0;
    int exit_code_ = -1;
    bool ended_ = false;
#ifdef _WIN32
    void* process_ = nullptr; ///< HANDLE
    void* job_ = nullptr;     ///< HANDLE
#endif
};

/// Where the loaded image containing `address` lives (a directory), or empty -- how the run
/// manager finds the `python/` runtime installed beside its own artifact.
std::string image_directory_of(const void* address);

/// ONE ENVIRONMENT VARIABLE, or empty when it is unset. Platform-split for the same reason the
/// rest of this file is: MSVC deprecates `getenv` (C4996, an error under this build's warnings),
/// and the Windows API answers the question directly.
std::string environment_value(const char* name);

} // namespace loom::runs

#endif // ZEN_RUNS_PROCESS_HPP
