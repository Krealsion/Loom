# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The text checks and the documentation lane's reach (AGENTS.md, the documentation lane). Each
# check is `tests/check_<name>.cmake`, run as `cmake -DZEN_REPO=<root> -P` by CTest and the lane
# alike; tests/CMakeLists.txt registers them from this list, so the two cannot drift.
set(ZEN_TEXT_CHECKS doc_links source_comments doc_standard code_values)

# Besides Markdown and the images under docs/, what a documentation-only change may touch: the
# text checks themselves and the files only they read.
set(ZEN_TEXT_ONLY_FILES tests/code_values.txt tests/private_ids.cmake)

# A C/C++ change that keeps every token and changes only comments qualifies too: no compiled test
# here reads a source file as text, and the text checks read the comments.
set(ZEN_COMMENT_ONLY_QUALIFIES ON)
