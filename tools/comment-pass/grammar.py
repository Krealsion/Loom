# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The checks' rules, read from the checks: what a private id is (tests/private_ids.cmake and the
# laws docs/laws/ declares), what a law pointer is, and which headers are installed (the root
# CMakeLists.txt's install call). The census, blocks.py and the proof share them.

import os
import re

TOKEN = re.compile(r"(?<![A-Za-z0-9_])[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)+[a-z]?(?![A-Za-z0-9_])")
NUMBERED = re.compile(r"[0-9]")
POINTER = re.compile(r"^\s*// [A-Z]+-[0-9]{2}(?:\.\.[0-9]{2}|, [A-Z]+-[0-9]{2})*; docs/")
UNCOUNTED = re.compile(r"^\s*(?://|#) *(?:SPDX-License-Identifier:|Copyright \(c\))")
REMOVAL = re.compile(r"(?:was|were) here|used to be here|what used to be", re.I)


class Grammar:
    def __init__(self, repo):
        with open(os.path.join(repo, "tests/private_ids.cmake"), encoding="utf-8") as f:
            self.not_ids = set(re.search(r"set\(ZEN_NOT_IDS ([^)]*)\)", f.read()).group(1).split())
        self.laws = set()
        for name in os.listdir(os.path.join(repo, "docs/laws")):
            if name.endswith(".md"):
                with open(os.path.join(repo, "docs/laws", name), encoding="utf-8") as f:
                    self.laws.update(re.findall(r"^## ([A-Z]+-[0-9]+) ", f.read(), re.M))
        with open(os.path.join(repo, "tests/check_source_comments.cmake"), encoding="utf-8") as f:
            self.limit = int(re.search(r"set\(ZEN_COMMENT_BLOCK_LIMIT (\d+)\)", f.read()).group(1))
        with open(os.path.join(repo, "CMakeLists.txt"), encoding="utf-8") as f:
            call = re.search(r"install\(DIRECTORY include/zen/([^)]*)\)", f.read()).group(1)
        self.install_excludes = set(re.findall(r'PATTERN "([^"]*)" +EXCLUDE', call))

    def private_ids(self, text):
        return [t for t in TOKEN.findall(text)
                if NUMBERED.search(t) and t not in self.not_ids and t not in self.laws]

    def installed(self, rel):
        return re.match(r"^include/zen/.*\.(h|hpp)$", rel) is not None and \
            not set(rel.split("/")) & self.install_excludes

    def counted(self, line):
        """Whether a comment line counts toward a block's length."""
        return not UNCOUNTED.match(line) and not POINTER.match(line)
