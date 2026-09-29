# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The rename's census: every word of the `maker` family in a current-facing file, where it
# stands (a document, a comment, a literal, code) and whose it is by the grammar in words.py
# (the person, the library, or a token -- a path, an identifier or a wire name). It also lists
# every identifier in code that spells the family. Read from a commit, never the working tree,
# so the numbers name what they measured.
#
#   python tools/weaver-rename/census.py --start <commit>                  the counts
#   python tools/weaver-rename/census.py --start <commit> --list <file>    ...and every word, a row each
#   python tools/weaver-rename/census.py --start <commit> --repo ../Loom   another repository

import argparse
import collections
import io
import os
import re
import subprocess
import sys
import tokenize

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.append(os.path.join(HERE, "..", "comment-pass"))
import words  # noqa: E402

try:
    import lex  # noqa: E402
except ImportError:  # Loom's comment pass keeps its lexer under the same name
    lex = None

# What the rename does not read: the frozen history, the quarry, vendored trees, and the
# finished mass-edit tools, whose sheets quote the text their own start commits held.
EXCLUDE = re.compile(r"^(docs/history/|reference/|archive/|tests/third_party/|third_party/|"
                     r"tools/|.*\.png$|.*\.ico$|LICENSE)")
ID_FAMILY = re.compile(r"[A-Za-z_][A-Za-z0-9_]*", re.ASCII)


def git(repo, *args):
    return subprocess.run(["git", "-C", repo, *args], capture_output=True, check=True).stdout


def files_at(repo, start):
    names = git(repo, "ls-tree", "-r", "--name-only", start).decode("utf-8").split("\n")
    return [n for n in names if n and not EXCLUDE.match(n)]


def text_at(repo, start, rel):
    data = git(repo, "show", f"{start}:{rel}")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def kind_of(rel):
    """'md', 'cxx', 'cmake', 'manifest', 'py' or 'data' (read whole as text)."""
    if rel.endswith(".md"):
        return "md"
    if rel.endswith(".py"):
        return "py"
    k = lex.kind_of(rel) if lex else None
    if k:
        return k
    return "data"


def python_spans(text):
    """[(kind, start, end)] for a Python file: comments and strings, the code between them."""
    offsets = [0]
    for line in text.split("\n"):
        offsets.append(offsets[-1] + len(line) + 1)
    spans = []
    pos = 0
    for tok in tokenize.generate_tokens(io.StringIO(text).readline):
        if tok.type not in (tokenize.COMMENT, tokenize.STRING) and not (
                hasattr(tokenize, "FSTRING_MIDDLE") and tok.type == tokenize.FSTRING_MIDDLE):
            continue
        s = offsets[tok.start[0] - 1] + tok.start[1]
        e = offsets[tok.end[0] - 1] + tok.end[1]
        if s > pos:
            spans.append(("code", pos, s))
        spans.append(("comment" if tok.type == tokenize.COMMENT else "literal", s, e))
        pos = e
    if pos < len(text):
        spans.append(("code", pos, len(text)))
    return spans


def spans_of(rel, text):
    k = kind_of(rel)
    if k == "md" or k == "data":
        return [("prose" if k == "md" else "data", 0, len(text))]
    if k == "py":
        return python_spans(text)
    return [(kind, s, e) for kind, s, e in lex.spans_of(rel, text)]


def occurrences(rel, text):
    """[(line, col, where, class, word, context)] for every word of the family in one file."""
    out = []
    for where, s, e in spans_of(rel, text):
        for m in words.WORD.finditer(text, s, e):
            if where == "code":
                cls = "code"
            else:
                cls = words.classify(text, m)
            line = text.count("\n", 0, m.start()) + 1
            col = m.start() - (text.rfind("\n", 0, m.start()) + 1)
            ctx = text[max(0, m.start() - 40):m.end() + 40].replace("\n", " ").replace("\t", " ")
            out.append((line, col, where, cls, m.group(0), ctx))
    return out


def identifiers(rel, text):
    """{identifier: count} for code tokens spelling the family (case-insensitive)."""
    found = collections.Counter()
    for where, s, e in spans_of(rel, text):
        if where != "code":
            continue
        for m in ID_FAMILY.finditer(text, s, e):
            if "maker" in m.group(0).lower():
                found[m.group(0)] += 1
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".")
    ap.add_argument("--start", required=True)
    ap.add_argument("--list")
    a = ap.parse_args()
    by_class = collections.Counter()
    by_where = collections.Counter()
    files_with = set()
    ids = collections.Counter()
    id_files = collections.defaultdict(set)
    rows = []
    for rel in files_at(a.repo, a.start):
        text = text_at(a.repo, a.start, rel)
        if text is None or "maker" not in text.lower():
            continue
        for line, col, where, cls, word, ctx in occurrences(rel, text):
            by_class[cls] += 1
            by_where[(where, cls)] += 1
            files_with.add(rel)
            rows.append((rel, line, col, where, cls, word, ctx))
        for ident, n in identifiers(rel, text).items():
            ids[ident] += n
            id_files[ident].add(rel)
    total = sum(by_class.values())
    print(f"words of the family: {total} in {len(files_with)} files")
    for cls in ("person", "library", "token", "code"):
        print(f"  {cls:8} {by_class[cls]}")
    for (where, cls), n in sorted(by_where.items()):
        print(f"    {where:8} {cls:8} {n}")
    print(f"identifiers in code spelling the family: {len(ids)}, "
          f"{sum(ids.values())} uses in {len(set().union(*id_files.values())) if ids else 0} files")
    for ident, n in sorted(ids.items(), key=lambda x: (-x[1], x[0])):
        print(f"  {n:5} {ident}  ({len(id_files[ident])} files)")
    if a.list:
        with open(a.list, "w", encoding="utf-8", newline="\n") as f:
            for r in rows:
                f.write("\t".join(str(x) for x in r) + "\n")


if __name__ == "__main__":
    main()
