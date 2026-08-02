#!/usr/bin/env python3
"""Validate relative Markdown links (and their #anchors) under given roots.

Documentation-validation tooling only: walks *.md files, resolves every
relative link against the filesystem, and — when the link carries a #fragment —
checks the target file contains a heading that slugifies to it. External URLs
are ignored. Exit 0 = every link resolves.

Usage: python3 tools/check-doc-links.py <root> [<root> ...]
"""
import os, re, sys

LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
SKIP_DIRS = {"build", "build-san", "build-win", "build-win-k", "build-nosdl",
             "_deps", "_install", ".git", "third_party", "vendor", "reference_quarry"}


def slug(text: str) -> str:
    # GitHub's convention: punctuation dropped, EACH space becomes a hyphen
    # (consecutive spaces stay consecutive hyphens — do not collapse).
    text = re.sub(r"[`*]", "", text.strip().lower())
    text = re.sub(r"[^a-z0-9\s-]", "", text)
    return re.sub(r"\s", "-", text)


def headings(path: str):
    out = set()
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if line.startswith("#"):
                    out.add(slug(line.lstrip("#")))
    except OSError:
        pass
    return out


def md_files(root: str):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if name.endswith(".md"):
                yield os.path.join(dirpath, name)


def main(roots):
    bad = []
    checked = 0
    for root in roots:
        for md in md_files(root):
            base = os.path.dirname(md)
            for target in LINK.findall(open(md, encoding="utf-8").read()):
                if re.match(r"^[a-z]+:", target) or target.startswith("#"):
                    continue  # external, or same-file anchor (heading check below is best-effort)
                path, _, frag = target.partition("#")
                resolved = os.path.normpath(os.path.join(base, path))
                checked += 1
                if not os.path.exists(resolved):
                    bad.append(f"{md}: broken path -> {target}")
                elif frag and resolved.endswith(".md") and slug(frag) not in headings(resolved):
                    bad.append(f"{md}: missing anchor -> {target}")
    for line in bad:
        print(line)
    print(f"checked {checked} relative links; {len(bad)} broken")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["."]))
