// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_BRIDGE_WAKE_HPP
#define ZEN_BRIDGE_WAKE_HPP

// The signal a crossing raises when it has work for a host's one wait (zen/host/reactor.hpp): the
// server's and the client's alike. Inside the library, never installed.

#include <zen/bridge/channel.hpp>

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace loom {

/// WHAT ENDS A HOST'S WAIT WHEN A CROSSING HAS WORK: a server that owes a peer something, a client
/// with a send queued. Off until a host first asks for the crossing's wait set; then `raise` makes
/// it ready and `clear`, at the start of each service or poll, makes it not. On Windows it is also
/// the event the crossing's sockets are selected on, so the network sets it too; on POSIX a pipe
/// whose one byte stands for "work".
class BridgeWake {
public:
    BridgeWake() = default;
    BridgeWake(const BridgeWake&) = delete;
    BridgeWake& operator=(const BridgeWake&) = delete;
    ~BridgeWake() {
#ifdef _WIN32
        if (event_ != WSA_INVALID_EVENT) {
            (void)::WSACloseEvent(event_);
        }
#else
        if (read_ >= 0) {
            ::close(read_);
        }
        if (write_ >= 0) {
            ::close(write_);
        }
#endif
    }

    /// Turn waiting on. False when the platform would not give the signal.
    bool start() {
        if (started_) {
            return ok();
        }
        started_ = true;
#ifdef _WIN32
        event_ = ::WSACreateEvent();
#else
        int fds[2] = {-1, -1};
        if (::pipe(fds) == 0) {
            for (const int fd : fds) {
                (void)::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
                (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
            read_ = fds[0];
            write_ = fds[1];
        }
#endif
        return ok();
    }
    bool ok() const noexcept {
#ifdef _WIN32
        return event_ != WSA_INVALID_EVENT;
#else
        return read_ >= 0;
#endif
    }

    /// Work is owed: a waiting host wakes.
    void raise() noexcept {
        if (!ok()) {
            return;
        }
#ifdef _WIN32
        (void)::WSASetEvent(event_);
#else
        if (!raised_) {
            const char one = 1;
            raised_ = ::write(write_, &one, 1) == 1;
        }
#endif
    }

    /// A service or a poll begins: what was owed is about to be paid. Whatever the network or a
    /// turn says after this sets it again.
    void clear() noexcept {
        if (!ok()) {
            return;
        }
#ifdef _WIN32
        (void)::WSAResetEvent(event_);
#else
        char buf[64];
        while (::read(read_, buf, sizeof buf) > 0) {
        }
        raised_ = false;
#endif
    }

    /// Select `s` on the event (Windows), so its readiness sets it. Nothing on POSIX, where the
    /// wait names each socket itself.
    void select(socket_t s) noexcept {
#ifdef _WIN32
        if (ok() && s != kInvalidSocket) {
            (void)::WSAEventSelect(static_cast<SOCKET>(s), event_,
                                   FD_ACCEPT | FD_READ | FD_WRITE | FD_CLOSE);
        }
#else
        (void)s;
#endif
    }

    std::int64_t handle() const noexcept {
#ifdef _WIN32
        return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(event_));
#else
        return read_;
#endif
    }

private:
    bool started_ = false;
#ifdef _WIN32
    WSAEVENT event_ = WSA_INVALID_EVENT;
#else
    int read_ = -1;
    int write_ = -1;
    bool raised_ = false;
#endif
};

} // namespace loom

#endif // ZEN_BRIDGE_WAKE_HPP
