// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_STORE_LOCK_HPP
#define ZEN_HOST_STORE_LOCK_HPP

// ONE HOST OWNS A DECISION STORE WHILE IT RUNS.
//
// WHY THIS IS NOT A CONVENIENCE. The authority store is written whole: every decision
// replaces the entire file from the writer's in-memory copy. Two hosts on one file each
// hold their own copy, so the second one to write does not merely lose its own approval —
// it RESTORES whatever the first host took away. A person revoked a permission in host A,
// approved an unrelated artifact in host B, and B's stale copy put the revoked permission
// back for every later boot. That is a revocation the product promised and did not keep,
// reachable by one person opening two terminals, and no amount of "give the second host
// its own --authority file" in a guide makes the default path safe.
//
// THE CHOICE MADE HERE, and what it costs. Exclusive ownership: the first host to open a
// store keeps it until it exits, and a second host that names the same store is REFUSED,
// by name, with the two ways forward in the refusal. The alternative — safe shared
// updates (read-modify-write under a lock, per-decision) — is a real design and is not
// ruled out; it is more mechanism than a single-operator decision file has yet earned,
// and it would still have to answer "which of you wins" for a decision made in both.
// Refusing is the version a person can reason about without being told a rule.
//
// WHAT THE OS DOES FOR US, and it is the reason this is a lock rather than a PID file: a
// `flock` and a Windows share-mode handle are both released BY THE KERNEL when the
// process ends, however it ends. A host that is killed, or that segfaults, leaves no
// stale claim to clean up, and the next host starts normally. A PID file would need a
// liveness check, a staleness policy and a way to break a lock — three decisions, each
// wrong in some case, in place of one fact the OS already keeps.
//
// WHAT IT IS NOT. It is not concurrency control for the bus (Loom is single-threaded by
// contract), it is not a network lock (a store on a shared filesystem is out of scope and
// says so), and it is not a claim about other writers: a person who edits the file in an
// editor while a host runs is doing exactly what the file's own guide invites, and the
// host will overwrite them on its next decision. The lock bounds HOSTS.

#include <string>

namespace loom::host {

/// An exclusive claim on one decision store, held for as long as this object lives.
///
/// The claim is taken on a SIDECAR (`<store>.lock`), never on the store itself, because
/// the store is replaced by rename on every decision — a handle to it would be a handle
/// to a file that is about to stop being the store. The sidecar is created if absent and
/// is never read: its contents carry nothing, and only the OS's claim on it means
/// anything.
class StoreLock {
public:
    StoreLock() = default;
    ~StoreLock();
    StoreLock(const StoreLock&) = delete;
    StoreLock& operator=(const StoreLock&) = delete;
    StoreLock(StoreLock&& other) noexcept;
    StoreLock& operator=(StoreLock&& other) noexcept;

    /// Claim `store`'s lock file. Returns:
    ///   true                  the claim is held (and an empty `store` is a trivial yes —
    ///                         an in-memory store has nothing to own);
    ///   false, *error         somebody else holds it, or the lock file could not be
    ///                         created. The two are different problems and the sentence
    ///                         says which.
    ///
    /// `held_by_another` distinguishes them for a caller that wants to exit differently.
    bool claim(const std::string& store, std::string* error, bool* held_by_another = nullptr);

    /// Give the claim up early. Idempotent; the destructor does it too.
    void release() noexcept;

    bool held() const noexcept { return handle_ != kNoHandle; }
    const std::string& path() const noexcept { return path_; }

    /// The sidecar this would claim for `store` — exposed so a diagnostic can name it
    /// without a caller reinventing the spelling.
    static std::string lock_path_for(const std::string& store) { return store + ".lock"; }

private:
#ifdef _WIN32
    using Handle = void*;                        ///< HANDLE, kept opaque here
    static constexpr Handle kNoHandle = nullptr; ///< not INVALID_HANDLE_VALUE: see the .cpp
#else
    using Handle = int;
    static constexpr Handle kNoHandle = -1;
#endif
    Handle handle_ = kNoHandle;
    std::string path_;
};

} // namespace loom::host

#endif // ZEN_HOST_STORE_LOCK_HPP
