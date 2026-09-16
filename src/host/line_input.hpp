// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_LINE_INPUT_HPP
#define ZEN_HOST_LINE_INPUT_HPP

// READING A PERSON'S NEXT COMMAND WITHOUT STOPPING THE WORLD.
//
// THE DEFECT THIS EXISTS FOR. The supplied host's loop was `std::getline` — which blocks
// until a line arrives — around `drain_until_idle()`, which is unbounded BY CONTRACT
// (MSG-09): work a handler queues during the drain belongs to the drain, so a weave whose
// short handler re-arms itself keeps the queue non-empty forever and the drain never
// returns. The host then never reached `getline` again. A probe with one approved
// self-addressed message made `stop probe` and `quit` — already typed, already in the
// pipe — unreadable. Every handler returned; nothing was stuck in native code. The host
// had simply promised the bus everything and the person nothing.
//
// SO THE LOOP OWES BOTH, and the two calls it needs are `Switchboard::pump_pending()` —
// which bounds itself at the backlog present when it started, so a self-re-arming
// producer cannot extend one turn — and this: "is there a line for me, within `ms`?".
// Together they are the host loop `pump_pending`'s own comment describes.
//
// WHAT THIS IS NOT. It is not a terminal, a line editor or a raw-mode backend. It reads
// whole lines from stdin exactly as `std::getline` did — no echo control, no key
// vocabulary, no history, no completion. `loom::TerminalBackend` (src/console/terminal.hpp)
// is the raw-mode seam and belongs to the TUI frontends; a REPL that types a line and
// presses Return wants the OS's own line discipline and only needs to be able to ask
// whether the OS has finished one. Two files, two jobs, and neither grows the other's.
//
// AND IT IS NOT A THREAD. There is no reader thread, no queue and no synchronization:
// both platforms can be ASKED whether stdin has something, and a host that spawned a
// thread to find that out would have put concurrency into a single-threaded program to
// avoid a syscall.

#include <string>

namespace loom::host {

/// Whole lines from stdin, with a deadline.
class LineInput {
public:
    LineInput();

    /// Wait up to `timeout_ms` for a complete line.
    ///
    ///   Line     `*out` holds it, newline stripped (and a trailing CR, so a file with
    ///            Windows endings piped into a POSIX host reads the same).
    ///   Idle     nothing was ready in time; ask again. NOT an error and NOT end of
    ///            input — the caller goes and does other work.
    ///   Closed   stdin is at end of file or unusable. A caller that treats this as Idle
    ///            spins forever on a piped script that has run out, so it is its own
    ///            answer rather than a flavour of one.
    enum class Status { Line, Idle, Closed };

    /// `timeout_ms <= 0` polls: it returns immediately, Idle if nothing is ready.
    Status read(std::string* out, int timeout_ms);

    /// Is a line (or end-of-input) already available, with no waiting at all? Cheap
    /// enough to ask between turns of a busy bus.
    bool ready() const;

private:
    /// Bytes read but not yet forming a complete line. A pipe hands over whatever
    /// happens to be in it, which is regularly half a line, and a reader that dropped
    /// the remainder would eat commands from a script.
    std::string partial_;
    bool closed_ = false;
    /// Set when the stream ended with a final line that had no newline after it, so that
    /// last command still runs.
    bool flushed_ = false;
};

} // namespace loom::host

#endif // ZEN_HOST_LINE_INPUT_HPP
