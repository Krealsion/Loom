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

## Documentation

A public repository names no path outside itself — not the workspace it is checked out in,
not a sibling, not a drive or a scratch directory, not a home; say the thing in words. The
`doc_links` entry (`tests/check_doc_links.cmake`) reads every current-facing text file for one.
