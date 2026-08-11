# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# THE DOCUMENTATION-LINK CHECK (VOLATILE-2) -- the `doc_links` CTest entry.
#
# It answers one question: does every repo-local documentation reference this repository
# currently makes still resolve? Two populations, one rule:
#
#   markdown          every relative link in a current-facing *.md, plus its #anchor
#   source comments   every repository-relative *.md path written in a first-party
#                     C/C++ comment, plus its #anchor
#
# WHY IT IS MECHANICAL. The reference itself was already the project's convention -- laws
# cite reference pages, reference pages cite tests, AGENTS cites both. What was missing is
# that a rename or a consolidation broke them silently, and the only thing standing between
# a reader and a dead pointer was an executor remembering to run a script by hand. A rule
# nobody enforces is a hope; this is the mechanism, and it rides the official lane so a red
# reaches whoever moved the file rather than whoever reads the docs six phases later.
#
# WHY CMAKE AND NOT PYTHON. Every repository-owned check here is a CMake script for the same
# reason: CMake is a dependency this project already has on every lane by construction, and
# a verifier may not depend on a tool that merely happens to be installed. The predecessor
# `tools/check-doc-links.py` could not run at all on the native Windows host this repository
# supports, which is precisely the failure mode -- a check that is absent on the lane most
# likely to break paths is not a weaker check, it is no check.
#
# WHY THE SOURCE-COMMENT HALF IS REPOSITORY-RELATIVE. A comment has no stable directory to
# be relative to: it moves when the code moves, and the same sentence gets copied between
# src/ and include/. `docs/reference/messaging.md` means the same thing from anywhere in the
# tree, so that is the accepted form and this check is what makes it one.
#
# WHAT IT DELIBERATELY DOES NOT DO
#
#   * it does not require a comment to carry a reference. It verifies references that
#     exist; there is no comment-density gate and no phase-code linter here;
#   * it does not reach outside this repository. A reference that resolves above the
#     repository root is counted and skipped, never demanded -- a standalone clone has no
#     sibling to look at, and a check that passes only inside one workspace layout is a
#     check that a consumer cannot run (POP-03's reasoning, applied to documentation);
#   * it does not police frozen history. `docs/history/` and the dated `docs/audits/`
#     describe the tree they were written against, and forcing their links to resolve here
#     would mean editing history to satisfy a checker. They are excluded BY RULE, below,
#     and the exclusion is a written position rather than a per-file silence.
#
# THE SELF-TEST IS NOT OPTIONAL. A tree with no broken links and a checker that finds
# nothing produce byte-identical output, so before answering it makes the real predicate say
# NO -- a path that does not exist and an anchor that does not exist, both refused -- and say
# YES to a live document and to a heading read out of that document at runtime. Nothing in
# the self-test is a hardcoded value that could go stale into a false green.
#
#   cmake -P tests/check_doc_links.cmake              (from the repository root)
#   cmake -DZEN_REPO=<repo> -P tests/check_doc_links.cmake

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ZEN_REPO OR ZEN_REPO STREQUAL "")
    set(ZEN_REPO "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
get_filename_component(ZEN_REPO "${ZEN_REPO}" ABSOLUTE)
if(NOT EXISTS "${ZEN_REPO}/AGENTS.md")
    message(FATAL_ERROR
        "doc-links: '${ZEN_REPO}' does not look like this repository's root (no AGENTS.md). "
        "Pass -DZEN_REPO=<repository root>.")
endif()

# ---- scope, declared here so a standalone clone carries its own rule -----------------
#
# Markdown is swept from the repository root rather than from a list of doc directories: a
# new documentation folder must be covered the moment it exists, and an omission that
# QUIETLY reduces coverage is the failure mode this file is here to prevent. Everything
# narrowing is therefore a written exclusion instead.

# Frozen or generated. Matched against the repository-relative path of every candidate.
set(ZEN_DOC_EXCLUDE
    "^build"                 # every build tree, including build-san / build-win / cmake-build-*
    "^cmake-build"
    "^_install"
    "^\\.git/"
    "^docs/history/"         # frozen: describes the tree at its source commit
    "^docs/audits/"          # dated audit artifacts and their repro programs
    "^archive/"              # rehomed historical source
    "third_party/")          # vendored

# First-party C/C++ whose comments are in scope. Anything not listed here is not scanned.
set(ZEN_DOC_SOURCE_ROOTS include src tests examples)
set(ZEN_DOC_SOURCE_GLOBS *.h *.hpp *.ipp *.c *.cc *.cpp *.cxx)

# The document the self-test interrogates. Every repository has one, it is current-facing by
# definition, and it carries headings.
set(ZEN_DOC_SELFTEST_FILE "AGENTS.md")

# ---- the slug, GitHub's convention ---------------------------------------------------
#
# Lowercase, drop the punctuation GitHub drops, keep word characters and hyphens, and turn
# EACH remaining whitespace character into one hyphen -- consecutive spaces stay consecutive
# hyphens, which is what makes `## POP-01 -- a law` anchor as `pop-01----a-law` rather than
# collapsing. Getting this wrong in the lenient direction would accept anchors GitHub does
# not serve, which is the same defect as not checking at all.
function(zen_doc_slug text out)
    string(STRIP "${text}" s)
    string(TOLOWER "${s}" s)
    string(REGEX REPLACE "[`*]" "" s "${s}")
    string(REGEX REPLACE "[^a-z0-9_ \t-]" "" s "${s}")
    string(REGEX REPLACE "[ \t]" "-" s "${s}")
    set(${out} "${s}" PARENT_SCOPE)
endfunction()

# READING A FILE WITHOUT LETTING ITS PUNCTUATION RESHAPE A CMAKE LIST.
#
# Two characters would otherwise decide the answer for us. `;` is CMake's list separator, so
# a semicolon anywhere in the prose splits an element; `\` escapes the next character, so a
# line ending in one swallows the separator that follows and silently welds two lines
# together. The first version of this file kept them and lost two thirds of
# reference/bounds.md -- every anchor into it read as broken, which is a false RED, and the
# same defect one edit away from being a false GREEN.
#
# Both are dropped at the door. Neither can change an answer: `;` and `\` are punctuation
# that the heading slug discards anyway (GitHub discards it too), and no path or anchor
# contains either.
function(zen_doc_read_markdown path out)
    file(READ "${path}" content)
    string(REPLACE "\r" "" content "${content}")
    string(REPLACE ";" "" content "${content}")
    string(REPLACE "\\" "" content "${content}")
    set(${out} "${content}" PARENT_SCOPE)
endfunction()

# The same, plus C/C++ escape removal -- `\X` pairs go first so that `\"` cannot close a
# string literal downstream, and so that a `\`-continued line joins the next one exactly as
# the language says it does (a `//` comment really does continue across one).
function(zen_doc_read_source path out)
    file(READ "${path}" content)
    string(REPLACE "\r" "" content "${content}")
    string(REPLACE ";" "" content "${content}")
    string(REGEX REPLACE "\\\\." "" content "${content}")
    string(REPLACE "\\" "" content "${content}")
    set(${out} "${content}" PARENT_SCOPE)
endfunction()

# EVERY COMMENT IN A C/C++ TRANSLATION UNIT, AS ONE BLOB OF TEXT.
#
# Whole-content pattern extraction rather than a line-by-line state machine, and the reason
# is measured: CMake stops treating `;` as an element boundary after a few dozen of them, so
# a file split into a list of lines silently arrives with its tail welded into one element.
# The pattern form has no list in it to go wrong, and it is an order of magnitude faster.
#
# String literals go first so that the `//` in "https://example.org" cannot open a comment --
# bounded to one line, so an unbalanced quote can never eat the file. Character literals are
# deliberately NOT stripped: `'/'` is one slash and two of them cannot be adjacent, so no
# character literal can forge a marker, while `'[^']*'` would happily pair the apostrophe in
# "the host's" with a later one and delete the sentence between them.
#
# Honest limits, both in the direction of scanning MORE text rather than less: a `//` inside
# a block comment re-matches as a line comment, and a raw string literal on one line may be
# stripped as an ordinary one. Neither can hide a reference; at worst a reference is seen
# twice, which is why the caller de-duplicates.
function(zen_doc_comments content out)
    string(REGEX REPLACE "\"[^\"\n]*\"" "" code "${content}")
    string(REGEX MATCHALL "//[^\n]*" line_comments "${code}")
    string(REGEX MATCHALL "/\\*([^*]|\\*+[^*/])*\\*+/" block_comments "${code}")
    set(${out} "${line_comments} ${block_comments}" PARENT_SCOPE)
endfunction()

# Every heading slug in a markdown file. An empty answer is itself meaningful -- see the
# self-test, which requires a real document to yield a real heading.
#
# MEMOISED, because a hub document is the target of dozens of anchors and re-reading and
# re-slugging it once per anchor made this the slowest entry in the lane. The cache key is
# the resolved path; the cached value is the slug list, which contains no `;` because the
# slug drops it. A GLOBAL property rather than a variable: a function cannot write its
# caller's scope, and threading a cache through every call site would put the memoisation in
# the predicate the self-test exercises.
function(zen_doc_headings path out)
    string(MAKE_C_IDENTIFIER "${path}" key)
    get_property(cached GLOBAL PROPERTY "zen_doc_headings_${key}" SET)
    if(cached)
        get_property(slugs GLOBAL PROPERTY "zen_doc_headings_${key}")
        set(${out} "${slugs}" PARENT_SCOPE)
        return()
    endif()
    set(slugs "")
    if(EXISTS "${path}")
        zen_doc_read_markdown("${path}" content)
        string(REGEX MATCHALL "\n#+[ \t][^\n]*" heads "\n${content}")
        foreach(head IN LISTS heads)
            string(REGEX REPLACE "^\n#+[ \t]" "" head "${head}")
            zen_doc_slug("${head}" s)
            list(APPEND slugs "${s}")
        endforeach()
    endif()
    set_property(GLOBAL PROPERTY "zen_doc_headings_${key}" "${slugs}")
    set(${out} "${slugs}" PARENT_SCOPE)
endfunction()

# ---- the predicate, in one place so the self-test exercises the real one -------------
#
# Sets ${out} to one of: ok | outside | broken:<reason>. `outside` is a real answer and not
# a failure: it is how a cross-repository or absolute reference is counted and declined.
function(zen_doc_verdict base_dir target out)
    if(target MATCHES "^[A-Za-z][A-Za-z0-9+.-]*:" OR target MATCHES "^//")
        set(${out} "outside" PARENT_SCOPE)   # a URL, or a protocol-relative one
        return()
    endif()
    string(FIND "${target}" "#" hash)
    if(hash EQUAL 0)
        set(${out} "outside" PARENT_SCOPE)   # same-file anchor; not a path claim
        return()
    endif()
    set(fragment "")
    set(path "${target}")
    if(NOT hash EQUAL -1)
        string(SUBSTRING "${target}" 0 ${hash} path)
        math(EXPR after "${hash} + 1")
        string(SUBSTRING "${target}" ${after} -1 fragment)
    endif()
    if(path STREQUAL "")
        set(${out} "outside" PARENT_SCOPE)
        return()
    endif()
    if(IS_ABSOLUTE "${path}")
        set(${out} "outside" PARENT_SCOPE)
        return()
    endif()

    get_filename_component(resolved "${base_dir}/${path}" ABSOLUTE)
    string(FIND "${resolved}" "${ZEN_REPO}/" inside)
    if(NOT inside EQUAL 0)
        set(${out} "outside" PARENT_SCOPE)   # above the repository root: not ours to demand
        return()
    endif()

    if(NOT EXISTS "${resolved}")
        set(${out} "broken:no such path" PARENT_SCOPE)
        return()
    endif()
    if(fragment STREQUAL "")
        set(${out} "ok" PARENT_SCOPE)
        return()
    endif()
    if(NOT resolved MATCHES "\\.md$")
        set(${out} "ok" PARENT_SCOPE)        # a fragment on a non-markdown target: not ours to slug
        return()
    endif()
    zen_doc_headings("${resolved}" slugs)
    zen_doc_slug("${fragment}" wanted)
    list(FIND slugs "${wanted}" found)
    if(found EQUAL -1)
        set(${out} "broken:no heading anchors to that fragment" PARENT_SCOPE)
        return()
    endif()
    set(${out} "ok" PARENT_SCOPE)
endfunction()

# ---- the self-test: make it say NO, and make it say YES ------------------------------

set(selftest_doc "${ZEN_REPO}/${ZEN_DOC_SELFTEST_FILE}")
if(NOT EXISTS "${selftest_doc}")
    message(FATAL_ERROR
        "doc-links: SELF-TEST cannot run -- '${ZEN_DOC_SELFTEST_FILE}' is missing, so there "
        "is no live document to prove the checker against. A checker that has not been made "
        "to say NO has not passed.")
endif()

zen_doc_verdict("${ZEN_REPO}" "${ZEN_DOC_SELFTEST_FILE}" v)
if(NOT v STREQUAL "ok")
    message(FATAL_ERROR
        "doc-links: SELF-TEST FAILED -- the checker rejected '${ZEN_DOC_SELFTEST_FILE}', "
        "which exists (${v}). It has started refusing live documents, which is a different "
        "defect and not a safer one.")
endif()

zen_doc_verdict("${ZEN_REPO}" "docs/zen-no-such-document-doclinks-selftest.md" v)
if(NOT v MATCHES "^broken:")
    message(FATAL_ERROR
        "doc-links: SELF-TEST FAILED -- a path that does not exist was accepted (${v}). "
        "Every green this check reports would be the same green a broken detector reports.")
endif()

zen_doc_headings("${selftest_doc}" selftest_slugs)
list(LENGTH selftest_slugs selftest_heading_count)
if(selftest_heading_count EQUAL 0)
    message(FATAL_ERROR
        "doc-links: SELF-TEST FAILED -- no headings were read out of "
        "'${ZEN_DOC_SELFTEST_FILE}'. With an empty heading set every anchor would look "
        "broken, or (worse, in a future edit) every anchor would be waved through.")
endif()
list(GET selftest_slugs 0 selftest_slug)
zen_doc_verdict("${ZEN_REPO}" "${ZEN_DOC_SELFTEST_FILE}#${selftest_slug}" v)
if(NOT v STREQUAL "ok")
    message(FATAL_ERROR
        "doc-links: SELF-TEST FAILED -- the anchor '#${selftest_slug}', slugified from a "
        "heading read out of '${ZEN_DOC_SELFTEST_FILE}' moments ago, was rejected (${v}).")
endif()
zen_doc_verdict("${ZEN_REPO}"
                "${ZEN_DOC_SELFTEST_FILE}#zen-no-such-anchor-doclinks-selftest" v)
if(NOT v MATCHES "^broken:")
    message(FATAL_ERROR
        "doc-links: SELF-TEST FAILED -- an anchor that no heading produces was accepted "
        "(${v}) on a file that does exist. Path checking alone would then be the whole "
        "check, silently.")
endif()

message(STATUS
    "doc-links: self-test OK -- a missing path and a missing anchor are both refused, a "
    "live document and one of its own headings are both accepted")

# ---- gathering the two populations -----------------------------------------------------

function(zen_doc_excluded rel out)
    foreach(pattern IN LISTS ZEN_DOC_EXCLUDE)
        if(rel MATCHES "${pattern}")
            set(${out} 1 PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out} 0 PARENT_SCOPE)
