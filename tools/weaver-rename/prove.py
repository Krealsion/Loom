# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The rename's proof: the third of sheet -> applier -> proof (AGENTS.md rule o). It regenerates
# nothing. It compares the start commit with the working tree, file by file, and passes only when
# nothing changed but comments, documents, messages and the names names.tsv lists:
#
#   - a Markdown document may change freely, outside docs/history/;
#   - a C++, CMake or manifest file, with its comments removed, keeps its tokens: an identifier
#     changes only as a names.tsv row for that file says, and a literal only as a message may --
#     each word of the `maker` family become its weaver form or a sheet replacement, or a whole
#     literal messages.tsv lists;
#   - a Python file keeps its tokens the same way, a docstring counted as a comment;
#   - a JSON file keeps its keys and structure, its strings changing only as a message may;
#   - any other file keeps every line but its whole-line `#` comments, and changes those lines
#     only as a message may;
#   - no file is added but this directory's, and none is removed or renamed;
#   - no literal that is a wire name changes, and names.tsv renames none of the names that
#     travel (FROZEN below).
#
# It then counts what is left: every word of the family in the working tree that the grammar
# would still call the person's, which is a sheet `keep` or a miss.
#
#   python tools/weaver-rename/prove.py --start <commit>                the proof
#   python tools/weaver-rename/prove.py --start <commit> --repo ../Loom --names <file> --sheet <file>

import argparse
import io
import json
import os
import re
import subprocess
import sys
import tokenize

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import apply  # noqa: E402
import census  # noqa: E402
import words  # noqa: E402

