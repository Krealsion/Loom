// The POSIX terminal backend — the existing console_tui.cpp terminal control, MOVED behind the
// TerminalBackend seam, behavior-identical. termios raw mode (the isatty gate + the atexit restore),
// ioctl(TIOCGWINSZ) size, the blocking byte read, and the VTIME-grace timed read (the old inline
// ESC-continuation, generalized to a millisecond argument). This is the safety property of the
// seam extraction: the Linux path is preserved by construction, not rewritten. Compiled only on
// non-Windows.

#include "terminal.hpp"

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <memory>

namespace loom {
namespace {

class PosixTerminal final : public TerminalBackend {
public:
    PosixTerminal() {
        interactive_ = isatty(STDIN_FILENO) != 0;
        if (!interactive_) {
            return; // piped/headless: touch no terminal state
        }
        tcgetattr(STDIN_FILENO, &saved_);
        s_saved = saved_;
        s_have_saved = true;
        std::atexit(&PosixTerminal::restore_atexit); // restore on return-from-main / std::exit too
        struct termios raw = saved_;
        raw.c_lflag = raw.c_lflag & ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN] = static_cast<cc_t>(1);
        raw.c_cc[VTIME] = static_cast<cc_t>(0);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
    ~PosixTerminal() override {
        if (interactive_) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
        }
    }
    PosixTerminal(const PosixTerminal&) = delete;
    PosixTerminal& operator=(const PosixTerminal&) = delete;

    bool is_interactive() const override { return interactive_; }

    bool size(int& rows, int& cols) override {
        struct winsize ws {};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
            rows = static_cast<int>(ws.ws_row);
            cols = static_cast<int>(ws.ws_col);
            return true;
        }
        return false;
    }

    int read_byte() override {
        unsigned char c = 0;
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        return n == 1 ? static_cast<int>(c) : -1;
    }

    int read_byte_timeout(int ms) override {
        if (!interactive_) {
            return read_byte(); // a pipe has no raw VTIME; EOF returns -1, so no indefinite block
        }
        // The old ESC-continuation read: temporarily VMIN=0/VTIME=<grace>, read one byte, restore.
        // VTIME is in deciseconds; round the ms request up and clamp to [1, 255].
        struct termios cur {};
        tcgetattr(STDIN_FILENO, &cur);
        struct termios timed = cur;
        timed.c_cc[VMIN] = static_cast<cc_t>(0);
        // ms<=0 => VTIME=0 => a non-blocking poll (the seam contract; matches the Windows backend).
        int ds = (ms <= 0) ? 0 : (ms + 99) / 100;
        if (ds > 255) {
            ds = 255;
        }
        timed.c_cc[VTIME] = static_cast<cc_t>(ds);
        tcsetattr(STDIN_FILENO, TCSANOW, &timed);
        unsigned char c = 0;
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        tcsetattr(STDIN_FILENO, TCSANOW, &cur); // restore the blocking VMIN=1/VTIME=0 mode
        return n == 1 ? static_cast<int>(c) : -1;
    }

    void write(std::string_view bytes) override {
        // Direct, unbuffered write to stdout — the raw equivalent of `std::cout << frame`. Loop over
        // partial writes; retry EINTR; a real I/O error just drops the rest (best-effort, as the old
        // ostream path was). STDOUT in raw mode is blocking, so EAGAIN does not arise here.
        std::size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t w = ::write(STDOUT_FILENO, bytes.data() + off, bytes.size() - off);
            if (w > 0) {
                off += static_cast<std::size_t>(w);
            } else if (w < 0 && errno == EINTR) {
                continue;
            } else {
                break; // error: drop the rest, like a failed ostream
            }
        }
    }

    void flush() override {} // ::write goes straight to the kernel; nothing is buffered here

private:
    static void restore_atexit() {
        if (s_have_saved) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_saved);
        }
    }
    bool interactive_ = false;
    struct termios saved_ {};
    static struct termios s_saved;
    static bool s_have_saved;
};

struct termios PosixTerminal::s_saved {};
bool PosixTerminal::s_have_saved = false;

} // namespace

std::unique_ptr<TerminalBackend> make_terminal() { return std::make_unique<PosixTerminal>(); }

} // namespace loom