endfunction()

# Excluded top-level directories are pruned BEFORE the walk, not filtered after it. The
# result is identical -- every file under them is excluded by the same rule either way -- but
# a repository with three dozen build trees on a slow filesystem spends over a minute
# enumerating files it is going to throw away, which is a real cost on every local run.
# Anything NOT excluded is still walked recursively, so a new documentation folder is covered
# the moment it exists.
#
# The two globs are separate for a sharp reason. `file(GLOB_RECURSE)` recurses from the last
# directory component of its expression, so handing it `<repo>/README.md` -- an expression
# with no wildcard in the directory part -- makes it walk the ENTIRE repository looking for
# files by that name, once per root-level document. Measured: seven root documents turned one
# tree walk into seven. Root files come from a plain, non-recursive `file(GLOB)`.
file(GLOB root_md RELATIVE "${ZEN_REPO}" "${ZEN_REPO}/*.md")

set(md_globs "")
set(pruned "")
file(GLOB top_level RELATIVE "${ZEN_REPO}" "${ZEN_REPO}/*")
foreach(entry IN LISTS top_level)
    if(IS_DIRECTORY "${ZEN_REPO}/${entry}")
        zen_doc_excluded("${entry}/" skip)
        if(skip)
            list(APPEND pruned "${entry}")
        else()
            list(APPEND md_globs "${ZEN_REPO}/${entry}/*.md")
        endif()
    endif()
