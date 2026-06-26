// The Windows terminal backend — the Win32 Console API behind the same TerminalBackend seam, so
// the SAME ANSI-emitting renderer and the SAME escape-sequence key parsing run unchanged on
// Windows. No new third-party dependency (Win32 only). Compiled only on Windows.
//
// BUILD/VERIFY DIVISION: this backend is written by-the-book against the Console API but is NOT
// compiled or run on the Linux CI box — it is Josh-verified in CLion on Windows. The Linux path is
// proven; this is best-effort-correct. See DESIGN.md (the build-verify division) and the report's
// Windows checklist.

#include "terminal.hpp"

#ifdef _WIN32

#include <windows.h>

#include <cstddef>
#include <memory>
#include <string_view>

namespace loom {
namespace {

class WindowsTerminal final : public TerminalBackend {
public:
    WindowsTerminal() {
        h_in_ = GetStdHandle(STD_INPUT_HANDLE);
        h_out_ = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD in_mode = 0;
        DWORD out_mode = 0;
        // GetStdHandle can return INVALID_HANDLE_VALUE (failure) OR NULL (no associated std handle);
        // reject both. GetConsoleMode then fails on a pipe/redirect => not interactive.
        const bool got_in = h_in_ != INVALID_HANDLE_VALUE && h_in_ != nullptr &&
                            GetConsoleMode(h_in_, &in_mode) != 0;
        const bool got_out = h_out_ != INVALID_HANDLE_VALUE && h_out_ != nullptr &&
                             GetConsoleMode(h_out_, &out_mode) != 0;
        interactive_ = got_in && got_out; // a pipe/redirect => GetConsoleMode fails => not interactive
        if (!interactive_) {
            return; // piped/headless: touch no console state (the isatty-gate equivalent)
        }
        saved_in_ = in_mode;
        saved_out_ = out_mode;

        // Input: drop line/echo/processed input, enable VT input so keys arrive as ANSI/VT escape
        // sequences — exactly what map_key's existing ESC parsing expects.
        DWORD raw_in = in_mode;
        raw_in &= ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        raw_in |= static_cast<DWORD>(ENABLE_VIRTUAL_TERMINAL_INPUT);
        // Output: enable VT processing so the renderer's ANSI escapes are interpreted.
        const DWORD raw_out = out_mode | static_cast<DWORD>(ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                                            ENABLE_PROCESSED_OUTPUT);

        // Older Windows (pre-VT) rejects these flags: fail gracefully — restore and report
        // not-interactive rather than pretending VT is on.
        if (SetConsoleMode(h_in_, raw_in) == 0 || SetConsoleMode(h_out_, raw_out) == 0) {
            SetConsoleMode(h_in_, saved_in_);
            SetConsoleMode(h_out_, saved_out_);
            interactive_ = false;
        }
    }
    ~WindowsTerminal() override {
        if (interactive_) {
            SetConsoleMode(h_in_, saved_in_);
            SetConsoleMode(h_out_, saved_out_);
        }
    }
    WindowsTerminal(const WindowsTerminal&) = delete;
    WindowsTerminal& operator=(const WindowsTerminal&) = delete;

    bool is_interactive() const override { return interactive_; }

    bool size(int& rows, int& cols) override {
        CONSOLE_SCREEN_BUFFER_INFO csbi {};
        if (GetConsoleScreenBufferInfo(h_out_, &csbi) != 0) {
            // The VISIBLE window (srWindow), not the larger scrollback buffer.
            const int r = static_cast<int>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
            const int c = static_cast<int>(csbi.srWindow.Right - csbi.srWindow.Left + 1);
            if (r > 0 && c > 0) {
                rows = r;
                cols = c;
                return true;
            }
        }
        return false;
    }

    int read_byte() override {
        char c = 0;
        DWORD n = 0;
        if (ReadFile(h_in_, &c, 1, &n, nullptr) != 0 && n == 1) {
            return static_cast<int>(static_cast<unsigned char>(c));
        }
        return -1; // 0 bytes (EOF) or error
    }

    int read_byte_timeout(int ms) override {
        // A console input handle signals when its queue is non-empty for ANY record type —
        // including key-up / focus / buffer-resize records that translate to ZERO VT bytes. So we
        // must NOT WaitForSingleObject then blindly ReadFile (ReadFile would filter the non-key
        // record and block past the deadline). Instead: wait against a deadline, PEEK, and drain any
        // non-key-down record before re-waiting — so the bound is honored and a key-down's VT bytes
        // are read without blocking. Contract: ms<=0 is a non-blocking poll (matches the POSIX
        // backend); -1 on timeout/EOF/error.
        const DWORD budget = ms <= 0 ? 0u : static_cast<DWORD>(ms);
        const DWORD start = GetTickCount();
        for (;;) {
            const DWORD elapsed = GetTickCount() - start; // wrap-safe via unsigned subtraction
            const DWORD remaining = elapsed >= budget ? 0u : budget - elapsed;
            if (WaitForSingleObject(h_in_, remaining) != WAIT_OBJECT_0) {
                return -1; // WAIT_TIMEOUT or error
            }
            INPUT_RECORD rec {};
            DWORD got = 0;
            if (PeekConsoleInputW(h_in_, &rec, 1, &got) == 0 || got == 0) {
                continue; // nothing peekable; re-wait against the same deadline
            }
            const bool key_down = rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown != 0;
            if (!key_down) {
                ReadConsoleInputW(h_in_, &rec, 1, &got); // discard the non-key-down record
                continue;
            }
            return read_byte(); // a key-down is queued: ReadFile yields its VT byte(s) without blocking
        }
    }

    void write(std::string_view bytes) override {
        // Direct write to the console output handle — the raw equivalent of `std::cout << frame`,
        // with VT processing enabled so the renderer's ANSI escapes are interpreted. Loop over
        // partial writes; a real error just drops the rest (best-effort, as the old ostream path was).
        std::size_t off = 0;
        while (off < bytes.size()) {
            const DWORD chunk =
                static_cast<DWORD>(bytes.size() - off > 0xFFFFu ? 0xFFFFu : bytes.size() - off);
            DWORD wrote = 0;
            if (WriteFile(h_out_, bytes.data() + off, chunk, &wrote, nullptr) == 0 || wrote == 0) {
                break; // error or zero progress: drop the rest, like a failed ostream
            }
            off += wrote;
        }
    }

    void flush() override {} // WriteFile goes straight to the console; nothing is buffered here

private:
    HANDLE h_in_ = nullptr;
    HANDLE h_out_ = nullptr;
    DWORD saved_in_ = 0;
    DWORD saved_out_ = 0;
    bool interactive_ = false;
};

} // namespace

std::unique_ptr<TerminalBackend> make_terminal() { return std::make_unique<WindowsTerminal>(); }

} // namespace loom

#endif // _WIN32
