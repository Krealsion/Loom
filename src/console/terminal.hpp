#ifndef ZEN_CONSOLE_TERMINAL_HPP
#define ZEN_CONSOLE_TERMINAL_HPP

// The terminal-backend seam — the single place the shared TUI's platform (and, later, transport)
// differences live. The TUI talks ONLY to this interface; make_terminal() is the one symbol
// selected per platform: a POSIX termios backend today, a Win32 Console backend next. It belongs
// to the zen-console-tui executable, NOT to zen-console — the engine library stays portable and
// terminal-free.
//
// Seam appreciation: this boundary is the hook the next two frontends plug into. The WSL remote
// console becomes "another TerminalBackend" (a socket transport, not a parallel codebase); the GUI
// becomes another renderer of the same widget tree, inheriting the same discipline (platform behind
// a seam, engine kept pure). The remote phase brought OUTPUT behind the seam too:
//   - write()/flush() now carry the rendered frame: the POSIX/Windows backends write stdout; a
//     socket backend that ships the frame on the wire is the clean extension this enables (decision
//     #3's "remote is just a backend") — HOOKED, not built (no consumer yet: the remote console
//     renders client-side off the operator-protocol, so it draws to its own real terminal).
//   - The TUI's synchronous read_byte loop is, for the remote client, generalized into a
//     single-threaded multiplexer over {input source, socket} with the same per-read deadline
//     read_byte_timeout already models. The bus stays single-threaded FIFO; the multiplexer is the
//     CLIENT's readiness-to-receive-from-many-sources, never bus concurrency.

#include <memory>
#include <string_view>

namespace loom {

/// A terminal I/O backend. The concrete backend enters raw mode on construction (gated on
/// is_interactive) and restores cooked mode in its destructor (RAII).
class TerminalBackend {
public:
    virtual ~TerminalBackend() = default;
    /// isatty-equivalent: a piped/headless run is not interactive (no raw mode is engaged).
    virtual bool is_interactive() const = 0;
    /// Visible window size into rows/cols; returns false if unavailable (caller falls back 80x24).
    virtual bool size(int& rows, int& cols) = 0;
    /// Read one byte (0..255), blocking; -1 on EOF/error.
    virtual int read_byte() = 0;
    /// Read one byte with an upper time bound; -1 on timeout OR EOF/error. Used to disambiguate a
    /// bare ESC (Cancel) from an escape sequence (ESC [ A ...) without blocking. Contract: callers
    /// pass a positive ms; ms<=0 is a non-blocking poll (return at once, -1 if nothing is ready).
    virtual int read_byte_timeout(int ms) = 0;

    /// Write the rendered frame bytes to the output. Unlike the reads, this is NOT gated on
    /// is_interactive (a piped run still emits output) — it mirrors the old `std::cout << frame`.
    /// The POSIX/Windows backends write stdout directly; a socket backend would write the wire.
    virtual void write(std::string_view bytes) = 0;
    /// Flush any buffered output. The direct-to-stdout backends are unbuffered, so this is a no-op;
    /// a buffered or socket backend overrides it. (Paired with write() to mirror `<< std::flush`.)
    virtual void flush() = 0;
};

/// Platform factory: constructs the backend, entering raw mode (gated on is_interactive). The only
/// symbol selected per platform; the shared TUI contains no platform headers.
std::unique_ptr<TerminalBackend> make_terminal();

} // namespace loom

#endif // ZEN_CONSOLE_TERMINAL_HPP
