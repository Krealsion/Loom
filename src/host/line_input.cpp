// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include "line_input.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
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

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#else
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <unistd.h>
#endif

namespace loom::host {

namespace {

/// How much one read asks for. Not a limit anybody can see — a read never adds more than the
/// waiting area has room for — only how the platform readers take bytes from the OS.
constexpr std::size_t kReadChunkBytes = 4096;

// A FULL READ ALWAYS FITS ONCE THE FINISHED LINES ARE TAKEN. What the area can hold unfinished
// is at most one command and a `\r` (anything longer is refused and released), so this is what
// guarantees a reader waiting for room is never waiting for a newline it has no room to read.
static_assert(kMaxCommandBytes + 1 + kReadChunkBytes <= kHeldInputBytes,
              "the waiting area must hold the longest unfinished command and one read");

/// The length of the command in `bytes[from, to)`: a `\r` before the newline (or before the end
/// of input) is part of the line's ending, not of the command.
std::size_t command_length(const std::string& bytes, std::size_t from, std::size_t to) {
    std::size_t len = to - from;
    if (len > 0 && bytes[to - 1] == '\r') {
        --len;
    }
    return len;
}

LineInput::Status status_of(HeldInput::Taken taken, std::string* out) {
    switch (taken) {
    case HeldInput::Taken::Line:
        return LineInput::Status::Line;
    case HeldInput::Taken::TooLong:
        out->clear(); // none of a refused line is handed out, not even by accident
        return LineInput::Status::TooLong;
    case HeldInput::Taken::Nothing:
        return LineInput::Status::Idle;
    case HeldInput::Taken::Ended:
        return LineInput::Status::Closed;
    }
    return LineInput::Status::Closed;
}

} // namespace

// ---- the waiting area, the same on every platform -------------------------------

std::size_t HeldInput::held() const noexcept { return bytes_.size() - head_; }

std::size_t HeldInput::peak() const noexcept { return peak_; }

std::size_t HeldInput::room() const noexcept {
    const std::size_t h = held();
    return h >= kHeldInputBytes ? 0 : kHeldInputBytes - h;
}

void HeldInput::add(const char* data, std::size_t n) {
    if (ended_ || n == 0) {
        return;
    }
    std::size_t from = 0;
    if (discarding_) {
        // THE REST OF A REFUSED LINE, dropped as it arrives: it was never going to be a command,
        // and holding it until its newline turned up is how a line that never ends would pin
        // the reader.
        const void* nl = std::memchr(data, '\n', n);
        if (nl == nullptr) {
            return;
        }
        from = static_cast<std::size_t>(static_cast<const char*>(nl) - data) + 1;
        discarding_ = false;
    }
    if (from == n) {
        return;
    }
    // What was taken is released before anything is added, so the buffer never grows past what
    // it holds plus one read. Reserved once at the limit, so it does not reallocate either.
    if (head_ != 0) {
        bytes_.erase(0, head_);
        scanned_ -= head_;
        head_ = 0;
    }
    if (bytes_.capacity() < kHeldInputBytes) {
        bytes_.reserve(kHeldInputBytes);
    }
    bytes_.append(data + from, n - from);
    peak_ = std::max(peak_, held());
}

void HeldInput::end() noexcept { ended_ = true; }

std::size_t HeldInput::newline() const {
    const std::size_t at = bytes_.find('\n', std::max(scanned_, head_));
    // Every byte is searched once, however many times the host asks while a line is unfinished.
    scanned_ = (at == std::string::npos) ? bytes_.size() : at;
    return at;
}

bool HeldInput::tail_too_long() const noexcept {
    // Asked only when no newline is held, so everything held is one unfinished line. It may hold
    // one byte more than a command, if that byte is the `\r` of a `\r\n` still arriving.
    const std::size_t tail = held();
    return tail > kMaxCommandBytes + 1 || (tail == kMaxCommandBytes + 1 && bytes_.back() != '\r');
}

bool HeldInput::ready() const {
    return newline() != std::string::npos || tail_too_long() || ended_;
}

void HeldInput::refuse(TooLongLine* refused) const {
    refused->line = line_;
    refused->beginning.assign(bytes_, head_, std::min(kTooLongShownBytes, held()));
}

void HeldInput::consume_to(std::size_t end) {
    head_ = end;
    scanned_ = end;
    ++line_;
    if (head_ == bytes_.size()) {
        bytes_.clear(); // keeps its capacity
        head_ = 0;
        scanned_ = 0;
    }
}

HeldInput::Taken HeldInput::take(std::string* line, TooLongLine* refused) {
    const std::size_t nl = newline();
    if (nl != std::string::npos) {
        const std::size_t len = command_length(bytes_, head_, nl);
        if (len > kMaxCommandBytes) {
            refuse(refused);
            consume_to(nl + 1);
            return Taken::TooLong;
        }
        line->assign(bytes_, head_, len);
        consume_to(nl + 1);
        return Taken::Line;
    }
    if (tail_too_long()) {
        // REFUSED BEFORE IT ENDS. Everything held is this one line, and it is already longer
        // than any command: release it now and discard the rest as it arrives.
        refuse(refused);
        consume_to(bytes_.size());
        discarding_ = !ended_;
        return Taken::TooLong;
    }
    if (ended_) {
        if (held() == 0) {
            return Taken::Ended;
        }
        // A stream that ended with a last line and no newline still runs that command once.
        line->assign(bytes_, head_, command_length(bytes_, head_, bytes_.size()));
        consume_to(bytes_.size());
        return Taken::Line;
    }
    return Taken::Nothing;
}

#ifdef _WIN32

// ---- Windows: a reader thread and the waiting area -----------------------------
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
    std::condition_variable for_host;   ///< a line, a refusal or the end can be taken
    std::condition_variable for_reader; ///< room was made, or the reader must stop
    HeldInput held;                     ///< under `m`
    bool stop = false;                  ///< under `m`
    HANDLE handle = nullptr;            ///< the reader's own; closed once the thread has left it
};

