// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE SUPPLIED HOST'S INPUT, THROUGH A REAL TERMINAL.
//
// WHY THIS IS NOT IN tests/host_process/run.cmake. That lane feeds the host a FILE, and on
// Windows a file is the one kind of stdin whose reader was never wrong. The console reader
// took a queued character to mean "a read will not block", and a console with line input —
// every ordinary console — does not finish a read until Enter: a person who paused halfway
// through a command stopped every weave in the host, and 32 key-ups in front of a finished
// line hid it for good. A redirected stdin cannot show either. So this program builds the
// terminal a person actually types into — its own hidden native console on Windows, a
// pseudo-terminal on POSIX — and types into it.
//
// THREE MODES, THREE CTest ENTRIES:
//
//   reader   `loom::host::LineInput` itself (portable): a half-typed line is Idle at once and
//            at its deadline; it completes when finished; lines behind non-character records
//            are reached; several lines typed at once are each read; stopping a reader with a
//            half-typed line pending is prompt and leaves the terminal usable for the next
//            reader; end of input is Closed; the input limits hold through the terminal's own
//            line discipline (a command of exactly the limit, a longer one refused, a pasted
//            burst while nothing is taken, a reader stopped while it waits for room); the
//            terminal's mode is exactly what it was.
//
//   streams  `LineInput` on a PIPE and a FILE (portable), with the same assertions on every
//            platform: a producer far ahead of a host that takes nothing is held back, not read
//            out, and every line then arrives exactly and in order; commands at, over and far
//            over the limit; a line that never ends; a read that waits for the rest of a line
//            until its deadline; the end of input; stopping a reader while its producer waits
//            for room, and while a read is pending; what a replaced reader loses. What the
//            reader holds is read from `LineInput::held()`, never guessed from process memory.
//            (Not a terminal — and a pipe is not evidence about one either.)
//
//   host     a real `loom-host` with the probe weave booted (kernel gate): a delayed answer is
//            delivered AND REPORTED while a command is half typed; cooperative work
//            progresses through a pause mid-command; a typed line longer than any command is
//            refused and the next command runs; inspection, authority administration,
//            `stop` and `quit` are all reachable with pauses and non-character records inside
//            the lines, and the host exits 0.
//
// THE DISCRIMINATING ARRANGEMENT, so this is not a timing test. The command that starts the
// delayed answer and the first half of the NEXT command are typed in one burst. The first
// line is finished; the second is not. A reader that blocks on a half-typed line blocks
// immediately after the first command, before the countdown can possibly complete, so its
// answer never arrives; a reader that does not, reports it. No sleep decides the outcome.
//
// On Windows the terminal work runs in a CHILD of this program, started detached and hidden,
// which allocates its own console. A console window is therefore never shown, and the ctest
// process's own console and pipes are never replaced. The child writes its report to a file
// that the parent prints.
//
// Exit codes: 0 every check passed, 1 a check failed, 2 bad command line, 3 the terminal could
// not be built — which is a failure, not a skip: a witness without a terminal proves nothing.

#include "line_input.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using loom::host::LineInput;

long long ms_since(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/// What "behind non-character input" means on this platform, for a check's own words. A
/// pseudo-terminal has no input record that yields no character, so there the same check is an
/// ordinary line and says so rather than claiming something it did not exercise.
#ifdef _WIN32
constexpr const char* kBehindNoise = "behind 48 non-character records";
#else
constexpr const char* kBehindNoise = "(a pseudo-terminal has no non-character records)";
#endif

// ---- the report ------------------------------------------------------------------

struct Report {
    int checks = 0;
    int failed = 0;
    std::ostringstream text;

    void check(const std::string& what, bool ok, const std::string& detail = std::string()) {
        ++checks;
        if (!ok) {
            ++failed;
        }
        text << (ok ? "  ok: " : "  FAILED: ") << what;
        if (!detail.empty()) {
            text << "  [" << detail << "]";
        }
        text << '\n';
    }
    void note(const std::string& line) { text << "  " << line << '\n'; }
};

const char* status_name(LineInput::Status s) {
    switch (s) {
    case LineInput::Status::Line:
        return "Line";
    case LineInput::Status::Idle:
        return "Idle";
    case LineInput::Status::TooLong:
        return "TooLong";
    case LineInput::Status::Closed:
        return "Closed";
    }
    return "?";
}

using loom::host::kHeldInputBytes;
using loom::host::kMaxCommandBytes;

/// Line `i` of a numbered burst: exactly `n` bytes (n >= 16) before its line ending, carrying its
/// own index so order and content are both checkable, and made only of what a terminal passes
/// through untouched (letters, digits, spaces).
std::string numbered(std::size_t i, std::size_t n) {
    std::string s = "line " + std::to_string(i) + " ";
    while (s.size() < n) {
        s.push_back(static_cast<char>('a' + (s.size() + i) % 26));
    }
    s.resize(n);
    return s;
}

/// The index a numbered line carries, or -1 when it is not one (a fragment, say).
long long index_of(const std::string& s) {
    if (s.rfind("line ", 0) != 0) {
        return -1;
    }
    std::size_t i = 5;
    long long v = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        ++i;
        any = true;
    }
    return any ? v : -1;
}

std::string held_detail(const LineInput::Held& h) {
    return "held " + std::to_string(h.bytes) + ", peak " + std::to_string(h.peak) + ", limit " +
           std::to_string(kHeldInputBytes);
}

// ---- a terminal a person types into -----------------------------------------------
//
// `type` delivers text the way a keyboard does (`\n` is Enter); `noise` delivers input that
// produces no character at all. A pseudo-terminal has no such records, so there it is a no-op.

#ifdef _WIN32

struct Terminal {
    HANDLE conin = INVALID_HANDLE_VALUE;

    bool open() {
        FreeConsole();
        if (AllocConsole() == 0) {
            return false;
        }
        if (HWND w = GetConsoleWindow()) {
            ShowWindow(w, SW_HIDE);
        }
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        conin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        return conin != INVALID_HANDLE_VALUE;
    }

    static INPUT_RECORD key(wchar_t ch, bool down, WORD vk, DWORD state) {
        INPUT_RECORD r{};
        r.EventType = KEY_EVENT;
        r.Event.KeyEvent.bKeyDown = down ? TRUE : FALSE;
        r.Event.KeyEvent.wRepeatCount = 1;
        r.Event.KeyEvent.wVirtualKeyCode = vk;
        r.Event.KeyEvent.uChar.UnicodeChar = ch;
        r.Event.KeyEvent.dwControlKeyState = state;
        return r;
    }

    void write(const std::vector<INPUT_RECORD>& rs) const {
        std::size_t done = 0;
        while (done < rs.size()) {
            DWORD n = 0;
            // In batches: a pasted burst is hundreds of thousands of records, and one call that
            // large is not a thing a keyboard or a paste ever asks of a console.
            const std::size_t batch = std::min<std::size_t>(rs.size() - done, 4096);
            if (WriteConsoleInputW(conin, rs.data() + done, static_cast<DWORD>(batch), &n) == 0 ||
                n == 0) {
                return;
            }
            done += n;
        }
    }

    /// Key-down and key-up per character, with the virtual key a keyboard would send — so
    /// `-` is VK_OEM_MINUS rather than a byte value that happens to be VK_INSERT.
    std::vector<INPUT_RECORD> keys(const std::string& text) const {
        std::vector<INPUT_RECORD> rs;
        for (char c : text) {
            if (c == '\n') {
                rs.push_back(key(L'\r', true, VK_RETURN, 0));
                rs.push_back(key(L'\r', false, VK_RETURN, 0));
                continue;
            }
            const SHORT scan = VkKeyScanA(c);
            const WORD vk = scan == -1 ? 0 : static_cast<WORD>(scan & 0xff);
            const DWORD state = (scan != -1 && (scan & 0x100) != 0) ? SHIFT_PRESSED : 0;
            const wchar_t ch = static_cast<wchar_t>(static_cast<unsigned char>(c));
            rs.push_back(key(ch, true, vk, state));
            rs.push_back(key(ch, false, vk, state));
        }
        return rs;
    }