MESSAGES = os.path.join(HERE, "messages.tsv")
OWN_DIR = "tools/weaver-rename/"
# The names that travel: a wire shape, a persisted identity, a permanent id. Renaming one waits
# for the founder, so a names.tsv row naming one is a red.
FROZEN = {"MakerPaneRequested", "MakerPaneAnswered", "kMakerPaneProvider"}
# A literal that is a wire or persisted name: a dotted schema or provider name, or a format tag.
WIRE = re.compile(r'^"(?:zengine|zen|loom)[.-][A-Za-z0-9_.-]*"$')
TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*|\d[\w.']*|\S")


def git(repo, *args):
    return subprocess.run(["git", "-C", repo, *args], capture_output=True, check=True).stdout


def changed(repo, start):
    """[(status, path)] from the start commit to the working tree, untracked files included."""
    out = git(repo, "diff", "--name-status", "--no-renames", start, "--").decode("utf-8")
    rows = [tuple(l.split("\t", 1)) for l in out.split("\n") if l]
    extra = git(repo, "ls-files", "--others", "--exclude-standard").decode("utf-8").split("\n")
    rows += [("A", p) for p in extra if p]
    return rows


def read_messages(path):
    """{(file, old literal): new literal}, for the messages reworded beyond a word."""
    out = {}
    if not os.path.exists(path):
        return out
    for _, (rel, old, new, _why) in apply.read_rows(path, 4):
        out[(rel, old)] = new
    return out


def replacements(sheet):
    return sorted({d[1:] for _, d in sheet.values() if d.startswith("=")}, key=len, reverse=True)


def message_equal(old, new, extra):
    """Does `new` differ from `old` only by words of the family becoming their weaver forms or a
    sheet replacement?"""
    if old == new:
        return True
    parts = []
    pos = 0
    for m in words.WORD.finditer(old):
        parts.append(re.escape(old[pos:m.start()]))
        alts = [m.group(0), words.weaver_form(m.group(0))] + extra
        parts.append("(?:" + "|".join(re.escape(a) for a in alts) + ")")
        pos = m.end()
    parts.append(re.escape(old[pos:]))
    return re.fullmatch("".join(parts), new, re.S) is not None


def code_tokens(rel, text):
    """[(kind, text)]: a literal whole, code split into tokens, comments dropped."""
    out = []
    for kind, s, e in census.spans_of(rel, text):
        if kind == "literal":
            out.append(("lit", text[s:e]))
        elif kind == "code":
            out.extend(("code", t) for t in TOKEN.findall(text[s:e]))
    return out


def python_tokens(text):
    """[(kind, text)] for a Python file, comments and docstrings dropped."""
    out = []
    prev = tokenize.NEWLINE
    toks = list(tokenize.generate_tokens(io.StringIO(text).readline))
    for i, tok in enumerate(toks):
        if tok.type in (tokenize.COMMENT, tokenize.NL, tokenize.ENCODING):
            continue
        if tok.type == tokenize.STRING:
            nxt = toks[i + 1].type if i + 1 < len(toks) else tokenize.ENDMARKER
            if prev in (tokenize.NEWLINE, tokenize.INDENT, tokenize.DEDENT) and \
                    nxt in (tokenize.NEWLINE, tokenize.ENDMARKER):
                prev = tok.type
                continue  # a docstring: the file's comment, free to change
            out.append(("lit", tok.string))
        elif tok.type not in (tokenize.NEWLINE, tokenize.INDENT, tokenize.DEDENT):
            out.append(("code", tok.string))
        prev = tok.type
    return out


def compare_tokens(rel, old, new, table, messages, extra, problems, notes):
    if len(old) != len(new):
        problems.append(f"{rel}: {len(old)} tokens became {len(new)}")
        return
    for k, ((ka, a), (kb, b)) in enumerate(zip(old, new)):
        if ka != kb:
            problems.append(f"{rel}: token {k} changed kind: {a!r} -> {b!r}")
            continue
        if a == b:
            continue
        if ka == "code":
            if table.get(a) != b:
                problems.append(f"{rel}: code `{a}` became `{b}`, which names.tsv does not list")
                continue
            plain = re.fullmatch(r"[a-z]+", a) is not None
            text_at = lambda j: old[j][1] if 0 <= j < len(old) else ""
            scoped = (text_at(k - 1), text_at(k - 2)) == (":", ":") or \
                (text_at(k + 1), text_at(k + 2)) == (":", ":")
            if plain and (scoped or text_at(k - 1) == "namespace"):
                problems.append(f"{rel}: `{a}` beside `::` or `namespace` is the library's")
            notes["names"] += 1
        else:
            if WIRE.match(a):
                problems.append(f"{rel}: the wire name {a} changed to {b}")
            elif messages.get((rel, a)) == b:
                notes["reworded"].append((rel, a, b))
            elif message_equal(a, b, extra):
                notes["messages"].append((rel, a, b))
            else:
                problems.append(f"{rel}: the literal {a} became {b}, not by a word of the family "
                                f"and not listed in messages.tsv")


def json_walk(rel, a, b, messages, extra, problems, notes, path="$"):
    if type(a) is not type(b):
        problems.append(f"{rel}: {path} changed type")
    elif isinstance(a, dict):
        if list(a) != list(b):
            problems.append(f"{rel}: {path} changed its keys")
        else:
            for k in a:
                json_walk(rel, a[k], b[k], messages, extra, problems, notes, f"{path}.{k}")
    elif isinstance(a, list):
        if len(a) != len(b):
            problems.append(f"{rel}: {path} changed its length")
        else:
            for i, (x, y) in enumerate(zip(a, b)):
                json_walk(rel, x, y, messages, extra, problems, notes, f"{path}[{i}]")
    elif isinstance(a, str):
        if a != b:
            if message_equal(a, b, extra):
                notes["messages"].append((rel, a, b))
            else:
                problems.append(f"{rel}: {path} changed beyond a word of the family")
    elif a != b:
        problems.append(f"{rel}: {path} changed")


def other_lines(rel, old, new, extra, problems, notes):
    strip = lambda t: [l for l in t.split("\n") if not l.lstrip().startswith("#")]
    a, b = strip(old), strip(new)
    if len(a) != len(b):
        problems.append(f"{rel}: its lines outside whole-line comments changed in number")
        return
    for x, y in zip(a, b):
        if x != y:
            if message_equal(x, y, extra):
                notes["messages"].append((rel, x.strip(), y.strip()))
            else:
                problems.append(f"{rel}: a line changed: {x.strip()[:80]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".")
    ap.add_argument("--start", required=True)
    ap.add_argument("--names", default=apply.NAMES)
    ap.add_argument("--sheet", default=apply.SHEET)
    ap.add_argument("--messages", default=MESSAGES)
    a = ap.parse_args()
    names = apply.read_names(a.names)
    sheet = apply.read_sheet(a.sheet)
    messages = read_messages(a.messages)
    extra = replacements(sheet)
    problems = []
    notes = {"names": 0, "messages": [], "reworded": [], "docs": 0, "comments_only": 0}
    for _, old, _new in names:
        if old in FROZEN or old.split("::")[-1] in FROZEN:
            problems.append(f"names.tsv renames {old}, a name that travels")
    rows = changed(a.repo, a.start)
    for status, rel in rows:
        if rel.startswith(OWN_DIR):
            continue
        if status != "M":
            problems.append(f"{rel}: {status} -- only a modification is a rename's")
            continue
        if rel.startswith("docs/history/"):
            problems.append(f"{rel}: the history is frozen")
            continue
        old = census.text_at(a.repo, a.start, rel)
        with open(os.path.join(a.repo, rel), encoding="utf-8", newline="") as f:
            new = f.read()
        kind = census.kind_of(rel)
        table = apply.names_for(rel, names)
        before = (len(problems), notes["names"], len(notes["messages"]), len(notes["reworded"]))
        if kind == "md":
            notes["docs"] += 1
            continue
        if kind in ("cxx", "cmake", "manifest"):
            compare_tokens(rel, code_tokens(rel, old), code_tokens(rel, new), table, messages,
                           extra, problems, notes)
        elif kind == "py":
            compare_tokens(rel, python_tokens(old), python_tokens(new), table, messages, extra,
                           problems, notes)
        elif rel.endswith(".json"):
            json_walk(rel, json.loads(old), json.loads(new), messages, extra, problems, notes)
        else:
            other_lines(rel, old, new, extra, problems, notes)
        after = (len(problems), notes["names"], len(notes["messages"]), len(notes["reworded"]))
        if before == after:
            notes["comments_only"] += 1
    # What is left: a word the grammar still calls the person's.
    left = []
    for rel in census.files_at(a.repo, "HEAD"):
        path = os.path.join(a.repo, rel)
        if not os.path.exists(path):
            continue
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        except UnicodeDecodeError:
            continue
        if "maker" not in text.lower():
            continue
        for line, col, where, cls, word, ctx in census.occurrences(rel, text):
            if cls == "person":
                left.append(f"{rel}:{line}: {ctx.strip()}")
    print(f"files changed {len(rows)}: documents {notes['docs']}, comments only "
          f"{notes['comments_only']}; identifiers renamed {notes['names']}; literals changed by "
          f"a word {len(notes['messages'])}; literals reworded {len(notes['reworded'])}")
    for rel, x, y in notes["messages"] + notes["reworded"]:
        print(f"  literal {rel}: {x[:70]}  ->  {y[:70]}")
    print(f"words the grammar still calls the person's: {len(left)}")
    for l in left:
        print("  left " + l[:150])
    if problems:
        print(f"PROOF FAILED: {len(problems)} problem(s)")
        for p in problems:
            print("  " + p)
        sys.exit(1)
    print("PROOF PASSED: nothing changed but comments, documents, messages and the listed names")


if __name__ == "__main__":
    main()
