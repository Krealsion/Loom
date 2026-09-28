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
# reason named below by its START literal, reworded to show no private id or stage name; a
# test-data value renamed below, every use together; the one assertion named below; a CI
# workflow's or shell script's whole-line `#` comments; a Python file's comments and docstrings,
# its syntax tree otherwise identical; a TEST_CASE or SUBCASE name the case map (cases.tsv)
# renames, whose old name is put back before the comparison. Every other changed file must be
# markdown or this directory's. Every law pointer (`// MSG-09; docs/laws/messaging-laws.md`, after
# `//`, `///` or `//!`) must stand where it stood: the same file, above the same code, and once. A
# pointer written over two lines is not one it sees. Every map row must have landed: its old name
# once at START, its new name once now, and the old name quoted in no current-facing document.

import argparse
import collections
import difflib
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cases  # noqa: E402
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
# The failure messages, runtime messages and exemption reason that showed a reader a private id,
# a plan's stage name or the maintainers' own setup, named by their START literals. Each may be
# reworded, its meaning unchanged, and the rewording may show no id.
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
    "src/terminal/terminal_main.cpp": (
        '"zen terminal (TERM-0).\\n"',),
    "src/switchboard/switchboard.cpp": (
        '"\' is already held (roles are singletons in this phase)"',),
    "src/bridge/channel.cpp": (
        '"AF_UNIX listen is POSIX-only (the Windows<->WSL crossing uses TCP)"',
        '"AF_UNIX connect is POSIX-only (the Windows<->WSL crossing uses TCP)"'),
    "tests/test_provenance.cpp": (
        '"R2B-1a: a weave\'s Bus must never expose lifecycle minting"',
        '"R2B-1a: Mail must never expose lifecycle minting"',
        '"R2B-1a: Mail must never expose lifecycle minting under another name"',
        '"R2B-1a: the lifecycle mint must not be a reachable static factory"',
        '"R2B-1a: the lifecycle mint must be private to the Switchboard"',
        '"R2B-1a: LifecycleAuthority must not be default-constructible"'),
    "tests/package/stranger_history.cpp": (
        '"stranger history witness (RTH-1a: the two halves, through the package)\\n"',),
    "tests/test_isolation.cpp": (
        '"C-2: ambient descriptors removed at exec while the netns holds"',
        '"C-2a: the child\'s environment is authored, not inherited"'),
    "tests/check_entry_population.cmake": (
        '"# The CTest-entry inventory\'s receipt for one official-lane run (VOLATILE-B1).\\n"',),
    "tests/test_weaver.cpp": (
        '"WEAVER-1: a Weaver is built from a capability and an operator seat"',
        '"WEAVER-1: a Weaver must never be constructible from a Switchboard"',
        '"WEAVER-1: a Weaver must never be constructible from a Switchboard"',
        '"WEAVER-1: the Weaver is an ordinary weave"'),
}
# Test data renamed because it carried a plan code or a private id: in each file, every use of the
# START value is the new one now, in literals and code alike, and the START value is left in no
# file's code. Each pair is put back before the comparison, so nothing else may differ.
RENAMED_VALUES = {
    "tests/CMakeLists.txt": (("zen_no_such_suite_R2FD", "zen_suite_that_does_not_exist"),),
    "tests/weavelib/test_weave.cpp": (
        ("COLD2-ESCAPE-PAYLOAD", "PARKED-DESCRIPTOR-PAYLOAD"),
        ("/tmp/zen_b4_secret.txt", "/tmp/zen_host_secret.txt"),
        ("ZEN_C2A_AMBIENT_SECRET", "ZEN_AMBIENT_SECRET")),
    "tests/test_isolation.cpp": (
        ("/tmp/zen_b4_secret.txt", "/tmp/zen_host_secret.txt"),
        ("/tmp/zen_c2_ambient_file.txt", "/tmp/zen_ambient_file.txt"),
        ("ZEN_C2A_AMBIENT_SECRET", "ZEN_AMBIENT_SECRET"),
        ("/zen-c2a-nonexistent-lib-dir", "/zen-nonexistent-lib-dir"),
        ("/zen-c2a-nonexistent-preload.so", "/zen-nonexistent-preload.so"),
        ('"c2a"', '"envprobe"'),
        ('"c2"', '"fdprobe"')),
    "tests/test_bridge.cpp": (("R2FA.Nothing", "Wire.Nothing"), ("R2FA.Bulk", "Wire.Bulk")),
    "tests/test_serialize.cpp": (("R2FA.", "Wire."),),
    "tests/test_joint.cpp": (("zen-joint-j22.so", "zen-joint-after-refusal.so"),),
}
# The one assertion the pass changes, named by its START and END code, each once in its file: a
# letter whose author was removed is refused SenderLifeEnded (MSG-03), never CapabilityDenied.
ASSERTIONS = {
    "tests/test_manager.cpp": (
        'CHECK(refused_count(tap, "zen.Bequest", RefusalReason::CapabilityDenied) == 0);',
        'CHECK(refused_count(tap, "zen.Bequest", RefusalReason::SenderLifeEnded) == 0);'),
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


def code_mask(path, text):
    """One flag per character of text: True outside a comment."""
    return [not c for c in lex.comment_mask(text, lex.spans_of(path, text))]


def outside_comments(path, text, needle):
    """The offsets at which needle starts outside a comment."""
    mask = code_mask(path, text)
    out, i = [], text.find(needle)
    while i != -1:
        if all(mask[i:i + len(needle)]):
            out.append(i)
        i = text.find(needle, i + 1)
    return out


def put_back_values(path, start_text, end_text):
    """(END's text with each renamed value put back as START had it, the pairs put back, refusals).
    A pair is refused unless START holds the old value outside comments and not the new one, and
    END holds the new one and not the old one."""
    pairs, refusals = [], []
    for old, new in RENAMED_VALUES.get(path, ()):
        at = outside_comments(path, end_text, new)
        if not outside_comments(path, start_text, old) or outside_comments(path, start_text, new):
            refusals.append("the renamed value %r is not START's alone" % old)
        elif outside_comments(path, end_text, old):
            refusals.append("the renamed value %r is still used, so not every use changed" % old)
        elif not at:
            refusals.append("the renamed value %r has no new use %r" % (old, new))
        else:
            for i in reversed(at):
                end_text = end_text[:i] + old + end_text[i + len(new):]
            pairs.append((old, new, len(at)))
    return end_text, pairs, refusals


def put_back_assertion(path, start_text, end_text):
    """(END's text with the named assertion put back, a refusal or None)."""
    if path not in ASSERTIONS:
        return end_text, None
    was, now = ASSERTIONS[path]
    if len(outside_comments(path, start_text, was)) != 1 or outside_comments(path, start_text, now):
        return end_text, "the changed assertion is not once at START: %s" % was
    at = outside_comments(path, end_text, now)
    if len(at) != 1 or outside_comments(path, end_text, was):
        return end_text, "the changed assertion is not once now: %s" % now
    return end_text[:at[0]] + was + end_text[at[0] + len(now):], None


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


def shell_code(text):
    """A shell script's lines with its whole-line `#` comments dropped, the first-line `#!` kept;
    a trailing comment stays, so it may not change."""
    lines = text.replace("\r\n", "\n").split("\n")
    return [l for k, l in enumerate(lines)
            if not l.lstrip().startswith("#") or (k == 0 and l.startswith("#!"))]


def python_code(text):
    """A Python file's syntax tree with every docstring removed, as text: comments never reach
    the tree, and every other string, name and statement does."""
    import ast
    tree = ast.parse(text)
    for node in ast.walk(tree):
        body = getattr(node, "body", None)
        if (isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef))
                and body and isinstance(body[0], ast.Expr)
                and isinstance(body[0].value, ast.Constant) and isinstance(body[0].value.value, str)):
            node.body = body[1:] or [ast.Pass()]
    return ast.dump(tree, include_attributes=False)


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


