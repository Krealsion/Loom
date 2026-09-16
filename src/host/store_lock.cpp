// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include "store_lock.hpp"

#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace loom::host {

namespace {

#ifdef _WIN32
/// `nullptr` is the "no handle" sentinel in the header rather than
/// `INVALID_HANDLE_VALUE`, so the header does not have to include <windows.h> to spell a
/// default. This is the one place the two meet.
inline void* to_sentinel(HANDLE h) { return h == INVALID_HANDLE_VALUE ? nullptr : h; }
#endif

} // namespace

StoreLock::~StoreLock() { release(); }

StoreLock::StoreLock(StoreLock&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)) {
    other.handle_ = kNoHandle;
    other.path_.clear();
}

StoreLock& StoreLock::operator=(StoreLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = other.handle_;
        path_ = std::move(other.path_);
        other.handle_ = kNoHandle;
        other.path_.clear();
    }
    return *this;
}

bool StoreLock::claim(const std::string& store, std::string* error, bool* held_by_another) {
    if (held_by_another != nullptr) {
        *held_by_another = false;
    }
    release();
    if (store.empty()) {
        return true; // an in-memory store: there is nothing to own and nobody to race
    }
    path_ = lock_path_for(store);

#ifdef _WIN32
    // dwShareMode 0: while this handle is open no other process can open the file at
    // all. That IS the claim — Windows' own mandatory sharing, released by the kernel
    // when the process ends however it ends. FILE_ATTRIBUTE_TEMPORARY keeps the sidecar
    // out of a person's way; it is still a real file, so a crash leaves it behind
    // harmlessly and the next host opens it again.
    const HANDLE h = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE, /*share=*/0,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        handle_ = to_sentinel(h);
        return true;
    }
    const DWORD why = GetLastError();
    // SHARING_VIOLATION is another host; everything else is a path or permission problem,
    // and a person chasing the wrong one of those wastes an afternoon.
    if (why == ERROR_SHARING_VIOLATION || why == ERROR_LOCK_VIOLATION) {
        if (held_by_another != nullptr) {
            *held_by_another = true;
        }
        *error = "another loom-host already owns the decision store '" + store + "'";
    } else {
        *error = "cannot claim the decision store '" + store + "': its lock file '" + path_ +
                 "' could not be opened (Windows error " +
                 std::to_string(static_cast<unsigned long>(why)) + ")";
    }
    path_.clear();
    return false;
#else
    const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        *error = "cannot claim the decision store '" + store + "': its lock file '" + path_ +
                 "' could not be opened (" + std::to_string(errno) + ")";
        path_.clear();
        return false;
    }
    // LOCK_NB: refuse now rather than block. A host that waited would look hung, and the
    // answer a person needs ("something else owns this") is available immediately.
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        handle_ = fd;
        return true;
    }
    const int why = errno;
    ::close(fd);
    if (why == EWOULDBLOCK || why == EAGAIN || why == EINTR) {
        if (held_by_another != nullptr) {
            *held_by_another = true;
        }
        *error = "another loom-host already owns the decision store '" + store + "'";
    } else {
        // flock over NFS and some network filesystems does not do what this needs, and a
        // silent success there would be the worst outcome of all — so the failure names
        // the possibility rather than pretending the claim held.
        *error = "cannot claim the decision store '" + store + "': locking '" + path_ +
                 "' failed (" + std::to_string(why) +
                 "). A store on a network filesystem may not support this.";
    }
    path_.clear();
    return false;
#endif
}

void StoreLock::release() noexcept {
    if (handle_ == kNoHandle) {
        return;
    }
#ifdef _WIN32
    CloseHandle(static_cast<HANDLE>(handle_));
#else
    // The close releases the flock; doing both would be the same act twice.
    ::close(handle_);
#endif
    handle_ = kNoHandle;
    path_.clear();
}

} // namespace loom::host
