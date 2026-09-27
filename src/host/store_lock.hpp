// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_STORE_LOCK_HPP
#define ZEN_HOST_STORE_LOCK_HPP

// One host owns a decision store while it runs. The store is written whole, so a second host
// on the same file would restore, from its stale copy, whatever the first revoked. The first
// host to open a store keeps it until it exits, and a second is refused by name with the ways
// forward; shared per-decision updates remain possible and are not built. It is a kernel-held
// lock (flock, a Windows share mode), so a killed host leaves no stale claim. It bounds hosts,
// not the bus, not a network filesystem, and not a person editing the file in an editor.

#include <string>

namespace loom::host {

/// An exclusive claim on one decision store, held for as long as this object lives. Taken on a
/// sidecar (`<store>.lock`), never the store, which is replaced by rename on every decision; the
/// sidecar is created if absent, never read, and only the OS's claim on it means anything.
class StoreLock {
public:
    StoreLock() = default;
    ~StoreLock();
    StoreLock(const StoreLock&) = delete;
    StoreLock& operator=(const StoreLock&) = delete;
    StoreLock(StoreLock&& other) noexcept;
    StoreLock& operator=(StoreLock&& other) noexcept;

    /// Claim `store`'s lock file. True: the claim is held (an empty `store`, in memory, is a
    /// trivial yes). False with `*error`: somebody else holds it, or the lock file could not be
    /// created, and the sentence says which; `held_by_another` tells them apart for a caller
    /// that exits differently.
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
