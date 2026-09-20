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
// WHAT IS OWNED, AND UNTIL WHEN. The unit of ownership is the worker's EXECUTION GROUP -- a
// Windows job object, a POSIX process group -- not the one process that leads it. A leader that
// exits leaving children behind has not ended the execution: `ended()` says the leader is gone,
// `alive()` says whether anything this object owns is still running, and `terminate()` ends the
// whole group in either case. Ownership begins at `spawn` and ends at `release` (the destructor),
// or earlier when the group drains by itself. It never ends merely because the leader exited,
// which is what makes a force-stop after a verdict mean something.
//
// AND WHY IT CANNOT WANDER. A numeric pid or group id that nothing holds is reused by the
// operating system, so a kill by remembered number can land on an unrelated later process. This
// object never does that: Windows holds a job HANDLE, which names that job and no other for as
// long as it is open; POSIX keeps the leader UNREAPED (a zombie, read with `waitid(WNOWAIT)`)
// for exactly as long as the group still has members, and a pid with a zombie on it is not
// recycled -- so the group id it also serves as is this group's and no other. When the group is
// empty the leader is reaped at once and ownership is given up, so there is nothing left to aim.
//
// WHEN THE HOST ITSELF ENDS. On a clean shutdown the manager destroys these objects and every
// owned group is ended. On Windows that also holds when the host dies ABRUPTLY: the job carries
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE and the kernel closes the handle for a dying process, so
// the group goes with it. On POSIX there is NO such guarantee -- an abruptly killed host leaves
// its workers running, reparented, with their records unfinished (docs/guides/sessions.md).

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

    /// Has the LEADER ended? Never waits. Once it has, `exit_code` is its code (on POSIX a signal
    /// ends it as 128 + the signal) and every later call answers the same. It says nothing about
    /// what the leader started: for that, ask `alive`.
    bool ended();

    /// THE LEADER'S OWN exit code -- never a descendant's, and never an aggregate for the group.
    /// Meaningful only when `exit_code_known()`: a leader can end without this object learning
    /// its code (on POSIX, when somebody else reaped it), and -1 is that absence, not a result.
    int exit_code() const noexcept { return exit_code_; }
    bool exit_code_known() const noexcept { return code_known_; }

    /// WAIT, AT MOST `milliseconds`, FOR THE LEADER TO END -- the one place this class waits at
    /// all, so that a caller about to write down a final claim ("it exited with N") can make the
    /// observation the claim needs. Returns `ended()`: false means the end was not observed in
    /// that time, and the caller must say so rather than reporting a code it never read. Zero
    /// milliseconds is one look and no wait.
    bool wait_for_end(int milliseconds);

    /// Is anything this object owns still running -- the leader, or a descendant it left in the
    /// execution group? Never waits. False once the group is empty (and then this object owns
    /// nothing more, so nothing later can be aimed at that group's number).
    bool alive();

    /// End the whole execution group, now, whether or not the leader has ended. Returns false
    /// when there was nothing left to end. Not required for an ordinary finish: it is the
    /// force-stop, and it is the only thing that stops a group whose leader already exited.
    bool terminate();

    /// Look a program up the way a shell would, for the one name the manager searches for when
    /// the catalog names no interpreter. Empty when it is not found.
    static std::string find_on_path(const std::string& name);

private:
    /// End and let go of the worker (the destructor's work, shared with move assignment).
    void release() noexcept;
    /// POSIX: has the execution group any member but the leader's own zombie? Reaps the leader
    /// and gives up ownership when it has not.
    bool group_alive();

    std::int64_t pid_ = 0;
    int exit_code_ = -1;
    bool ended_ = false;   ///< the LEADER has ended
    bool code_known_ = false; ///< ...and `exit_code_` is the code this object actually read
    bool owned_ = false;   ///< this object still owns the execution group (see the header note)
#ifndef _WIN32
    bool reaped_ = false;  ///< the leader's zombie is gone; its pid may be recycled from now on
#else
    void* process_ = nullptr; ///< HANDLE
    void* job_ = nullptr;     ///< HANDLE
#endif
};

/// REPLACE `to` WITH `from`, as one step where the platform offers one. False, with the
/// operating system's own reason, when the replacement did not happen -- and then `to` is
/// untouched and still whatever it was, which is what lets a caller keep its last valid file.
bool replace_file(const std::string& from, const std::string& to, std::string* why);

/// Where the loaded image containing `address` lives (a directory), or empty -- how the run
/// manager finds the `python/` runtime installed beside its own artifact.
std::string image_directory_of(const void* address);

/// ONE ENVIRONMENT VARIABLE, or empty when it is unset. Platform-split for the same reason the
/// rest of this file is: MSVC deprecates `getenv` (C4996, an error under this build's warnings),
/// and the Windows API answers the question directly.
std::string environment_value(const char* name);

} // namespace loom::runs

#endif // ZEN_RUNS_PROCESS_HPP
