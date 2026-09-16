// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include "line_input.hpp"

#include <cstddef>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h> // _beginthreadex: a real thread HANDLE on MinGW and MSVC alike

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace loom::host {

namespace {

/// Take one complete line out of `buf` if there is one. The newline goes with it; a
/// trailing CR goes too, so a CRLF script piped into a POSIX host does not leave every
/// command with an invisible character on the end (which is how `quit\r` became an
/// unknown command) — and so a Windows console's `\r\n` reads as one line ending.
bool take_line(std::string& buf, std::string* out) {
    const std::size_t nl = buf.find('\n');
    if (nl == std::string::npos) {
        return false;
    }
    std::string line = buf.substr(0, nl);
    buf.erase(0, nl + 1);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    *out = std::move(line);
    return true;
}

/// A stream that ended with a last line and no newline still runs that command once.
std::string last_line(std::string rest) {
    if (!rest.empty() && rest.back() == '\r') {
        rest.pop_back();
    }
    return rest;
}

} // namespace

#ifdef _WIN32

// ---- Windows: a reader thread and a queue of finished lines --------------------
//
// See the header for why. What is here is the mechanics, and the three facts they rest on,
// each measured on a real console before it was relied on:
//
//   1. a cooked `ReadFile` skips key-ups, focus, mouse, menu and buffer-size records and
//      modifier key-downs by itself, and returns the line behind them at once;
//   2. `CancelSynchronousIo` ends a cooked read that is waiting on a half-typed line;
//   3. the cancelled read's state goes away with the handle it was issued on only when that
//      handle is a separate `CONIN$` object — which is why the reader opens its own.

namespace {

/// How long the destructor keeps cancelling before it gives the thread up to process exit.
constexpr int kStopAttempts = 200; // x 10 ms

/// What the reader thread and the host thread share, and nothing else. Held by both through
/// `shared_ptr`, so a reader left to process exit never touches freed memory.
struct Shared {
    std::mutex m;
    std::condition_variable ready;
    std::deque<std::string> lines; ///< finished lines, oldest first
    bool closed = false;           ///< the reader will add nothing more
    std::atomic<bool> stop{false};
    HANDLE handle = nullptr; ///< the reader's own; closed once the thread has left it
};

unsigned __stdcall read_lines(void* arg) {
    // Take ownership of the one reference the starter handed over.
    std::shared_ptr<Shared> s = std::move(*static_cast<std::shared_ptr<Shared>*>(arg));
    delete static_cast<std::shared_ptr<Shared>*>(arg);

    std::string pending; // bytes read but not yet a line; the thread's alone
    char buf[512];
    while (!s->stop.load()) {
        DWORD got = 0;
        // THE BLOCKING READ, where blocking stops nobody. For a console this is the cooked read:
        // it returns a finished line and nothing sooner. For a pipe it returns what is there.
        if (ReadFile(s->handle, buf, static_cast<DWORD>(sizeof buf), &got, nullptr) == 0 ||
            got == 0) {
            // End of file, a broken pipe, Ctrl+Z at the start of a console line — or a
            // cancellation, which `stop` distinguishes below.
            break;
        }
        pending.append(buf, static_cast<std::size_t>(got));
        bool any = false;
        {
            std::lock_guard<std::mutex> lock(s->m);
            for (std::string line; take_line(pending, &line);) {
                s->lines.push_back(std::move(line));
                any = true;
            }
        }
        if (any) {
            s->ready.notify_all();
        }
    }
    {
        std::lock_guard<std::mutex> lock(s->m);
        if (!s->stop.load() && !pending.empty()) {
            s->lines.push_back(last_line(std::move(pending)));
        }
        s->closed = true;
    }
    s->ready.notify_all();
    return 0;
}

} // namespace

struct LineInput::Impl {
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    HANDLE thread = nullptr;
    bool started = false;

    /// Open the reader's own handle and start it. Anything that fails here is stdin being
    /// unusable, which is `Closed` — the same answer a missing stdin always got.
    void start() {
        started = true;
        const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE own = nullptr;
        DWORD mode = 0;
        if (in == nullptr || in == INVALID_HANDLE_VALUE) {
            own = nullptr;
        } else if (GetConsoleMode(in, &mode) != 0) {
            // A CONSOLE: a separate `CONIN$` object, so that closing it after a cancellation
            // takes the cancelled read with it (fact 3). A NUL device or a serial port also
            // reports FILE_TYPE_CHAR, which is why the test is GetConsoleMode, not the type.
            const HANDLE h = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                         OPEN_EXISTING, 0, nullptr);
            own = (h == INVALID_HANDLE_VALUE) ? nullptr : h;
        } else {
            // A pipe, a file or a device: this process's own copy of the same stream.
            if (DuplicateHandle(GetCurrentProcess(), in, GetCurrentProcess(), &own, 0, FALSE,
                                DUPLICATE_SAME_ACCESS) == 0) {
                own = nullptr;
            }
        }
        if (own == nullptr) {
            std::lock_guard<std::mutex> lock(shared->m);
            shared->closed = true;
            return;
        }
        shared->handle = own;
        auto* ref = new std::shared_ptr<Shared>(shared);
        const std::uintptr_t t = _beginthreadex(nullptr, 0, read_lines, ref, 0, nullptr);
        if (t == 0) {
            delete ref;
            CloseHandle(own);
            shared->handle = nullptr;
            std::lock_guard<std::mutex> lock(shared->m);
            shared->closed = true;
            return;
        }
        thread = reinterpret_cast<HANDLE>(t);
    }

    ~Impl() {
        if (thread == nullptr) {
            return;
        }
        shared->stop.store(true);
        // REPEATED, because a cancel that lands between two reads finds nothing to cancel and
        // the next read would then wait for a line nobody is going to type.
        for (int i = 0; i < kStopAttempts && WaitForSingleObject(thread, 10) == WAIT_TIMEOUT;
             ++i) {
            (void)CancelSynchronousIo(thread);
        }
        if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) {
            CloseHandle(shared->handle); // after the thread has left it, and not before
            shared->handle = nullptr;
        }
        // Otherwise the thread is still inside a read on that handle: leave both to process
        // exit. It holds its own share of `Shared`.
        CloseHandle(thread);
    }
};