unsigned __stdcall read_lines(void* arg) {
    // Take ownership of the one reference the starter handed over.
    std::shared_ptr<Shared> s = std::move(*static_cast<std::shared_ptr<Shared>*>(arg));
    delete static_cast<std::shared_ptr<Shared>*>(arg);

    char buf[kReadChunkBytes];
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(s->m);
            // THE BOUND ON READ-AHEAD. The thread reads only while a whole read fits in the
            // waiting area, and otherwise waits for the host to take a line. It used to read
            // whatever a pipe or a file held, as fast as it could, into a queue with no limit.
            s->for_reader.wait(lock, [&s] { return s->stop || s->held.room() >= kReadChunkBytes; });
            if (s->stop) {
                break;
            }
        }
        DWORD got = 0;
        // THE BLOCKING READ, where blocking stops nobody. For a console this is the cooked read:
        // it returns a finished line (or the next part of a long one) and nothing sooner. For a
        // pipe it returns what is there.
        if (ReadFile(s->handle, buf, static_cast<DWORD>(sizeof buf), &got, nullptr) == 0 ||
            got == 0) {
            // End of file, a broken pipe, Ctrl+Z at the start of a console line — or a
            // cancellation, after which nobody reads what is held.
            break;
        }
        {
            std::lock_guard<std::mutex> lock(s->m);
            s->held.add(buf, static_cast<std::size_t>(got));
        }
        s->for_host.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(s->m);
        s->held.end();
    }
    s->for_host.notify_all();
    return 0;
}

} // namespace

