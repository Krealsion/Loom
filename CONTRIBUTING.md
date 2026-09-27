# Contributing

## Getting a Loom you can work with

Before anything else: [the tools you need](docs/guides/tools.md) — a C++20 compiler and
CMake, which are real prerequisites and not assumed knowledge — and then
[from nothing to a running weave](docs/guides/running-loom.md), which installs Loom,
starts the supplied host and builds something against the installed package. Do that
first even if you intend to change Loom itself: the shortest way to understand what a
change breaks is to have used the thing it breaks.

The build and the official test lane are in [the README](README.md#build--test); the
build rules a machine collaborator needs are in [AGENTS.md](AGENTS.md).

## Code contributions

Issues, testing, design discussion, reproductions, and feedback are welcome.

Before merging substantial third-party code contributions to the core
repository, the project will publish explicit contributor terms so ownership
and licensing remain clear for contributors and maintainers alike.

Please open an issue before preparing a large core contribution.

This note is deliberately minimal: it is not a CLA, it requires no copyright
assignment, and it sets no terms beyond asking that big core changes start
with a conversation. Experimentation, packages, and weaves of your own need
no permission at all — they are yours.

## Comments and documents

Everything current-facing here — a source comment, a page under `docs/`, this file — is written
for a reader who has ordinary programming knowledge and this repository, and nothing else. It
states the present fact.

- **Nothing private.** No development-phase name, plan code or private id, and nothing about
  the maintainers' own machine, workspace, process, roles or reports. A law is cited by the id
  [`docs/laws/`](docs/laws/README.md) declares (`MSG-09`); any other reason is said in words.
  History belongs to [`docs/history/`](docs/history/README.md) and to Git.
- **No path outside the repository** — not the workspace it is checked out in, not a sibling,
  not a drive or a scratch directory, not a home; say the thing in words.
- **Terms in order.** A page explains a term where a reader first meets it, or links the
  [terminology index](docs/terminology.md), before relying on it.

### Source comments

A comment stays only if, without it, a competent reader with the code and the laws open would
make a mistake. Prefer a clearer name to a comment; keep a comment to one line where one line
will do, and point to the page that holds the rest. Be conservative where authority, custody or
lifetime is at stake.

- **What stays.** The SPDX pair and a line of purpose at the top of a file. A law pointer above
  the code it governs (`// MSG-09; docs/laws/messaging-laws.md`). A banner that names a section.
  A why the code cannot say, beside the code it explains.
- **What moves.** Reasoning a reader needs and no page holds yet goes where a reader first
  needs it: the subsystem's reference page, a decision record when it settles a choice, a law
  when it must never become false.
- **What goes.** History — what was removed, what a retired piece did, how the code came to be
  this way — is Git's. So is a restatement of the code, of a law or of a neighbouring comment.
- **Installed headers are documentation.** `include/zen/` is the installed API, read by
  strangers while they use it. A header says what a thing is and what it deliberately is not
  (so a reader does not infer a capability from an architecture), its contract, and what fires
  when that contract is broken: a guard whose trigger is misdescribed is worse than none. The
  reasoning behind the contract goes to its reference page.
- **Tests are witnesses.** A case's name says what it proves, in words. Its comments say what
  its name and code cannot: why a setup, a bound, a repeat or an oracle has its shape, and what
  a mutation found that the case now guards, wherever a reader would otherwise simplify the case
  into one that passes for the wrong reason.

### The checks

Three entries on the official lane hold this, each naming the file and line of what it
refuses:

- `source_comments` (`tests/check_source_comments.cmake`) reads first-party C/C++, CMake and the
  two population manifests: a comment block over six lines outside an installed header, a
  removal note, or a private id.
- `doc_standard` (`tests/check_doc_standard.cmake`) reads every current-facing Markdown file for
  a private id.
- `doc_links` (`tests/check_doc_links.cmake`) resolves every repository-relative link and
  refuses a path outside the repository in any current-facing text file.

What a private id is, `tests/private_ids.cmake` says once for both. Files not yet brought to
this standard are listed in each check, and the list only shrinks. No check can see history or
a private process written in words, or read a comment for truth; that is a reviewer's.
`tools/comment-pass/` measures, prints and proves a comment pass (`census.py`, `blocks.py`,
`edit.py`, `prove.py`).
