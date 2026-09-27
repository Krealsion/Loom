# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The `doc_links` entry: does every repository-local documentation reference resolve -- each
# relative link and #anchor in a current-facing *.md, each repository-relative *.md path in a
# first-party C/C++ comment -- and does no current-facing text file name a path outside this
# repository (CONTRIBUTING.md#comments-and-documents)? It requires no reference, demands nothing
# above the repository root (counted and declined), and excludes frozen history by written rule.

# A comment's path is read from the repository root, because a comment moves with its code. The
# self-test makes the real predicate say no (a missing path, a missing anchor) and yes (a live
# document and a heading read from it at runtime) before it answers.
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
    "^\\.git(/|$)"           # the repository's own store -- a directory in a clone, and in a
                             # git WORKTREE a one-line file naming the main checkout's path,
                             # which is the VCS's record of itself and never documentation
    "^out/"                  # a build tree name .gitignore also names
    "^\\.idea/"              # editor state, gitignored: it holds this machine's paths by design
    "^\\.vscode/"
    "^\\.claude/"            # harness state, gitignored
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

# ---- paths outside this repository ----------------------------------------------------
# The spellings of paths outside the tree: a workspace root, a drive and its mount, a harness's
# scratch directory, a build root beyond the tree, a home. Every current-facing text file is read
# whole and raw (`G:\` is found as written); this file alone declares them and may carry them, and
# the self-test checks it carries each. The backslash spelling is last, since `\` before a `;`
# escapes CMake's list separator; the self-test counts the list, so a reorder is a red.
set(ZEN_DOC_OUTSIDE_SPELLINGS
    "Zen/" "reportbacks/" "/mnt/g/" "G:/" "programming/cpp" "scratchpad" "zen-build"
    "Temp/claude" "/home/joshua" "Users/Joshua" "G:\\")
set(ZEN_DOC_OUTSIDE_SPELLING_COUNT 11)
set(ZEN_DOC_OUTSIDE_DECLARERS tests/check_doc_links.cmake)
set(ZEN_DOC_TEXT_EXTENSIONS md h hpp ipp c cc cpp cxx cmake txt json in yml yaml py sh)

# ---- the slug, GitHub's convention ---------------------------------------------------
# Lowercase, drop the punctuation GitHub drops, keep word characters and hyphens, and turn each
# whitespace character into one hyphen (`## POP-01 -- a law` anchors as `pop-01----a-law`): a
# lenient slug would accept anchors GitHub does not serve.
function(zen_doc_slug text out)
    string(STRIP "${text}" s)
    string(TOLOWER "${s}" s)
    string(REGEX REPLACE "[`*]" "" s "${s}")
    string(REGEX REPLACE "[^a-z0-9_ \t-]" "" s "${s}")
    string(REGEX REPLACE "[ \t]" "-" s "${s}")
    set(${out} "${s}" PARENT_SCOPE)
endfunction()

# Reading a file without letting its punctuation reshape a CMake list: `;` would split an element
# and a trailing `\` would weld two lines, so both are dropped at the door. Neither can change an
# answer: the heading slug discards both, as GitHub does, and no path or anchor contains either.
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

# Every comment in a C/C++ translation unit, as one blob: a file split into a list of lines
# arrives with its tail welded, so the extraction is by pattern over the whole content. String
# literals go first, bounded to a line, so the `//` in a URL opens no comment; character literals
# stay, since `'/'` cannot forge a marker and `'[^']*'` would pair apostrophes in prose. Its limits
# all read more text, never less, so a reference is at worst seen twice and the caller
# de-duplicates.
function(zen_doc_comments content out)
    string(REGEX REPLACE "\"[^\"\n]*\"" "" code "${content}")
    string(REGEX MATCHALL "//[^\n]*" line_comments "${code}")
    string(REGEX MATCHALL "/\\*([^*]|\\*+[^*/])*\\*+/" block_comments "${code}")
    set(${out} "${line_comments} ${block_comments}" PARENT_SCOPE)
endfunction()

