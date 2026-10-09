# Contributing

## Getting a Loom you can work with

Before anything else: [the tools you need](docs/guides/tools.md) — a C++20 compiler and
CMake, which are real prerequisites and not assumed knowledge — and then
[from nothing to a running weave](docs/guides/running-loom.md), which installs Loom,
starts the supplied host and builds something against the installed package. Do that
first even if you intend to change Loom itself: the shortest way to understand what a
change breaks is to have used the thing it breaks.

The build and the official test lane are in [the README](README.md#build-and-test); the
build rules a machine collaborator needs are in [AGENTS.md](AGENTS.md).

## Taking an issue

Bounded work is a GitHub issue labelled `ready`.
[Taking an issue](docs/contributing/taking-an-issue.md) goes from one to a pull request ready to
merge, with nothing outside this repository's own documentation, and
[best practices](docs/contributing/best-practices.md) says what good work looks like here,
subsystem by subsystem.

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

### Values the code owns

A version, a limit, a count or a default the code owns is named by its owner — the constant,
the macro, the shape — wherever a sentence needs it: "at most `kMaxFences` fences", "the current
version is `ZEN_ABI_VERSION`". A comment names the owner and no more. Where a page's reader
needs the number itself, it stands in a marker that renders as nothing:

```markdown
| `kMaxJointOfferBytes` | <!-- value kMaxJointOfferBytes KiB -->64<!-- /value --> KiB | … |
<!-- value kMaxCommandBytes in "longer than {} bytes" -->
```

The first holds the number between its two comments, written as the word after the id says
(`KiB`, `MiB`, `GiB`, `s`, `pow2`, `grouped`, or nothing for the integer); two ids joined by a
comma are one value two owners share, and they must agree. The second holds a spelling on its
own line, or, standing alone, the next line or the fenced block after it: the form for a
number inside code or a transcript. A marker shown inside a fenced block, as here, is an
example and holds nothing.

`code_values` (`tests/check_code_values.cmake`) reads each value
[`tests/code_values.txt`](tests/code_values.txt) registers from its owner, and a marker that
says otherwise is a red; `cmake -DZEN_VALUES_WRITE=ON -P tests/check_code_values.cmake` rewrites
the markers to the owners' values. It also refuses an unmarked copy of a value it knows: the
owner's name with a number beside it, in a page or a comment, and a spelling the registry names
— a claim of the current value (`C ABI (version **{}**)`) always, and a mention (`ABI v{}`) at
the current value unless it states that version's own fact ("since ABI v9", "v5 adds").

A test count outside the population files names, beside it, the commit and the command that
measured it, or it is not written; `code_values` refuses one that names neither. No change
sweeps for copies by hand: when review finds a moving value the registry does not know, it is
registered in the change that found it.

### The checks

Four entries on the official lane hold this, each naming the file and line of what it
refuses:

- `source_comments` (`tests/check_source_comments.cmake`) reads first-party C/C++, CMake, the
  two population manifests, Python and shell, and the session tooling's launchers: a comment
  block over six lines outside an installed header, a removal note, or a private id. A Python,
  shell or batch file is read whole for an id, docstrings and strings included; only a comment on
  a line of its own counts toward a block there. Every C/C++ string literal and the code of
  every CMake file are read for a private id too, as their comments are; the three checks whose
  self-tests must spell ids are spared that reading. It reads each `TEST_CASE` and `SUBCASE`
  name, every one on a line: one holding a private id or a label (`J1:`, `S3b`), or a
  `TEST_CASE` name another case already has in the one test binary, is refused.
- `doc_standard` (`tests/check_doc_standard.cmake`) reads every current-facing Markdown file for
  a private id, for the development process used as a unit of time, and for a page's history in
  the few forms that told it every time they were read by hand: a heading marked retired, a note
  of what a thing was before or what it was called, and a bold note opening on the past. The
  check names each form in its own words.
- `doc_links` (`tests/check_doc_links.cmake`) resolves every repository-relative link, and every
  `.md` path in a C/C++ or CMake comment, and refuses a path outside the repository in any
  current-facing text file, whether it is spelled out or reached by climbing above it with `../`.
- `code_values` (`tests/check_code_values.cmake`) holds the values the code owns, as the
  section above says.

What a private id is, `tests/private_ids.cmake` says once for both. Every current-facing
document and every first-party source file is held to this standard; vendored code is not. No
check can see history or a private process written in other words, a label shape in a string (in
data `c2a` looks like `r1` or `Ping2`), or a comment's truth; that is a reviewer's.
