# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The rename's applier: the second of sheet -> applier -> proof (AGENTS.md rule o). For every
# current-facing file (census.py names what that excludes) it writes the start commit's text with
# the rename applied: each word of the `maker` family that names the person becomes its weaver
# form (words.py decides, sheet.tsv overrides one occurrence), each identifier names.tsv lists is
# renamed as a whole token, a heading's changed anchor is followed by every link to it, and a
# comment or paragraph line the longer word pushed past its width hands its last words down. It
# regenerates from the start commit, so a rerun converges; hand edits are commits made after the
# applier's own, replayed on top of a rerun.
#
#   python tools/weaver-rename/apply.py --start <commit>              write the working tree
#   python tools/weaver-rename/apply.py --start <commit> --dry-run    say what would change
#   python tools/weaver-rename/apply.py --start <commit> --repo ../Loom --sheet <file> --names <file>

import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import census  # noqa: E402
import words  # noqa: E402

SHEET = os.path.join(HERE, "sheet.tsv")
NAMES = os.path.join(HERE, "names.tsv")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*")
SIMPLE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
# The documents whose list items are one line each: the registers and routers under agents/
# (a decision record wraps its items).
ONE_LINE_ITEMS = re.compile(r"^agents/(?!decisions/)")
CODE_WIDTH = 100
DOC_WIDTH = 98


def read_rows(path, width):
    rows = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != width:
                sys.exit(f"{path}:{n}: a row has {width} tab-separated fields")
            rows.append((n, parts))
    return rows


def read_sheet(path):
    """{(file, line, col): decision}: `weaver`, `keep`, or `=<text>` for the word's replacement."""
    out = {}
    for n, (rel, line, col, word, decision, _why) in read_rows(path, 6):
        key = (rel, int(line), int(col))
        if key in out:
            sys.exit(f"{path}:{n}: {key} has a second row")
        if decision not in ("weaver", "keep") and not decision.startswith("="):
            sys.exit(f"{path}:{n}: a decision is weaver, keep or =<text>")
        out[key] = (word, decision)
    return out


def read_names(path):
    """[(scope, old, new)]: scope `*` is every file; a path is that file alone. A row whose old
    name is qualified (`Panels::maker`) is a spelling in prose and comments."""
    out = []
    for n, (scope, old, new, _why) in read_rows(path, 4):
        if old == new or not IDENT.fullmatch(old) or not IDENT.fullmatch(new):
            sys.exit(f"{path}:{n}: a row renames one identifier to another")
        out.append((scope, old, new))
    return out


def names_for(rel, names):
    """{old: new} for one file: its own rows over the `*` rows."""
    table = {old: new for scope, old, new in names if scope == "*"}
    table.update({old: new for scope, old, new in names if scope == rel})
    return table


def width(s):
    return len(s.encode("utf-8"))


def rename(rel, text, sheet, names, used):
    """The file's text with every word and name renamed, before any line is rewrapped."""
    table = names_for(rel, names)
    edits = []
    for where, s, e in census.spans_of(rel, text):
        taken = set()
        if where != "code":
            for m in words.WORD.finditer(text, s, e):
                line = text.count("\n", 0, m.start()) + 1
                col = m.start() - (text.rfind("\n", 0, m.start()) + 1)
                key = (rel, line, col)
                cls = words.classify(text, m)
                decision = "weaver" if cls == "person" else "keep"
                if key in sheet:
                    word, decision = sheet[key]
                    if word != m.group(0):
                        sys.exit(f"sheet.tsv names {key} as `{word}`; the start commit has "
                                 f"`{m.group(0)}`")
                    used.add(key)
                if decision == "weaver":
                    edits.append((m.start(), m.end(), words.weaver_form(m.group(0))))
                elif decision.startswith("="):
                    edits.append((m.start(), m.end(), decision[1:]))
                if decision != "keep":
                    taken.add(m.start())
        # A qualified spelling (`Panels::maker`) is renamed whole, in prose and comments alone.
        qualified = {old: new for old, new in table.items() if "::" in old}
        done = []
        if where != "code":
            for old, new in qualified.items():
                for m in re.finditer(r"(?<![A-Za-z0-9_:])" + re.escape(old) + r"(?![A-Za-z0-9_])",
                                     text[s:e]):
                    a, b = s + m.start(), s + m.end()
                    edits.append((a, b, new))
                    done.append((a, b))
        for m in SIMPLE.finditer(text, s, e):
            tok = m.group(0)
            new = table.get(tok)
            if new is None:
                continue
            if any(a <= m.start() < b for a, b in done):
                continue
            plain = re.fullmatch(r"[a-z]+", tok) is not None
            if plain and where != "code":
                continue  # a plain word in prose is the word rule's, never a name row's
            joined = text[max(0, m.start() - 2):m.start()] == "::" or \
                text[m.end():m.end() + 2] == "::"
            if plain and joined:
                continue  # `zengine::maker`, `maker::Weave`: the library's namespace
            if plain and re.search(r"namespace\s+$", text[max(0, m.start() - 20):m.start()]):
                continue
            if m.start() in taken:
                continue
            edits.append((m.start(), m.end(), new))
    edits.sort()
    for a, b in zip(edits, edits[1:]):
        if a[1] > b[0]:
            sys.exit(f"{rel}: two edits overlap at {a} and {b}")
    out = []
    pos = 0
    for s, e, new in edits:
        out.append(text[pos:s])
        out.append(new)
        pos = e
    out.append(text[pos:])
    return "".join(out)


