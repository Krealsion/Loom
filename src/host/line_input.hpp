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
//            keeps history and consumes non-character records itself — and puts what it read
//            into the waiting area below, where the host thread waits for a finished line with
//            a real deadline. Pipes and files go through the same thread.
//
// ---- WHAT THE READER MAY HOLD, AND WHAT IT REFUSES: ONE RULE, EVERY PLATFORM ----
//
// Input can arrive faster than the host takes it — a script piped in, a file, a paste. The
// Windows reader thread once moved all of it into a queue with no limit, as fast as the
// producer could supply it: 25.6 MB of commands in a file became about 37 MB of this
// process's memory before the host had taken one. Neither platform limited a line that never
// ended. So the whole input path has TWO LIMITS, defined once, below, and applied by one
// platform-neutral waiting area, `HeldInput`, which is the only code that decides what a line
// is, what is too long, how much may wait and what the end of input means. The platform
// readers only move bytes into it and wait; they cannot drift apart on any of those questions.
//
//   kMaxCommandBytes  The longest command. A longer line is REFUSED — `Status::TooLong`, at its
//                     place in the input, with its line number — and none of it is handed out:
//                     it is never cut down into a shorter command, and its remainder never runs
//                     as the next one. The rest of that line is discarded as it arrives, so a
//                     line that never ends is not held while the reader waits for its newline.
//                     The number is below the 4095 bytes a Linux terminal keeps of a typed line
//                     (measured: it drops the rest without a sign), so a line that terminal cut
//                     short is refused rather than run; a Windows console delivers a long line
//                     whole (measured to 100,000 characters) and gets the same rule.
//
//   kHeldInputBytes   Everything the reader holds for the host at once: finished lines not yet
//                     taken AND the line still arriving, counted in bytes in one buffer, so a
//                     flood of empty lines cannot hold more than its bytes say. When it is full
//                     the reader WAITS, and resumes as the host takes lines. Nothing is dropped,
//                     reordered or counted against a lifetime total: a stream of any length is
//                     processed as fast as the host keeps up, and what it has not reached stays
//                     with its producer — a pipe's writer waits, a file stays on disk, a terminal
//                     keeps what was typed.
//
// How full the area gets is where the platforms differ, and that is all: POSIX reads only when
// the host asks and nothing is finished; the Windows thread reads ahead until the area is full.
//
// ---- THE READER THREAD, AND EVERYTHING IT IS NOT ALLOWED TO TOUCH ---------------
//
// It owns one input handle and shares the waiting area with the host thread under one mutex.
// It never touches the bus, a weave, the console engine or stdout, so dispatch stays
// single-threaded. It changes no console or terminal mode, so there is no mode to restore
// when the host exits or fails.
//
// Stopping it (the destructor) wakes it if it is waiting for room, and cancels a pending read
// with `CancelSynchronousIo` until it leaves, then closes its handle. For a console that handle
// is the reader's OWN `CONIN$` object and not the process's stdin, and the difference is
// measured, not stylistic: a cancelled cooked read otherwise survives inside the console and
// eats the next line typed into it — through the shared handle and through a duplicate of it
// alike, since a duplicate is the same kernel object. A reader that cannot be stopped within
// its bound is left to process exit rather than terminated, because `TerminateThread` can
// abandon a lock the runtime needs.
//
// ---- REPLACING A READER ----------------------------------------------------------
//
// What a reader has taken from stdin and not handed out goes with it: at most
// kHeldInputBytes, on either platform. On a Windows console that includes a half-typed line,
// whose cooked read is cancelled with the reader; a POSIX terminal keeps a half-typed line in
// its own line discipline, and the next reader receives it whole once it is finished. On a
// pipe or a file it can include the start of a line — or of a line being refused — whose rest
// the next reader then receives as a line of its own. So a stream wants one reader for its
// whole life, which is how the supplied host reads stdin.
//
// ---- WHAT IT IS NOT ------------------------------------------------------------
//
// Not a terminal, a line editor or a raw-mode backend. It returns whole lines exactly as
// the OS's line discipline produced them — no key vocabulary, no completion. The raw-mode
// seam is `loom::TerminalBackend` (src/console/terminal.hpp) and belongs to the TUI
// frontends; a REPL that types a line and presses Return wants the OS's own editing, and
// only needs to be told, promptly, whether the OS has finished one.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace loom::host {