# Every heading slug in a markdown file; the self-test requires a real document to yield one.
# Memoised in a GLOBAL property keyed by the resolved path, because a hub document is the target
# of dozens of anchors and a function cannot write its caller's scope.
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

# The outside-path predicate: the INDICES (into ZEN_DOC_OUTSIDE_SPELLINGS) of every spelling
# the text carries, empty when it names nothing outside the tree. Indices rather than the
# spellings themselves, so the backslash one can never reach a CMake list.
function(zen_doc_outside_hits text out)
    set(hits "")
    set(i 0)
    foreach(spelling IN LISTS ZEN_DOC_OUTSIDE_SPELLINGS)
        string(FIND "${text}" "${spelling}" at)
        if(NOT at EQUAL -1)
            list(APPEND hits "${i}")
        endif()
        math(EXPR i "${i} + 1")
    endforeach()
    set(${out} "${hits}" PARENT_SCOPE)
endfunction()

list(LENGTH ZEN_DOC_OUTSIDE_SPELLINGS outside_spelling_count)
if(NOT outside_spelling_count EQUAL ZEN_DOC_OUTSIDE_SPELLING_COUNT)
    message(FATAL_ERROR
        "doc-links: SELF-TEST FAILED -- ZEN_DOC_OUTSIDE_SPELLINGS holds ${outside_spelling_count} "
        "elements and ${ZEN_DOC_OUTSIDE_SPELLING_COUNT} were written. A spelling has welded into "
        "its neighbour (a trailing backslash before the list separator), and the welded pair "
        "would match nothing.")
endif()
zen_doc_outside_hits("the matrix is in Zen/reportbacks/X-evidence.md, on G:/ and /mnt/g/" outside_yes)
zen_doc_outside_hits("see docs/reference/messaging.md, and /home/you/my-thing in the guide" outside_no)
file(READ "${CMAKE_CURRENT_LIST_FILE}" outside_own_text)
zen_doc_outside_hits("${outside_own_text}" outside_own)
list(LENGTH outside_yes n_outside_yes)
list(LENGTH outside_own n_outside_own)
if(NOT n_outside_yes EQUAL 4 OR NOT outside_no STREQUAL ""
   OR NOT n_outside_own EQUAL outside_spelling_count)
    message(FATAL_ERROR
        "doc-links: SELF-TEST FAILED -- the outside-path predicate found ${n_outside_yes} of 4 "
        "planted spellings, '${outside_no}' in a clean sentence, and ${n_outside_own} of "
        "${outside_spelling_count} in the file that declares them. Every 'no leak' below would "
        "then be meaningless.")
endif()

message(STATUS
    "doc-links: self-test OK -- a missing path and a missing anchor are both refused, a "
    "live document and one of its own headings are both accepted; four planted outside paths "
    "are found, a clean sentence is not, and this file carries every declared spelling")

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

# Excluded top-level directories are pruned before the walk rather than filtered after it: the
# result is the same, and a tree with many build directories is not enumerated. Root files come
# from a plain file(GLOB): file(GLOB_RECURSE) given `<repo>/README.md` walks the whole repository
# looking for that name.
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

# Population 3: every current-facing file of a text kind -- root files by a plain glob, then
# everything under each unpruned top-level directory. Text is decided by extension, plus the
# extensionless and dot-named files at the root (LICENSE, .gitignore, .clang-format).
file(GLOB root_any RELATIVE "${ZEN_REPO}" "${ZEN_REPO}/*")
set(any_globs "")
foreach(entry IN LISTS top_level)
    if(IS_DIRECTORY "${ZEN_REPO}/${entry}" AND NOT entry IN_LIST pruned)
        list(APPEND any_globs "${ZEN_REPO}/${entry}/*")
    endif()
endforeach()
set(nested_any "")
if(any_globs)
    file(GLOB_RECURSE nested_any RELATIVE "${ZEN_REPO}" ${any_globs})