    void type(const std::string& text) const { write(keys(text)); }

    /// `count` records that produce no character: key-ups, modifier key-downs, focus, mouse,
    /// menu and buffer-size events, in rotation.
    std::vector<INPUT_RECORD> noise_records(int count) const {
        std::vector<INPUT_RECORD> rs;
        for (int i = 0; i < count; ++i) {
            INPUT_RECORD r{};
            switch (i % 6) {
            case 0:
                r = key(0, false, 'Q', 0);
                break;
            case 1:
                r = key(0, true, VK_SHIFT, SHIFT_PRESSED);
                break;
            case 2:
                r.EventType = FOCUS_EVENT;
                r.Event.FocusEvent.bSetFocus = (i % 4) == 0 ? TRUE : FALSE;
                break;
            case 3:
                r.EventType = MOUSE_EVENT;
                r.Event.MouseEvent.dwEventFlags = MOUSE_MOVED;
                r.Event.MouseEvent.dwMousePosition = {static_cast<SHORT>(i % 80), 1};
                break;
            case 4:
                r.EventType = MENU_EVENT;
                r.Event.MenuEvent.dwCommandId = 1;
                break;
            default:
                r.EventType = WINDOW_BUFFER_SIZE_EVENT;
                r.Event.WindowBufferSizeEvent.dwSize = {120, 30};
                break;
            }
            rs.push_back(r);
        }
        return rs;
    }

    void noise(int count) const { write(noise_records(count)); }

    /// Noise first, then text, in ONE write: the records are in the buffer together.
    void noise_then_type(int count, const std::string& text) const {
        std::vector<INPUT_RECORD> rs = noise_records(count);
        const std::vector<INPUT_RECORD> ks = keys(text);
        rs.insert(rs.end(), ks.begin(), ks.end());
        write(rs);
    }

    std::string mode() const {
        DWORD m = 0;
        if (GetConsoleMode(conin, &m) == 0) {
            return "unreadable";
        }
        return std::to_string(m);
    }
    bool line_input() const {
        DWORD m = 0;
        return GetConsoleMode(conin, &m) != 0 && (m & ENABLE_LINE_INPUT) != 0;
    }
    void end_of_input() const { type(std::string(1, '\x1a') + "\n"); }
};

#else

struct Terminal {
    int master = -1;
    std::string slave_name;

    bool open() {
        master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
            return false;
        }
        const char* name = ptsname(master);
        if (name == nullptr) {
            return false;
        }
        slave_name = name;
        return true;
    }

    void type(const std::string& text) const {
        std::size_t done = 0;
        while (done < text.size()) {
            const ssize_t n = ::write(master, text.data() + done, text.size() - done);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                return;
            }
            done += static_cast<std::size_t>(n);
        }
    }
    void noise(int) const {}
    void noise_then_type(int, const std::string& text) const { type(text); }
};

#endif

/// A long burst typed on its own thread — a pasted script. A pseudo-terminal's writer waits for
/// its reader once the line discipline is full, so typing it from the checking thread would
/// deadlock the check it is part of. On POSIX it also drains what the terminal echoes.
class Typist {
public:
    Typist(const Terminal& term, std::string text) {
#ifndef _WIN32
        const int master = term.master;
        drain_ = std::thread([this, master] {
            char buf[4096];
            while (!stop_.load()) {
                struct pollfd p {};
                p.fd = master;
                p.events = POLLIN;
                if (::poll(&p, 1, 20) > 0 && ::read(master, buf, sizeof buf) <= 0) {
                    return;
                }
            }
        });
#endif
        // The flag is shared, not a member: a typist still blocked when a check gives up is
        // detached, and must not write into an object that is gone.
        typing_ = std::thread([done = done_, &term, burst = std::move(text)] {
            term.type(burst);
            done->store(true);
        });
    }
    Typist(const Typist&) = delete;
    Typist& operator=(const Typist&) = delete;

    /// Has everything been typed, within `limit_ms`?
    bool finished(int limit_ms) const {
        const auto t = Clock::now();
        while (!done_->load() && ms_since(t) < limit_ms) {
            sleep_ms(10);
        }
        return done_->load();
    }

    ~Typist() {
        if (typing_.joinable()) {
            if (done_->load()) {
                typing_.join();
            } else {
                typing_.detach(); // reported by the check that waited; the process ends soon
            }
        }
        stop_.store(true);
        if (drain_.joinable()) {
            drain_.join();
        }
    }

private:
    std::shared_ptr<std::atomic<bool>> done_ = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> stop_{false};
    std::thread typing_;
    std::thread drain_;
};

// ---- reader mode: LineInput on a real terminal ---------------------------------------

#ifndef _WIN32

/// The pseudo-terminal's slave made this process's stdin for the duration, then put back.
struct StdinFromTerminal {
    int saved = -1;
    int slave = -1;
    termios before{};
    bool ok = false;

    explicit StdinFromTerminal(const Terminal& t) {
        slave = ::open(t.slave_name.c_str(), O_RDWR | O_NOCTTY);
        saved = ::dup(STDIN_FILENO);
        ok = slave >= 0 && saved >= 0 && ::dup2(slave, STDIN_FILENO) >= 0 &&
             tcgetattr(STDIN_FILENO, &before) == 0;
    }
    ~StdinFromTerminal() {
        if (saved >= 0) {
            (void)::dup2(saved, STDIN_FILENO);
            ::close(saved);
        }
        if (slave >= 0) {
            ::close(slave);
        }
    }
    bool mode_unchanged() const {
        termios now{};
        return tcgetattr(STDIN_FILENO, &now) == 0 && now.c_lflag == before.c_lflag &&
               now.c_iflag == before.c_iflag && now.c_oflag == before.c_oflag;
    }
    bool canonical() const { return (before.c_lflag & ICANON) != 0; }
    char eof_char() const { return static_cast<char>(before.c_cc[VEOF]); }
};

#endif

/// A step that must not be able to hang the witness. If it has not finished within `limit_ms`, a
/// finished line is typed to release whatever is waiting on one — so a reader that blocks is
/// REPORTED, check by check, instead of being killed by the harness's timeout with nothing said.
template <class Step>
auto guarded(const Terminal& term, int limit_ms, Step step) {
    std::atomic<bool> done{false};
    std::thread guard([&term, &done, limit_ms] {
        const auto t = Clock::now();
        while (!done.load() && ms_since(t) < limit_ms) {
            sleep_ms(10);
        }
        if (!done.load()) {
            term.type("\n");
        }
    });
    auto result = step();
    done.store(true);
    guard.join();
    return result;
}