LineInput::LineInput() : impl_(std::make_unique<Impl>()) {}

LineInput::~LineInput() = default;

LineInput::Status LineInput::read(std::string* out, int timeout_ms) {
    if (!impl_->started) {
        impl_->start();
    }
    Shared& s = *impl_->shared;
    std::unique_lock<std::mutex> lock(s.m);
    if (timeout_ms > 0) {
        // THE DEADLINE, and it is a real one: nothing on this thread is inside a read.
        (void)s.ready.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [&s] { return !s.lines.empty() || s.closed; });
    }
    if (!s.lines.empty()) {
        *out = std::move(s.lines.front());
        s.lines.pop_front();
        return Status::Line;
    }
    return s.closed ? Status::Closed : Status::Idle;
}

#else // POSIX

// ---- POSIX: poll, then read what poll promised ---------------------------------

struct LineInput::Impl {
    /// Bytes read but not yet forming a complete line. A pipe hands over whatever happens
    /// to be in it, which is regularly half a line, and a reader that dropped the remainder
    /// would eat commands from a script.
    std::string partial;
    bool closed = false;
    /// Set once the stream's newline-less last line has been handed out.
    bool flushed = false;
};

LineInput::LineInput() : impl_(std::make_unique<Impl>()) {}

LineInput::~LineInput() = default;

LineInput::Status LineInput::read(std::string* out, int timeout_ms) {
    Impl& in = *impl_;
    // A line already buffered is answered without consulting the OS at all: a pipe hands
    // over several commands in one go, and a caller that went back to waiting between
    // them would serve a script one line per timeout.
    if (take_line(in.partial, out)) {
        return Status::Line;
    }
    if (in.closed) {
        if (!in.flushed && !in.partial.empty()) {
            in.flushed = true;
            *out = last_line(std::move(in.partial));
            in.partial.clear();
            return Status::Line;
        }
        return Status::Closed;
    }

    struct pollfd p {};
    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    // A canonical-mode terminal is readable only at a finished line, so a half-typed
    // command is `0` here and the caller goes back to the bus.
    const int ready_fds = ::poll(&p, 1, timeout_ms > 0 ? timeout_ms : 0);
    if (ready_fds == 0) {
        return Status::Idle;
    }
    if (ready_fds < 0) {
        if (errno == EINTR) {
            return Status::Idle; // a signal is not end of input
        }
        in.closed = true;
        return Status::Closed;
    }
    char buf[512];
    const ssize_t got = ::read(STDIN_FILENO, buf, sizeof buf);
    if (got < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return Status::Idle;
        }
        in.closed = true;
        return Status::Closed;
    }
    if (got == 0) {
        in.closed = true;
        return read(out, 0); // one more pass, to flush a newline-less last line
    }
    in.partial.append(buf, static_cast<std::size_t>(got));
    return take_line(in.partial, out) ? Status::Line : Status::Idle;
}

#endif

} // namespace loom::host
