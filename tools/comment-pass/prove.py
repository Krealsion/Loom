# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The comment pass's proof: that a change from a START commit to the working tree changed
# nothing but comments, documents and the pass's own instruments. It regenerates nothing.
#
#   python tools/comment-pass/prove.py --start <commit>                 the proof
#   python tools/comment-pass/prove.py --start <commit> --demo [FILE..] ...and show what it catches
#
# For every C/C++ and CMake file and the two population manifests, START's and the working tree's,
# it strips comments, collapses whitespace, drops blank lines, and asks whether the two are equal
# line for line and carry the same literals. A manifest is also read as its check reads it,
# through CMake, since an open bracket or a non-ASCII byte in a comment changes that reading.
#
# What may differ, each printed: the instruments, the checks' own files (INSTRUMENTS); a directory
# left to Git whole (LEFT_TO_GIT); the checks' registration lines in tests/CMakeLists.txt and their
# rows in tests/entry_population.txt, when START lacks them; a failure message or exemption
# reason named below by its START literal, reworded to show no private id or stage name; a CI
# workflow's whole-line `#` comments. Every other changed file must be markdown or this
# directory's. Every law pointer (`// MSG-09; docs/laws/messaging-laws.md`, after `//`, `///` or
# `//!`) must stand where it stood: the same file, above the same code, and once. A pointer written
# over two lines is not one it sees.

import argparse
import collections
import difflib
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import grammar as grammar_mod  # noqa: E402
import lex  # noqa: E402

TOOLS = "tools/comment-pass/"
INSTRUMENTS = ("tests/check_source_comments.cmake", "tests/check_doc_standard.cmake",
               "tests/private_ids.cmake", "tests/check_doc_links.cmake")
# Directories the pass leaves to Git: removed whole, every file in them, and named.
LEFT_TO_GIT = ("docs/audits/",)
REGISTRATION_FILE = "tests/CMakeLists.txt"
REGISTRATION = (
    "add_test(NAME source_comments COMMAND ${CMAKE_COMMAND}",
    "-DZEN_REPO=${CMAKE_SOURCE_DIR}",
    "-P ${CMAKE_CURRENT_SOURCE_DIR}/check_source_comments.cmake)",
    "add_test(NAME doc_standard COMMAND ${CMAKE_COMMAND}",
    "-DZEN_REPO=${CMAKE_SOURCE_DIR}",
    "-P ${CMAKE_CURRENT_SOURCE_DIR}/check_doc_standard.cmake)",
)
# Each manifest, the check whose reading is restated below, and the rows the pass may add.
MANIFESTS = {
    "tests/suite_population.txt": ("tests/check_population.cmake", ()),
    "tests/entry_population.txt": ("tests/check_entry_population.cmake",
                                   ("source_comments portable", "doc_standard portable")),
}
# The failure messages, runtime messages and exemption reason that showed a reader a private id
# or a plan's stage name, named by their START literals. Each may be reworded, its meaning
# unchanged, and the rewording may show no id.
REWORDED = {
    "tests/CMakeLists.txt": (
        '"the F-22 negative control: same source as zen_test_contract_applied, contract "',),
    "tests/check_commit_attribution.cmake": (
        '"Self-test: the wave HIST-1 removed\\n\\nCo-Authored-By: Claude Opus 5 <noreply@anthropic.com>"',
        '"exact trailer HIST-1 removed from 14 commits. It would have reported a clean "',
        '"amend if it is the tip, otherwise rewrite the affected messages as HIST-1 did "'),
    "tests/check_weave_contract.cmake": (
        '"symbols in total). The negative control has stopped reproducing F-22, so "',),
    "tests/verify.cmake": (
        '"A lane that proceeded without it would be the lane VOLATILE-2a closed: one that "',),
    "include/zen/ui/tree.hpp": (
        '"of Stage 3: intent and relationship, never coordinates)."',),
    "src/console/console.cpp": (
        '" — Stage 1 compose sets only scalar fields (the gate backstops a required one)"',
        '"\' is not supported in Stage 2"'),
    "src/console/console_term.cpp": (
        '"zen console (stage 2). commands: weaves | describe <Shape> <v> | "',),
    "src/switchboard/switchboard.cpp": (
        '"\' is already held (roles are singletons in this phase)"',),
}
WORKFLOWS = ".github/workflows/"
# The manifests' reading in their checks, restated; the line it keys on must still be in each
# check, or this restatement is stale and the proof says so.
MANIFEST_KEY_LINE = 'string(REGEX REPLACE "#.*$" "" line "${line}")'
READ_AS_POPULATION = r'''file(STRINGS "${MANIFEST}" manifest_lines)
foreach(line IN LISTS manifest_lines)
    string(REGEX REPLACE "#.*$" "" line "${line}")
    string(REPLACE "\t" " " line "${line}")
    string(STRIP "${line}" line)
    if(NOT line STREQUAL "")
        message(STATUS "ENTRY ${line}")
    endif()
endforeach()
'''
# A law pointer in any line-comment form. The check's own grammar (grammar.POINTER) is the `//`
# form alone, the one its block count skips.
LAW_LINE = re.compile(r"^\s*//[/!]? [A-Z]+-[0-9]{2}(?:\.\.[0-9]{2}|, [A-Z]+-[0-9]{2})*; docs/")
GRAMMAR = []
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]{2,}")