CASE_FILES = re.compile(r"^tests/.*\.(h|hpp|ipp|inl|c|cc|cpp|cxx)$")
DOC_FROZEN = ("docs/history/", "archive/", "tests/third_party/")


def canonical(path, text, back=None):
    """A test file with each case's name as one literal right after its `(`, however the source
    wraps it; with `back` ({new: old}), a renamed case's old name put back. Returns (text, the
    names put back)."""
    if not CASE_FILES.match(path):
        return text, []
    found = cases.cases_in(path, text)
    put, parts, pos = [], [], 0
    for c in found:
        name = c.name
        if back and name in back:
            name = back[name]
            put.append(c.name)
        opening = text.rfind("(", 0, c.pieces[0][0]) + 1
        parts.append(text[pos:opening] + '"%s"' % name)
        pos = c.pieces[-1][1]
    parts.append(text[pos:])
    return "".join(parts), put


def map_verdicts(repo, rows, start_text, end_text):
    """Failures: every row's old name must be once in its file at START and nowhere now, its new
    name once now, and the old name quoted in no current-facing document."""
    failures = []
    names_at = {}
    for (rel, old), new in sorted(rows.items()):
        if rel not in start_text or rel not in end_text:
            failures.append("%s: the case map names a file the proof does not hold" % rel)
            continue
        if rel not in names_at:
            names_at[rel] = ([c.name for c in cases.cases_in(rel, start_text[rel])],
                             [c.name for c in cases.cases_in(rel, end_text[rel])])
        was, now = names_at[rel]
        if was.count(old) != 1:
            failures.append("%s: the map renames %r, named by %d START cases" % (rel, old[:60], was.count(old)))
        if now.count(new) != 1 or old in now:
            failures.append("%s: the map's rename of %r has not landed once (new name %d times, old "
                            "%d times)" % (rel, old[:60], now.count(new), now.count(old)))
    docs = [p for p in git_lines(repo, "ls-files") if p.endswith(".md") and not p.startswith(DOC_FROZEN)]
    olds = {old for (_, old) in rows}
    for p in docs:
        with open(os.path.join(repo, p), encoding="utf-8") as f:
            text = re.sub(r"\s+", " ", f.read())
        for old in olds:
            if old in text:
                failures.append("%s: quotes the renamed case %r" % (p, old[:60]))
    return failures


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
    workflows, scripts = [], []
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
        if p.endswith((".py", ".sh")) and not p.startswith(TOOLS) and os.path.exists(os.path.join(repo, p)):
            if p not in start_text:
                start_text.update(read_start(repo, args.start, [p]))
            with open(os.path.join(repo, p), encoding="utf-8", newline="") as f:
                now_text = f.read()
            form = python_code if p.endswith(".py") else shell_code
            try:
                same = form(start_text[p]) == form(now_text)
            except SyntaxError as e:
                same = False
                failures.append("%s: does not parse: %s" % (p, e))
            if same:
                scripts.append(p)
            else:
                failures.append("%s: %s" % (p, "its syntax tree changed beyond docstrings"
                                            if p.endswith(".py") else
                                            "a line that is not a whole-line comment changed"))
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
    rows = cases.read_map()
    ends, renamed = {}, collections.Counter()
    renamed_values, assertions = [], []
    for p in sorted(start & end):
        with open(os.path.join(repo, p), encoding="utf-8", newline="") as f:
            end_text = ends[p] = f.read().replace("\r\n", "\n")
        start_p, _ = canonical(p, start_text[p])
        end_text, put = canonical(p, end_text, {new: old for (f, old), new in rows.items() if f == p})
        renamed[p] += len(put)
        law_failures, added, single = law_changes(p, start_p, end_text)
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
        if p in RENAMED_VALUES:
            end_text, pairs, refusals = put_back_values(p, start_p, end_text)
            renamed_values.extend((p, old, new, n) for old, new, n in pairs)
            failures.extend("%s: %s" % (p, r) for r in refusals)
        if p in ASSERTIONS:
            end_text, refused = put_back_assertion(p, start_p, end_text)
            if refused:
                failures.append("%s: %s" % (p, refused))
            else:
                assertions.append((p,) + ASSERTIONS[p])
        if p in REWORDED:
            end_text, pairs, refused = put_back(p, start_p, end_text)
            reworded.extend((p, was, now) for was, now in pairs)
            if refused:
                failures.append("%s: %s" % (p, refused))
        new = new_lines_for(p, start_text[p])
        if new:
            added_lines[p] = new
        compared += 1
        why = compare(p, start_p, end_text, new)
        if why:
            failures.append("%s: %s" % (p, why))
    failures.extend(map_verdicts(repo, rows, start_text, ends))
    olds = {old for pairs in RENAMED_VALUES.values() for old, _ in pairs}
    for p, text in sorted(ends.items()):
        for old in sorted(olds):
            if old in text and outside_comments(p, text, old):
                failures.append("%s: still uses the renamed value %r" % (p, old))
    print("prove: %d C/C++, CMake and manifest files compared, START %s against the working tree; "
          "%d law pointers at START, each still above the same code, once" % (
              compared, args.start, laws_start))
    print("prove: the case map (%d rows) put back %d renamed case names in %d files before comparing" % (
        len(rows), sum(renamed.values()), sum(1 for n in renamed.values() if n)))
    for p, n in workflows:
        print("prove: %s changed its whole-line comments only: %d other lines, identical" % (p, n))
    for p in scripts:
        print("prove: %s changed its %s only" % (
            p, "comments and docstrings" if p.endswith(".py") else "whole-line comments"))
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
    for p, old, new, n in renamed_values:
        print("prove: set aside by name, a test-data value renamed in %s, %d use(s): %s -> %s" % (
            p, n, old, new))
    for p, was, now in assertions:
        print("prove: set aside by name, the one changed assertion, in %s:\n    was: %s\n    now: %s" % (
            p, was, now))
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
            if path.endswith((".py", ".sh")):
                status |= demo_script(repo, args.start, path)
            else:
                status |= demo(repo, start_text, path)
    return status