endif()
set(text_files "")
foreach(rel IN LISTS root_any nested_any)
    if(IS_DIRECTORY "${ZEN_REPO}/${rel}")
        continue()
    endif()
    zen_doc_excluded("${rel}" skip)
    if(skip)
        continue()
    endif()
    get_filename_component(name "${rel}" NAME)
    get_filename_component(ext "${rel}" LAST_EXT)
    string(REGEX REPLACE "^\\." "" ext "${ext}")
    if(ext STREQUAL "" OR name MATCHES "^\\.[A-Za-z-]+$")
        if(NOT rel MATCHES "/")
            list(APPEND text_files "${rel}")
        endif()
    elseif(ext IN_LIST ZEN_DOC_TEXT_EXTENSIONS)
        list(APPEND text_files "${rel}")
    endif()
endforeach()
list(REMOVE_DUPLICATES text_files)

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
# Comment text only, resolved against the repository root rather than the file, since a comment
# travels with its code. A file that mentions no `.md` is skipped whole.

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

# ---- population 3: paths outside this repository ---------------------------------------
#
# Every current-facing text file, read whole and raw. A hit names the file, the line and the
# spelling; the remedy is to say the thing in words, never to widen the exclusions. The
# declaring file is exempt by written rule and counted as such.

set(outside_read 0)
set(outside_exempt 0)
set(outside_leaks 0)
foreach(rel IN LISTS text_files)
    if(rel IN_LIST ZEN_DOC_OUTSIDE_DECLARERS)
        math(EXPR outside_exempt "${outside_exempt} + 1")
        continue()
    endif()
    file(READ "${ZEN_REPO}/${rel}" raw)
    math(EXPR outside_read "${outside_read} + 1")
    zen_doc_outside_hits("${raw}" hits)
    foreach(i IN LISTS hits)
        list(GET ZEN_DOC_OUTSIDE_SPELLINGS ${i} spelling)
        string(FIND "${raw}" "${spelling}" at)
        string(SUBSTRING "${raw}" 0 ${at} before)
        string(REGEX MATCHALL "\n" newlines "${before}")
        list(LENGTH newlines line0)
        math(EXPR line "${line0} + 1")
        string(REPLACE "\\" "<backslash>" shown "${spelling}")
        string(CONCAT problem "${rel}:${line}: names a path outside this repository ('${shown}')"
               "   (the external-reader rule, CONTRIBUTING.md: say the thing in words)")
        list(APPEND problems "${problem}")
        math(EXPR outside_leaks "${outside_leaks} + 1")
    endforeach()
endforeach()
if(outside_read EQUAL 0)
    message(FATAL_ERROR
        "doc-links: the sweep read ZERO current-facing text files for outside paths. An "
        "expectation of nothing is satisfied by anything; check the text-kind list and the "
        "exclusion rules at the top of this file.")
endif()

# ---- the report ------------------------------------------------------------------------

list(LENGTH pruned pruned_count)
message(STATUS "doc-links: ${md_count} markdown files (${md_excluded} excluded by rule, "
               "${pruned_count} top-level directories pruned), ${md_checked} repo-local "
               "links checked")
message(STATUS "doc-links: ${src_count} first-party C/C++ files, ${src_scanned} carrying a "
               ".md reference, ${src_refs} comment references checked")
message(STATUS "doc-links: ${outside} references counted and declined (external URL, "
               "same-file anchor, or above the repository root)")
message(STATUS "doc-links: ${outside_read} current-facing text files read whole for a path "
               "outside this repository (${outside_spelling_count} spellings; "
               "${outside_exempt} declaring file exempt by rule) -- ${outside_leaks} found")

if(NOT problems STREQUAL "")
    list(LENGTH problems problem_count)
    set(text "")
    foreach(problem IN LISTS problems)
        string(APPEND text "  ${problem}\n")
    endforeach()
    message(FATAL_ERROR
        "doc-links FAILED: ${problem_count} broken repo-local documentation reference(s) or "
        "path(s) outside this repository.\n"
        "${text}\n"
        "  Fix the reference, or -- if the document is deliberately frozen history -- widen "
        "ZEN_DOC_EXCLUDE in this file with a written rule rather than silencing one path. A "
        "path outside the repository is reworded into words, never excluded.")
endif()

message(STATUS "doc-links: PASSED -- every repo-local documentation reference resolves, and no "
               "current-facing file names a path outside this repository")