def git_lines(repo, *args):
    out = subprocess.run(["git", "-C", repo] + list(args), capture_output=True, check=True)
    return [p for p in out.stdout.decode("utf-8").splitlines() if p]


def read_start(repo, commit, paths):
    """{path: text} for every path at the commit, through one `git cat-file --batch`."""
    names = "".join("%s:%s\n" % (commit, p) for p in paths).encode("utf-8")
    out = subprocess.run(["git", "-C", repo, "cat-file", "--batch"], input=names,
                         capture_output=True, check=True).stdout
    texts, i = {}, 0
    for p in paths:
        header_end = out.index(b"\n", i)
        size = int(out[i:header_end].split()[2])
        body = out[header_end + 1:header_end + 1 + size]
        texts[p] = body.decode("utf-8", "replace").replace("\r\n", "\n")
        i = header_end + 1 + size + 1
    return texts


def code_form(path, text):
    """(normalized code lines, literals)."""
    spans = lex.spans_of(path, text)
    return lex.normalized_lines(text, spans), lex.literals(text, spans)


def put_back(path, start_text, end_text):
    """(END's text with each named literal put back as START had it, the rewordings as (was, now),
    a refusal or None). Only a literal REWORDED names is put back, once; a literal count that
    differs is left for the comparison to report."""
    names = list(REWORDED.get(path, ()))
    was_all = lex.literals(start_text, lex.spans_of(path, start_text))
    now_spans = [(s, e) for kind, s, e in lex.spans_of(path, end_text) if kind == lex.LITERAL]
    if not names or len(was_all) != len(now_spans):
        return end_text, [], None
    parts, pairs, pos = [], [], 0
    for was, (s, e) in zip(was_all, now_spans):
        now = end_text[s:e]
        if was == now or was not in names:
            continue
        names.remove(was)
        if GRAMMAR[0].private_ids(now):
            return end_text, pairs, "a reworded literal still shows a private id: %s" % now
        parts.append(end_text[pos:s] + was)
        pos = e
        pairs.append((was, now))
    parts.append(end_text[pos:])
    return "".join(parts), pairs, None


