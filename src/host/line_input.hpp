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
// The deadline has to be real while a person is HALFWAY THROUGH TYPING one, because the
// bus is turned by the same thread that asks.
//
// ---- WHAT "IS THERE A LINE" TAKES, PER PLATFORM --------------------------------
//
//   POSIX    `poll` on stdin. A terminal in canonical mode — every ordinary terminal — is
//            readable only once its line discipline holds a COMPLETE line, and a pipe or a
//            file is readable when it has bytes, so a read after a positive poll does not
//            block. Proven through a real pseudo-terminal, not only a pipe.
//
//   Windows  A console CANNOT be asked. `WaitForSingleObject` on its handle signals for any
//            input record, and `PeekConsoleInput` sees records — a key-down for `x` — not
//            whether the console's cooked read has a finished line. This reader used to
//            treat a queued character as "a read will not block" and call `ReadFile`, which
//            with line input enabled (every normal console) returns only at Enter: a
//            zero-deadline read took 606 ms when Enter came at 600 ms, and never returned
//            when it did not come, with the bus stopped on the same thread. It also peeked 32
//            records and consumed none, so 32 key-ups in front of a finished line hid it for
//            good. Both were invisible to a lane that feeds the host through a file.
//
//            So on Windows the blocking read happens where blocking is harmless: on a READER
//            THREAD that performs the console's own cooked `ReadFile` — which echoes, edits,
//            keeps history and consumes non-character records itself — and hands finished
//            lines to the host thread, which waits for one with a real deadline. Pipes and
//            files go through the same thread, replacing a 10 ms `PeekNamedPipe` poll.
//
// ---- THE READER THREAD, AND EVERYTHING IT IS NOT ALLOWED TO TOUCH ---------------
//
// It owns one input handle and a queue of lines. It never touches the bus, a weave, the
// console engine or stdout, so dispatch stays single-threaded; the only shared state is the
// queue, under one mutex. It changes no console or terminal mode, so there is no mode to
// restore when the host exits or fails.
//
// Stopping it (the destructor) cancels its pending read with `CancelSynchronousIo` until
// it leaves, then closes its handle. For a console that handle is the reader's OWN `CONIN$`
// object and not the process's stdin, and the difference is measured, not stylistic: a
// cancelled cooked read otherwise survives inside the console and eats the next line typed
// into it — through the shared handle and through a duplicate of it alike, since a
// duplicate is the same kernel object. A reader that cannot be stopped within its bound is
// left to process exit rather than terminated, because `TerminateThread` can abandon a lock
// the runtime needs.
//
// ---- WHAT IT IS NOT ------------------------------------------------------------
//
// Not a terminal, a line editor or a raw-mode backend. It returns whole lines exactly as
// the OS's line discipline produced them — no key vocabulary, no completion. The raw-mode
// seam is `loom::TerminalBackend` (src/console/terminal.hpp) and belongs to the TUI
// frontends; a REPL that types a line and presses Return wants the OS's own editing, and
// only needs to be told, promptly, whether the OS has finished one.

#include <memory>
#include <string>

namespace loom::host {

/// Whole lines from this process's stdin, with a deadline that holds while one is typed.
class LineInput {
public:
    LineInput();
    ~LineInput();
    LineInput(const LineInput&) = delete;
    LineInput& operator=(const LineInput&) = delete;

    /// Wait up to `timeout_ms` for a complete line.
    ///
    ///   Line     `*out` holds it, newline stripped (and a trailing CR, so a file with
    ///            Windows endings piped into a POSIX host reads the same).
    ///   Idle     nothing was ready in time; ask again. NOT an error and NOT end of
    ///            input — the caller goes and does other work. A half-typed line is Idle.
    ///   Closed   stdin is at end of file or unusable. A caller that treats this as Idle
    ///            spins forever on a piped script that has run out, so it is its own
    ///            answer rather than a flavour of one.
    enum class Status { Line, Idle, Closed };

    /// `timeout_ms <= 0` polls: it returns immediately, Idle if no line is finished.
    ///
    /// Nothing is read from stdin until the first call — so a `LineInput` that is built and
    /// never asked consumes no input and starts no thread.
    Status read(std::string* out, int timeout_ms);

private:
    struct Impl; ///< per platform, in line_input.cpp
    std::unique_ptr<Impl> impl_;
};

} // namespace loom::host

#endif // ZEN_HOST_LINE_INPUT_HPP