int run_reader(Report& r) {
    Terminal term;
    if (!term.open()) {
        r.note("could not build a terminal to type into");
        return 3;
    }
#ifdef _WIN32
    SetStdHandle(STD_INPUT_HANDLE, term.conin);
    r.check("the console has line input on, as every ordinary console does", term.line_input(),
            "mode " + term.mode());
    const std::string mode_before = term.mode();
#else
    StdinFromTerminal stdin_tty(term);
    if (!stdin_tty.ok) {
        r.note("could not make the pseudo-terminal this process's stdin");
        return 3;
    }
    r.check("the terminal is in canonical mode, as every ordinary terminal is",
            stdin_tty.canonical());
#endif

    {
        LineInput input;
        std::string line;
        // Every read below is GUARDED (see `guarded`): its limit is its own deadline plus
        // 1500 ms, after which a finished line is typed to release a reader that blocked.
        const auto timed_read = [&](int timeout_ms) {
            return guarded(term, (timeout_ms > 0 ? timeout_ms : 0) + 1500,
                           [&] { return input.read(&line, timeout_ms); });
        };

        // Before anything is typed: nothing, at once.
        const auto t0 = Clock::now();
        LineInput::Status s = timed_read(0);
        const long long idle_ms = ms_since(t0);
        r.check("nothing typed: a zero deadline is Idle at once",
                s == LineInput::Status::Idle && idle_ms < 200,
                std::string(status_name(s)) + " in " + std::to_string(idle_ms) + " ms");

        // THE FINDING. A half-typed line, and a zero deadline.
        term.type("x");
        sleep_ms(100);
        const auto t1 = Clock::now();
        s = timed_read(0);
        const long long zero_ms = ms_since(t1);
        r.check("a half-typed line with a zero deadline is Idle at once",
                s == LineInput::Status::Idle && zero_ms < 200,
                std::string(status_name(s)) + " in " + std::to_string(zero_ms) + " ms");

        // ...and with a positive deadline, Idle AT the deadline: not sooner, not at Enter.
        const auto t2 = Clock::now();
        s = timed_read(300);
        const long long wait_ms = ms_since(t2);
        r.check("a half-typed line with a 300 ms deadline is Idle at the deadline",
                s == LineInput::Status::Idle && wait_ms >= 250 && wait_ms < 1500,
                std::string(status_name(s)) + " in " + std::to_string(wait_ms) + " ms");

        // Finishing it delivers exactly what was typed.
        term.type("\n");
        const auto t3 = Clock::now();
        s = timed_read(5000);
        r.check("finishing the line delivers it", s == LineInput::Status::Line && line == "x",
                std::string(status_name(s)) + " '" + line + "' in " +
                    std::to_string(ms_since(t3)) + " ms");

        // A finished line BEHIND input that produces no character (Windows records; a
        // pseudo-terminal has none, and there this is an ordinary line).
        term.noise_then_type(48, "yes\n");
        s = timed_read(5000);
        r.check(std::string("a finished line is reached ") + kBehindNoise,
                s == LineInput::Status::Line && line == "yes",
                std::string(status_name(s)) + " '" + line + "'");

        // Several lines typed in one burst are each read.
        term.type("first\nsecond\n");
        s = timed_read(5000);
        const bool first_ok = s == LineInput::Status::Line && line == "first";
        s = timed_read(5000);
        r.check("two lines typed at once are read as two lines",
                first_ok && s == LineInput::Status::Line && line == "second",
                std::string(status_name(s)) + " '" + line + "'");
    }

    // STOPPING A READER WITH A HALF-TYPED LINE PENDING is prompt, and leaves the terminal
    // usable: the next reader reads the next line, not a remnant and not nothing.
    {
        std::string line;
        long long stop_ms = 0;
        {
            auto input = std::make_unique<LineInput>();
            (void)guarded(term, 1500, [&] { return input->read(&line, 0); });
            term.type("part");
            sleep_ms(200);
            const auto t = Clock::now();
            (void)guarded(term, 5000, [&] {
                input.reset();
                return true;
            });
            stop_ms = ms_since(t);
        }
        r.check("stopping a reader while a line is half typed is prompt", stop_ms < 3000,
                std::to_string(stop_ms) + " ms");
#ifdef _WIN32
        // The half-typed text went with the reader it was typed to (its cooked read was
        // cancelled and its console handle closed). Nothing is left to eat the next line.
        term.type("next\n");
        const std::string expected = "next";
#else
        // A pseudo-terminal's line discipline keeps what was typed; the next reader gets the
        // whole line when it is finished. Either way, a finished line is delivered.
        term.type("next\n");
        const std::string expected = "partnext";
#endif
        LineInput next;
        const auto t = Clock::now();
        LineInput::Status s = guarded(term, 6500, [&] { return next.read(&line, 5000); });
        r.check("the next reader on the same terminal reads the next line",
                s == LineInput::Status::Line && line == expected,
                std::string(status_name(s)) + " '" + line + "' in " +
                    std::to_string(ms_since(t)) + " ms");

        // END OF INPUT typed at the start of a line (Ctrl+Z on Windows, the VEOF character on a
        // terminal) is Closed, and stays Closed.
#ifdef _WIN32
        term.end_of_input();
#else
        term.type(std::string(1, stdin_tty.eof_char()));
#endif
        s = guarded(term, 6500, [&] { return next.read(&line, 5000); });
        const LineInput::Status again = guarded(term, 1500, [&] { return next.read(&line, 0); });
        r.check("end of input typed at the start of a line is Closed, and stays Closed",
                s == LineInput::Status::Closed && again == LineInput::Status::Closed,
                std::string(status_name(s)) + ", then " + status_name(again));
    }

    // ---- THE INPUT LIMITS, THROUGH THE TERMINAL'S OWN LINE DISCIPLINE -------------------
    //
    // The stream checks prove the rules on pipes and files; these prove them where the platforms
    // differ. A Linux terminal keeps only the first 4095 bytes of a typed line and drops the rest
    // without a sign; a Windows console delivers the whole line.
    {
        LineInput input;
        std::string line;

        const std::string at_limit = numbered(1, kMaxCommandBytes);
        term.type(at_limit + "\n");
        LineInput::Status s = guarded(term, 6500, [&] { return input.read(&line, 5000); });
        r.check("a command of exactly the limit, typed, is read whole",
                s == LineInput::Status::Line && line == at_limit,
                std::string(status_name(s)) + ", " + std::to_string(line.size()) + " bytes");

        // `quit` and a line of spaces is the command a reader that truncates — or runs what a
        // terminal truncated — would execute as `quit`. It is refused whole, and the next line
        // is read as usual.
        term.type("quit" + std::string(4996, ' ') + "\n");
        s = guarded(term, 6500, [&] { return input.read(&line, 5000); });
        const bool refused = s == LineInput::Status::TooLong && line.empty() &&
                             input.too_long().beginning.rfind("quit ", 0) == 0;
        const std::string refused_detail = std::string(status_name(s)) + " (input line " +
                                           std::to_string(input.too_long().line) + ")";
        term.type("after\n");
        const LineInput::Status next = guarded(term, 6500, [&] { return input.read(&line, 5000); });
        r.check("a typed line longer than any command is refused whole, and the next line is read",
                refused && next == LineInput::Status::Line && line == "after",
                refused_detail + ", then " + status_name(next) + " '" + line + "'");
    }

    // A PASTED BURST WHILE THE HOST TAKES NOTHING: bounded, then every line, in order.
    constexpr std::size_t kBurstLines = 1500; // 150 KB of 100-byte lines, twice the limit
    std::string burst;
    for (std::size_t i = 0; i < kBurstLines; ++i) {
        burst += numbered(i, 99) + "\n";
    }
    {
        Typist typist(term, burst);
        LineInput input;
        std::string line;
        LineInput::Status s = guarded(term, 6500, [&] { return input.read(&line, 5000); });
        const bool first = s == LineInput::Status::Line && line == numbered(0, 99);
        sleep_ms(500); // THE HOST TAKES NOTHING
        const LineInput::Held during = input.held();
        r.check("a pasted burst while the host takes nothing: the reader holds no more than the "
                "limit",
                first && during.bytes <= kHeldInputBytes && during.peak <= kHeldInputBytes,
                held_detail(during));
        std::size_t n = 1;
        bool exact = first;
        s = guarded(term, 60000, [&] {
            LineInput::Status last = LineInput::Status::Idle;
            const auto t = Clock::now();
            while (n < kBurstLines && ms_since(t) < 55000) {
                last = input.read(&line, 1000);
                if (last == LineInput::Status::Idle) {
                    continue;
                }
                if (last != LineInput::Status::Line || line != numbered(n, 99)) {
                    exact = false;
                }
                ++n;
            }
            return last;
        });
        r.check("...then taken again, every line arrives exactly and in order",
                exact && n == kBurstLines && typist.finished(5000),
                std::to_string(n) + " of " + std::to_string(kBurstLines) + " lines, " +
                    held_detail(input.held()));
    }

    // A READER STOPPED WHILE IT WAITS FOR ROOM (the Windows thread's arrangement; a
    // pseudo-terminal reader reads only when asked), and the terminal it leaves behind: the next
    // reader reads intact lines, in order, to the end of the burst.
    {
        Typist typist(term, burst);
        std::string line;
        long long stop_ms = 0;
        LineInput::Held at_stop{};
        {
            auto input = std::make_unique<LineInput>();
            (void)guarded(term, 6500, [&] { return input->read(&line, 5000); });
#ifdef _WIN32
            // The reader thread fills the area and then waits; a POSIX reader never reads ahead.
            const auto t = Clock::now();
            while (input->held().bytes + 4096 <= kHeldInputBytes && ms_since(t) < 3000) {
                sleep_ms(10);
            }
#else
            sleep_ms(200);
#endif
            at_stop = input->held();
            const auto stop = Clock::now();
            input.reset();
            stop_ms = ms_since(stop);
        }
#ifdef _WIN32
        const bool waiting = at_stop.bytes + 4096 > kHeldInputBytes;
        const std::string how = "its reader thread waiting for room";
#else
        const bool waiting = at_stop.bytes <= kHeldInputBytes;
        const std::string how = "a pseudo-terminal reader holds only what it was asked for";
#endif
        r.check("stopping a reader while a burst waits behind it is prompt (" + how + ")",
                waiting && stop_ms < 1500,
                std::to_string(stop_ms) + " ms, " + held_detail(at_stop));

        LineInput next;
        std::vector<std::string> lines;
        (void)guarded(term, 60000, [&] {
            const auto t = Clock::now();
            while (ms_since(t) < 55000) {
                const LineInput::Status s = next.read(&line, 1000);
                if (s == LineInput::Status::Line) {
                    lines.push_back(line);
                    if (line == numbered(kBurstLines - 1, 99)) {
                        break;
                    }
                }
            }
            return true;
        });
        const long long k = lines.empty() ? -1 : index_of(lines.front());
        bool in_order = k >= 1 && !lines.empty();
        for (std::size_t j = 0; in_order && j < lines.size(); ++j) {
            in_order = lines[j] == numbered(static_cast<std::size_t>(k) + j, 99);
        }
        const std::size_t lost = k >= 1 ? static_cast<std::size_t>(k - 1) * 100 : 0;
        r.check("...and the next reader reads intact lines in order to the end, having lost no "
                "more than the limit",
                in_order && lines.size() == kBurstLines - static_cast<std::size_t>(k) &&
                    lost <= kHeldInputBytes && typist.finished(5000),
                "resumed at line " + std::to_string(k) + ", " + std::to_string(lines.size()) +
                    " lines, " + std::to_string(lost) + " bytes lost with the stopped reader");
    }

#ifdef _WIN32
    r.check("the console's mode is exactly what it was", term.mode() == mode_before,
            mode_before + " -> " + term.mode());
#else
    r.check("the terminal's mode is exactly what it was", stdin_tty.mode_unchanged());
#endif
    return r.failed == 0 ? 0 : 1;
}

