# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# Test case names and the case map (cases.tsv): read every TEST_CASE and SUBCASE name under
# tests/, adjacent literals joined, and rename a file's cases as the map says. The map is the one
# owner of a rename; prove.py puts each old name back before it compares, so a name the map does
# not hold is a difference.
#
#   python tools/comment-pass/cases.py --list [FILE..]      each case, and the ids its name holds
#   python tools/comment-pass/cases.py --apply FILE..       rename the file's cases as the map says
#
# The applier rewrites a case's literals only, and refuses a name that another case of the same
# file already has, a row naming no case, and a row striking nothing (new equal to old). A row
# struck with a leading `#` is not applied; applying is idempotent.

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import grammar as grammar_mod  # noqa: E402
import lex  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
MAP = os.path.join(HERE, "cases.tsv")
MACRO = re.compile(r"\b(TEST_CASE|SUBCASE)\s*\(\s*$")
WIDTH = 100


class Case:
    __slots__ = ("path", "macro", "name", "pieces", "line")

    def __init__(self, path, macro, name, pieces, line):
        self.path = path      # repository-relative
        self.macro = macro    # TEST_CASE or SUBCASE
        self.name = name      # the joined literal text, escapes as written
        self.pieces = pieces  # [(start, end)] of each literal, quotes included
        self.line = line      # 1-based line of the first literal


def cases_in(rel, text):
    """The cases one C++ file declares, in source order."""
    spans = lex.cxx_spans(text)
    out = []
    for i, (kind, s, e) in enumerate(spans):
        if kind != lex.CODE:
            continue
        m = MACRO.search(text, s, e)
        if not m or m.end() != e:
            continue
        j = i + 1
        pieces = []
        while j < len(spans):
            k, ps, pe = spans[j]
            if k == lex.LITERAL and text[ps] == '"':
                pieces.append((ps, pe))
            elif not ((k == lex.CODE and text[ps:pe].strip() == "") or k == lex.COMMENT):
                break
            j += 1
        if pieces:
            name = "".join(text[ps + 1:pe - 1] for ps, pe in pieces)
            out.append(Case(rel, m.group(1), name, pieces, text.count("\n", 0, pieces[0][0]) + 1))
    return out


def read_map(path=MAP):
    """{(file, old): new}, struck rows left out."""
    rows = {}
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 3 or not parts[2] or parts[1] == parts[2]:
                sys.exit("cases.tsv:%d: a row is file, old, new, and new differs from old" % n)
            if (parts[0], parts[1]) in rows:
                sys.exit("cases.tsv:%d: %s has a second row" % (n, parts[1]))
            rows[(parts[0], parts[1])] = parts[2]
    return rows


def literal_text(text, case, new):
    """The source that replaces a case's literals, from its first quote to its last: one literal
    when it fits the line, else wrapped at spaces as the source wraps, each piece but the last
    keeping the space that ends its words."""
    start, end = case.pieces[0][0], case.pieces[-1][1]
    line_start = text.rfind("\n", 0, start) + 1
    line_end = text.find("\n", end)
    line_end = len(text) if line_end == -1 else line_end
    col = len(text[line_start:start].encode("utf-8"))
    suffix = len(text[end:line_end].encode("utf-8"))
    if col + len(new.encode("utf-8")) + 2 + suffix <= WIDTH:
        return '"%s"' % new
    if len(case.pieces) > 1:
        second = case.pieces[1][0]
        indent = text[text.rfind("\n", 0, second) + 1:second]
    else:
        indent = " " * (col + 4 if text[line_start:start].strip() else col)
    lines, cur = [], ""
    for w in new.split(" "):
        cap = (WIDTH - col - 3) if not lines else (WIDTH - len(indent) - 3)
        cand = w if not cur else cur + " " + w
        if cur and len(cand.encode("utf-8")) > cap:
            lines.append(cur)
            cur = w
        else:
            cur = cand
    lines.append(cur)
    if len(lines) > 1 and len(lines[-1].encode("utf-8")) + len(indent) + 2 + suffix > WIDTH:
        head, tail = lines[-1].rsplit(" ", 1)
        lines[-1:] = [head, tail]
    pieces = ['"%s "' % ln for ln in lines[:-1]] + ['"%s"' % lines[-1]]
    return ("\n" + indent).join(pieces)


def apply(repo, rel, rows):
    """Rename one file's cases as the map says, in the working tree. Returns (renamed, already)."""
    path = os.path.join(repo, rel)
    with open(path, encoding="utf-8", newline="") as f:
        text = f.read()
    crlf = "\r\n" in text
    text = text.replace("\r\n", "\n")
    mine = {old: new for (f, old), new in rows.items() if f == rel}
    found = cases_in(rel, text)
    names = [c.name for c in found]
    edits, already = [], 0
    for old, new in mine.items():
        hits = [c for c in found if c.name == old]
        if not hits:
            if new in names:
                already += 1
                continue
            sys.exit("%s: the map renames %r, which names no case" % (rel, old))
        if len(hits) > 1:
            sys.exit("%s: the map renames %r, which names %d cases" % (rel, old, len(hits)))
        c = hits[0]
        between = text[c.pieces[0][0]:c.pieces[-1][1]]
        if "//" in re.sub(r'"(\\.|[^"\\])*"', "", between):
            sys.exit("%s:%d: a comment sits between the literals of a renamed case" % (rel, c.line))
        edits.append((c.pieces[0][0], c.pieces[-1][1], literal_text(text, c, new)))
    after = [mine.get(n, n) for n in names]
    twice = sorted({n for n in after if after.count(n) > 1} - {n for n in names if names.count(n) > 1})
    if twice:
        sys.exit("%s: the map would give two cases one name: %s" % (rel, "; ".join(twice)))
    for s, e, repl in sorted(edits, reverse=True):
        text = text[:s] + repl + text[e:]
    if edits:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text.replace("\n", "\r\n") if crlf else text)
    return len(edits), already


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".")
    ap.add_argument("--map", default=MAP)
    ap.add_argument("--list", nargs="*", metavar="FILE")
    ap.add_argument("--apply", nargs="+", metavar="FILE")
    a = ap.parse_args()
    if a.apply:
        rows = read_map(a.map)
        for rel in a.apply:
            done, already = apply(a.repo, rel, rows)
            print("cases: %s: %d renamed, %d already renamed" % (rel, done, already))
        return 0
    g = grammar_mod.Grammar(a.repo)
    files = a.list or sorted(
        os.path.relpath(os.path.join(r, f), a.repo).replace(os.sep, "/")
        for r, _, fs in os.walk(os.path.join(a.repo, "tests")) for f in fs
        if lex.kind_of(f) == "cxx" and "third_party" not in r)
    for rel in files:
        with open(os.path.join(a.repo, rel), encoding="utf-8") as f:
            text = f.read().replace("\r\n", "\n")
        for c in cases_in(rel, text):
            ids = g.private_ids(c.name)
            print("%s:%d\t%s\t%s\t%s" % (rel, c.line, c.macro, ",".join(ids), c.name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
