// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_LINE_INPUT_HPP
#define ZEN_HOST_LINE_INPUT_HPP

// Reading a person's next command without stopping the world: "is there a whole line within
// `ms`?", with a deadline that holds while a line is half typed, because the thread that asks
// also turns the bus (`pump_pending()`, MSG-09). POSIX polls stdin; a Windows console cannot be
// asked whether a line is finished, so a reader thread does the blocking read there
// (line_input.cpp). One platform-neutral waiting area, `HeldInput`, decides what a line is and
// applies the two input limits (docs/reference/bounds.md).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace loom::host {

/// The longest command the host accepts, in bytes, not counting its line ending (`\n` or
/// `\r\n`). Why this number: docs/reference/bounds.md.
inline constexpr std::size_t kMaxCommandBytes = 4000;

/// Everything the reader holds for the host at once, in bytes: finished lines not yet taken,
/// plus the line still arriving. One Linux pipe's worth.
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

/// Whole lines from this process's stdin, with a deadline that holds while one is typed. Not a
/// line editor: it returns lines as the OS produced them; raw keys are `loom::TerminalBackend`'s
/// (src/console/terminal.hpp). What a reader has taken and not handed out goes with it, so a
/// stream wants one reader for its whole life.
class LineInput {
public:
    LineInput();
    ~LineInput();
    LineInput(const LineInput&) = delete;
    LineInput& operator=(const LineInput&) = delete;

    /// Wait up to `timeout_ms` for a complete line. Line: `*out` holds it, newline and any
    /// trailing CR stripped. Idle: nothing was ready in time, not an error and not the end; a
    /// half-typed line is Idle. TooLong: the next line exceeded kMaxCommandBytes and was
    /// refused; `*out` is emptied and `too_long()` says which. Closed: stdin ended or is
    /// unusable, its own answer, since a caller treating it as Idle spins on a finished script.
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