endforeach()

set(nested_md "")
if(md_globs)
    file(GLOB_RECURSE nested_md RELATIVE "${ZEN_REPO}" ${md_globs})
endif()
set(all_md ${root_md} ${nested_md})

set(md_files "")
set(md_excluded 0)
foreach(rel IN LISTS all_md)
    zen_doc_excluded("${rel}" skip)
    if(skip)
        math(EXPR md_excluded "${md_excluded} + 1")
    else()
        list(APPEND md_files "${rel}")
    endif()
endforeach()

set(source_globs "")
foreach(root IN LISTS ZEN_DOC_SOURCE_ROOTS)
    foreach(glob IN LISTS ZEN_DOC_SOURCE_GLOBS)
        list(APPEND source_globs "${ZEN_REPO}/${root}/${glob}")
    endforeach()
endforeach()
file(GLOB_RECURSE all_src RELATIVE "${ZEN_REPO}" ${source_globs})
set(src_files "")
set(src_excluded 0)
foreach(rel IN LISTS all_src)
    zen_doc_excluded("${rel}" skip)
    if(skip)
        math(EXPR src_excluded "${src_excluded} + 1")
    else()
        list(APPEND src_files "${rel}")
    endif()
endforeach()

list(LENGTH md_files md_count)
list(LENGTH src_files src_count)
if(md_count EQUAL 0 OR src_count EQUAL 0)
    message(FATAL_ERROR
        "doc-links: the sweep found ${md_count} markdown file(s) and ${src_count} first-party "
        "source file(s). An expectation of nothing is satisfied by anything (POP-01), so an "
        "empty population is a failure here and not a quiet pass -- check ZEN_REPO and the "
        "exclusion rules at the top of this file.")
