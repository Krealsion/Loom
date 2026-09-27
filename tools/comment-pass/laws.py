# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# Print the laws a file's comments name -- each declared id's LAW, MEANS and DOES NOT MEAN from
# docs/laws/ -- so a comment can be read against its owner before it is kept, moved or cut.
# Read-only.
#
#   python tools/comment-pass/laws.py src/switchboard/switchboard.cpp [--repo .]

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lex  # noqa: E402

ID = re.compile(r"\b([A-Z]+-[0-9]{2})\b")


def entries(repo):
    """{id: [lines]} for every `## <id>` entry of every register under docs/laws/."""
    out = {}
    folder = os.path.join(repo, "docs/laws")
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".md"):
            continue
        cur = None
        with open(os.path.join(folder, name), encoding="utf-8") as f:
            for line in f:
                m = re.match(r"^## ([A-Z]+-[0-9]+) ", line)
                if m:
                    cur = m.group(1)
                    out[cur] = ["%s  (docs/laws/%s)" % (line[3:].rstrip(), name)]
                elif line.startswith("## "):
                    cur = None
                elif cur:
                    out[cur].append(line.rstrip())
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--repo", default=".")
    args = ap.parse_args()
    with open(os.path.join(args.repo, args.file), encoding="utf-8") as f:
        text = f.read()
    named = set()
    for _, _, body in lex.comments(text, lex.spans_of(args.file, text)):
        named.update(ID.findall(body))
    laws = entries(args.repo)
    for i in sorted(named, key=lambda s: (s.split("-")[0], int(s.split("-")[1]))):
        body = laws.get(i)
        if body is None:
            print("!! %s is named and no register declares it\n" % i)
            continue
        keep, section = [body[0]], None
        for line in body[1:]:
            head = line.split(" ", 1)[0]
            if head in ("LAW", "MEANS", "DOES", "PROVEN", "WHY"):
                section = head
            if section in ("LAW", "MEANS", "DOES") and line.strip():
                keep.append("  " + line)
        print("\n".join(keep) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
