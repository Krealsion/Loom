# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# Apply a hand-written comment edit to one or more files, touching comments and nothing else.
# The edit names lines of the file as it is now; each file's operations apply in one pass,
# from the bottom up, and the file is written only when its code and literals are unchanged.
#
#   python tools/comment-pass/edit.py <edit-file> [--dry-run] [--show]
#
# --show prints each operation's edges before it runs -- the line above a range, its first and
# last lines, the line below -- so a line number that is off by one is seen, not shipped. The
# loop per file: read its comments with blocks.py, write the edit, run it with --dry-run --show,
# read the edges, apply, then run the check (tests/check_source_comments.cmake) and prove.py.
#
# The edit file:
#
#   = src/kernel/kernel.cpp     the file the operations below apply to
#   D 12-40                     delete lines 12..40; each is a comment or blank line
#   R 55-57                     replace lines 55..57 (comment or blank) with the `|` lines below
#   |// The one line that stays.
#   T 80                        drop line 80's trailing comment; a `|` line below replaces it
#   I 90                        insert the `|` lines below before line 90
#
# A deletion that leaves two blank lines side by side drops one: that is the gap a removed
# comment leaves, and nothing else about the layout changes.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lex  # noqa: E402


WIDTH = 100  # the widest line an operation may write


class Refused(Exception):
    pass


def parse(path):
    files = []
    cur = None
    op = None
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.rstrip("\n").rstrip("\r")
            if line.startswith("|"):
                if op is None:
                    raise Refused("%s:%d: a `|` line with no operation above it" % (path, n))
                op["text"].append(line[1:])
                continue
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            if s.startswith("= "):
                cur = {"path": s[2:].strip(), "ops": []}
                files.append(cur)
                op = None
                continue
            if cur is None:
                raise Refused("%s:%d: an operation before any `= <file>` line" % (path, n))
            kind, _, rng = s.partition(" ")
            if kind not in ("D", "R", "T", "I"):
                raise Refused("%s:%d: unknown operation %r" % (path, n, kind))
            a, _, b = rng.strip().partition("-")
            a = int(a)
            b = int(b) if b else a
            if b < a:
                raise Refused("%s:%d: range %d-%d runs backwards" % (path, n, a, b))
            op = {"kind": kind, "a": a, "b": b, "text": [], "at": n}
            cur["ops"].append(op)
    return files


def classes_of(path, text):
    spans = lex.spans_of(path, text)
    return [c for c, _, _ in lex.line_bytes(text, spans)], spans


def trailing_comment_cut(path, line):
    """Where the trailing comment of a code line starts."""
    spans = lex.spans_of(path, line)
    comment_starts = [s for kind, s, e in spans if kind == lex.COMMENT]
    if not comment_starts:
        return None
    cut = comment_starts[-1]
    # the comment must run to the end of the line
    tail = [e for kind, s, e in spans if kind == lex.COMMENT and s == cut][0]
    if line[tail:].strip():
        return None
    return cut


def close_gap(lines, at):
    """The gap a deletion at index `at` leaves: two blank lines meeting there become one, and
    a blank line left first in the file or just inside an opening brace goes. Only the line
    at `at` is ever dropped -- the operations above it have not run yet."""
    blank = lambda k: 0 <= k < len(lines) and not lines[k].strip()
    if blank(at) and (at == 0 or blank(at - 1)):
        del lines[at]
    elif blank(at) and at > 0 and lines[at - 1].rstrip().endswith("{"):
        del lines[at]


def show_edges(rel, lines, ops):
    def at(k):
        return lines[k - 1].strip()[:60] if 1 <= k <= len(lines) else "(edge of file)"
    print("== %s" % rel)
    for o in ops:
        a, b = o["a"], o["b"]
        if o["kind"] in ("D", "R"):
            print("  %s %d-%d  ^ %s\n      [ %s\n      ] %s\n      v %s" % (
                o["kind"], a, b, at(a - 1), at(a), at(b), at(b + 1)))
        else:
            print("  %s %d  ^ %s\n      @ %s" % (o["kind"], a, at(a - 1), at(a)))


