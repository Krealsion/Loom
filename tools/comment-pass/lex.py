# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The lexical scan the comment pass shares: a C++ or CMake file, or a population manifest, cut
# into code, comment and literal spans. It is not a parser. It knows where a
# comment starts and ends, and where a string, character or bracket literal starts and ends,
# which is all that the counts, the applier and the proof need.

import re

CODE, COMMENT, LITERAL = "code", "comment", "literal"

CXX_SUFFIXES = (".h", ".hpp", ".ipp", ".c", ".cc", ".cpp", ".cxx", ".inl")
# The population manifests: their rows are data, and the checks that read them strip `#.*$` from
# every line, so a `#` anywhere starts a comment and no quote protects one.
MANIFESTS = ("suite_population.txt", "entry_population.txt")
IDENT_CHAR = re.compile(r"[A-Za-z0-9_]")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
RAW_PREFIX = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\n]{0,16})\(')


def kind_of(path):
    """'cxx', 'cmake', 'manifest' or None, from the file name alone."""
    name = path.rsplit("/", 1)[-1]
    if name.endswith(CXX_SUFFIXES):
        return "cxx"
    if name == "CMakeLists.txt" or name.endswith(".cmake") or name.endswith(".cmake.in"):
        return "cmake"
    if name in MANIFESTS:
        return "manifest"
    return None


def _starts_identifier(text, i):
    return i > 0 and IDENT_CHAR.match(text[i - 1]) is not None


def _pp_number_before(text, i):
    """True when the `'` at i continues a numeric literal (a digit separator, 1'000)."""
    j = i
    while j > 0 and (IDENT_CHAR.match(text[j - 1]) or text[j - 1] in "'."):
        j -= 1
    return j < i and text[j].isdigit() and i + 1 < len(text) and IDENT_CHAR.match(text[i + 1])


def cxx_spans(text):
    """[(kind, start, end)] covering text: comments (`//` to its line end, a trailing
    backslash continuing it; `/* */`), literals (strings, raw strings, characters) and the
    code between them. A literal span starts at its opening quote; an encoding prefix
    (`u8`, `L`, ...) stays in the code before it, except for a raw string, whose prefix
    is part of the literal."""
    spans = []
    n = len(text)
    i = 0
    code_start = 0

    def flush(upto):
        if upto > code_start:
            spans.append((CODE, code_start, upto))

    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = i
            while True:
                k = text.find("\n", j)
                if k == -1:
                    k = n
                    break
                line = text[j:k].rstrip("\r")
                if line.endswith("\\"):
                    j = k + 1
                    continue
                break
            flush(i)
            spans.append((COMMENT, i, k))
            i = code_start = k
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            k = text.find("*/", i + 2)
            k = n if k == -1 else k + 2
            flush(i)
            spans.append((COMMENT, i, k))
            i = code_start = k
            continue
        if c == '"':
            # a raw string: R"d( ... )d", its prefix one token with it
            m = None
            for back in (1, 2, 3):
                s = i - back
                if s < 0:
                    continue
                mm = RAW_PREFIX.match(text, s)
                if mm and mm.start() == s and text.find('"', s) == i and not _starts_identifier(text, s):
                    m = mm
                    break
            if m:
                delim = m.group(1)
                close = text.find(")" + delim + '"', m.end())
                k = n if close == -1 else close + len(delim) + 2
                flush(m.start())
                spans.append((LITERAL, m.start(), k))
                i = code_start = k
                continue
        if c == '"' or (c == "'" and not _pp_number_before(text, i)):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                if text[j] == "\\":
                    j += 1
                j += 1
            k = min(j + 1, n)
            flush(i)
            spans.append((LITERAL, i, k))
            i = code_start = k
            continue
        i += 1
    flush(n)
    return spans


def _cmake_bracket_at(text, i):
    """Length of a bracket opener `[==[` at i, else 0."""
    m = re.match(r"\[(=*)\[", text[i:i + 64])
    return m.end() if m else 0


def cmake_spans(text):
    """[(kind, start, end)] for a CMake file: `#` comments and `#[[ ]]` bracket comments,
    quoted arguments and bracket arguments as literals, the rest code."""
    spans = []
    n = len(text)
    i = 0
    code_start = 0

    def flush(upto):
        if upto > code_start:
            spans.append((CODE, code_start, upto))

    while i < n:
        c = text[i]
        if c == "#":
            b = _cmake_bracket_at(text, i + 1)
            if b:
                eq = b - 2
                close = text.find("]" + "=" * eq + "]", i + 1 + b)
                k = n if close == -1 else close + eq + 2
            else:
                k = text.find("\n", i)
                k = n if k == -1 else k
            flush(i)
            spans.append((COMMENT, i, k))
            i = code_start = k
            continue
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    j += 1
                j += 1
            k = min(j + 1, n)
            flush(i)
            spans.append((LITERAL, i, k))
            i = code_start = k
            continue
        if c == "[" and (i == 0 or text[i - 1] in " \t\n("):
            b = _cmake_bracket_at(text, i)
            if b:
                eq = b - 2
                close = text.find("]" + "=" * eq + "]", i + b)
                k = n if close == -1 else close + eq + 2
                flush(i)
                spans.append((LITERAL, i, k))
                i = code_start = k
                continue
        if c == "\\" and i + 1 < n:
            i += 2
            continue
        i += 1
    flush(n)
    return spans


def manifest_spans(text):
    """[(kind, start, end)] for the manifest: each `#` to its line end a comment, the rest code
    (the manifest's data). It has no literals."""
    spans = []
    n = len(text)
    code_start = 0
    while True:
        k = text.find("#", code_start)
        if k == -1:
            break
        e = text.find("\n", k)
        e = n if e == -1 else e
        if k > code_start:
            spans.append((CODE, code_start, k))
        spans.append((COMMENT, k, e))
        code_start = e
    if n > code_start:
        spans.append((CODE, code_start, n))
    return spans


def spans_of(path, text):
    k = kind_of(path)
    if k == "cxx":
        return cxx_spans(text)
    if k == "cmake":
        return cmake_spans(text)
    if k == "manifest":
        return manifest_spans(text)
    raise ValueError("not a C++, CMake or manifest file: " + path)


def without_comments(text, spans):
    """The text with every comment replaced by one space (a comment is whitespace to the
    compiler), newlines inside a block comment kept so line numbers survive."""
    out = []
    for kind, s, e in spans:
        if kind == COMMENT:
            out.append(" " + "\n" * text.count("\n", s, e))
        else:
            out.append(text[s:e])
    return "".join(out)


def normalized_lines(text, spans):
    """The proof's form of a file: comments removed, each line's whitespace runs collapsed
    to one space and trimmed, blank lines dropped. What survives is the code and its
    literals, line by line."""
    lines = []
    for line in without_comments(text, spans).split("\n"):
        line = re.sub(r"[ \t\r\f\v]+", " ", line).strip()
        if line:
            lines.append(line)
    return lines


def literals(text, spans):
    return [text[s:e] for kind, s, e in spans if kind == LITERAL]


def comments(text, spans):
    return [(s, e, text[s:e]) for kind, s, e in spans if kind == COMMENT]


def comment_mask(text, spans):
    """One flag per character of text: True inside a comment."""
    mask = [False] * len(text)
    for kind, s, e in spans:
        if kind == COMMENT:
            for k in range(s, e):
                mask[k] = True
    return mask


def line_bytes(text, spans):
    """Per line: (class, comment bytes, code bytes), UTF-8 bytes. The class is 'blank',
    'comment' (only comment and whitespace) or 'code'. A comment line's bytes are all
    comment, its indentation and line end included; a code line's comment bytes are its
    trailing or inline comments and the whitespace that leads into each."""
    mask = comment_mask(text, spans)
    out = []
    pos = 0
    lines = text.split("\n")
    if text.endswith("\n"):
        lines.pop()
    for line in lines:
        size = len(line.encode("utf-8")) + 1
        has_code = has_comment = False
        for k, ch in enumerate(line):
            if mask[pos + k]:
                has_comment = True
            elif not ch.isspace():
                has_code = True
        if not has_code:
            out.append(("comment", size, 0) if has_comment else ("blank", 0, 0))
        else:
            com = 0
            k = 0
            while k < len(line):
                if mask[pos + k]:
                    j = k
                    while j < len(line) and mask[pos + j]:
                        j += 1
                    lead = k
                    while lead > 0 and line[lead - 1] in " \t" and not mask[pos + lead - 1]:
                        lead -= 1
                    com += len(line[lead:j].encode("utf-8"))
                    k = j
                else:
                    k += 1
            out.append(("code", com, size - com))
        pos += len(line) + 1
    return out


