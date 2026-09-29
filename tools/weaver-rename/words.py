# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The rename's grammar, shared by the census, the applier and the proof: where a text says
# `maker`, whether it names the person (the weaver) or the maker library, and what the weaver
# form of the word is. A word names the library when it is part of a path or an identifier
# (`maker/`, `zengine::maker`, `zengine-maker`, `test_maker`), when it is quoted as code
# (`` `maker` ``, the suite), or when it modifies one of the library's nouns (a maker weave, the
# maker path, a maker definition). Every other word names the person. The sheet (sheet.tsv)
# overrides a single occurrence either way.

import re

# A word of the family, and the forms it takes: maker, makers, maker's, makers', in lower case,
# title case or capitals. The apostrophe may be typographic.
WORD = re.compile(r"(?<![A-Za-z0-9_])(maker|Maker|MAKER)(s|S)?(?:(['’])(s|S)?)?(?![A-Za-z0-9_])")

# A word written against one of these characters is part of a path, an identifier or a wire
# name, never a prose word: `maker/`, `zengine.maker`, `maker::Weave`, `test_maker`. A colon
# joins only as `::`; a single one ends a clause ("waiting on the maker: the rig").
JOINED_BEFORE = set("./\\_$@<")
JOINED_AFTER = set("/\\_(<")

# The nouns a word of the family modifies when it names the library: a maker weave is a weave
# the library builds; the maker path is the library's path from a definition to a weave.
LIBRARY_NOUNS = {
    "weave", "weaves", "path", "package", "library", "definition", "definitions", "file", "files",
    "schema", "schemas", "law", "suite", "registration", "registrations", "admission",
    "artifact", "artifacts", "participant", "participants", "composition", "runtime",
    "interpreter", "shape", "shapes", "field", "graph", "register", "registers", "tests",
    "fixture", "format", "header", "headers", "target", "targets", "subject", "subjects",
}

# The hyphenated compounds that name the person: a maker-made pane is a pane a weaver made.
PERSON_COMPOUNDS = {
    "made", "facing", "visible", "configuration", "supplied", "controlled", "namespace", "input",
    "pane",
}


def weaver_form(word):
    """The weaver form of one word of the family, its case and ending kept."""
    m = WORD.fullmatch(word)
    if not m:
        raise ValueError(word)
    stem = {"maker": "weaver", "Maker": "Weaver", "MAKER": "WEAVER"}[m.group(1)]
    return stem + word[len(m.group(1)):]


# Spaces, and at most one wrap: the next line's comment leader, or the quotes that join two
# literal pieces ("... maker " / "weave ..."). A blank line ends the phrase.
_NEXT = re.compile(r'[ \t]*(?:"?[ \t]*\n[ \t]*(?:///?|#|\*|")?[ \t]*)?([A-Za-z]+)')


def _next_word(text, end):
    """The word after `end`, across spaces and one wrap; None when something else comes first."""
    m = _NEXT.match(text, end)
    if not m or m.end(1) - len(m.group(1)) == end:
        return None
    return m.group(1)


def classify(text, m):
    """'person', 'library' or 'token' for one WORD match in `text`. A token is part of a path, an
    identifier, a wire name or a link target: the prose rule does not touch it."""
    start, end = m.start(), m.end()
    before = text[start - 1] if start > 0 else ""
    after = text[end] if end < len(text) else ""
    after2 = text[end + 1] if end + 1 < len(text) else ""
    if before in JOINED_BEFORE or after in JOINED_AFTER:
        return "token"
    if text[max(0, start - 2):start] == "::" or text[end:end + 2] == "::":
        return "token"
    if re.search(r"namespace\s+$", text[max(0, start - 20):start]):
        return "token"          # namespace maker = zengine::maker
    if after == "." and after2.isalnum():
        return "token"          # maker.md, maker.Definition
    # a link target or an anchor: [text](path#slug-maker-slug)
    line_start = text.rfind("\n", 0, start) + 1
    head = text[line_start:start]
    if re.search(r"\]\([^)\s]*$", head) or re.search(r"#[A-Za-z0-9_-]*$", head):
        return "token"
    if before == "-" or (after == "-" and after2.isalpha()):
        # a hyphenated compound: the person's when it is prose (maker-made), else a name
        if before == "-":
            return "token"
        tail = re.match(r"-([A-Za-z]+)", text[end:]).group(1)
        rest = text[end + 1 + len(tail):end + 2 + len(tail)]
        if tail.lower() in PERSON_COMPOUNDS and rest not in (".", "/", "-", "_"):
            return "person"
        return "token"
    if before == "`" and after == "`":
        return "token"          # `maker`, the suite or the package, quoted as code
    if m.group(2) is None and m.group(3) is None:
        nxt = _next_word(text, end)
        if nxt and nxt.lower() in LIBRARY_NOUNS and (nxt.islower() or nxt.isupper() == m.group(1).isupper()):
            return "library"
    return "person"