// ---- streams mode: LineInput on a pipe and on a file -------------------------------
//
// Every assertion here is the same on every platform. The ARRANGEMENT differs — a POSIX reader
// reads only when asked and nothing is finished; the Windows reader thread reads ahead until the
// waiting area is full — so what a reader holds at a given moment differs as well. The checks
// bound it with `LineInput::held()`; they never expect one platform's number.

#ifdef _WIN32
using PipeEnd = HANDLE;
#else
using PipeEnd = int;
#endif

/// A pipe whose read end becomes this process's stdin and whose write end a `Producer` takes.
struct Pipe {
#ifdef _WIN32
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    bool open() { return CreatePipe(&rd, &wr, nullptr, 0) != 0; }
    void close_read() {
        if (rd != nullptr) {
            CloseHandle(rd);
            rd = nullptr;
        }
    }
#else
    int rd = -1;
    int wr = -1;
    bool open() {
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) {
            return false;
        }
        rd = fds[0];
        wr = fds[1];
        return true;
    }
    void close_read() {
        if (rd >= 0) {
            ::close(rd);
            rd = -1;
        }
    }
#endif
    Pipe() = default;
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    ~Pipe() { close_read(); }
};

/// `in` as this process's stdin for the scope, and the previous stdin back after it.
struct StdinFrom {
#ifdef _WIN32
    HANDLE saved;
    explicit StdinFrom(HANDLE in) : saved(GetStdHandle(STD_INPUT_HANDLE)) {
        SetStdHandle(STD_INPUT_HANDLE, in);
    }
    ~StdinFrom() { SetStdHandle(STD_INPUT_HANDLE, saved); }
#else
    int saved;
    explicit StdinFrom(int in) : saved(::dup(STDIN_FILENO)) { (void)::dup2(in, STDIN_FILENO); }
    ~StdinFrom() {
        if (saved >= 0) {
            (void)::dup2(saved, STDIN_FILENO);
            ::close(saved);
        } else {
            ::close(STDIN_FILENO); // there was none: leave none, or the stream would stay open
        }
    }
#endif
    StdinFrom(const StdinFrom&) = delete;
    StdinFrom& operator=(const StdinFrom&) = delete;
};

/// What a script piped into the host is: a writer on its own thread, writing as fast as the pipe
/// accepts it, in pieces of awkward sizes so that lines are split every way a pipe splits them.
/// With `pause`, it stops after `first` until `release()`. It closes its end when it is done, or
/// when the reading end has gone. Its state is shared, so a writer still blocked when a check
/// gives up never touches freed memory.
class Producer {
public:
    Producer(Pipe& pipe, std::string first, std::string rest = std::string(), bool pause = false)
        : state_(std::make_shared<State>()) {
        state_->paused = pause;
        const PipeEnd wr = pipe.wr;
#ifdef _WIN32
        pipe.wr = nullptr;
#else
        pipe.wr = -1;
#endif
        thread_ = std::thread([s = state_, wr, a = std::move(first), b = std::move(rest)] {
            bool ok = write_all(*s, wr, a);
            if (ok) {
                std::unique_lock<std::mutex> lock(s->m);
                s->cv.wait(lock, [&s] { return !s->paused; });
            }
            ok = ok && write_all(*s, wr, b);
            s->broken.store(!ok);
#ifdef _WIN32
            CloseHandle(wr);
#else
            ::close(wr);
#endif
            s->finished.store(true);
        });
    }
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    std::size_t written() const { return state_->written.load(); }
    bool broken() const { return state_->broken.load(); }

    void release() {
        {
            std::lock_guard<std::mutex> lock(state_->m);
            state_->paused = false;
        }
        state_->cv.notify_all();
    }

    bool finished(int limit_ms) const {
        const auto t = Clock::now();
        while (!state_->finished.load() && ms_since(t) < limit_ms) {
            sleep_ms(10);
        }
        return state_->finished.load();
    }

    /// Has it stopped making progress — `quiet_ms` without a byte accepted, and not finished —
    /// within `limit_ms`? That is a writer waiting for its reader.
    bool held_back(int quiet_ms, int limit_ms) const {
        const auto t = Clock::now();
        std::size_t last = written();
        auto since = Clock::now();
        while (ms_since(t) < limit_ms) {
            sleep_ms(20);
            const std::size_t now = written();
            if (now != last || state_->finished.load()) {
                last = now;
                since = Clock::now();
            } else if (ms_since(since) >= quiet_ms) {
                return true;
            }
        }
        return false;
    }