def as_population_reads(text):
    """A manifest's rows as its check reads them, through CMake itself: `file(STRINGS)` splits a
    line at a non-ASCII byte and joins lines across an open bracket."""
    with tempfile.TemporaryDirectory() as d:
        manifest, script = os.path.join(d, "manifest.txt"), os.path.join(d, "read.cmake")
        with open(manifest, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        with open(script, "w", encoding="utf-8", newline="\n") as f:
            f.write(READ_AS_POPULATION)
        out = subprocess.run(["cmake", "-DMANIFEST=" + manifest.replace("\\", "/"), "-P", script],
                             capture_output=True, check=True).stdout.decode("utf-8", "replace")
    return [line[len("-- ENTRY "):] for line in out.splitlines() if line.startswith("-- ENTRY ")]


def without_block(lines, block):
    """lines with block, a run of consecutive lines, removed; None unless it is there exactly once."""
    lines, n = list(lines), len(block)
    if not n:
        return lines
    at =[i for i in range(len(lines) - n + 1) if lines[i:i + n] == list(block)]
    if len(at) != 1:
        return None
    return lines[:at[0]] + lines[at[0] + n:]


def compare(path, start_text, end_text, new_lines):
    """None when equal, else the first difference in words. new_lines are the code lines END may
    add to this file, each exactly once."""
    if path in MANIFESTS:
        a_reads = as_population_reads(start_text)
        b_reads = without_block([re.sub(r"\s+", " ", r) for r in as_population_reads(end_text)],
                               new_lines)
        if b_reads is None:
            return "as its check reads it, the added rows are not there exactly once, together"
        a_reads = [re.sub(r"\s+", " ", r) for r in a_reads]
        if a_reads != b_reads:
            for k, (x, y) in enumerate(zip(a_reads, b_reads)):
                if x != y:
                    return "as its check reads it, row %d: %s  ->  %s" % (k + 1, x[:60], y[:60])
            return "as its check reads it, %d rows -> %d" % (len(a_reads), len(b_reads))
        new_lines = [re.sub(r"\s+", " ", r) for r in new_lines]
    a_lines, a_lits = code_form(path, start_text)
    b_lines, b_lits = code_form(path, end_text)
    if new_lines:
        b_lines = without_block(b_lines, new_lines)
        if b_lines is None:
            return "the added lines are not there exactly once, together"
    if a_lits != b_lits:
        for k, (x, y) in enumerate(zip(a_lits, b_lits)):
            if x != y:
                return "literal %d: %s -> %s" % (k, x[:60], y[:60])
        return "literal count %d -> %d" % (len(a_lits), len(b_lits))
    if a_lines != b_lines:
        for k, (x, y) in enumerate(zip(a_lines, b_lines)):
            if x != y:
                return "code line %d: %s  ->  %s" % (k + 1, x[:70], y[:70])
        return "code line count %d -> %d" % (len(a_lines), len(b_lines))
    return None


def workflow_code(text):
    """A workflow's lines with its whole-line `#` comments dropped; a trailing comment stays."""
    return [l for l in text.replace("\r\n", "\n").split("\n") if not l.lstrip().startswith("#")]


def law_lines(path, text):
    """Counter of (path, law pointer, the first code line after it)."""
    if lex.kind_of(path) != "cxx":
        return collections.Counter()
    classes = [c for c, _, _ in lex.line_bytes(text, lex.spans_of(path, text))]
    lines = text.split("\n")
    out = collections.Counter()
    for k, line in enumerate(lines[:len(classes)]):
        if classes[k] == "comment" and LAW_LINE.match(line):
            nxt = next((re.sub(r"\s+", " ", lines[j]).strip() for j in range(k + 1, len(classes))
                        if classes[j] == "code"), "")
            out[(path, re.sub(r"^\s*//[/!]?\s*", "", line).rstrip(), nxt)] += 1
    return out


def law_changes(path, start_text, end_text):
    """(failures, added, made single) for one file's law pointers. A pointer START had and END
    lacks is a failure, and so is one END repeats above the same code."""
    a, b = law_lines(path, start_text), law_lines(path, end_text)
    failures = ["%s: law pointer dropped or moved: %s (above: %s)" % (k[0], k[1][:70], k[2][:50])
                for k in sorted(a) if k not in b]
    failures += ["%s: law pointer repeated %d times above the same code: %s (above: %s)" % (
        k[0], n, k[1][:70], k[2][:50]) for k, n in sorted(b.items()) if n > 1]
    added = sorted(k for k in b if k not in a)
    single = sorted(k for k in b if a[k] > 1 and b[k] == 1)
    return failures, added, single


def new_lines_for(path, start_text):
    """The code lines END may add to a file: the registration and the manifest rows, when START
    lacks them."""
    if path == REGISTRATION_FILE and REGISTRATION[0] not in code_form(path, start_text)[0]:
        return list(REGISTRATION)
    if path in MANIFESTS:
        rows = MANIFESTS[path][1]
        have = [re.sub(r"\s+", " ", r) for r in as_population_reads(start_text)]
        return list(rows) if rows and rows[0] not in have else []
    return []


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".")
    ap.add_argument("--start", required=True)
    ap.add_argument("--demo", nargs="*", metavar="FILE",
                    help="show what the comparison catches, on each FILE "
                         "(default tests/CMakeLists.txt and tests/suite_population.txt)")
    args = ap.parse_args()
    repo = args.repo
    GRAMMAR.append(grammar_mod.Grammar(repo))
    start = {p for p in git_lines(repo, "ls-tree", "-r", "--name-only", args.start)
             if lex.kind_of(p)}
    end = {p for p in git_lines(repo, "ls-files") + git_lines(
        repo, "ls-files", "--others", "--exclude-standard") if lex.kind_of(p)}
    end = {p for p in end if os.path.exists(os.path.join(repo, p))}
    start_text = read_start(repo, args.start, sorted(start))
    failures, set_aside, reworded, added_lines = [], {}, [], {}
    left_to_git = collections.Counter()
    for p in sorted(start - end):
        gone = next((d for d in LEFT_TO_GIT if p.startswith(d)), None)
        if gone and not any(q.startswith(gone) for q in end):
            left_to_git[gone] += 1
        else:
            failures.append("%s: removed" % p)
    for p in sorted(end - start):
        if p not in INSTRUMENTS:
            failures.append("%s: added" % p)
    changed = set(git_lines(repo, "diff", "--name-only", args.start)) | set(
        git_lines(repo, "ls-files", "--others", "--exclude-standard"))
    workflows = []
    for p in sorted(changed):
        if p.startswith(WORKFLOWS) and p.endswith(".yml") and os.path.exists(os.path.join(repo, p)):
            was = workflow_code(read_start(repo, args.start, [p])[p])
            with open(os.path.join(repo, p), encoding="utf-8", newline="") as f:
                now = workflow_code(f.read())
            if was != now:
                first = next((i for i, (a, b) in enumerate(zip(was, now)) if a != b),
                             min(len(was), len(now)))
                failures.append("%s: a line that is not a whole-line comment changed, near its "
                                "line %d of %d without them" % (p, first + 1, len(now)))
            else:
                workflows.append((p, len(now)))
            continue
        if not lex.kind_of(p) and not p.endswith(".md") and not p.startswith(TOOLS):
            failures.append("%s: changed, and it is neither code this proof reads, markdown nor "
                            "this directory's" % p)
    for p, (check, _) in MANIFESTS.items():
        with open(os.path.join(repo, check), encoding="utf-8") as f:
            if MANIFEST_KEY_LINE not in f.read():
                failures.append("%s: its manifest reading changed; restate it here" % check)
    compared = 0
    laws_start, laws_added, laws_single = 0, [], []
    for p in sorted(start & end):
        with open(os.path.join(repo, p), encoding="utf-8", newline="") as f:
            end_text = f.read().replace("\r\n", "\n")
        law_failures, added, single = law_changes(p, start_text[p], end_text)
        failures.extend(law_failures)
        laws_added.extend(added)
        laws_single.extend(single)
        laws_start += len(law_lines(p, start_text[p]))
        if p in INSTRUMENTS:
            a, _ = code_form(p, start_text[p])
            b, _ = code_form(p, end_text)
            set_aside[p] = [d for d in difflib.unified_diff(a, b, lineterm="", n=0)
                            if d[:1] in "+-" and d[:3] not in ("+++", "---")]
            continue
        if p in REWORDED:
            end_text, pairs, refused = put_back(p, start_text[p], end_text)
            reworded.extend((p, was, now) for was, now in pairs)
            if refused:
                failures.append("%s: %s" % (p, refused))
        new = new_lines_for(p, start_text[p])
        if new:
            added_lines[p] = new
        compared += 1
        why = compare(p, start_text[p], end_text, new)
        if why:
            failures.append("%s: %s" % (p, why))
    print("prove: %d C/C++, CMake and manifest files compared, START %s against the working tree; "
          "%d law pointers at START, each still above the same code, once" % (
              compared, args.start, laws_start))
    for p, n in workflows:
        print("prove: %s changed its whole-line comments only: %d other lines, identical" % (p, n))
    for key in laws_added:
        print("prove: law pointer added in %s: %s (above: %s)" % (key[0], key[1][:70], key[2][:50]))
    for key in laws_single:
        print("prove: a repeated law pointer made single in %s: %s (above: %s)" % (
            key[0], key[1][:70], key[2][:50]))
    for d, n in sorted(left_to_git.items()):
        print("prove: set aside by name, left to Git: %s, %d code file(s) removed with it" % (d, n))
    for p in INSTRUMENTS:
        if p in end:
            print("prove: set aside, the instrument %s: %s" % (
                p, "new" if p not in start else "%d code lines differ" % len(set_aside.get(p, []))))
    for p, lines in sorted(added_lines.items()):
        print("prove: set aside by name, %d added line(s) in %s: %s" % (len(lines), p, " | ".join(lines)))
    for p, was, now in reworded:
        print("prove: set aside by name, a literal reworded in %s:\n    was: %s\n    now: %s" % (p, was, now))
    for p in MANIFESTS:
        if p in start:
            print("prove: %s read as its check reads it, through CMake: %d rows at START" % (
                p, len(as_population_reads(start_text[p]))))
    for f in failures:
        print("  DIFFERS  " + f)
    print("prove: %s" % ("identical" if not failures else "%d differences" % len(failures)))
    status = 0 if not failures else 1
    if args.demo is not None:
        for path in args.demo or ["tests/CMakeLists.txt", "tests/suite_population.txt"]:
            status |= demo(repo, start_text, path)
    return status