def apply_file(repo, spec, dry_run, show=False):
    rel = spec["path"]
    full = os.path.join(repo, rel)
    with open(full, "rb") as f:
        raw = f.read()
    if b"\r\n" in raw:
        raise Refused("%s: CRLF line endings; this tool writes LF only" % rel)
    text = raw.decode("utf-8")
    classes, spans = classes_of(rel, text)
    lines = text.split("\n")
    trailing_newline = text.endswith("\n")
    if trailing_newline:
        lines.pop()
    ops = sorted(spec["ops"], key=lambda o: (o["a"], o["b"]))
    for o, nxt in zip(ops, ops[1:]):
        if nxt["a"] <= o["b"]:
            raise Refused("%s: operations at lines %d and %d overlap" % (rel, o["a"], nxt["a"]))
    for o in ops:
        if o["b"] > len(lines):
            raise Refused("%s: line %d is past the end (%d lines)" % (rel, o["b"], len(lines)))
        if o["kind"] in ("D", "R"):
            for k in range(o["a"], o["b"] + 1):
                if classes[k - 1] == "code":
                    raise Refused("%s:%d: a %s range holds code: %r" % (rel, k, o["kind"], lines[k - 1]))
        if o["kind"] == "D" and o["text"]:
            raise Refused("%s:%d: D takes no `|` lines; use R" % (rel, o["a"]))
        if o["kind"] in ("R", "I") and not o["text"]:
            raise Refused("%s:%d: %s needs `|` lines" % (rel, o["a"], o["kind"]))
        for t in o["text"]:
            if len(t) > WIDTH:
                raise Refused("%s:%d: a written line is %d columns, over %d: %r" % (rel, o["a"], len(t), WIDTH, t))
        if o["kind"] == "T":
            if o["a"] != o["b"]:
                raise Refused("%s:%d: T takes one line" % (rel, o["a"]))
            if classes[o["a"] - 1] != "code":
                raise Refused("%s:%d: T needs a code line with a trailing comment" % (rel, o["a"]))
            if trailing_comment_cut(rel, lines[o["a"] - 1]) is None:
                raise Refused("%s:%d: no trailing comment to drop: %r" % (rel, o["a"], lines[o["a"] - 1]))
            if len(o["text"]) > 1:
                raise Refused("%s:%d: T takes at most one `|` line" % (rel, o["a"]))
    if show:
        show_edges(rel, lines, ops)
    # bottom-up, so earlier line numbers stay valid; a deletion's gap is closed at once,
    # which touches only lines at or below it
    for o in sorted(ops, key=lambda o: -o["a"]):
        a, b = o["a"], o["b"]
        if o["kind"] == "D":
            del lines[a - 1:b]
            close_gap(lines, a - 1)
        elif o["kind"] == "R":
            lines[a - 1:b] = o["text"]
        elif o["kind"] == "I":
            lines[a - 1:a - 1] = o["text"]
        elif o["kind"] == "T":
            # a replaced comment keeps its column; a dropped one takes its lead-in with it
            line = lines[a - 1]
            cut = trailing_comment_cut(rel, line)
            lines[a - 1] = line[:cut] + o["text"][0].lstrip() if o["text"] else line[:cut].rstrip()
    new_text = "\n".join(lines) + ("\n" if trailing_newline else "")
    new_classes, new_spans = classes_of(rel, new_text)
    # every line an operation wrote must be comment or blank (T lines excepted: code)
    before = lex.normalized_lines(text, spans)
    after = lex.normalized_lines(new_text, new_spans)
    if before != after:
        for i, (x, y) in enumerate(zip(before, after)):
            if x != y:
                raise Refused("%s: code changed at normalized line %d:\n  was: %s\n  now: %s" % (rel, i + 1, x, y))
        raise Refused("%s: code changed: %d normalized lines became %d" % (rel, len(before), len(after)))
    if lex.literals(text, spans) != lex.literals(new_text, new_spans):
        raise Refused("%s: a literal changed" % rel)
    if not dry_run:
        with open(full, "w", encoding="utf-8", newline="\n") as f:
            f.write(new_text)
    return len(text.encode("utf-8")), len(new_text.encode("utf-8"))


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) < 2:
        print(__doc__ or "usage: edit.py <edit-file> [--dry-run]")
        return 2
    repo = os.getcwd()
    dry = "--dry-run" in sys.argv
    show = "--show" in sys.argv
    try:
        specs = parse(sys.argv[1])
        results = [(s["path"],) + apply_file(repo, s, dry, show) for s in specs]
    except Refused as e:
        print("REFUSED: %s" % e)
        return 1
    for path, b0, b1 in results:
        print("%s %s: %d -> %d bytes (%+d)" % ("checked" if dry else "wrote", path, b0, b1, b1 - b0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
