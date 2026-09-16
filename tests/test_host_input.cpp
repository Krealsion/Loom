// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE SUPPLIED HOST'S INPUT LIMITS, WHERE THEY ARE DECIDED (src/host/line_input.hpp).
//
// `HeldInput` is the one place both platform readers take their answers from: what a line is,
// what is too long, how much may wait, and what the end of input means. These cases feed it bytes
// directly — every boundary, every split, every ordering — so the rules are pinned on every
// platform by the same assertions, with nothing about threads, terminals or timing in the way.
// What only a real stdin can show (a pipe's writer held back, a console's cooked read, a
// pseudo-terminal that cuts long lines) is `tests/host_terminal/witness.cpp`'s.

#include <doctest.h>

#include "line_input.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using loom::host::HeldInput;
using loom::host::kHeldInputBytes;
using loom::host::kMaxCommandBytes;
using loom::host::kTooLongShownBytes;
using loom::host::TooLongLine;

namespace {

using Taken = HeldInput::Taken;

void add(HeldInput& in, const std::string& bytes) {
    // A reader never adds more than there is room for; neither does this.
    REQUIRE(bytes.size() <= in.room());
    in.add(bytes.data(), bytes.size());
}

/// One `take`, and what it produced.
struct Got {
    Taken taken = Taken::Nothing;
    std::string line;
    TooLongLine refused;
};

Got take(HeldInput& in) {
    Got g;
    g.line = "(untouched)";
    g.taken = in.take(&g.line, &g.refused);
    return g;
}

/// A deterministic command of exactly `n` bytes, different for every `i`, never containing a
/// newline or ending in `\r`.
std::string command(std::size_t i, std::size_t n) {
    std::string s = "c" + std::to_string(i) + ":";
    while (s.size() < n) {
        s.push_back(static_cast<char>('a' + (s.size() * 7 + i) % 26));
    }
    s.resize(n);
    return s;
}

} // namespace