def demo_script(repo, start, path):
    """Edits to one Python or shell file, in memory: a code token must be caught, and a comment
    edit (and a docstring edit, in Python) must not be."""
    was = read_start(repo, start, [path])[path]
    with open(os.path.join(repo, path), encoding="utf-8", newline="") as f:
        now = f.read().replace("\r\n", "\n")
    form = python_code if path.endswith(".py") else shell_code
    lines = now.split("\n")
    edits = {}
    com = [k for k, l in enumerate(lines) if l.lstrip().startswith("#") and not l.startswith("#!")]
    if com:
        k = com[len(com) // 2]
        edits["comment-only change"] = (lines[:k] + [lines[k] + " q"] + lines[k + 1:], False)
    if path.endswith(".py"):
        import io
        import keyword
        import tokenize
        names = [t.end for t in tokenize.generate_tokens(io.StringIO(now).readline)
                 if t.type == tokenize.NAME and not keyword.iskeyword(t.string)]
        at = [(r - 1, c) for r, c in names]
    else:
        at = [(k, list(re.finditer(r"[A-Za-z_][A-Za-z0-9_]{2,}", l))[-1].end())
              for k, l in enumerate(lines) if l.strip() and not l.lstrip().startswith("#")
              and re.search(r"[A-Za-z_][A-Za-z0-9_]{2,}", l)]
    if at:
        k, c = at[len(at) // 2]
        edits["one-token change"] = (lines[:k] + [lines[k][:c] + "z" + lines[k][c:]]
                                     + lines[k + 1:], True)
    if path.endswith(".py"):
        m = re.search(r'"""(.)', now)
        if m:
            s = m.start(1)
            edits["docstring-only change"] = ((now[:s] + "q" + now[s:]).split("\n"), False)
    ok = 0
    for what, (edited, want) in edits.items():
        try:
            caught = form(was) != form("\n".join(edited))
        except SyntaxError:
            caught = True
        print("demo: %s in %s: %s" % (what, path, "caught" if caught else "not a difference"))
        ok += caught == want
    print("demo: %d of %d as expected in %s" % (ok, len(edits), path))
    return 0 if ok == len(edits) and len(edits) >= 2 else 1


# What each demo edit must do: be caught (True) or pass as comment-only (False).
DEMO_EDITS = (("one-token change", True), ("literal change", True), ("comment-only change", False),
              ("an open bracket in a manifest comment", True),
              ("a non-ASCII byte in a manifest comment", True),
              ("a law pointer repeated", True), ("a law pointer dropped", True),
              ("a case name changed outside the map", True),
              ("the map's renames, read without the map", True))


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
    reads. A reworded file's named literals and a renamed case's old name are put back first, as
    the proof puts them back; in a test file, a case renamed outside the map must be caught, and so
    must the map's own renames when the map is not read."""
    if path not in start_text:
        print("demo: %s is not a START file" % path)
        return 1
    with open(os.path.join(repo, path), encoding="utf-8") as f:
        raw = f.read().replace("\r\n", "\n")
    start_p, _ = canonical(path, start_text[path])
    back = {new: old for (f, old), new in cases.read_map().items() if f == path}
    end_text, put = canonical(path, raw, back)
    if put:
        print("demo: %d renamed case name(s) put back in %s first" % (len(put), path))
    extra = {}
    if path in RENAMED_VALUES:
        old, new = RENAMED_VALUES[path][0]
        i = outside_comments(path, end_text, new)[0]
        extra["a renamed value changed in one use only"] = put_back_values(
            path, start_p, end_text[:i] + old + end_text[i + len(new):])[2]
        end_text, pairs, refusals = put_back_values(path, start_p, end_text)
        if refusals:
            print("demo: %s: %s" % (path, refusals[0]))
            return 1
        print("demo: %d renamed value(s) put back in %s first" % (len(pairs), path))
    if path in ASSERTIONS:
        was, now = ASSERTIONS[path]
        i = outside_comments(path, end_text, now)[0]
        twice = end_text[:i] + now + " " + now + end_text[i + len(now):]
        extra["the changed assertion written twice"] = [put_back_assertion(path, start_p, twice)[1]]
        end_text, refused = put_back_assertion(path, start_p, end_text)
        if refused:
            print("demo: %s: %s" % (path, refused))
            return 1
        print("demo: the changed assertion put back in %s first" % path)
    if path in REWORDED:
        end_text, pairs, refused = put_back(path, start_p, end_text)
        if refused:
            print("demo: %s: %s" % (path, refused))
            return 1
        print("demo: %d named literal(s) put back in %s first" % (len(pairs), path))
    new = new_lines_for(path, start_text[path])
    edits = mutations(path, end_text, start_p)
    found = cases.cases_in(path, end_text) if CASE_FILES.match(path) else []
    if found:
        c = found[len(found) // 2]
        s = c.pieces[0][0] + 1
        edits["a case name changed outside the map"] = end_text[:s] + "z" + end_text[s:]
    if put:
        edits["the map's renames, read without the map"] = canonical(path, raw)[0]
    ok = tried = 0
    for what, want in DEMO_EDITS:
        if what not in edits:
            if ("manifest" not in what or path in MANIFESTS) and "map" not in what:
                print("demo: %s: %s has nothing to change" % (what, path))
            continue
        tried += 1
        why = compare(path, start_p, edits[what], new) or next(
            iter(law_changes(path, start_p, edits[what])[0]), None)
        caught = why is not None
        print("demo: %s in %s: %s%s" % (what, path, "caught" if caught else "not a difference",
                                       (" -- " + why) if why else ""))
        ok += caught == want
    for what, refusals in extra.items():
        tried += 1
        caught = any(refusals)
        print("demo: %s in %s: %s%s" % (what, path, "caught" if caught else "not a difference",
                                       (" -- " + next(r for r in refusals if r)) if caught else ""))
        ok += caught
    print("demo: %d of %d as expected in %s" % (ok, tried, path))
    return 0 if ok == tried and tried >= 3 else 1


if __name__ == "__main__":
    sys.exit(main())