# ---- headings and their anchors --------------------------------------------------------------

HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")


def slug(title):
    """GitHub's anchor for a heading: lower case, punctuation but `-` and `_` dropped, spaces
    to hyphens."""
    s = title.strip().lower()
    s = re.sub(r"[`*]", "", s)
    s = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", s)
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


def headings(text):
    out = []
    fence = False
    for line in text.split("\n"):
        if line.lstrip().startswith("```"):
            fence = not fence
            continue
        if fence:
            continue
        m = HEADING.match(line)
        if m:
            out.append(slug(m.group(2)))
    return out


def anchor_moves(old, new):
    """{old slug: new slug} for the headings a rename changed, in order."""
    a, b = headings(old), headings(new)
    if len(a) != len(b):
        sys.exit("a rename changed how many headings a document has")
    return {x: y for x, y in zip(a, b) if x != y}


def follow_anchors(rel, text, moves_by_file):
    """Rewrite every `path#anchor` and `(#anchor)` whose target heading moved."""
    base = os.path.dirname(rel)

    def fix(m):
        target, anchor = m.group(1), m.group(2)
        if target == "":
            dest = rel
        elif "://" in target:
            return m.group(0)
        else:
            dest = os.path.normpath(os.path.join(base, target)).replace(os.sep, "/")
            if not dest.endswith(".md"):
                dest2 = target.replace(os.sep, "/")
                dest = dest2 if dest2 in moves_by_file else dest
        moved = moves_by_file.get(dest, {})
        if anchor in moved:
            return f"{target}#{moved[anchor]}"
        return m.group(0)

    return re.sub(r"([A-Za-z0-9_./-]*)#([a-z0-9_-]+)", fix, text)


# ---- rewrapping --------------------------------------------------------------------------------

LEADER = re.compile(r"^(\s*)(///<\s|///\s|//!\s|//\s|#\s|\*\s|)")
POINTER = re.compile(r"^\s*(//|#)\s*(?:(?:WL|MW|VM)-[A-Z]+-\d+|[A-Z][A-Za-z]* law:)")
MD_BLOCK = re.compile(r"^\s*(?:[-*+]\s|\d+[.)]\s|\||#|```|>|$)")


def leader(line, kind):
    if kind == "md":
        return re.match(r"^\s*", line).group(0)
    m = LEADER.match(line)
    return m.group(0) if m and m.group(2) else None


def continues(prev_lead, line, kind):
    """Does `line` continue the paragraph whose lines lead with `prev_lead`?"""
    if kind == "md":
        if MD_BLOCK.match(line):
            return False
        return re.match(r"^\s*", line).group(0) == prev_lead
    if POINTER.match(line):
        return False
    lead = leader(line, kind)
    if lead is None or lead != prev_lead:
        return False
    body = line[len(lead):]
    return body.strip() != "" and not body.startswith(("-", "*", "|", "```"))