/// THE LONGEST COMMAND the host accepts, in bytes, not counting its line ending (`\n` or
/// `\r\n`). See the header note for why this number, and docs/reference/bounds.md.
inline constexpr std::size_t kMaxCommandBytes = 4000;

/// EVERYTHING THE READER HOLDS FOR THE HOST AT ONCE, in bytes: finished lines not yet taken,
/// plus the line still arriving. One Linux pipe's worth; see the header note.
inline constexpr std::size_t kHeldInputBytes = 64 * 1024;

/// How many of a refused line's first bytes are kept, so a person can tell which line it was.
inline constexpr std::size_t kTooLongShownBytes = 32;

/// A line refused for its length.
struct TooLongLine {
    std::uint64_t line = 0; ///< its number in the input, counting from 1
    std::string beginning;  ///< its first bytes (at most kTooLongShownBytes), exactly as read
};

/// THE WAITING AREA: every byte the reader has taken from stdin and the host has not yet taken
/// as a line, and the one place the input limits are applied. Platform-neutral and not
/// synchronized — a reader that shares it across threads holds its own lock around every call.
class HeldInput {
public:
    /// What `take` found.
    enum class Taken {
        Line,    ///< a finished command, line ending removed
        TooLong, ///< a line longer than kMaxCommandBytes, refused where it stood in the input
        Nothing, ///< no finished line yet
        Ended,   ///< the input has ended and everything in it has been taken
    };

    /// How many more bytes may be added now without holding more than kHeldInputBytes.
    std::size_t room() const noexcept;

    /// Bytes just read, in input order. A reader reads no more than `room()`, which is what
    /// keeps `held()` within its limit: nothing here drops input to make it fit. Bytes that
    /// belong to a refused line are discarded here, through that line's newline.
    void add(const char* data, std::size_t n);

    /// The input has ended; nothing more will be added. A last line without a newline is still
    /// a line (and is still refused if it is too long).
    void end() noexcept;

    /// Would `take` find a line, a refusal or the end?
    bool ready() const;

    /// The next line or refusal, strictly in input order. On `Line`, `*line` is the command
    /// without its ending (a trailing `\r` goes too, so CRLF input reads the same everywhere).
    /// On `TooLong`, `*refused` says which line it was, and none of its bytes is handed out.
    Taken take(std::string* line, TooLongLine* refused);

    /// Bytes held now: finished lines not taken, plus the line still arriving.
    std::size_t held() const noexcept;
    /// The most ever held — the direct evidence that the limit held.
    std::size_t peak() const noexcept;

private:
    std::size_t newline() const;
    bool tail_too_long() const noexcept;
    void refuse(TooLongLine* refused) const;
    void consume_to(std::size_t end);

    std::string bytes_;               ///< the held bytes are [head_, size)
    std::size_t head_ = 0;
    mutable std::size_t scanned_ = 0; ///< [head_, scanned_) is known to hold no newline
    std::uint64_t line_ = 1;          ///< the number of the line that starts at head_
    bool discarding_ = false;         ///< the rest of a refused line is still arriving
    bool ended_ = false;
    std::size_t peak_ = 0;
};

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
    ///   TooLong  the next line was longer than kMaxCommandBytes and was REFUSED: `*out` is
    ///            emptied, so a caller that runs it anyway runs nothing, and `too_long()` says
    ///            which line it was. Lines after it are read as usual.
    ///   Closed   stdin is at end of file or unusable. A caller that treats this as Idle
    ///            spins forever on a piped script that has run out, so it is its own
    ///            answer rather than a flavour of one.
    enum class Status { Line, Idle, TooLong, Closed };

    /// `timeout_ms <= 0` polls: it returns immediately, Idle if no line is finished.
    ///
    /// Nothing is read from stdin until the first call — so a `LineInput` that is built and
    /// never asked consumes no input and starts no thread.
    Status read(std::string* out, int timeout_ms);

    /// The line the most recent `TooLong` refused.
    const TooLongLine& too_long() const noexcept;

    /// What this reader holds for its caller now, and the most it has ever held, in bytes.
    /// Both are at most kHeldInputBytes; they are how a test proves it.
    struct Held {
        std::size_t bytes = 0;
        std::size_t peak = 0;
    };
    Held held() const;

private:
    struct Impl; ///< per platform, in line_input.cpp
    std::unique_ptr<Impl> impl_;
};

} // namespace loom::host

#endif // ZEN_HOST_LINE_INPUT_HPP