# What each demo edit must do: be caught (True) or pass as comment-only (False).
DEMO_EDITS = (("one-token change", True), ("literal change", True), ("comment-only change", False),
              ("an open bracket in a manifest comment", True),
              ("a non-ASCII byte in a manifest comment", True),
              ("a law pointer repeated", True), ("a law pointer dropped", True))


def mutations(path, text, start_text=""):
    """Edits to a file, each in its middle: one code token, one literal, one comment; in a
    manifest, the two comment edits only its CMake reading sees; and in a file with a law pointer
    START had, that pointer repeated and dropped."""
    spans = lex.spans_of(path, text)
    out = {}
    if path in MANIFESTS:
        lines = text.split("\n")
        classes = [c for c, _, _ in lex.line_bytes(text, spans)]
        above = [k for k in range(1, len(classes)) if classes[k] == "code" and classes[k - 1] == "comment"]
        if above:
            k = above[len(above) // 2] - 1
            out["an open bracket in a manifest comment"] = "\n".join(
                lines[:k] + [lines[k] + " [ still open"] + lines[k + 1:])
            out["a non-ASCII byte in a manifest comment"] = "\n".join(
                lines[:k] + [lines[k] + " — then more"] + lines[k + 1:])
    if lex.kind_of(path) == "cxx":
        lines = text.split("\n")
        classes = [c for c, _, _ in lex.line_bytes(text, spans)]
        had = {k[1] for k in law_lines(path, start_text)}
        pointers = [k for k in range(len(classes))
                    if classes[k] == "comment" and LAW_LINE.match(lines[k])
                    and re.sub(r"^\s*//[/!]?\s*", "", lines[k]).rstrip() in had]
        if pointers:
            k = pointers[len(pointers) // 2]
            out["a law pointer repeated"] = "\n".join(lines[:k + 1] + [lines[k]] + lines[k + 1:])
            out["a law pointer dropped"] = "\n".join(lines[:k] + lines[k + 1:])
    idents = []
    for kind, s, e in spans:
        if kind != lex.CODE:
            continue
        for m in IDENT.finditer(text, s, e):
            line_start = text.rfind("\n", 0, m.start()) + 1
            if not text[line_start:m.start()].lstrip().startswith("#"):
                idents.append(m)
    if idents:
        m = idents[len(idents) // 2]
        out["one-token change"] = text[:m.end()] + "z" + text[m.end():]
    lits = [(s, e) for kind, s, e in spans if kind == lex.LITERAL and text[s] == '"' and e - s >= 4]
    if lits:
        s, e = lits[len(lits) // 2]
        c = "x" if text[s + 1] != "x" else "y"
        out["literal change"] = text[:s + 1] + c + text[s + 2:]
    coms = [(s, e) for kind, s, e in spans if kind == lex.COMMENT and re.search(r"[A-Za-z]", text[s + 1:e])]
    if coms:
        s, e = coms[len(coms) // 2]
        k = s + 1 + re.search(r"[A-Za-z]", text[s + 1:e]).start()
        c = "q" if text[k] != "q" else "j"
        out["comment-only change"] = text[:k] + c + text[k + 1:]
    return out


def demo(repo, start_text, path):
    """Edits to one END file, in memory: a code token and a literal must be caught, a comment-only
    edit must not be, and in a manifest so must the two comment edits that change what its check
    reads. A reworded file's named literals are put back first, as the proof puts them back."""
    if path not in start_text:
        print("demo: %s is not a START file" % path)
        return 1
    with open(os.path.join(repo, path), encoding="utf-8") as f:
        end_text = f.read()
    if path in REWORDED:
        end_text, pairs, refused = put_back(path, start_text[path], end_text)
        if refused:
            print("demo: %s: %s" % (path, refused))
            return 1
        print("demo: %d named literal(s) put back in %s first" % (len(pairs), path))
    new = new_lines_for(path, start_text[path])
    edits = mutations(path, end_text, start_text[path])
    ok = tried = 0
    for what, want in DEMO_EDITS:
        if what not in edits:
            if "manifest" not in what or path in MANIFESTS:
                print("demo: %s: %s has nothing to change" % (what, path))
            continue
        tried += 1
        why = compare(path, start_text[path], edits[what], new) or next(
            iter(law_changes(path, start_text[path], edits[what])[0]), None)
        caught = why is not None
        print("demo: %s in %s: %s%s" % (what, path, "caught" if caught else "not a difference",
                                       (" -- " + why) if why else ""))
        ok += caught == want
    print("demo: %d of %d as expected in %s" % (ok, tried, path))
    return 0 if ok == tried and tried >= 3 else 1


if __name__ == "__main__":
    sys.exit(main())