endif()

# ---- population 1: markdown links ------------------------------------------------------
#
# Link targets are resolved relative to the FILE, which is how markdown itself resolves them
# and how they render on the forge.

set(problems "")
set(checked 0)
set(outside 0)

foreach(rel IN LISTS md_files)
    set(path "${ZEN_REPO}/${rel}")
    get_filename_component(base "${path}" DIRECTORY)
    zen_doc_read_markdown("${path}" content)
    string(REGEX MATCHALL "\\[[^]]*\\]\\([^)\r\n \t]+\\)" links "${content}")
    foreach(link IN LISTS links)
        if(NOT link MATCHES "\\(([^)]+)\\)$")
            continue()
        endif()
        set(target "${CMAKE_MATCH_1}")
        zen_doc_verdict("${base}" "${target}" v)
        if(v STREQUAL "outside")
            math(EXPR outside "${outside} + 1")
        elseif(v MATCHES "^broken:(.*)$")
            list(APPEND problems "${rel}: ${CMAKE_MATCH_1} -> ${target}")
            math(EXPR checked "${checked} + 1")
        else()
            math(EXPR checked "${checked} + 1")
        endif()
    endforeach()
endforeach()

if(checked EQUAL 0)
    message(FATAL_ERROR
        "doc-links: ${md_count} markdown files yielded ZERO repo-local links to check. Either "
        "the extractor stopped recognising markdown link syntax or every document lost its "
        "cross-references; both are failures, and neither is a green.")
