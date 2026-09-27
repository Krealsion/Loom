# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The `source_comments` entry: do first-party comments meet the source comment standard
# (CONTRIBUTING.md#comments-and-documents)? A long block, a removal note or a private id is a red
# that names where the text belongs; an empty population is a red. It cannot see history, a phase
# or the maintainers' process described in words, and it does not read a comment for truth.
#   cmake -P tests/check_source_comments.cmake    (from the repository root, or -DZEN_REPO=<repo>)

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ZEN_REPO OR ZEN_REPO STREQUAL "")
    set(ZEN_REPO "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
get_filename_component(ZEN_REPO "${ZEN_REPO}" ABSOLUTE)
if(NOT EXISTS "${ZEN_REPO}/AGENTS.md")
    message(FATAL_ERROR "source-comments: '${ZEN_REPO}' is not this repository's root "
                        "(no AGENTS.md). Pass -DZEN_REPO=<repository root>.")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/private_ids.cmake")

# ---- scope -----------------------------------------------------------------------------
# The first-party roots: a directory is read whole, a file alone. Vendored code is never held.
set(ZEN_COMMENT_ROOTS CMakeLists.txt cmake examples include src tests)
set(ZEN_COMMENT_EXCLUDED "^tests/third_party/")
# Files not yet brought to the standard, as regular expressions over the repository-relative
# path. The list only shrinks: a file leaves it when its comments meet the standard.
set(ZEN_COMMENT_PENDING
    "^include/"
    "^src/"
    "^tests/.*[.](h|hpp|ipp|inl|c|cc|cpp|cxx)$"
    "^cmake/loom-weave[.]cmake$"
    "^cmake/loomConfig[.]cmake[.]in$"
    "^examples/CMakeLists[.]txt$"
    "^examples/answering[.]cpp$"
    "^examples/heartbeat[.]cpp$"
    "^examples/heartbeat_woven[.]cpp$"
    "^examples/quickstart[.]cpp$"
    "^tests/check_commit_attribution[.]cmake$"
    "^tests/check_doc_links[.]cmake$"
    "^tests/check_entry_population[.]cmake$"
    "^tests/check_population[.]cmake$"
    "^tests/check_weave_contract[.]cmake$"
    "^tests/check_weave_population[.]cmake$"
    "^tests/entry_population[.]txt$"
    "^tests/hook_return/CMakeLists[.]txt$"
    "^tests/host_process/run[.]cmake$"
    "^tests/package/CMakeLists[.]txt$"
    "^tests/package/run[.]cmake$"
    "^tests/suite_population[.]txt$"
    "^tests/verify[.]cmake$"
    "^tests/weave_population[.]cmake$")
set(ZEN_COMMENT_GLOBS *.h *.hpp *.ipp *.inl *.c *.cc *.cpp *.cxx CMakeLists.txt *.cmake
    *.cmake.in suite_population.txt entry_population.txt)
# A long block is more comment lines in a row than this -- the SPDX pair and a law pointer
# (`// MSG-09; docs/laws/messaging-laws.md`) not counted -- outside an installed header, which
# documents a public API and is spared this rule alone.
set(ZEN_COMMENT_BLOCK_LIMIT 6)

# The four characters a CMake list cannot hold, swapped for control characters no source
# carries, and restored only for printing.
string(ASCII 1 ZEN_SOH)
string(ASCII 2 ZEN_STX)
string(ASCII 3 ZEN_ETX)
string(ASCII 4 ZEN_EOT)
set(ZEN_STAR "⭐")

function(zen_comments_swap text out)
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE ";" "${ZEN_SOH}" text "${text}")
    string(REPLACE "[" "${ZEN_STX}" text "${text}")
    string(REPLACE "]" "${ZEN_ETX}" text "${text}")
    string(REPLACE "\\" "${ZEN_EOT}" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(zen_comments_show text out)
    string(REPLACE "${ZEN_SOH}" ";" text "${text}")
    string(REPLACE "${ZEN_STX}" "[" text "${text}")
    string(REPLACE "${ZEN_ETX}" "]" text "${text}")
    string(REPLACE "${ZEN_EOT}" "\\" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# The findings about one comment's text: a removal note (a star, or a note that something stood
# here once: what was removed is Git's to keep), and every private id.
function(zen_comments_judge where comment laws out)
    set(found "")
    string(TOLOWER "${comment}" lower)
    string(FIND "${comment}" "${ZEN_STAR}" star)
    if(NOT star EQUAL -1 OR lower MATCHES "(was|were) here|used to be here|what used to be")
        list(APPEND found "${where}: a removal note -- what was removed belongs to Git history, not the source")
    endif()
    zen_private_ids("${comment}" "${laws}" ids)
    foreach(id IN LISTS ids)
        list(APPEND found "${where}: `${id}` is a development-phase name or private id -- say the reason in words, and cite a law only by an id docs/laws/ declares")
    endforeach()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# One C/C++ line's comment text and whether the line is comment alone, given whether it opens
# inside a `/* */` comment; string and character literals are code. Sets <out>_comment,
# <out>_whole and <out>_open (the line ends inside a `/* */` comment).
function(zen_comments_cxx_line line open out)
    string(REGEX REPLACE "\"([^\"${ZEN_EOT}]|${ZEN_EOT}.)*\"" "\"\"" rest "${line}")
    string(REGEX REPLACE "'([^'${ZEN_EOT}]|${ZEN_EOT}.)'" "''" rest "${rest}")
    set(comment "")
    set(code "")
    while(TRUE)
        if(open)
            string(FIND "${rest}" "*/" close)
            if(close EQUAL -1)
                string(APPEND comment " ${rest}")
                set(rest "")
                break()
            endif()
            math(EXPR after "${close} + 2")
            string(SUBSTRING "${rest}" 0 ${after} part)
            string(APPEND comment " ${part}")
            string(SUBSTRING "${rest}" ${after} -1 rest)
            set(open FALSE)
        endif()
        string(FIND "${rest}" "//" slash)
        string(FIND "${rest}" "/*" star)
        if(slash EQUAL -1 AND star EQUAL -1)
            string(APPEND code "${rest}")
            break()
        endif()
        if(star EQUAL -1 OR (NOT slash EQUAL -1 AND slash LESS star))
            string(SUBSTRING "${rest}" 0 ${slash} before)
            string(SUBSTRING "${rest}" ${slash} -1 part)
            string(APPEND code "${before}")
            string(APPEND comment " ${part}")
            break()
        endif()
        string(SUBSTRING "${rest}" 0 ${star} before)
        string(APPEND code "${before}")
        string(SUBSTRING "${rest}" ${star} -1 rest)
        set(open TRUE)
    endwhile()
    set(whole FALSE)
    if(code MATCHES "^[ \t]*$" AND NOT comment STREQUAL "")
        set(whole TRUE)
    endif()
    set(${out}_comment "${comment}" PARENT_SCOPE)
    set(${out}_whole ${whole} PARENT_SCOPE)
    set(${out}_open ${open} PARENT_SCOPE)
endfunction()

# Every finding in one file's swapped text. `kind` is cxx, cmake or manifest; `exempt` is TRUE for
# an installed header. A comment line is comment alone: `//` or `/* */` text, or `#` outside a
# quoted CMake argument, or a line inside an open `/* */`. Every comment is judged for a removal
# note and a private id, a trailing one too.
function(zen_comments_scan rel kind exempt content laws out)
    set(findings "")
    string(REPLACE "\n" ";" lines "${content}")
    set(n 0)
    set(block 0)
    set(block_start 0)
    set(in_quote FALSE)
    set(open FALSE)
    foreach(line IN LISTS lines)
        math(EXPR n "${n} + 1")
        set(comment "")
        set(whole FALSE)
        if(kind STREQUAL "cmake")
            # Quoted arguments may span lines; a `#` inside one is text, not a comment.
            set(rest "${line}")
            if(in_quote)
                if(rest MATCHES "^([^\"${ZEN_EOT}]|${ZEN_EOT}.)*\"(.*)$")
                    set(rest "${CMAKE_MATCH_2}")
                    set(in_quote FALSE)
                else()
                    set(rest "")
                endif()
            endif()
            if(NOT in_quote)
                # A closed argument leaves no quote behind: a `"` still found opens one.
                string(REGEX REPLACE "\"([^\"${ZEN_EOT}]|${ZEN_EOT}.)*\"" "__" bare "${rest}")
                string(FIND "${bare}" "#" hash)
                string(FIND "${bare}" "\"" quote)
                if(NOT hash EQUAL -1 AND (quote EQUAL -1 OR hash LESS quote))
                    string(SUBSTRING "${bare}" ${hash} -1 comment)
                    string(SUBSTRING "${bare}" 0 ${hash} before)
                    if(before MATCHES "^[ \t]*$" AND rest STREQUAL line)
                        set(whole TRUE)
                    endif()
                elseif(NOT quote EQUAL -1)
                    set(in_quote TRUE)
                endif()
            endif()
        elseif(kind STREQUAL "manifest")
            # The population checks strip `#.*$` from every line: no quote protects a `#`.
            string(FIND "${line}" "#" hash)
            if(NOT hash EQUAL -1)
                string(SUBSTRING "${line}" ${hash} -1 comment)
                string(SUBSTRING "${line}" 0 ${hash} before)
                if(before MATCHES "^[ \t]*$")
                    set(whole TRUE)
                endif()
            endif()
        else()
            set(was_open ${open})
            zen_comments_cxx_line("${line}" ${open} cl)
            set(comment "${cl_comment}")
            set(whole ${cl_whole})
            set(open ${cl_open})
            if(was_open AND line MATCHES "^[ \t]*$")
                set(whole TRUE)
            endif()
        endif()
        if(whole)
            if(block EQUAL 0)
                set(block_start ${n})
            endif()
            if(NOT line MATCHES "^[ \t]*(//|#) *(SPDX-License-Identifier:|Copyright \\(c\\))" AND
               NOT line MATCHES "^[ \t]*// [A-Z]+-[0-9][0-9]([.][.][0-9][0-9]|, [A-Z]+-[0-9][0-9])*${ZEN_SOH} docs/")
                math(EXPR block "${block} + 1")
            endif()
        else()
            if(block GREATER ZEN_COMMENT_BLOCK_LIMIT AND NOT exempt)
                list(APPEND findings "${rel}:${block_start}: a comment block of ${block} lines (at most ${ZEN_COMMENT_BLOCK_LIMIT}) -- keep what a reader needs in a line and move the reasoning to its owner: a reference page, a decision record or a law")
            endif()
            set(block 0)
        endif()
        if(NOT comment STREQUAL "" AND
           NOT comment MATCHES "^[ \t]*(//|#) *(SPDX-License-Identifier:|Copyright \\(c\\))")
            zen_comments_judge("${rel}:${n}" "${comment}" "${laws}" judged)
            list(APPEND findings ${judged})
        endif()
    endforeach()
    if(block GREATER ZEN_COMMENT_BLOCK_LIMIT AND NOT exempt)
        list(APPEND findings "${rel}:${block_start}: a comment block of ${block} lines at the end of the file")
    endif()
    set(${out} "${findings}" PARENT_SCOPE)
endfunction()

# The directories and files the root CMakeLists.txt's `install(DIRECTORY include/zen/ ...)` call
# leaves out, read from the call itself: the `PATTERN "<name>" EXCLUDE` names. A pattern with a
# wildcard is not read here and is a red, so the reading cannot silently grow wrong.
function(zen_comments_install_excludes text out)
    zen_comments_swap("${text}" text)
    if(NOT text MATCHES "install\\(DIRECTORY include/zen/([^)]*)\\)")
        set(${out} "NOTFOUND" PARENT_SCOPE)
        return()
    endif()
    string(REGEX MATCHALL "PATTERN \"[^\"]*\" +EXCLUDE" excludes "${CMAKE_MATCH_1}")
    set(names "")
    foreach(exclude IN LISTS excludes)
        string(REGEX REPLACE "^PATTERN \"([^\"]*)\".*$" "\\1" name "${exclude}")
        if(name MATCHES "[*?${ZEN_STX}]")
            set(${out} "WILDCARD:${name}" PARENT_SCOPE)
            return()
        endif()
        list(APPEND names "${name}")
    endforeach()
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

# Whether a held file is an installed header: a `.h` or `.hpp` under include/zen/ with no path
# part the install call excludes.
function(zen_comments_installed rel excludes out)
    set(installed FALSE)
    if(rel MATCHES "^include/zen/.*[.](h|hpp)$")
        set(installed TRUE)
        string(REPLACE "/" ";" parts "${rel}")
        foreach(part IN LISTS parts)
            if(part IN_LIST excludes)
                set(installed FALSE)
            endif()
        endforeach()
    endif()
    set(${out} ${installed} PARENT_SCOPE)
endfunction()

function(zen_comments_kind rel out)
    set(kind cxx)
    if(rel MATCHES "(^|/)(CMakeLists\\.txt|[^/]*\\.cmake|[^/]*\\.cmake\\.in)$")
        set(kind cmake)
    elseif(rel MATCHES "(^|/)(suite|entry)_population\\.txt$")
        set(kind manifest)
    endif()
    set(${out} ${kind} PARENT_SCOPE)
endfunction()

# Whether a path is held: not vendored, not pending.
function(zen_comments_held rel out)
    set(held TRUE)
    foreach(pattern IN LISTS ZEN_COMMENT_EXCLUDED ZEN_COMMENT_PENDING)
        if(rel MATCHES "${pattern}")
            set(held FALSE)
        endif()
    endforeach()
    set(${out} ${held} PARENT_SCOPE)
endfunction()

# ---- the population ----------------------------------------------------------------------
# The globs as one name pattern, so each root is walked once.
set(ZEN_COMMENT_NAMES "")
set(separator "")
foreach(glob IN LISTS ZEN_COMMENT_GLOBS)
    string(REPLACE "." "\\." name "${glob}")
    string(REPLACE "*" "[^/]*" name "${name}")
    string(APPEND ZEN_COMMENT_NAMES "${separator}${name}")
    set(separator "|")
endforeach()
set(ZEN_COMMENT_NAMES "(^|/)(${ZEN_COMMENT_NAMES})$")
set(found_under_roots "")
foreach(root IN LISTS ZEN_COMMENT_ROOTS)
    if(IS_DIRECTORY "${ZEN_REPO}/${root}")
        file(GLOB_RECURSE found RELATIVE "${ZEN_REPO}" "${ZEN_REPO}/${root}/*")
        list(FILTER found INCLUDE REGEX "${ZEN_COMMENT_NAMES}")
        list(APPEND found_under_roots ${found})
    elseif(EXISTS "${ZEN_REPO}/${root}")
        list(APPEND found_under_roots "${root}")
    else()
        message(FATAL_ERROR "source-comments: the root '${root}' names nothing in ${ZEN_REPO}")
    endif()
endforeach()
list(REMOVE_DUPLICATES found_under_roots)
list(SORT found_under_roots)
set(population "")
set(pending_count 0)
foreach(rel IN LISTS found_under_roots)
    zen_comments_held("${rel}" held)
    if(held)
        list(APPEND population "${rel}")
    elseif(NOT rel MATCHES "${ZEN_COMMENT_EXCLUDED}")
        math(EXPR pending_count "${pending_count} + 1")
    endif()
endforeach()
list(LENGTH population population_count)

file(READ "${ZEN_REPO}/CMakeLists.txt" install_text)
zen_comments_install_excludes("${install_text}" install_excludes)
zen_declared_laws("${ZEN_REPO}" laws)

# ---- the self-test ---------------------------------------------------------------------
# Every predicate says no where it must and yes where it must, before the tree is read.
zen_private_ids_self_test("source-comments" "${laws}")
function(zen_comments_expect label kind text want)
    zen_comments_swap("${text}" swapped)
    zen_comments_scan("self-test" ${kind} FALSE "${swapped}" "${laws}" got)
    list(LENGTH got count)
    if(NOT count EQUAL want)
        zen_comments_show("${got}" shown)
        message(FATAL_ERROR "source-comments: self-test '${label}' found ${count}, want ${want}: ${shown}")
    endif()
endfunction()
set(six "// one\n// two\n// three\n// four\n// five\n// six\n")
zen_comments_expect("a seven-line block" cxx "${six}// seven\nint x;\n" 1)
zen_comments_expect("six lines, the SPDX pair and a pointer" cxx
    "// SPDX-License-Identifier: MPL-2.0\n// Copyright (c) 2026 A\n${six}// MSG-07, MSG-04; docs/laws/messaging-laws.md\nint x;\n" 0)
zen_comments_expect("a law named in prose is a counted line" cxx
    "${six}// MSG-07 says a sender speaks as an office deliberately\nint x;\n" 1)
zen_comments_expect("a seven-line /* */ comment" cxx
    "/*\n * one\n * two\n *\n * four\n * five\n */\nint x;\n" 1)
zen_comments_expect("a six-line /* */ comment" cxx "/* one\n * two\n * three\n * four\n * five\n */\nint x;\n" 0)
zen_comments_expect("an id inside /* */" cxx "/* one\n * since R2E-0 */\nint x;\n" 1)
zen_comments_expect("a /* */ comment closed on a code line" cxx "int y; /* R2E-0\n */ int x;\n" 1)
zen_comments_expect("a removal note" cxx "// ${ZEN_STAR} the old arm WAS HERE\nint x;\n" 1)
zen_comments_expect("a private id" cxx "// see VD-27 for why\nint x;\n" 1)
zen_comments_expect("declared laws and standards" cxx "// MSG-09, POP-01, UTF-8, button-1\nint x;\n" 0)
zen_comments_expect("an id in a trailing comment" cxx "int x; // P-WORK-22\n" 1)
zen_comments_expect("an id in a literal" cxx "const char* s = \"VD-27 // x\"; // fine\n" 0)
string(REPLACE "//" "#" hashes "${six}")
zen_comments_expect("a CMake block" cmake "${hashes}# seven\nset(x 1)\n" 1)
zen_comments_expect("hashes inside a quoted argument" cmake "set(x \"\n${hashes}# seven\n\")\n" 0)
zen_comments_expect("a block after a closed argument" cmake "set(x \"a\")\n${hashes}# seven\nset(y 1)\n" 1)
zen_comments_expect("a manifest block" manifest "${hashes}# seven\nswitchboard portable 22\n" 1)
zen_comments_expect("a quote protects no manifest comment" manifest
    "x portable \"a # VD-27\"\n" 1)
zen_comments_install_excludes("install(DIRECTORY include/zen/\n  FILES_MATCHING PATTERN \"*.hpp\"\n  PATTERN \"ui\"   EXCLUDE\n  PATTERN \"a.hpp\" EXCLUDE)\n" got)
if(NOT got STREQUAL "ui;a.hpp")
    message(FATAL_ERROR "source-comments: self-test 'the install call's exclusions' found '${got}', want 'ui;a.hpp'")
endif()
zen_comments_install_excludes("install(DIRECTORY include/zen/ PATTERN \"u*\" EXCLUDE)" got)
if(NOT got STREQUAL "WILDCARD:u*")
    message(FATAL_ERROR "source-comments: self-test 'a wildcard exclusion is refused' found '${got}'")
endif()
function(zen_comments_expect_path rel want_held want_kind want_installed)
    zen_comments_held("${rel}" held)
    zen_comments_kind("${rel}" kind)
    zen_comments_installed("${rel}" "ui;a.hpp" installed)
    if(NOT held STREQUAL want_held OR NOT kind STREQUAL want_kind OR NOT installed STREQUAL want_installed)
        message(FATAL_ERROR "source-comments: self-test '${rel}' is held ${held} as ${kind}, installed "
                            "${installed}; want ${want_held} as ${want_kind}, installed ${want_installed}")
    endif()
endfunction()
set(ZEN_COMMENT_PENDING_KEPT "${ZEN_COMMENT_PENDING}")
set(ZEN_COMMENT_PENDING "^src/")
zen_comments_expect_path(tests/third_party/doctest.h FALSE cxx FALSE)
zen_comments_expect_path(src/kernel/kernel.cpp FALSE cxx FALSE)
zen_comments_expect_path(CMakeLists.txt TRUE cmake FALSE)
zen_comments_expect_path(cmake/loomConfig.cmake.in TRUE cmake FALSE)
zen_comments_expect_path(tests/suite_population.txt TRUE manifest FALSE)
zen_comments_expect_path(tests/package/run.cmake TRUE cmake FALSE)
zen_comments_expect_path(include/zen/weave/weave.hpp TRUE cxx TRUE)
zen_comments_expect_path(include/zen/ui/theme.hpp TRUE cxx FALSE)
zen_comments_expect_path(include/zen/core/a.hpp TRUE cxx FALSE)
set(ZEN_COMMENT_PENDING "${ZEN_COMMENT_PENDING_KEPT}")
zen_comments_install_excludes("" nothing)
if(NOT nothing STREQUAL "NOTFOUND")
    message(FATAL_ERROR "source-comments: self-test 'no install call' found '${nothing}'")
endif()
function(zen_comments_expect_name rel want)
    set(named FALSE)
    if(rel MATCHES "${ZEN_COMMENT_NAMES}")
        set(named TRUE)
    endif()
    if(NOT named STREQUAL want)
        message(FATAL_ERROR "source-comments: self-test name '${rel}' is ${named}, want ${want}")
    endif()
endfunction()
zen_comments_expect_name(a/b/x.hpp TRUE)
zen_comments_expect_name(a/b/x.cmake.in TRUE)
zen_comments_expect_name(a/entry_population.txt TRUE)
zen_comments_expect_name(a/b/x_hpp FALSE)
zen_comments_expect_name(a/b/notes.txt FALSE)

if(population_count EQUAL 0)
    message(FATAL_ERROR "source-comments: the population is empty -- nothing held under ${ZEN_COMMENT_ROOTS}")
endif()
if(install_excludes STREQUAL "NOTFOUND" OR install_excludes MATCHES "^WILDCARD:")
    message(FATAL_ERROR "source-comments: the root CMakeLists.txt's install(DIRECTORY include/zen/ ...) "
                        "call could not be read (${install_excludes}); say here which headers are installed")
endif()
# A pointer line read from the tree now is one the block count skips.
file(STRINGS "${ZEN_REPO}/include/zen/switchboard/switchboard.hpp" pointer
    REGEX "^[ \t]*// [A-Z]+-[0-9][0-9](, [A-Z]+-[0-9][0-9])*; docs/" LIMIT_COUNT 1)
if(pointer STREQUAL "")
    message(FATAL_ERROR "source-comments: no law pointer found in include/zen/switchboard/switchboard.hpp to self-test against")
endif()
string(REPLACE "\\;" ";" pointer "${pointer}")  # file(STRINGS) escapes the separator it returns
zen_comments_expect("a pointer read from the tree" cxx "${six}${pointer}\nint x;\n" 0)

# ---- the tree --------------------------------------------------------------------------
set(findings "")
set(lines_read 0)
set(installed_count 0)
foreach(rel IN LISTS population)
    file(READ "${ZEN_REPO}/${rel}" content)
    zen_comments_swap("${content}" content)
    zen_comments_kind("${rel}" kind)
    zen_comments_installed("${rel}" "${install_excludes}" exempt)
    if(exempt)
        math(EXPR installed_count "${installed_count} + 1")
    endif()
    zen_comments_scan("${rel}" ${kind} ${exempt} "${content}" "${laws}" found)
    list(APPEND findings ${found})
    string(REGEX MATCHALL "\n" breaks "${content}")
    list(LENGTH breaks count)
    math(EXPR lines_read "${lines_read} + ${count}")
endforeach()

list(LENGTH findings finding_count)
message(STATUS "source-comments: ${population_count} files held under ${ZEN_COMMENT_ROOTS}, "
               "${lines_read} lines read; ${pending_count} files not yet held (ZEN_COMMENT_PENDING); "
               "${installed_count} installed headers exempt from the ${ZEN_COMMENT_BLOCK_LIMIT}-line "
               "block rule; self-test passed")
if(finding_count GREATER 0)
    zen_comments_show("${findings}" shown)
    string(REPLACE ";" "\n  " shown "${shown}")
    message(FATAL_ERROR "source-comments: ${finding_count} findings:\n  ${shown}")
endif()
message(STATUS "source-comments: PASSED -- no long block, removal note or private id")
