// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_CONSOLE_TERMINAL_HPP
#define ZEN_CONSOLE_TERMINAL_HPP

// The terminal-backend seam: the one place the shared TUI's platform differences live. The TUI
// talks only to this interface, and make_terminal() is the one symbol selected per platform (a
// POSIX termios backend, a Win32 Console backend). It belongs to the TUI executables, not to
// zen-console, which stays portable and terminal-free. Output is behind the seam too
// (write/flush), so a backend that ships the frame over a socket would plug in here; none does:
// the remote console renders client-side to its own terminal.

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

    /// Write the rendered frame bytes to the output. Unlike the reads, this is not gated on
    /// is_interactive: a piped run still emits output. The POSIX and Windows backends write
    /// stdout directly; a socket backend would write the wire.
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
