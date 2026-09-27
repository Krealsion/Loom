// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_RUNS_PROCESS_HPP
#define ZEN_RUNS_PROCESS_HPP

// One worker process, started and watched without blocking the host: `poll` never waits,
// because the manager lives on the host's one bus thread. The child gets the null device for
// stdin, one output file, the run's directory and the host's environment plus named variables,
// and nothing else (docs/reference/capabilities.md#the-exec-boundary-three-independent-facts).
// Not a sandbox: same user, same filesystem and network (docs/guides/sessions.md). What is owned
// is the execution group (a Windows job, a POSIX process group), from `spawn` to `release`.

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

    /// WAIT, AT MOST `milliseconds`, FOR THE LEADER TO END, so that a caller about to write down
    /// a final claim ABOUT THE LEADER ("it exited with N") can make the observation the claim
    /// needs. Returns `ended()`: false means the end was not observed in that time, and the
    /// caller must say so rather than reporting a code it never read. Zero milliseconds is one
    /// look and no wait. It says NOTHING about the execution -- see `wait_for_group_end`.
    bool wait_for_end(int milliseconds);

    /// Wait, at most `milliseconds`, for the whole owned execution to end, the leader and
    /// anything it left in the group, so a caller about to write "it is over" has observed it.
    /// False means that was not established in time (or nothing was ever started); zero is one
    /// look. The leader's code is read along the way where still readable; never a descendant's.
    /// These two waits and `release()` are the only places this class waits.
    bool wait_for_group_end(int milliseconds);

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
    /// End and let go of the worker (the destructor's work, shared with move assignment). A host
    /// killed outright never runs it: on Windows the job's KILL_ON_JOB_CLOSE still ends the
    /// group, and on POSIX the workers are left running (docs/guides/sessions.md).
    void release() noexcept;
    /// Has the WHOLE owned execution ended -- the leader AND the group? Both are asked, leader
    /// first; neither answers for the other (see the definition).
    bool execution_over();
    /// POSIX: has the execution group any member but the leader's own zombie? Reaps the leader
    /// and gives up ownership when it has not. The leader stays unreaped while the group has
    /// members, so its pid, which is also the group id, cannot be recycled under a later kill;
    /// Windows holds the job handle for the same reason.
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
