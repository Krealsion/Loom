# Licensing

Copyright (c) 2026 Joshua DeMoss.

The first-party software in this repository is licensed under the GNU Affero
General Public License, version 3 only (AGPL-3.0-only), unless a file or
directory states otherwise.

See [LICENSE](LICENSE) for the complete license text.

## What this means

You may use, study, change and share this software under the terms of
AGPL-3.0-only. If you share it, or a work based on it, you share it under
those terms, with its source. If you change it and let others use it over a
network, you offer them its source too.

A creations exception is being drafted: an additional permission meant to
keep what you create with Zen yours. Until it is published there is none, and
building on Zen means the AGPL's terms.

This summary is explanatory only. LICENSE is authoritative.

## Ownership

The copyright holder for first-party material identified as such is Joshua
DeMoss.

"Unknown Works" / unknownworks.net is currently a project brand and domain,
not the copyright owner and not an LLC.

## Third-party material

Files or directories carrying their own license or copyright notice remain
under those terms. Bundled third-party material is listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); today that is the doctest
test framework (`tests/third_party/doctest.h`, MIT), and it is the whole
list — the build fetches no dependency and links no third-party library.
Should one ever be fetched at build time, it would not be distributed in this
repository and would remain under its own license.

Files without an individual header — build scripts, CMake lists, Markdown
documentation, JSON fixtures — are covered by this repository-level statement
rather than per-file notices.
