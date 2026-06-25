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
// a seam, engine kept pure). Two things to know before that lands cleanly (flagged, not yet built):
//   - OUTPUT is not yet behind this seam: the shared draw() still writes the rendered frame to
//     stdout (fine for POSIX + Windows VT). A remote backend must route output through here too —
//     the natural extension is a write()/flush() pair, deferred to the remote phase.
//   - The TUI owns the I/O LOOP synchronously (read_byte -> dispatch -> draw). A blocking read with
//     a timeout-read for escape disambiguation maps cleanly onto a socket (recv / poll), but a
//     fully async transport would want to invert loop ownership. Noted for the remote phase.

#include <memory>

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
};

/// Platform factory: constructs the backend, entering raw mode (gated on is_interactive). The only
/// symbol selected per platform; the shared TUI contains no platform headers.
std::unique_ptr<TerminalBackend> make_terminal();

} // namespace loom

#endif // ZEN_CONSOLE_TERMINAL_HPP