    ~Producer() {
        release();
        if (state_->finished.load()) {
            thread_.join();
        } else {
            thread_.detach();
        }
    }

private:
    struct State {
        std::atomic<std::size_t> written{0};
        std::atomic<bool> finished{false};
        std::atomic<bool> broken{false};
        std::mutex m;
        std::condition_variable cv;
        bool paused = false;
    };

    static bool write_all(State& s, PipeEnd wr, const std::string& data) {
        static constexpr std::size_t kPieces[] = {1, 7, 4093, 511, 65537, 3, 20000};
        std::size_t off = 0;
        for (std::size_t k = 0; off < data.size(); ++k) {
            const std::size_t n = std::min(kPieces[k % 7], data.size() - off);
#ifdef _WIN32
            DWORD w = 0;
            if (WriteFile(wr, data.data() + off, static_cast<DWORD>(n), &w, nullptr) == 0) {
                return false;
            }
            off += w;
            s.written.fetch_add(w);
#else
            const ssize_t w = ::write(wr, data.data() + off, n);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            off += static_cast<std::size_t>(w);
            s.written.fetch_add(static_cast<std::size_t>(w));
#endif
        }
        return true;
    }

    std::shared_ptr<State> state_;
    std::thread thread_;
};

/// A burst of `count` numbered commands of 127 bytes each, one per line.
std::string numbered_burst(std::size_t count) {
    std::string data;
    data.reserve(count * 128);
    for (std::size_t i = 0; i < count; ++i) {
        data += numbered(i, 127);
        data += '\n';
    }
    return data;
}

/// Read line after line until the input ends or `limit_ms` passes, and say whether every line was
/// the next numbered one of 127 bytes, starting at `from`. Returns how many arrived.
std::size_t read_numbered(LineInput& input, std::size_t from, int limit_ms, bool* exact,
                          LineInput::Status* last) {
    std::string line;
    std::size_t n = 0;
    const auto t = Clock::now();
    for (;;) {
        const LineInput::Status s = input.read(&line, 1000);
        *last = s;
        if (s == LineInput::Status::Closed || ms_since(t) >= limit_ms) {
            break;
        }
        if (s == LineInput::Status::Idle) {
            continue;
        }
        if (s != LineInput::Status::Line || line != numbered(from + n, 127)) {
            *exact = false;
        }
        ++n;
    }
    return n;
}

void stream_burst_held_back(Report& r) {
    // Forty times the limit, from a producer far faster than a host that takes nothing.
    constexpr std::size_t kLines = 20000;
    const std::string data = numbered_burst(kLines);
    Pipe pipe;
    if (!pipe.open()) {
        r.check("a pipe could be made", false);
        return;
    }
    Producer producer(pipe, data);
    {
        StdinFrom from(pipe.rd);
        LineInput input;
        std::string line;
        LineInput::Status s = input.read(&line, 5000);
        bool exact = s == LineInput::Status::Line && line == numbered(0, 127);

        sleep_ms(500); // THE HOST TAKES NOTHING
        const LineInput::Held during = input.held();
        const std::size_t written = producer.written();
        r.check("pipe: while the host takes nothing, the reader holds no more than the limit",
                exact && during.bytes <= kHeldInputBytes && during.peak <= kHeldInputBytes,
                held_detail(during));
        r.check("pipe: ...and the rest waits with its producer, which is held back, not read out",
                written < data.size(),
                "written " + std::to_string(written) + " of " + std::to_string(data.size()));

        const std::size_t n = 1 + read_numbered(input, 1, 60000, &exact, &s);
        r.check("pipe: taken again, every line arrives exactly and in order, then the end",
                exact && n == kLines && s == LineInput::Status::Closed,
                std::to_string(n) + " lines, then " + status_name(s));
        r.check("pipe: the most the reader ever held stayed within the limit",
                input.held().peak <= kHeldInputBytes, held_detail(input.held()));
    }
    r.check("pipe: the producer finished, once the host kept up",
            producer.finished(10000) && !producer.broken());
}

void stream_file_held_back(Report& r, const std::string& work) {
    constexpr std::size_t kLines = 20000;
    const std::string path = work + "/burst.txt";
    const std::string data = numbered_burst(kLines);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << data;
    }
#ifdef _WIN32
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool opened = file != INVALID_HANDLE_VALUE;
#else
    const int file = ::open(path.c_str(), O_RDONLY);
    const bool opened = file >= 0;
#endif
    if (!opened) {
        r.check("a file of commands could be opened", false, path);
        return;
    }
    {
        StdinFrom from(file);
        LineInput input;
        std::string line;
        LineInput::Status s = input.read(&line, 5000);
        bool exact = s == LineInput::Status::Line && line == numbered(0, 127);
        sleep_ms(300); // THE HOST TAKES NOTHING, and the whole file is there to be read
        const LineInput::Held during = input.held();
        r.check("file: while the host takes nothing, the reader holds no more than the limit",
                exact && during.bytes <= kHeldInputBytes && during.peak <= kHeldInputBytes,
                held_detail(during) + " of a " + std::to_string(data.size()) + "-byte file");
        const std::size_t n = 1 + read_numbered(input, 1, 60000, &exact, &s);
        r.check("file: taken again, every line arrives exactly and in order, then the end",
                exact && n == kLines && s == LineInput::Status::Closed &&
                    input.held().peak <= kHeldInputBytes,
                std::to_string(n) + " lines, then " + status_name(s) + ", " +
                    held_detail(input.held()));
    }
#ifdef _WIN32
    CloseHandle(file);
#else
    ::close(file);
#endif
    std::remove(path.c_str());
}

void stream_limits(Report& r) {
    const std::string at_limit = numbered(1, kMaxCommandBytes);
    const std::string at_limit_crlf = numbered(2, kMaxCommandBytes);
    // One byte over, and shaped so that a reader that cut it down would run `quit`.
    const std::string over = "quit" + std::string(kMaxCommandBytes - 3, ' ');
    // A megabyte of one line, ending in something a reader that split it would run.
    const std::string endless = std::string(std::size_t{1} << 20, ' ') + "quit";
    const std::string data = at_limit + "\n" + at_limit_crlf + "\r\n" + over + "\n" +
                             "after three\n" + endless + "\n" + "after five\n" +
                             "last, without a newline";
    Pipe pipe;
    if (!pipe.open()) {
        r.check("a pipe could be made", false);
        return;
    }
    Producer producer(pipe, data);
    StdinFrom from(pipe.rd);
    LineInput input;
    std::vector<std::string> seen;
    std::string line;
    const auto t = Clock::now();
    int closed = 0;
    while (closed < 2 && ms_since(t) < 60000) {
        line = "(stale)";
        const LineInput::Status s = input.read(&line, 1000);
        if (s == LineInput::Status::Idle) {
            continue;
        }
        if (s == LineInput::Status::TooLong) {
            // Nothing of a refused line is handed out: `line` is emptied, not left stale.
            seen.push_back("TOO LONG line " + std::to_string(input.too_long().line) + " '" +
                           input.too_long().beginning.substr(0, 4) + "'" +
                           (line.empty() ? "" : " WITH TEXT"));
        } else if (s == LineInput::Status::Closed) {
            seen.push_back("CLOSED");
            ++closed;
        } else {
            seen.push_back(line);
        }
    }
    const auto at = [&seen](std::size_t i) {
        return i < seen.size() ? seen[i] : std::string("(none)");
    };
    r.check("pipe: a command of exactly the limit is read whole, with either line ending",
            at(0) == at_limit && at(1) == at_limit_crlf,
            std::to_string(at(0).size()) + " and " + std::to_string(at(1).size()) + " bytes");
    r.check("pipe: one byte over is refused whole where it stood, and the next line is read",
            at(2) == "TOO LONG line 3 'quit'" && at(3) == "after three", at(2) + ", then " + at(3));
    r.check("pipe: a megabyte-long line is refused, never held, and the next line is read",
            at(4) == "TOO LONG line 5 '    '" && at(5) == "after five" &&
                input.held().peak <= kHeldInputBytes,
            at(4) + ", then " + at(5) + ", " + held_detail(input.held()));
    r.check("pipe: a last line without a newline is read, then the end, and the end stays the end",
            at(6) == "last, without a newline" && at(7) == "CLOSED" && at(8) == "CLOSED" &&
                seen.size() == 9,
            std::to_string(seen.size()) + " results; last three: " + at(6) + " / " + at(7) +
                " / " + at(8));
    (void)producer.finished(5000);
}