TEST_SUITE("host_input") {

TEST_CASE("a line is handed out whole and without its ending, and CRLF reads the same") {
    HeldInput in;
    add(in, "status\r\nweaves\nshow m1\r\n");
    Got a = take(in);
    Got b = take(in);
    Got c = take(in);
    CHECK((a.taken == Taken::Line && a.line == "status"));
    CHECK((b.taken == Taken::Line && b.line == "weaves"));
    CHECK((c.taken == Taken::Line && c.line == "show m1"));
    CHECK(take(in).taken == Taken::Nothing);
    CHECK(in.held() == 0);
}

TEST_CASE("a line split across reads is one line, and an unfinished one is not handed out") {
    HeldInput in;
    add(in, "author");
    CHECK_FALSE(in.ready());
    Got g = take(in);
    CHECK(g.taken == Taken::Nothing);
    CHECK(g.line == "(untouched)");
    CHECK(in.held() == 6); // the unfinished line is held, and counted
    add(in, "ity sh");
    add(in, "ow probe");
    add(in, "\n");
    CHECK(in.ready());
    g = take(in);
    CHECK(g.taken == Taken::Line);
    CHECK(g.line == "authority show probe");
}

TEST_CASE("a command of exactly the limit is a command; one byte more is refused whole") {
    HeldInput in;
    const std::string at_limit = command(1, kMaxCommandBytes);
    const std::string over = command(2, kMaxCommandBytes + 1);
    add(in, at_limit + "\n");
    add(in, over + "\n");
    add(in, "after\n");

    Got a = take(in);
    CHECK(a.taken == Taken::Line);
    CHECK(a.line == at_limit);

    Got b = take(in);
    REQUIRE(b.taken == Taken::TooLong);
    // NONE OF IT IS HANDED OUT, and the refusal says where it stood and how it began.
    CHECK(b.line == "(untouched)");
    CHECK(b.refused.line == 2);
    CHECK(b.refused.beginning == over.substr(0, kTooLongShownBytes));

    Got c = take(in);
    CHECK(c.taken == Taken::Line);
    CHECK(c.line == "after");
}

TEST_CASE("a CRLF ending is not part of the command, at the limit and across reads") {
    HeldInput in;
    const std::string at_limit = command(3, kMaxCommandBytes);
    add(in, at_limit + "\r");
    // One byte over a command, but that byte may be the `\r` of a `\r\n` still arriving: WAIT,
    // do not refuse.
    CHECK_FALSE(in.ready());
    CHECK(take(in).taken == Taken::Nothing);
    add(in, "\n");
    Got g = take(in);
    CHECK(g.taken == Taken::Line);
    CHECK(g.line == at_limit);

    // The same `\r` at the very end of the input is still a line ending.
    HeldInput end;
    add(end, at_limit + "\r");
    end.end();
    Got last = take(end);
    CHECK(last.taken == Taken::Line);
    CHECK(last.line == at_limit);
    CHECK(take(end).taken == Taken::Ended);
}

TEST_CASE("an unfinished line is refused once it cannot be a command, and its rest never runs") {
    HeldInput in;
    add(in, "first\n");
    // One byte more than a command and a `\r` could be: already too long, though unfinished.
    const std::string head = std::string("quit") + std::string(kMaxCommandBytes - 2, ' ');
    REQUIRE(head.size() == kMaxCommandBytes + 2);
    add(in, head);

    CHECK(take(in).line == "first");
    CHECK(in.ready()); // nothing more has to arrive for this answer
    Got g = take(in);
    REQUIRE(g.taken == Taken::TooLong);
    CHECK(g.refused.line == 2);
    CHECK(g.refused.beginning.rfind("quit", 0) == 0);
    CHECK(in.held() == 0); // released now, not when its newline turns up

    // THE REST OF THAT LINE — including something that looks like a command — is discarded as
    // it arrives, through its newline, and only then does input count again.
    add(in, std::string(3000, ' '));
    add(in, "quit");
    CHECK(in.held() == 0);
    CHECK(take(in).taken == Taken::Nothing);
    add(in, "\nstatus\n");
    Got next = take(in);
    CHECK(next.taken == Taken::Line);
    CHECK(next.line == "status");
    CHECK(take(in).taken == Taken::Nothing);

    // Numbering carries on from the refused line.
    add(in, command(4, kMaxCommandBytes + 10) + "\n");
    Got again = take(in);
    REQUIRE(again.taken == Taken::TooLong);
    CHECK(again.refused.line == 4);
}

TEST_CASE("a line that never ends is not held while the reader waits for its newline") {
    HeldInput in;
    // A reader adding what there is room for, as fast as a producer supplies it: a megabyte and
    // more of one line, with the host taking whatever is ready between reads.
    const std::string chunk(4096, 'x');
    std::size_t refusals = 0;
    std::size_t added = 0;
    for (int i = 0; i < 300; ++i) {
        REQUIRE(in.room() >= chunk.size()); // never stuck waiting for room
        add(in, chunk);
        added += chunk.size();
        Got g = take(in);
        if (g.taken == Taken::TooLong) {
            ++refusals;
        } else {
            CHECK(g.taken == Taken::Nothing);
        }
    }
    CHECK(added > 1000000);
    CHECK(refusals == 1);          // one line, one refusal
    CHECK(in.held() == 0);         // and none of it kept
    CHECK(in.peak() <= kMaxCommandBytes + chunk.size());
    add(in, "\nquit\n");
    Got g = take(in);
    CHECK(g.taken == Taken::Line);
    CHECK(g.line == "quit");
}

TEST_CASE("the end of input: a last line without a newline runs once, and the end stays the end") {
    HeldInput in;
    add(in, "first\nlast");
    in.end();
    CHECK(take(in).line == "first");
    Got last = take(in);
    CHECK(last.taken == Taken::Line);
    CHECK(last.line == "last");
    CHECK(take(in).taken == Taken::Ended);
    CHECK(take(in).taken == Taken::Ended);
    CHECK(in.ready());

    // Nothing at all is an end, not a line.
    HeldInput empty;
    empty.end();
    CHECK(take(empty).taken == Taken::Ended);

    // A last line too long to be a command is refused, even with nothing after it.
    HeldInput over;
    add(over, command(5, kMaxCommandBytes + 1));
    over.end();
    Got g = take(over);
    CHECK(g.taken == Taken::TooLong);
    CHECK(take(over).taken == Taken::Ended);

    // And input that arrives after the end is not input.
    add(in, "late\n");
    CHECK(take(in).taken == Taken::Ended);
}

TEST_CASE("an input ending inside a refused line ends there") {
    HeldInput in;
    add(in, command(6, kMaxCommandBytes + 2));
    CHECK(take(in).taken == Taken::TooLong);
    add(in, "still the same line");
    in.end();
    CHECK(take(in).taken == Taken::Ended);
}

TEST_CASE("refusals keep their place: nothing is reordered around them") {
    HeldInput in;
    add(in, "one\n");
    add(in, command(7, kMaxCommandBytes + 5) + "\n");
    add(in, "two\n\n");
    add(in, command(8, kMaxCommandBytes + 1) + "\r\n");
    add(in, "three");
    in.end();

    std::vector<std::string> seen;
    for (;;) {
        Got g = take(in);
        if (g.taken == Taken::Ended) {
            break;
        }
        REQUIRE(g.taken != Taken::Nothing);
        seen.push_back(g.taken == Taken::Line ? "line:" + g.line
                                              : "refused:" + std::to_string(g.refused.line));
    }
    const std::vector<std::string> expected{"line:one", "refused:2", "line:two", "line:",
                                            "refused:5", "line:three"};
    CHECK(seen == expected);
}

TEST_CASE("what is held counts finished and unfinished bytes, and never passes the limit") {
    HeldInput in;
    const std::string line = command(9, 99) + "\n"; // 100 bytes
    while (in.room() >= line.size()) {
        add(in, line);
    }
    add(in, std::string(in.room(), 'u')); // fill the rest with the start of an unfinished line
    CHECK(in.room() == 0);
    CHECK(in.held() == kHeldInputBytes);
    CHECK(in.peak() == kHeldInputBytes);

    // Taking a line makes exactly its bytes of room.
    Got g = take(in);
    CHECK(g.taken == Taken::Line);
    CHECK(in.room() == line.size());
    CHECK(in.held() == kHeldInputBytes - line.size());
    CHECK(in.peak() == kHeldInputBytes);
}

TEST_CASE("empty lines are lines, and each costs its bytes") {
    HeldInput in;
    add(in, "\n\r\n\n");
    CHECK(in.held() == 4);
    int lines = 0;
    for (Got g = take(in); g.taken == Taken::Line; g = take(in)) {
        CHECK(g.line.empty());
        ++lines;
    }
    CHECK(lines == 3);
    CHECK(in.held() == 0);
}

TEST_CASE("bytes inside a command are the command's, whatever they are") {
    HeldInput in;
    std::string odd = "a\rb";
    odd.push_back('\0');
    odd += "c\t\x7f";
    add(in, odd + "\n");
    Got g = take(in);
    CHECK(g.taken == Taken::Line);
    CHECK(g.line == odd);
}

TEST_CASE("a stream of any length passes through, exactly and in order, as the host takes it") {
    // A producer much faster than the host, a host that stops taking for a while, and every
    // length from empty to the limit: nothing lost, nothing reordered, nothing over the limit,
    // and no total that runs out.
    HeldInput in;
    constexpr std::size_t kLines = 8000;
    std::string stream;
    std::vector<std::string> sent;
    sent.reserve(kLines);
    for (std::size_t i = 0; i < kLines; ++i) {
        // Half of them short, so a read carries many lines; half anywhere up to the limit.
        const std::size_t spread = (i * 2654435761U) >> 7;
        const std::size_t len = (i % 2 == 0) ? spread % 64 : spread % (kMaxCommandBytes + 1);
        sent.push_back(command(i, len));
        stream += sent.back();
        stream += (i % 3 == 0) ? "\r\n" : "\n";
    }
    std::size_t offset = 0;
    std::size_t received = 0;
    std::size_t waits_for_room = 0;
    bool ok = true;
    for (std::size_t turn = 0; received < kLines; ++turn) {
        // The producer: as much as fits, in reads of 4 KiB, until the area is full.
        while (offset < stream.size() && in.room() > 0) {
            const std::size_t n = std::min<std::size_t>({4096, in.room(), stream.size() - offset});
            in.add(stream.data() + offset, n);
            offset += n;
        }
        if (offset < stream.size()) {
            ++waits_for_room;
        } else {
            in.end();
        }
        // The host: holds back for a while every so often, then takes a few lines.
        if (turn % 50 < 10) {
            continue;
        }
        for (int k = 0; k < 7 && received < kLines; ++k) {
            Got g = take(in);
            if (g.taken == Taken::Nothing) {
                break;
            }
            if (g.taken != Taken::Line || g.line != sent[received]) {
                ok = false;
            }
            ++received;
        }
        if (!ok || turn > 10 * kLines) {
            break;
        }
    }
    CHECK(ok);
    CHECK(received == kLines);
    CHECK(take(in).taken == Taken::Ended);
    CHECK(in.peak() <= kHeldInputBytes);
    CHECK(in.peak() > kHeldInputBytes - 4096); // the producer really was ahead, and waited
    CHECK(waits_for_room > 0);
}

} // TEST_SUITE("host_input")
