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
// TWO MODES, TWO CTest ENTRIES:
//
//   reader   `loom::host::LineInput` itself (portable): a half-typed line is Idle at once and
//            at its deadline; it completes when finished; lines behind non-character records
//            are reached; several lines typed at once are each read; stopping a reader with a
//            half-typed line pending is prompt and leaves the terminal usable for the next
//            reader; end of input is Closed; the terminal's mode is exactly what it was.
//
//   host     a real `loom-host` with the probe weave booted (kernel gate): a delayed answer is
//            delivered AND REPORTED while a command is half typed; cooperative work
//            progresses through a pause mid-command; inspection, authority administration,
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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
    case LineInput::Status::Closed:
        return "Closed";
    }
    return "?";
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
            if (WriteConsoleInputW(conin, rs.data() + done, static_cast<DWORD>(rs.size() - done),
                                   &n) == 0 ||
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

#ifdef _WIN32
    r.check("the console's mode is exactly what it was", term.mode() == mode_before,
            mode_before + " -> " + term.mode());
#else
    r.check("the terminal's mode is exactly what it was", stdin_tty.mode_unchanged());
#endif
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