void stream_endless_line(Report& r) {
    // A line that never ends, from a producer that keeps writing it. A reader that held it until
    // its newline — or waited for room its own size rule would never give it — would stop
    // reading, and the producer would stop with it.
    constexpr std::size_t kEndless = std::size_t{4} << 20;
    Pipe pipe;
    if (!pipe.open()) {
        r.check("a pipe could be made", false);
        return;
    }
    Producer producer(pipe, std::string(kEndless, 'x'), "\nstatus\n", /*pause=*/true);
    StdinFrom from(pipe.rd);
    LineInput input;
    std::string line;
    int refusals = 0;
    int lines = 0;
    std::size_t most_held = 0;
    const auto t = Clock::now();
    while (producer.written() < kEndless && ms_since(t) < 30000) {
        const LineInput::Status s = input.read(&line, 20);
        refusals += s == LineInput::Status::TooLong ? 1 : 0;
        lines += s == LineInput::Status::Line ? 1 : 0;
        most_held = std::max(most_held, input.held().bytes);
    }
    r.check("pipe: a line that never ends does not stop the reader: its producer is read to the "
            "last byte, and the line is refused once",
            producer.written() == kEndless && refusals == 1 && lines == 0,
            "written " + std::to_string(producer.written()) + ", refusals " +
                std::to_string(refusals) + ", lines " + std::to_string(lines));
    r.check("pipe: ...and none of it is held", most_held <= kHeldInputBytes &&
                                                   input.held().peak <= kHeldInputBytes,
            "most held " + std::to_string(most_held) + ", " + held_detail(input.held()));
    producer.release();
    LineInput::Status s = input.read(&line, 5000);
    const bool status_read = s == LineInput::Status::Line && line == "status";
    s = input.read(&line, 5000);
    r.check("pipe: when it finally ends, the next line is read, then the end",
            status_read && s == LineInput::Status::Closed, std::string(status_name(s)));
}

void stream_deadline(Report& r) {
    // A LINE THAT ARRIVES IN PIECES, THE LAST AFTER THE READ BEGAN. A read with a deadline waits
    // for a finished line until that deadline, on every platform — it does not answer Idle as soon
    // as one read finished nothing while more is on its way. (The POSIX reader once did, and the
    // Windows thread never did: the same call meant two different things.)
    Pipe pipe;
    if (!pipe.open()) {
        r.check("a pipe could be made", false);
        return;
    }
    Producer producer(pipe, "status of the first ", "half\n", /*pause=*/true);
    StdinFrom from(pipe.rd);
    LineInput input;
    std::thread releaser([&producer] {
        sleep_ms(300);
        producer.release();
    });
    std::string line;
    const auto t = Clock::now();
    const LineInput::Status s = input.read(&line, 5000);
    const long long waited = ms_since(t);
    releaser.join();
    r.check("pipe: a read with a deadline waits for the rest of a line that arrives after it began",
            s == LineInput::Status::Line && line == "status of the first half" && waited >= 200,
            std::string(status_name(s)) + " '" + line + "' in " + std::to_string(waited) + " ms");
}

void stream_stop_while_waiting(Report& r) {
    constexpr std::size_t kLines = 20000;
    const std::string data = numbered_burst(kLines);
    Pipe pipe;
    if (!pipe.open()) {
        r.check("a pipe could be made", false);
        return;
    }
    Producer producer(pipe, data);
    bool held_back = false;
    long long stop_ms = -1;
    LineInput::Held at_stop{};
    {
        StdinFrom from(pipe.rd);
        auto input = std::make_unique<LineInput>();
        std::string line;
        (void)input->read(&line, 5000);
        held_back = producer.held_back(300, 5000); // the producer is waiting for room
        at_stop = input->held();
        const auto t = Clock::now();
        input.reset(); // SHUTDOWN
        stop_ms = ms_since(t);
    }
    pipe.close_read(); // the stream goes away with the host
    const bool released = producer.finished(5000);
    r.check("pipe: stopping a reader while its producer waits for room is prompt",
            held_back && stop_ms >= 0 && stop_ms < 1500,
            std::to_string(stop_ms) + " ms, producer at " + std::to_string(producer.written()) +
                " of " + std::to_string(data.size()) + ", " + held_detail(at_stop));
#ifdef _WIN32
    r.check("pipe: ...and it was stopped while its reader thread waited for room",
            at_stop.bytes + 4096 > kHeldInputBytes, held_detail(at_stop));
#endif
    r.check("pipe: ...and the producer is released when the stream goes, not left waiting",
            released && producer.broken());
}

void stream_stop_while_reading(Report& r) {
    Pipe pipe;
    if (!pipe.open()) {
        r.check("a pipe could be made", false);
        return;
    }
    Producer producer(pipe, std::string(), "after\n", /*pause=*/true); // nothing, yet
    StdinFrom from(pipe.rd);
    long long stop_ms = -1;
    LineInput::Status first = LineInput::Status::Line;
    {
        auto input = std::make_unique<LineInput>();
        std::string line;
        first = input->read(&line, 200); // a read is now pending on an empty pipe
        const auto t = Clock::now();
        input.reset();
        stop_ms = ms_since(t);
    }
    producer.release();
    LineInput next;
    std::string line;
    LineInput::Status s = next.read(&line, 5000);
    const bool after = s == LineInput::Status::Line && line == "after";
    s = next.read(&line, 5000);
    r.check("pipe: stopping a reader with a read pending on an empty pipe is prompt, and the next "
            "reader reads the pipe to its end",
            first == LineInput::Status::Idle && stop_ms >= 0 && stop_ms < 1500 && after &&
                s == LineInput::Status::Closed,
            std::to_string(stop_ms) + " ms; then " + (after ? "'after'" : "not 'after'") + ", " +
                status_name(s));
}

void stream_empty(Report& r) {
    Pipe pipe;
    if (!pipe.open()) {
        r.check("a pipe could be made", false);
        return;
    }
    Producer producer(pipe, std::string());
    StdinFrom from(pipe.rd);
    LineInput input;
    std::string line;
    const LineInput::Status a = input.read(&line, 5000);
    const LineInput::Status b = input.read(&line, 0);
    r.check("pipe: an input with nothing in it is Closed, and stays Closed",
            a == LineInput::Status::Closed && b == LineInput::Status::Closed,
            std::string(status_name(a)) + ", then " + status_name(b));
}