struct LineInput::Impl {
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    HANDLE thread = nullptr;
    bool started = false;
    TooLongLine too_long; ///< the host thread's alone

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
            shared->held.end();
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
            shared->held.end();
            return;
        }
        thread = reinterpret_cast<HANDLE>(t);
    }

    ~Impl() {
        if (thread == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(shared->m);
            shared->stop = true;
        }
        // A thread waiting for room wakes and leaves without reading again...
        shared->for_reader.notify_all();
        // ...and one inside a read is cancelled — REPEATEDLY, because a cancel that lands
        // between two reads finds nothing to cancel and the next read would then wait for a
        // line nobody is going to type.
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
    HeldInput::Taken taken = HeldInput::Taken::Nothing;
    {
        std::unique_lock<std::mutex> lock(s.m);
        if (timeout_ms > 0) {
            // THE DEADLINE, and it is a real one: nothing on this thread is inside a read.
            (void)s.for_host.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                      [&s] { return s.held.ready(); });
        }
        taken = s.held.take(out, &impl_->too_long);
    }
    if (taken == HeldInput::Taken::Line || taken == HeldInput::Taken::TooLong) {
        s.for_reader.notify_one(); // room was made
    }
    return status_of(taken, out);
}

LineInput::Held LineInput::held() const {
    std::lock_guard<std::mutex> lock(impl_->shared->m);
    return Held{impl_->shared->held.held(), impl_->shared->held.peak()};
}

#else // POSIX

// ---- POSIX: poll, then read what poll promised, into the waiting area ------------

struct LineInput::Impl {
    HeldInput held;
    TooLongLine too_long;
    char buf[kReadChunkBytes];
};

LineInput::LineInput() : impl_(std::make_unique<Impl>()) {}

LineInput::~LineInput() = default;

LineInput::Status LineInput::read(std::string* out, int timeout_ms) {
    Impl& in = *impl_;
    // A line already held is answered without consulting the OS at all: a pipe hands over
    // several commands in one go, and a caller that went back to waiting between them would
    // serve a script one line per timeout.
    HeldInput::Taken taken = in.held.take(out, &in.too_long);
    if (taken != HeldInput::Taken::Nothing) {
        return status_of(taken, out);
    }

    // NOTHING IS FINISHED, so what is held is one unfinished line no longer than a command, and
    // a whole read fits. The reader reads only now, and only until a line is ready — which is its
    // whole bound on read-ahead: with finished lines waiting, it leaves the rest with the OS.
    //
    // UNTIL THE DEADLINE, not for one read. A read can complete nothing — part of a line, or part
    // of a refused one being discarded — while more is already there to read, and "Idle" means
    // nothing was ready in time, on every platform.
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        struct pollfd p {};
        p.fd = STDIN_FILENO;
        p.events = POLLIN;
        // A canonical-mode terminal is readable only at a finished line, so a half-typed
        // command is `0` here and the caller goes back to the bus.
        const int ready_fds = ::poll(&p, 1, left.count() > 0 ? static_cast<int>(left.count()) : 0);
        if (ready_fds == 0) {
            return Status::Idle;
        }
        if (ready_fds < 0) {
            if (errno == EINTR) {
                return Status::Idle; // a signal is not end of input
            }
            in.held.end();
            return status_of(in.held.take(out, &in.too_long), out);
        }
        const std::size_t want = std::min(sizeof in.buf, in.held.room());
        if (want == 0) {
            // Unreachable while the static_assert above holds — and a zero-byte read would come
            // back as 0, which is end of input. No room is never the end.
            return Status::Idle;
        }
        const ssize_t got = ::read(STDIN_FILENO, in.buf, want);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                return Status::Idle;
            }
            in.held.end();
            return status_of(in.held.take(out, &in.too_long), out);
        }
        if (got == 0) {
            in.held.end(); // and the pass below flushes a newline-less last line
            return status_of(in.held.take(out, &in.too_long), out);
        }
        in.held.add(in.buf, static_cast<std::size_t>(got));
        taken = in.held.take(out, &in.too_long);
        if (taken != HeldInput::Taken::Nothing || Clock::now() >= deadline) {
            return status_of(taken, out);
        }
    }
}

LineInput::Held LineInput::held() const { return Held{impl_->held.held(), impl_->held.peak()}; }

#endif

const TooLongLine& LineInput::too_long() const noexcept { return impl_->too_long; }

} // namespace loom::host