def rewrap(rel, old, new, kind):
    """Hand the last words of any line the rename pushed past its width down to the next line of
    its paragraph, and on down until every line fits. Returns (text, lines left too long)."""
    cap = DOC_WIDTH if kind == "md" else CODE_WIDTH
    old_lines = old.split("\n")
    lines = new.split("\n")
    if len(old_lines) != len(lines):
        sys.exit(f"{rel}: the rename changed a line count before rewrapping")
    was = [width(x) for x in old_lines]
    left = []
    fence = False
    i = 0
    while i < len(lines):
        line = lines[i]
        if kind == "md" and line.lstrip().startswith("```"):
            fence = not fence
        too_long = width(line) > cap and was[i] <= cap
        if not too_long or fence:
            i += 1
            continue
        banner = re.match(r"^(.*\S\s+)([-=])\2{3,}$", line)
        if banner:
            # a section banner filled with a rule to the width: the rule gives way, not the words
            room = cap - width(banner.group(1))
            if room >= 4:
                lines[i] = banner.group(1) + banner.group(2) * room
                i += 1
                continue
        item_line = re.match(r"^\s*(?:[-*+]\s|\d+[.)]\s)", line) is not None
        if kind == "md" and MD_BLOCK.match(line) and (not item_line or ONE_LINE_ITEMS.match(rel)):
            left.append((i + 1, line))  # a table row, a heading, or a register's one-line item
            i += 1
            continue
        lead = leader(line, kind)
        if lead is None:
            left.append((i + 1, line))
            i += 1
            continue
        if kind == "md":
            item = re.match(r"^(\s*)(?:[-*+]\s|\d+[.)]\s)", line)
            cont_lead = (item.group(1) + " " * (len(item.group(0)) - len(item.group(1)))) if item else lead
        else:
            cont_lead = lead
        carry = []
        while width(line) > cap and " " in line[len(lead):].strip():
            head, word = line.rsplit(" ", 1)
            carry.insert(0, word)
            line = head
        lines[i] = line
        carry_text = " ".join(carry)
        j = i + 1
        if j < len(lines) and continues(cont_lead, lines[j], kind):
            nxt = lines[j]
            nlead = leader(nxt, kind)
            lines[j] = nlead + carry_text + " " + nxt[len(nlead):]
            was[j] = min(was[j], cap)  # a line that takes words down must fit, as it did
        else:
            lines.insert(j, cont_lead + carry_text)
            was.insert(j, cap)
        i += 1
    return "\n".join(lines), left


def plan(repo, start, sheet, names):
    renamed = {}
    used = set()
    for rel in census.files_at(repo, start):
        text = census.text_at(repo, start, rel)
        if text is None:
            continue
        low = text.lower()
        if "maker" not in low:
            continue
        new = rename(rel, text, sheet, names, used)
        if new != text:
            renamed[rel] = (text, new)
    unused = sorted(set(sheet) - used)
    if unused:
        sys.exit("sheet rows naming no word at the start commit:\n" +
                 "\n".join(f"  {k}" for k in unused))
    moves = {}
    for rel, (old, new) in renamed.items():
        if rel.endswith(".md"):
            m = anchor_moves(old, new)
            if m:
                moves[rel] = m
    out = {}
    too_long = []
    for rel in census.files_at(repo, start):
        if rel in renamed:
            old, new = renamed[rel]
        elif moves and (rel.endswith(".md") or census.kind_of(rel) in ("cxx", "cmake")):
            old = census.text_at(repo, start, rel)
            if old is None or "#" not in old:
                continue
            new = old
        else:
            continue
        if moves:
            new = follow_anchors(rel, new, moves)
        kind = "md" if rel.endswith(".md") else "code"
        wrapped, left = rewrap(rel, old, new, kind)
        too_long.extend((rel, n, line) for n, line in left)
        if wrapped != old:
            out[rel] = wrapped
    return out, moves, too_long


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".")
    ap.add_argument("--start", required=True)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--sheet", default=SHEET)
    ap.add_argument("--names", default=NAMES)
    a = ap.parse_args()
    sheet = read_sheet(a.sheet)
    names = read_names(a.names)
    out, moves, too_long = plan(a.repo, a.start, sheet, names)
    print(f"files written {len(out)}; headings moved {sum(len(m) for m in moves.values())} in "
          f"{len(moves)} documents; lines left past their width {len(too_long)}")
    for rel, n, line in too_long:
        print(f"  too long {rel}:{n}: {line.strip()[:90]}")
    if a.dry_run:
        for rel in sorted(out):
            print("  would write", rel)
        return
    for rel, text in out.items():
        with open(os.path.join(a.repo, rel), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


if __name__ == "__main__":
    main()
