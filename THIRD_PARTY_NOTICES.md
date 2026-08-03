# Third-party notices

Bundled third-party material actually present in this repository:

| Component | Location | Copyright | License | License text |
|---|---|---|---|---|
| doctest | `tests/third_party/doctest.h` | Copyright (c) 2016-2023 Viktor Kirilov | MIT | stated in the file's own header; canonical text at <https://opensource.org/licenses/MIT> |

Not bundled, fetched at build time only (and therefore not distributed by
this repository): the optional pinned SDL library used by the gated
`zen-ui-sdl` renderer (zlib license, fetched via CMake FetchContent when
enabled). Its license accompanies its own distribution.