endif()
set(md_checked "${checked}")

# ---- population 2: repository-relative doc paths in first-party C/C++ comments ---------
#
# Comment text only, and resolved against the REPOSITORY ROOT rather than the file: a
# comment travels with the code it explains, so a file-relative reference in one would break
# on the next move for a reason that has nothing to do with documentation.
#
# Files mentioning no `.md` at all are skipped whole, which is nearly all of them -- the
# scan costs nothing where there is nothing to say.

set(src_scanned 0)
set(src_refs 0)

foreach(rel IN LISTS src_files)
    set(path "${ZEN_REPO}/${rel}")
    zen_doc_read_source("${path}" content)
    string(FIND "${content}" ".md" mentions)
    if(mentions EQUAL -1)
        continue()
    endif()
    math(EXPR src_scanned "${src_scanned} + 1")

    zen_doc_comments("${content}" comment_text)
    string(REGEX MATCHALL "[A-Za-z0-9_.][A-Za-z0-9_./-]*\\.md(#[A-Za-z0-9_-]+)?"
           refs "${comment_text}")
    if(refs)
        list(REMOVE_DUPLICATES refs)
    endif()
    foreach(ref IN LISTS refs)
        zen_doc_verdict("${ZEN_REPO}" "${ref}" v)
        if(v STREQUAL "outside")
            math(EXPR outside "${outside} + 1")
        elseif(v MATCHES "^broken:(.*)$")
            string(CONCAT problem "${rel}: ${CMAKE_MATCH_1} -> ${ref}"
                   "   (a source comment's reference is resolved against the"
                   " repository root, not against the file)")
            list(APPEND problems "${problem}")
            math(EXPR src_refs "${src_refs} + 1")
        else()
            math(EXPR src_refs "${src_refs} + 1")
        endif()
    endforeach()
endforeach()

# ---- the report ------------------------------------------------------------------------

list(LENGTH pruned pruned_count)
message(STATUS "doc-links: ${md_count} markdown files (${md_excluded} excluded by rule, "
               "${pruned_count} top-level directories pruned), ${md_checked} repo-local "
               "links checked")
message(STATUS "doc-links: ${src_count} first-party C/C++ files, ${src_scanned} carrying a "
               ".md reference, ${src_refs} comment references checked")
message(STATUS "doc-links: ${outside} references counted and declined (external URL, "
               "same-file anchor, or above the repository root)")

if(NOT problems STREQUAL "")
    list(LENGTH problems problem_count)
    set(text "")
    foreach(problem IN LISTS problems)
        string(APPEND text "  ${problem}\n")
    endforeach()
    message(FATAL_ERROR
        "doc-links FAILED: ${problem_count} broken repo-local documentation reference(s).\n"
        "${text}\n"
        "  Fix the reference, or -- if the document is deliberately frozen history -- widen "
        "ZEN_DOC_EXCLUDE in this file with a written rule rather than silencing one path.")
endif()

message(STATUS "doc-links: PASSED -- every repo-local documentation reference resolves")
