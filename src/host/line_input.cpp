// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include "line_input.hpp"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
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
/// unknown command).
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

#ifdef _WIN32

/// The Windows stdin handle, or nullptr when there is not one (a GUI subsystem process,
/// or a closed handle).
HANDLE stdin_handle() {
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    return (h == INVALID_HANDLE_VALUE || h == nullptr) ? nullptr : h;
}

/// IS THERE INPUT WAITING? Windows needs a different question per handle kind, and asking
/// the wrong one is not a slow answer but a wrong one:
///
///   console   `WaitForSingleObject` on a console handle signals on ANY input record —
///             a key release, a mouse move, a focus change — so a bare wait would report
///             "ready" for events that produce no bytes and the caller would spin.
///             `PeekConsoleInput` lets us look for a real key-down first.
///   pipe      `PeekNamedPipe` is the only honest peek, and a pipe handle is not
///             waitable in a way that means "has bytes".
///   file      always ready; a read returns bytes or end of file immediately.
///
/// Returns true when a read will not block.
bool input_ready(HANDLE h) {
    const DWORD kind = GetFileType(h);
    if (kind == FILE_TYPE_DISK) {
        return true;
    }
    if (kind == FILE_TYPE_PIPE) {
        DWORD available = 0;
        if (PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr) == 0) {
            return true; // broken pipe: let the read report end of input
        }
        return available != 0;
    }
    // A console. Look for something that will actually yield a byte.
    DWORD pending = 0;
    if (GetNumberOfConsoleInputEvents(h, &pending) == 0 || pending == 0) {
        return false;
    }
    INPUT_RECORD records[32];
    DWORD peeked = 0;
    if (PeekConsoleInputA(h, records, 32, &peeked) == 0) {
        return false;
    }
    for (DWORD i = 0; i < peeked; ++i) {
        if (records[i].EventType == KEY_EVENT && records[i].Event.KeyEvent.bKeyDown != 0 &&
            records[i].Event.KeyEvent.uChar.AsciiChar != 0) {
            return true;
        }
    }
    return false;
}

#endif // _WIN32

} // namespace

LineInput::LineInput() = default;

bool LineInput::ready() const {
    if (closed_ || partial_.find('\n') != std::string::npos) {
        return true;
    }
#ifdef _WIN32
    const HANDLE h = stdin_handle();
    return h != nullptr && input_ready(h);
#else
    struct pollfd p {};
    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    return ::poll(&p, 1, 0) > 0;
#endif
}

LineInput::Status LineInput::read(std::string* out, int timeout_ms) {
    // A line already buffered is answered without consulting the OS at all: a pipe hands
    // over several commands in one go, and a caller that went back to waiting between
    // them would serve a script one line per timeout.
    if (take_line(partial_, out)) {
        return Status::Line;
    }
    if (closed_) {
        // The stream ended with a last line that had no newline; run it once, then stay
        // closed for good.
        if (!flushed_ && !partial_.empty()) {
            flushed_ = true;
            *out = std::move(partial_);
            partial_.clear();
            if (!out->empty() && out->back() == '\r') {
                out->pop_back();
            }
            return Status::Line;
        }
        return Status::Closed;
    }

    char buf[512];

#ifdef _WIN32
    const HANDLE h = stdin_handle();
    if (h == nullptr) {
        closed_ = true;
        return Status::Closed;
    }
    if (!input_ready(h)) {
        if (timeout_ms <= 0) {
            return Status::Idle;
        }
        // A console handle is waitable; a pipe is not, so it is polled. The 10 ms grain
        // is well under a person's reaction time and costs nothing beside a bus that is
        // doing real work — this branch is only reached when the bus is IDLE.
        const DWORD kind = GetFileType(h);
        if (kind == FILE_TYPE_CHAR) {
            (void)WaitForSingleObject(h, static_cast<DWORD>(timeout_ms));
        } else {
            int waited = 0;
            while (waited < timeout_ms && !input_ready(h)) {
                Sleep(10);
                waited += 10;
            }
        }
        if (!input_ready(h)) {
            return Status::Idle;
        }
    }
    DWORD got = 0;
    if (ReadFile(h, buf, static_cast<DWORD>(sizeof buf), &got, nullptr) == 0 || got == 0) {
        closed_ = true;
        return read(out, 0); // one more pass, to flush a newline-less last line
    }
    partial_.append(buf, static_cast<std::size_t>(got));
#else
    struct pollfd p {};
    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    const int ready_fds = ::poll(&p, 1, timeout_ms > 0 ? timeout_ms : 0);
    if (ready_fds == 0) {
        return Status::Idle;
    }
    if (ready_fds < 0) {
        if (errno == EINTR) {
            return Status::Idle; // a signal is not end of input
        }
        closed_ = true;
        return Status::Closed;
    }
    const ssize_t got = ::read(STDIN_FILENO, buf, sizeof buf);
    if (got < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return Status::Idle;
        }
        closed_ = true;
        return Status::Closed;
    }
    if (got == 0) {
        closed_ = true;
        return read(out, 0); // one more pass, to flush a newline-less last line
    }
    partial_.append(buf, static_cast<std::size_t>(got));
#endif

    return take_line(partial_, out) ? Status::Line : Status::Idle;
}

} // namespace loom::host