void stream_replacement(Report& r) {
    constexpr std::size_t kLines = 2000;
    const std::string data = numbered_burst(kLines);
    Pipe pipe;
    if (!pipe.open()) {
        r.check("a pipe could be made", false);
        return;
    }
    Producer producer(pipe, data);
    StdinFrom from(pipe.rd);
    std::string line;
    bool first_three = true;
    {
        LineInput a;
        for (std::size_t i = 0; i < 3; ++i) {
            first_three = first_three && a.read(&line, 5000) == LineInput::Status::Line &&
                          line == numbered(i, 127);
        }
        sleep_ms(200); // time to read ahead, where the arrangement does
    }
    LineInput b;
    std::vector<std::string> lines;
    const auto t = Clock::now();
    for (;;) {
        const LineInput::Status s = b.read(&line, 1000);
        if (s == LineInput::Status::Closed || ms_since(t) > 60000) {
            break;
        }
        if (s == LineInput::Status::Line) {
            lines.push_back(line);
        }
    }
    // The first line may be the rest of one the stopped reader had begun; everything after it is
    // whole, and in order.
    std::size_t j = (!lines.empty() && index_of(lines.front()) < 0) ? 1 : 0;
    const long long k = j < lines.size() ? index_of(lines[j]) : -1;
    bool in_order = k >= 3;
    for (std::size_t i = j; in_order && i < lines.size(); ++i) {
        in_order = lines[i] == numbered(static_cast<std::size_t>(k) + (i - j), 127);
    }
    const std::size_t lost = k >= 3 ? static_cast<std::size_t>(k - 3) * 128 : 0;
    r.check("pipe: a replaced reader loses no more than the limit, and its successor reads whole "
            "lines in order to the end",
            first_three && in_order && lost <= kHeldInputBytes + 128 &&
                lines.size() - j == kLines - static_cast<std::size_t>(k),
            "successor resumed at line " + std::to_string(k) + (j == 1 ? " after a fragment" : "") +
                ", " + std::to_string(lost) + " bytes went with the first reader");
}

int run_streams(Report& r, const std::string& work) {
#ifndef _WIN32
    // A producer writing into a pipe whose reader has gone is told so by EPIPE, and must not be
    // killed by SIGPIPE for asking.
    (void)signal(SIGPIPE, SIG_IGN);
#endif
    stream_burst_held_back(r);
    stream_file_held_back(r, work);
    stream_limits(r);
    stream_endless_line(r);
    stream_deadline(r);
    stream_stop_while_waiting(r);
    stream_stop_while_reading(r);
    stream_empty(r);
    stream_replacement(r);
    return r.failed == 0 ? 0 : 1;
}

// ---- host mode: a real loom-host, typed at -------------------------------------------

/// Everything the host has written, as it arrives.
struct Output {
    std::mutex m;
    std::string text;

    void append(const char* data, std::size_t n) {
        std::lock_guard<std::mutex> lock(m);
        text.append(data, n);
    }
    std::string snapshot() {
        std::lock_guard<std::mutex> lock(m);
        return text;
    }
    /// The position just past `marker`'s first occurrence at or after `from`, or npos once
    /// `timeout_ms` has passed without it.
    std::size_t wait_for(const std::string& marker, std::size_t from, int timeout_ms) {
        const auto t = Clock::now();
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(m);
                const std::size_t at = text.find(marker, from);
                if (at != std::string::npos) {
                    return at + marker.size();
                }
            }
            if (ms_since(t) >= timeout_ms) {
                return std::string::npos;
            }
            sleep_ms(10);
        }
    }
};

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/// A running host behind the terminal.
struct Host {
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
    HANDLE out_read = nullptr;
#else
    pid_t pid = -1;
#endif
    std::thread reader;
    Output output;
    bool running = false;

    bool start(const Terminal& term, const std::string& exe, const std::vector<std::string>& args,
               const std::string& work) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE out_write = nullptr;
        if (CreatePipe(&out_read, &out_write, &inherit, 0) == 0) {
            return false;
        }
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
        std::string cmd = "\"" + exe + "\"";
        for (const std::string& a : args) {
            cmd += " \"" + a + "\"";
        }
        STARTUPINFOA si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = term.conin; // the console a person types into: a real console handle
        si.hStdOutput = out_write;
        si.hStdError = out_write;
        const BOOL made = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                         work.c_str(), &si, &pi);
        CloseHandle(out_write);
        if (made == 0) {
            return false;
        }
        running = true;
        reader = std::thread([this] {
            char buf[4096];
            for (;;) {
                DWORD n = 0;
                if (ReadFile(out_read, buf, sizeof buf, &n, nullptr) == 0 || n == 0) {
                    return;
                }
                output.append(buf, n);
            }
        });
        return true;
#else
        pid = fork();
        if (pid < 0) {
            return false;
        }
        if (pid == 0) {
            (void)setsid();
            const int slave = ::open(term.slave_name.c_str(), O_RDWR); // the controlling terminal
            if (slave < 0) {
                _exit(126);
            }
            (void)dup2(slave, STDIN_FILENO);
            (void)dup2(slave, STDOUT_FILENO);
            (void)dup2(slave, STDERR_FILENO);
            if (slave > STDERR_FILENO) {
                ::close(slave);
            }
            ::close(term.master);
            if (chdir(work.c_str()) != 0) {
                _exit(126);
            }
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(exe.c_str()));
            for (const std::string& a : args) {
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);
            execv(exe.c_str(), argv.data());
            _exit(127);
        }
        running = true;
        const int master = term.master;
        reader = std::thread([this, master] {
            char buf[4096];
            for (;;) {
                const ssize_t n = ::read(master, buf, sizeof buf);
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n <= 0) {
                    return; // EIO once the host has exited and the last slave is closed
                }
                output.append(buf, static_cast<std::size_t>(n));
            }
        });
        return true;
#endif
    }

    /// Wait up to `timeout_ms` for the host to exit; its exit code, or -1 if it did not.
    int wait_exit(int timeout_ms) {
#ifdef _WIN32
        if (WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) {
            return -1;
        }
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        running = false;
        return static_cast<int>(code);
#else
        const auto t = Clock::now();
        for (;;) {
            int status = 0;
            const pid_t done = waitpid(pid, &status, WNOHANG);
            if (done == pid) {
                running = false;
                return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            if (ms_since(t) >= timeout_ms) {
                return -1;
            }
            sleep_ms(10);
        }
#endif
    }

    ~Host() {
#ifdef _WIN32
        if (running) {
            TerminateProcess(pi.hProcess, 99);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        if (reader.joinable()) {
            reader.join(); // the pipe breaks when the host is gone
        }
        if (pi.hProcess != nullptr) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        if (out_read != nullptr) {
            CloseHandle(out_read);
        }
#else
        if (running && pid > 0) {
            kill(pid, SIGKILL);
            int status = 0;
            (void)waitpid(pid, &status, 0);
        }
        if (reader.joinable()) {
            reader.detach(); // blocked on the master until it is closed with the terminal
        }
#endif
    }
};

/// The number after `ticks=` at or after `from`, or -1.
long long ticks_after(const std::string& text, std::size_t from) {
    const std::size_t at = text.find("ticks=", from);
    if (at == std::string::npos) {
        return -1;
    }
    std::size_t i = at + 6;
    long long v = 0;
    bool any = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        v = v * 10 + (text[i] - '0');
        ++i;
        any = true;
    }
    return any ? v : -1;
}

int run_host(Report& r, const std::string& host_exe, const std::string& probe_lib,
             const std::string& work) {
    Terminal term;
    if (!term.open()) {
        r.note("could not build a terminal to type into");
        return 3;
    }
    const std::string plan = work + "/plan.json";
    const std::string decisions = work + "/decisions.json";
    std::string lib = probe_lib;
    for (char& c : lib) {
        if (c == '\\') {
            c = '/'; // JSON-safe on Windows, and the host accepts either separator
        }
    }
    write_file(plan, "{\"boot\":[{\"name\":\"probe\",\"path\":\"" + lib +
                         "\",\"role\":\"probe\",\"enabled\":true,\"on_failure\":\"stop\"}]}\n");
    write_file(decisions,
               "{\"rules\":[{\"artifact\":\"probe\",\"content_id\":\"\",\"may_run\":true,"
               "\"trust_rebuilds\":true,\"send\":[\"Startup v1 -> role probe\","
               "\"Spin v1 -> role probe\",\"Tock v1 -> role probe\"],\"observe\":[],"
               "\"note\":\"\"}]}\n");

    Host host;
    if (!host.start(term, host_exe, {"--boot", plan, "--authority", decisions}, work)) {
        r.note("could not start " + host_exe);
        return 3;
    }
    Output& out = host.output;
    constexpr int kWait = 15000;
    std::size_t at = 0;
    // Wait for `marker` after everything already matched. A command the host never executes
    // means it has stopped reading, and every later check would only repeat that, so the run
    // ends there and says which checks it did not reach.
    const auto expect = [&](const std::string& marker, const std::string& what) {
        const std::size_t next = out.wait_for(marker, at, kWait);
        r.check(what, next != std::string::npos);
        if (next != std::string::npos) {
            at = next;
        }
        return next != std::string::npos;
    };
    const auto give_up = [&](const char* where) {
        r.check(std::string("the host kept reading commands (it stopped at: ") + where + ")",
                false);
        r.note("host output:\n" + out.snapshot());
        return 1;
    };

    if (!expect("loom> ", "the host boots the probe and reaches its prompt")) {
        return give_up("boot");
    }

    // THE FINDING, AS ONE BURST: a command whose answer comes late, then HALF of the next
    // command. The first line is finished and the second is not, and non-character input
    // arrives while it waits.
    term.type("send 5 Countdown 1 turns=64\nauthority sh");
    if (!expect("no answer yet", "the delayed conversation is reported pending, not invented")) {
        return give_up("the countdown command");
    }
    term.noise(24);
    (void)expect("counted down",
                 "its answer is delivered and reported while the next command is half typed");
    term.type("ow probe\n");
    if (!expect("LIVE, weave 5", "...and the half-typed command then completes and runs")) {
        return give_up("the completed half-typed command");
    }

    // A FINISHED COMMAND BEHIND NON-CHARACTER INPUT — more of it than the old reader peeked.
    term.noise_then_type(48, "authority show probe\n");
    if (!expect("LIVE, weave 5", std::string("a finished command is executed ") + kBehindNoise)) {
        return give_up("a command behind non-character records");
    }

    // COOPERATIVE WORK THROUGH A PAUSE MID-COMMAND. Spin keeps the bus busy forever; the person
    // types half an inspection, stops for half a second (with non-character input), finishes.
    const std::size_t before_ticks = at;
    term.type("send 5 Spin 1\nsend 5 Insp");
    sleep_ms(500);
    term.noise(24);
    term.type("ect 1\n");
    if (!expect("ticks=", "inspection is reachable during sustained traffic, across a pause")) {
        return give_up("the inspection");
    }
    const long long ticks = ticks_after(out.snapshot(), before_ticks);
    // A reader that blocked on the half-typed line would have stopped Spin within one turn of
    // the command that started it: at most 17 ticks (16 settle turns and one loop turn).
    r.check("cooperative work progressed while the inspection was half typed", ticks >= 200,
            "ticks=" + std::to_string(ticks));

    // A TYPED LINE LONGER THAN ANY COMMAND, refused where it stands, and the next command runs.
    // `quit` and a line of spaces is what a host that ran a shortened line — its own shortening,
    // or a terminal's — would execute as `quit`: the host would exit and nothing below would run.
    term.type("quit" + std::string(4996, ' ') + "\n");
    if (!expect("refused: input line",
                "a typed line longer than any command is refused, not run")) {
        return give_up("the over-long line");
    }
    term.type("authority show probe\n");
    if (!expect("LIVE, weave 5", "...and the next command runs")) {
        return give_up("the command after the over-long line");
    }
    // A command of exactly the limit (trailing spaces are part of it) is a command.
    term.type("authority show probe" + std::string(kMaxCommandBytes - 20, ' ') + "\n");
    if (!expect("LIVE, weave 5", "a typed command of exactly the limit runs")) {
        return give_up("the command at the limit");
    }

    // AUTHORITY ADMINISTRATION, STOP AND QUIT, each with a pause inside the line.
    term.type("authority allow probe Greet v1 -");
    sleep_ms(300);
    term.noise(12);
    term.type("> any target\n");
    if (!expect("remembered.", "authority administration is reachable mid-traffic, across a pause")) {
        return give_up("authority allow");
    }
    term.type("authority show pro");
    sleep_ms(300);
    term.type("be\n");
    // `may say:` is `authority show`'s own line; the `allow` answer above names the rule too, so
    // matching the rule text alone would not prove the inspection ran.
    if (!expect("may say:  Greet v1 -> any target",
                "...and inspection shows the permission just granted")) {
        return give_up("authority show");
    }
    term.type("stop pro");
    sleep_ms(300);
    term.noise(12);
    term.type("be\n");
    if (!expect("'probe' stopped", "stop is reachable, across a pause")) {
        return give_up("stop");
    }
    term.type("qu");
    sleep_ms(300);
    term.type("it\n");
    const int code = host.wait_exit(kWait);
    r.check("quit is reachable across a pause, and the host exits 0", code == 0,
            "exit " + std::to_string(code));

    if (r.failed != 0) {
        r.note("host output:\n" + out.snapshot());
    }
    return r.failed == 0 ? 0 : 1;
}

int usage() {
    std::fprintf(stderr, "usage: zen-host-terminal-witness reader\n"
                         "       zen-host-terminal-witness streams <work-dir>\n"
                         "       zen-host-terminal-witness host <loom-host> <probe-lib> <work-dir>\n");
    return 2;
}

int run_mode(const std::vector<std::string>& args, Report& r) {
    if (args.empty()) {
        return usage();
    }
    if (args[0] == "reader" && args.size() == 1) {
        return run_reader(r);
    }
    if (args[0] == "streams" && args.size() == 2) {
        return run_streams(r, args[1]);
    }
    if (args[0] == "host" && args.size() == 4) {
        return run_host(r, args[1], args[2], args[3]);
    }
    return usage();
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

#ifdef _WIN32
    // THE CHILD does the terminal work in a console of its own and writes its report to a file.
    if (args.size() >= 2 && args[0] == "--child") {
        const std::string report_path = args[1];
        args.erase(args.begin(), args.begin() + 2);
        Report r;
        const int code = run_mode(args, r);
        r.text << (code == 0 ? "PASSED" : "FAILED") << " -- " << (r.checks - r.failed) << " of "
               << r.checks << " checks (exit " << code << ")\n";
        write_file(report_path, r.text.str());
        return code;
    }
    // THE PARENT starts it detached and hidden, so no console window is ever shown and this
    // process keeps the console and pipes ctest gave it.
    if (args.empty()) {
        return usage();
    }
    char self[MAX_PATH];
    if (GetModuleFileNameA(nullptr, self, MAX_PATH) == 0) {
        return 3;
    }
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    const std::string report_path = std::string(temp) + "zen-host-terminal-witness-" +
                                    std::to_string(GetCurrentProcessId()) + ".txt";
    std::remove(report_path.c_str());
    std::string cmd = "\"" + std::string(self) + "\" --child \"" + report_path + "\"";
    for (const std::string& a : args) {
        cmd += " \"" + a + "\"";
    }
    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                       nullptr, &si, &pi) == 0) {
        std::printf("could not start the witness child (Windows error %lu)\n", GetLastError());
        return 3;
    }
    const DWORD waited = WaitForSingleObject(pi.hProcess, 200000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 98);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::ifstream in(report_path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    in.close();
    std::remove(report_path.c_str());
    std::fputs(text.str().c_str(), stdout);
    if (waited != WAIT_OBJECT_0) {
        std::printf("FAILED -- the witness did not finish within 200 s and was stopped\n");
        return 1;
    }
    return static_cast<int>(code);
#else
    Report r;
    const int code = run_mode(args, r);
    r.text << (code == 0 ? "PASSED" : "FAILED") << " -- " << (r.checks - r.failed) << " of "
           << r.checks << " checks (exit " << code << ")\n";
    std::fputs(r.text.str().c_str(), stdout);
    return code;
#endif
}
