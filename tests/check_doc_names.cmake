# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Joshua DeMoss
#
# The `doc_names` entry (CONTRIBUTING.md#the-checks): a name still names a file or folder, a
# suite, an entry or a gate, a backticked name in parentheses after a routing-table path (an
# entry), or each law in a range, and the bullets under "The checks" are ZEN_TEXT_CHECKS.
#   cmake -P tests/check_doc_names.cmake    (from the repository root, or -DZEN_REPO=<repo>)

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ZEN_REPO OR ZEN_REPO STREQUAL "")
    set(ZEN_REPO "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
get_filename_component(ZEN_REPO "${ZEN_REPO}" ABSOLUTE)
if(NOT EXISTS "${ZEN_REPO}/AGENTS.md")
    message(FATAL_ERROR "doc-names: '${ZEN_REPO}' is not this repository's root "
                        "(no AGENTS.md). Pass -DZEN_REPO=<repository root>.")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/private_ids.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/text_checks.cmake")

# ---- scope ------------------------------------------------------------------------------------
# The pages read: the two routers and the pages a contributor is sent to. A guide names the files
# its reader makes, and a reference page or a law is read for what it says, so neither is read.
set(ZEN_NAMES_PAGES "^AGENTS[.]md$" "^CONTRIBUTING[.]md$" "^docs/CONTEXT[.]md$" "^docs/README[.]md$"
    "^docs/contributing/")
# A name in backticks is read as a path when it ends in `/` or in one of these, opens with no `/`,
# and holds nothing but a path's characters, `*` and a `{a,b}` choice; anything else is code.
set(ZEN_NAMES_EXTENSIONS cmake json yaml cpp hpp ipp cxx yml txt md py sh cc in h c)
# What is no part of the tree a name may be found in: build and editor state, and the store.
set(ZEN_NAMES_NOT_TREE "^build(-[^/]*)?$" "^cmake-build" "^_install$" "^out$" "^[.]git$" "^[.]idea$"
    "^[.]vscode$" "^[.]claude$")
# The kinds a row of tests/doc_names.txt may give a name the tree does not hold.
set(ZEN_NAMES_KINDS written generated installed)

string(ASCII 2 ZEN_NAMES_STX)   # [
string(ASCII 3 ZEN_NAMES_ETX)   # ]
string(ASCII 4 ZEN_NAMES_EOT)   # ;
string(REPLACE ";" "|" ZEN_NAMES_EXT_RX "${ZEN_NAMES_EXTENSIONS}")
set(ZEN_NAMES_PATH_RX "^[A-Za-z0-9_.*{][A-Za-z0-9_.+*{},/-]*(/|[.](${ZEN_NAMES_EXT_RX}))$")
# A path written bare in a routing table's cell: no `*`, no choice, maybe climbing with `../`.
set(ZEN_NAMES_BARE_RX "([.][.]/)*[A-Za-z0-9_][A-Za-z0-9_.+-]*(/[A-Za-z0-9_.+-]+)*([.](${ZEN_NAMES_EXT_RX})|/)")
# That path, then a parenthesis whose only contents are one backticked name: the name is an entry.
set(ZEN_NAMES_ENTRY_RX "${ZEN_NAMES_BARE_RX} \\(`[a-z][a-z0-9_]*`\\)")
set(ZEN_NAMES_ID "`[a-z][a-z0-9_]*`")
set(ZEN_NAMES_SEP "(, and | and | or |, )")
# A suite, an entry or a gate named in a sentence: the names before the word, or the names after.
set(ZEN_NAMES_SAID_RX "(${ZEN_NAMES_ID}${ZEN_NAMES_SEP})*${ZEN_NAMES_ID}( CTest)? (suites|suite|entries|entry|gates|gate)|(suites|suite|entries|entry|gates|gate) (${ZEN_NAMES_ID}${ZEN_NAMES_SEP}?)+")

# ---- the readings, one place each so the self-test exercises the real ones --------------------

# Markdown as one string no character of which reshapes a CMake list: `\` dropped, `[` and `]`
# swapped, `;` kept as a control character.
function(zen_names_swap text out)
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE "\\" "" text "${text}")
    string(REPLACE ";" "${ZEN_NAMES_EOT}" text "${text}")
    string(REPLACE "[" "${ZEN_NAMES_STX}" text "${text}")
    string(REPLACE "]" "${ZEN_NAMES_ETX}" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# Every name a `{a,b}` choice stands for.
function(zen_names_choices name out)
    set(work "${name}")
    set(found "")
    while(work)
        list(GET work 0 item)
        list(REMOVE_AT work 0)
        if(item MATCHES "^([^{}]*)[{]([^{}]*)[}](.*)$")
            set(before "${CMAKE_MATCH_1}")
            set(body "${CMAKE_MATCH_2}")
            set(after "${CMAKE_MATCH_3}")
            string(REPLACE "," ";" parts "${body}")
            foreach(part IN LISTS parts)
                list(APPEND work "${before}${part}${after}")
            endforeach()
        else()
            list(APPEND found "${item}")
        endif()
    endwhile()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# Is `tail` the end of a line of the tree's list (ZEN_NAMES_TREE: one `/path` a line), after
# `lead`? A folder's name ends in `/` and is found as the start of what follows it; a `*` stands
# for any run of a file name's characters.
function(zen_names_in_tree lead tail out)
    set(end "\n")
    if(tail MATCHES "/$")
        set(end "")
    endif()
    if(tail MATCHES "[*]")
        string(REPLACE "." "[.]" rx "${lead}${tail}")
        string(REPLACE "+" "[+]" rx "${rx}")
        string(REPLACE "*" "[^/\n]*" rx "${rx}")
        string(REGEX MATCH "${rx}${end}" hit "${ZEN_NAMES_TREE}")
        if(hit STREQUAL "")
            set(${out} FALSE PARENT_SCOPE)
        else()
            set(${out} TRUE PARENT_SCOPE)
        endif()
        return()
    endif()
    string(FIND "${ZEN_NAMES_TREE}" "${lead}${tail}${end}" at)
    if(at EQUAL -1)
        set(${out} FALSE PARENT_SCOPE)
    else()
        set(${out} TRUE PARENT_SCOPE)
    endif()
endfunction()

# Does a path name something? Read from the root, then from the page's folder `dir`; a name that
# opens with no top-level folder is also the tail of a path in the tree (`test_gate.cpp`,
# `zen/kernel/control.hpp`), and one opening with `_` or `*` the tail of a file's name. `./` and
# `../` are read from `dir` alone, and a name that climbs above the root is another repository's,
# not asked. Each name of a `{a,b}` choice is asked. `out` is empty when it resolves, else the
# first name that does not.
function(zen_names_resolve name dir out)
    set(${out} "" PARENT_SCOPE)
    zen_names_choices("${name}" choices)
    foreach(one IN LISTS choices)
        set(anchored FALSE)
        if(one MATCHES "^[.][.]?/")
            get_filename_component(abs "${dir}/${one}" ABSOLUTE BASE_DIR "/zen-names-root")
            if(NOT abs MATCHES "^/zen-names-root/(.+)$")
                continue()
            endif()
            set(rest "${CMAKE_MATCH_1}")
            if(one MATCHES "/$")
                string(APPEND rest "/")
            endif()
            set(one "${rest}")
            set(anchored TRUE)
        endif()
        zen_names_in_tree("\n/" "${one}" hit)
        if(NOT hit AND NOT anchored AND NOT dir STREQUAL "")
            zen_names_in_tree("\n/${dir}/" "${one}" hit)
        endif()
        if(NOT hit AND NOT anchored)
            string(REGEX REPLACE "/.*$" "" first "${one}")
            string(REGEX REPLACE "/$" "" inner "${one}")
            if(one MATCHES "^[_*]")
                zen_names_in_tree("" "${one}" hit)
            elseif(NOT inner MATCHES "/" OR NOT first IN_LIST ZEN_NAMES_TOP)
                zen_names_in_tree("/" "${one}" hit)
            endif()
        endif()
        if(NOT hit)
            set(${out} "${one}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# Is an unresolved name one tests/doc_names.txt holds: a row of that name, or a row ending in `/`
# that the name opens with? The row is marked used.
function(zen_names_excepted name out)
    set(${out} FALSE PARENT_SCOPE)
    foreach(row IN LISTS ZEN_NAMES_ROWS)
        string(LENGTH "${row}" len)
        string(LENGTH "${name}" name_len)
        set(held FALSE)
        if(row STREQUAL name)
            set(held TRUE)
        elseif(row MATCHES "/$" AND name_len GREATER len)
            string(SUBSTRING "${name}" 0 ${len} head)
            if(head STREQUAL row)
                set(held TRUE)
            endif()
        endif()
        if(held)
            set_property(GLOBAL APPEND PROPERTY ZEN_NAMES_USED "${row}")
            set(${out} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# One path of a page, asked: a problem when it is neither the tree's nor a row's.
function(zen_names_ask_path label n name dir)
    zen_names_resolve("${name}" "${dir}" missing)
    if(missing STREQUAL "")
        return()
    endif()
    zen_names_excepted("${name}" excepted)
    if(excepted)
        return()
    endif()
    set(what "names no file or folder of this tree")
    if(NOT missing STREQUAL name)
        set(what "names no file or folder of this tree (${missing})")
    endif()
    set_property(GLOBAL APPEND PROPERTY ZEN_NAMES_PROBLEMS "${label}:${n}: `${name}` ${what}")
endfunction()

# One page, read: each problem, `<page>:<line>: `<name>` <what>`, appended to the global
# ZEN_NAMES_PROBLEMS, the names read counted in ZEN_NAMES_READ. A fenced block is code and is not
# read; a span left open at a line's end closes on the next. A sentence's suites are read with
# the line after it, since a list wraps, and each is told at the line it stands on.
function(zen_names_page label dir text)
    string(REPLACE "\n" ";" lines "${text}")
    list(APPEND lines "")
    list(LENGTH lines count)
    math(EXPR last "${count} - 2")
    set(read 0)
    set(fence FALSE)
    set(carry FALSE)
    set(routing FALSE)
    foreach(i RANGE 0 ${last})
        list(GET lines ${i} line)
        math(EXPR n "${i} + 1")
        math(EXPR j "${i} + 1")
        list(GET lines ${j} next)
        if(line MATCHES "^[ \t]*```")
            if(fence)
                set(fence FALSE)
            else()
                set(fence TRUE)
            endif()
            set(carry FALSE)
            continue()
        endif()
        if(fence)
            continue()
        endif()
        if(line MATCHES "^[ \t]*$")
            set(carry FALSE)
            set(routing FALSE)
            continue()
        endif()
        if(next MATCHES "^[ \t]*(```|$)")
            set(next "")
        endif()

        # ---- a range of laws: every law in it is declared -------------------------------------
        string(REGEX MATCHALL "[A-Z][A-Z]+-[0-9][0-9][.][.][0-9][0-9]" ranges "${line}")
        foreach(range IN LISTS ranges)
            string(REGEX MATCH "^([A-Z]+)-([0-9][0-9])[.][.]([0-9][0-9])$" matched "${range}")
            set(family "${CMAKE_MATCH_1}")
            math(EXPR low "1${CMAKE_MATCH_2} - 100")
            math(EXPR high "1${CMAKE_MATCH_3} - 100")
            math(EXPR read "${read} + 1")
            if(high LESS low)
                set_property(GLOBAL APPEND PROPERTY ZEN_NAMES_PROBLEMS "${label}:${n}: `${range}` runs backwards")
                continue()
            endif()
            foreach(k RANGE ${low} ${high})
                set(id "${family}-${k}")
                if(k LESS 10)
                    set(id "${family}-0${k}")
                endif()
                if(NOT id IN_LIST ZEN_NAMES_LAWS)
                    set_property(GLOBAL APPEND PROPERTY ZEN_NAMES_PROBLEMS
                        "${label}:${n}: `${range}` names ${id}, which docs/laws/ does not declare")
                endif()
            endforeach()
        endforeach()

        # ---- a suite, an entry or a gate a sentence names ---------------------------------------
        string(LENGTH "${line}" line_len)
        set(rest "${line} ${next}")
        set(base 0)
        while(rest MATCHES "${ZEN_NAMES_SAID_RX}")
            set(said "${CMAKE_MATCH_0}")
            string(FIND "${rest}" "${said}" at)
            math(EXPR start "${base} + ${at}")
            if(NOT start LESS line_len)
                break()
            endif()
            string(LENGTH "${said}" said_len)
            math(EXPR cut "${at} + ${said_len}")
            string(SUBSTRING "${rest}" ${cut} -1 rest)
            math(EXPR base "${base} + ${cut}")
            set(population "${ZEN_NAMES_SUITES}")
            set(kind "a suite or a CTest entry, and tests/suite_population.txt and tests/entry_population.txt declare none of that name")
            if(said MATCHES "(^| )gates?( |$)")
                set(population "${ZEN_NAMES_GATES}")
                set(kind "a gate, and no row of tests/suite_population.txt or tests/entry_population.txt rides it")
            endif()
            string(REGEX MATCHALL "${ZEN_NAMES_ID}" ids "${said}")
            foreach(id IN LISTS ids)
                math(EXPR read "${read} + 1")
                string(REPLACE "`" "" bare "${id}")
                if(bare IN_LIST population)
                    continue()
                endif()
                set(at_line ${n})
                string(FIND "${line}" "${id}" here)
                if(here EQUAL -1)
                    math(EXPR at_line "${n} + 1")
                endif()
                set_property(GLOBAL APPEND PROPERTY ZEN_NAMES_PROBLEMS "${label}:${at_line}: `${bare}` is named as ${kind}")
            endforeach()
        endwhile()

        # ---- a routing table's bare paths: every cell after the topic's -----------------------
        if(line MATCHES "^[|]")
            if(line MATCHES "^[|] *Topic *[|] *Read *[|]")
                set(routing TRUE)
            elseif(routing AND NOT line MATCHES "^[|][-| :]+$")
                string(REGEX REPLACE "^[|][^|]*[|]" "" cells "${line}")
                # Before the backticks go: a bare path then (`name`) names an entry.
                string(REGEX MATCHALL "${ZEN_NAMES_ENTRY_RX}" paren_entries "${cells}")
                foreach(paren_entry IN LISTS paren_entries)
                    math(EXPR read "${read} + 1")
                    if(NOT paren_entry MATCHES "\\(`([a-z][a-z0-9_]*)`\\)$")
                        continue()
                    endif()
                    set(bare "${CMAKE_MATCH_1}")
                    if(bare IN_LIST ZEN_NAMES_SUITES)
                        continue()
                    endif()
                    set_property(GLOBAL APPEND PROPERTY ZEN_NAMES_PROBLEMS
                        "${label}:${n}: `${bare}` is named as a suite or a CTest entry, and tests/suite_population.txt and tests/entry_population.txt declare none of that name")
                endforeach()
                string(REGEX REPLACE "`[^`]*`" " " cells "${cells}")
                string(REGEX REPLACE "https?://[^ )]*" " " cells "${cells}")
                string(REGEX MATCHALL "${ZEN_NAMES_BARE_RX}" bare_paths "${cells}")
                foreach(name IN LISTS bare_paths)
                    math(EXPR read "${read} + 1")
                    zen_names_ask_path("${label}" ${n} "${name}" "${dir}")
                endforeach()
            endif()
        else()
            set(routing FALSE)
        endif()

        # ---- paths in backticks -----------------------------------------------------------------
        if(NOT line MATCHES "`")
            continue()
        endif()
        set(rest "${line}")
        if(carry)
            string(FIND "${rest}" "`" at)
            math(EXPR at "${at} + 1")
            string(SUBSTRING "${rest}" ${at} -1 rest)
            set(carry FALSE)
        endif()
        string(REGEX MATCHALL "`[^`]+`" spans "${rest}")
        string(REGEX REPLACE "`[^`]+`" "" left "${rest}")
        if(left MATCHES "`")
            set(carry TRUE)
        endif()
        foreach(span IN LISTS spans)
            string(REPLACE "`" "" name "${span}")
            if(NOT name MATCHES "${ZEN_NAMES_PATH_RX}" OR name MATCHES "[.][.][.]")
                continue()
            endif()
            math(EXPR read "${read} + 1")
            zen_names_ask_path("${label}" ${n} "${name}" "${dir}")
        endforeach()
    endforeach()
    get_property(total GLOBAL PROPERTY ZEN_NAMES_READ)
    math(EXPR total "0${total} + ${read}")
    set_property(GLOBAL PROPERTY ZEN_NAMES_READ ${total})
endfunction()

# The bullets under "### The checks", to the next heading. The first backticked name on each is a
# check, and the set is exactly `checks` (ZEN_TEXT_CHECKS on the real page). No heading names
# every check missing. A second occurrence is twice. Either difference is a problem.
function(zen_names_checks_held label raw checks)
    zen_names_swap("${raw}" text)
    string(REPLACE "\n" ";" lines "${text}")
    set(in FALSE)
    set(heading 0)
    set(named "")
    set(where "")
    set(n 0)
    set(read 0)
    foreach(line IN LISTS lines)
        math(EXPR n "${n} + 1")
        if(NOT in)
            if(line MATCHES "^### The checks[ \t]*$")
                set(in TRUE)
                set(heading ${n})
            endif()
            continue()
        endif()
        if(line MATCHES "^#+ ")
            break()
        endif()
        if(NOT line MATCHES "^[ \t]*- ")
            continue()
        endif()
        string(REGEX MATCH "`[^`]*`" span "${line}")
        if(span STREQUAL "")
            continue()
        endif()
        string(REPLACE "`" "" name "${span}")
        if(NOT name MATCHES "^[a-z][a-z0-9_]*$")
            continue()
        endif()
        math(EXPR read "${read} + 1")
        if(name IN_LIST named)
            set_property(GLOBAL APPEND PROPERTY ZEN_NAMES_PROBLEMS
                "${label}:${n}: `${name}` is named twice in this list")
            continue()
        endif()
        list(APPEND named "${name}")
        list(APPEND where "${n}")
    endforeach()
    if(heading EQUAL 0)
        foreach(check IN LISTS checks)
            set_property(GLOBAL APPEND PROPERTY ZEN_NAMES_PROBLEMS
                "${label}:1: `${check}` is one of ZEN_TEXT_CHECKS and this list does not name it")
        endforeach()
    else()
        foreach(check IN LISTS checks)
            if(check IN_LIST named)
                continue()
            endif()
            set_property(GLOBAL APPEND PROPERTY ZEN_NAMES_PROBLEMS
                "${label}:${heading}: `${check}` is one of ZEN_TEXT_CHECKS and this list does not name it")
        endforeach()
        list(LENGTH named named_count)
        if(named_count GREATER 0)
            math(EXPR last "${named_count} - 1")
            foreach(i RANGE 0 ${last})
                list(GET named ${i} name)
                list(GET where ${i} at)
                if(name IN_LIST checks)
                    continue()
                endif()
                set_property(GLOBAL APPEND PROPERTY ZEN_NAMES_PROBLEMS
                    "${label}:${at}: `${name}` is named in this list and is not one of ZEN_TEXT_CHECKS")
            endforeach()
        endif()
    endif()
    get_property(total GLOBAL PROPERTY ZEN_NAMES_READ)
    math(EXPR total "0${total} + ${read}")
    set_property(GLOBAL PROPERTY ZEN_NAMES_READ ${total})
endfunction()

# ---- the self-test: a canary page over a canary tree ------------------------------------------
# Each line of the canary holds names that resolve beside names that do not, and the reading must
# name exactly the second kind, by page, line and name, before the tree is asked anything.
set(ZEN_NAMES_TREE "\n/tests/test_gate.cpp\n/tests/test_schema.cpp\n/tests/session/test_client.py\n/docs/reference/kernel.md\n/docs/laws/kernel-laws.md\n/include/zen/kernel/control.hpp\n/src/host/door.hpp\n")
set(ZEN_NAMES_TOP tests docs include src)
set(ZEN_NAMES_SUITES kernel policy all population)
set(ZEN_NAMES_GATES portable kernel)
set(ZEN_NAMES_LAWS MSG-01 MSG-02 MSG-03)
set(ZEN_NAMES_ROWS "made.json" "share/loom/")
string(CONCAT st_page
    "# canary\n"                                                                              # 1
    "\n"
    "`tests/test_gate.cpp`, `tests/test_gone.cpp`, `test_schema.cpp`, `zen/kernel/control.hpp`, a `word`\n"  # 3
    "`tests/test_{gate,schema}.cpp`, `tests/test_{gate,lost}.cpp`, `tests/test_*.cpp`, `src/lost_*.hpp`\n"  # 4
    "`reference/kernel.md`, `../src/host/door.hpp`, `../src/host/lost.hpp`, `../../other/page.md`\n"  # 5
    "`made.json`, `stray.json`, `share/loom/tools/`, `tests/door.hpp`, `session/`, `lost/`, a span `left\n"  # 6
    "open.md` then `src/host/door.hpp`, and `docs/...md`\n"                                    # 7
    "```\n"
    "`tests/fenced.cpp` and MSG-01..09\n"                                                       # 9
    "```\n"
    "\n"
    "Laws MSG-01..03 hold, MSG-02..05 do not, SHA-256 is no law, and MSG-03..01 is no range.\n"  # 12
    "\n"
    "The `kernel`, `policy` and `gone_a` suites, the `population` CTest entry, suite `gone_b`,\n"  # 14
    "entries `all`, `kernel`,\n"                                                               # 15
    "`gone_c` and `policy`, run together. Gate `portable` is none: gate `portable`, gate `gone_d`.\n"  # 16
    "A `word` alone names no suite, nor does the `gone_e` case.\n"                              # 17
    "\n"
    "| Topic | Read | Laws | Tests | Why |\n"                                                  # 19
    "|---|---|---|---|---|\n"
    "| a topic/ of topic/lost.md | reference/kernel.md#an-anchor, reference/lost.md | MSG-01 | tests/test_gate.cpp, test_schema.cpp, session/test_client.py, test_lost.cpp, lost_dir/, tests/test_gate.cpp (`population`), tests/test_gate.cpp (`gone_f`), tests/test_gate.cpp (the `gone_g` cases) | (why)(laws/kernel-laws.md), ../include/zen/kernel/control.hpp, https://example.org/a/b.md |\n"  # 21
    "\n"
    "| Other | table |\n"
    "|---|---|\n"
    "| x | bare/lost.md is no routing cell |\n")
zen_names_swap("${st_page}" st_text)
set_property(GLOBAL PROPERTY ZEN_NAMES_PROBLEMS "")
set_property(GLOBAL PROPERTY ZEN_NAMES_USED "")
set_property(GLOBAL PROPERTY ZEN_NAMES_READ 0)
zen_names_page("canary/page.md" "docs" "${st_text}")
get_property(st_problems GLOBAL PROPERTY ZEN_NAMES_PROBLEMS)
set(st_want
    "canary/page.md:3: `tests/test_gone.cpp` names no file or folder of this tree"
    "canary/page.md:4: `tests/test_{gate,lost}.cpp` names no file or folder of this tree (tests/test_lost.cpp)"
    "canary/page.md:4: `src/lost_*.hpp` names no file or folder of this tree"
    "canary/page.md:5: `../src/host/lost.hpp` names no file or folder of this tree (src/host/lost.hpp)"
    "canary/page.md:6: `stray.json` names no file or folder of this tree"
    "canary/page.md:6: `tests/door.hpp` names no file or folder of this tree"
    "canary/page.md:6: `lost/` names no file or folder of this tree"
    "canary/page.md:12: `MSG-02..05` names MSG-04, which docs/laws/ does not declare"
    "canary/page.md:12: `MSG-02..05` names MSG-05, which docs/laws/ does not declare"
    "canary/page.md:12: `MSG-03..01` runs backwards"
    "canary/page.md:14: `gone_a` is named as a suite or a CTest entry, and tests/suite_population.txt and tests/entry_population.txt declare none of that name"
    "canary/page.md:14: `gone_b` is named as a suite or a CTest entry, and tests/suite_population.txt and tests/entry_population.txt declare none of that name"
    "canary/page.md:16: `gone_c` is named as a suite or a CTest entry, and tests/suite_population.txt and tests/entry_population.txt declare none of that name"
    "canary/page.md:16: `gone_d` is named as a gate, and no row of tests/suite_population.txt or tests/entry_population.txt rides it"
    "canary/page.md:21: `gone_f` is named as a suite or a CTest entry, and tests/suite_population.txt and tests/entry_population.txt declare none of that name"
    "canary/page.md:21: `reference/lost.md` names no file or folder of this tree"
    "canary/page.md:21: `test_lost.cpp` names no file or folder of this tree"
    "canary/page.md:21: `lost_dir/` names no file or folder of this tree")
if(NOT st_problems STREQUAL st_want)
    string(REPLACE ";" "\n  " st_shown "${st_problems}")
    string(REPLACE ";" "\n  " st_wanted "${st_want}")
    message(FATAL_ERROR "doc-names: SELF-TEST FAILED -- the canary page read as\n  ${st_shown}\nwant\n  ${st_wanted}")
endif()
get_property(st_used GLOBAL PROPERTY ZEN_NAMES_USED)
if(NOT st_used STREQUAL "made.json;share/loom/")
    message(FATAL_ERROR "doc-names: SELF-TEST FAILED -- the canary used the rows '${st_used}', "
                        "want 'made.json;share/loom/'")
endif()
set_property(GLOBAL PROPERTY ZEN_NAMES_PROBLEMS "")
set_property(GLOBAL PROPERTY ZEN_NAMES_USED "")
set_property(GLOBAL PROPERTY ZEN_NAMES_READ 0)

# The list's canary: a missing name and an extra name are red, and the same names are green.
set(st_checks "doc_links;doc_names")
string(CONCAT st_list
    "### The checks\n"
    "\n"
    "- `doc_links` refuses a link.\n"
    "- `extra_one` refuses something.\n")
zen_names_checks_held("canary/checks.md" "${st_list}" "${st_checks}")
get_property(st_problems GLOBAL PROPERTY ZEN_NAMES_PROBLEMS)
set(st_want
    "canary/checks.md:1: `doc_names` is one of ZEN_TEXT_CHECKS and this list does not name it"
    "canary/checks.md:4: `extra_one` is named in this list and is not one of ZEN_TEXT_CHECKS")
if(NOT st_problems STREQUAL st_want)
    string(REPLACE ";" "\n  " st_shown "${st_problems}")
    string(REPLACE ";" "\n  " st_wanted "${st_want}")
    message(FATAL_ERROR "doc-names: SELF-TEST FAILED -- the check list read as\n  ${st_shown}\nwant\n  ${st_wanted}")
endif()
string(CONCAT st_list
    "### The checks\n"
    "\n"
    "- `doc_names` refuses a name.\n"
    "- `doc_links` refuses a link.\n")
set_property(GLOBAL PROPERTY ZEN_NAMES_PROBLEMS "")
zen_names_checks_held("canary/checks-green.md" "${st_list}" "${st_checks}")
get_property(st_problems GLOBAL PROPERTY ZEN_NAMES_PROBLEMS)
if(NOT st_problems STREQUAL "")
    message(FATAL_ERROR "doc-names: SELF-TEST FAILED -- a list that matches read as '${st_problems}'")
endif()
string(CONCAT st_list
    "### The checks\n"
    "\n"
    "- `doc_links` once.\n"
    "- `doc_names` once.\n"
    "- `doc_links` again.\n")
set_property(GLOBAL PROPERTY ZEN_NAMES_PROBLEMS "")
zen_names_checks_held("canary/checks-twice.md" "${st_list}" "${st_checks}")
get_property(st_problems GLOBAL PROPERTY ZEN_NAMES_PROBLEMS)
set(st_want
    "canary/checks-twice.md:5: `doc_links` is named twice in this list")
if(NOT st_problems STREQUAL st_want)
    string(REPLACE ";" "\n  " st_shown "${st_problems}")
    string(REPLACE ";" "\n  " st_wanted "${st_want}")
    message(FATAL_ERROR "doc-names: SELF-TEST FAILED -- a repeated check read as\n  ${st_shown}\nwant\n  ${st_wanted}")
endif()
set(st_list "A page with no such heading.\n- `doc_links` sits outside it.\n")
set_property(GLOBAL PROPERTY ZEN_NAMES_PROBLEMS "")
zen_names_checks_held("canary/checks-none.md" "${st_list}" "${st_checks}")
get_property(st_problems GLOBAL PROPERTY ZEN_NAMES_PROBLEMS)
set(st_want
    "canary/checks-none.md:1: `doc_links` is one of ZEN_TEXT_CHECKS and this list does not name it"
    "canary/checks-none.md:1: `doc_names` is one of ZEN_TEXT_CHECKS and this list does not name it")
if(NOT st_problems STREQUAL st_want)
    string(REPLACE ";" "\n  " st_shown "${st_problems}")
    string(REPLACE ";" "\n  " st_wanted "${st_want}")
    message(FATAL_ERROR "doc-names: SELF-TEST FAILED -- a page with no check list read as\n  ${st_shown}\nwant\n  ${st_wanted}")
endif()
set_property(GLOBAL PROPERTY ZEN_NAMES_PROBLEMS "")
set_property(GLOBAL PROPERTY ZEN_NAMES_USED "")
set_property(GLOBAL PROPERTY ZEN_NAMES_READ 0)

# ---- the tree ---------------------------------------------------------------------------------
file(GLOB top RELATIVE "${ZEN_REPO}" "${ZEN_REPO}/*" "${ZEN_REPO}/.*")
set(ZEN_NAMES_TOP "")
set(tree_files "")
foreach(entry IN LISTS top)
    set(skip FALSE)
    foreach(rx IN LISTS ZEN_NAMES_NOT_TREE)
        if(entry MATCHES "${rx}")
            set(skip TRUE)
        endif()
    endforeach()
    if(skip OR entry STREQUAL "." OR entry STREQUAL "..")
        continue()
    endif()
    if(IS_DIRECTORY "${ZEN_REPO}/${entry}")
        list(APPEND ZEN_NAMES_TOP "${entry}")
        file(GLOB_RECURSE found RELATIVE "${ZEN_REPO}" "${ZEN_REPO}/${entry}/*")
        list(APPEND tree_files ${found})
    else()
        list(APPEND tree_files "${entry}")
    endif()
endforeach()
set(page_files "")
foreach(file IN LISTS tree_files)
    if(NOT file MATCHES "[.]md$")
        continue()
    endif()
    foreach(rx IN LISTS ZEN_NAMES_PAGES)
        if(file MATCHES "${rx}")
            list(APPEND page_files "${file}")
            break()
        endif()
    endforeach()
endforeach()
string(REPLACE ";" "\n/" ZEN_NAMES_TREE "${tree_files}")
set(ZEN_NAMES_TREE "\n/${ZEN_NAMES_TREE}\n")

# ---- the populations and the laws -------------------------------------------------------------
set(ZEN_NAMES_SUITES "")
set(ZEN_NAMES_GATES "")
foreach(manifest suite_population.txt entry_population.txt)
    file(STRINGS "${ZEN_REPO}/tests/${manifest}" rows REGEX "^[a-z]")
    foreach(row IN LISTS rows)
        if(row MATCHES "^([a-z][a-z0-9_]*)[ \t]+([a-z]+)")
            list(APPEND ZEN_NAMES_SUITES "${CMAKE_MATCH_1}")
            list(APPEND ZEN_NAMES_GATES "${CMAKE_MATCH_2}")
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES ZEN_NAMES_SUITES)
list(REMOVE_DUPLICATES ZEN_NAMES_GATES)
zen_declared_laws("${ZEN_REPO}" ZEN_NAMES_LAWS)
list(LENGTH tree_files tree_count)
list(LENGTH page_files page_count)
list(LENGTH ZEN_NAMES_SUITES suite_count)
list(LENGTH ZEN_NAMES_GATES gate_count)
list(LENGTH ZEN_NAMES_LAWS law_count)
if(tree_count EQUAL 0 OR page_count EQUAL 0 OR suite_count EQUAL 0 OR gate_count EQUAL 0 OR law_count EQUAL 0)
    message(FATAL_ERROR "doc-names: ${tree_count} files, ${page_count} pages, ${suite_count} suites and "
                        "entries, ${gate_count} gates and ${law_count} laws read; an empty population "
                        "is a failure, not a pass (POP-01)")
endif()

# ---- the names the tree does not hold ---------------------------------------------------------
set(problems "")
set(ZEN_NAMES_ROWS "")
file(STRINGS "${ZEN_REPO}/tests/doc_names.txt" name_rows REGEX "^[^#]")
foreach(row IN LISTS name_rows)
    if(NOT row MATCHES "^([^ \t]+)[ \t]+([a-z]+)[ \t]*$")
        list(APPEND problems "tests/doc_names.txt: '${row}' is not `name kind`")
        continue()
    endif()
    set(row_name "${CMAKE_MATCH_1}")
    set(row_kind "${CMAKE_MATCH_2}")
    if(NOT row_kind IN_LIST ZEN_NAMES_KINDS)
        string(REPLACE ";" ", " kinds "${ZEN_NAMES_KINDS}")
        list(APPEND problems "tests/doc_names.txt: ${row_name}'s kind '${row_kind}' is none of ${kinds}")
    endif()
    if(row_name IN_LIST ZEN_NAMES_ROWS)
        list(APPEND problems "tests/doc_names.txt names ${row_name} twice")
    endif()
    list(APPEND ZEN_NAMES_ROWS "${row_name}")
endforeach()

# ---- the pages --------------------------------------------------------------------------------
foreach(page IN LISTS page_files)
    file(READ "${ZEN_REPO}/${page}" text)
    if(page STREQUAL "CONTRIBUTING.md")
        zen_names_checks_held("${page}" "${text}" "${ZEN_TEXT_CHECKS}")
    endif()
    zen_names_swap("${text}" text)
    get_filename_component(dir "${page}" DIRECTORY)
    zen_names_page("${page}" "${dir}" "${text}")
endforeach()
get_property(found GLOBAL PROPERTY ZEN_NAMES_PROBLEMS)
list(APPEND problems ${found})
get_property(used GLOBAL PROPERTY ZEN_NAMES_USED)
foreach(row IN LISTS ZEN_NAMES_ROWS)
    if(NOT row IN_LIST used)
        list(APPEND problems "tests/doc_names.txt: no page read names ${row} unresolved -- a row the tree or the pages no longer need is removed")
    endif()
endforeach()

get_property(read GLOBAL PROPERTY ZEN_NAMES_READ)
list(LENGTH ZEN_NAMES_ROWS row_count)
message(STATUS "doc-names: ${read} names read in ${page_count} pages against ${tree_count} files, "
               "${suite_count} suites and entries, ${gate_count} gates and ${law_count} laws; "
               "${row_count} names the tree does not hold")
if(read EQUAL 0)
    message(FATAL_ERROR "doc-names: no name read; an empty population is a failure, not a pass (POP-01)")
endif()
if(problems)
    list(LENGTH problems problem_count)
    string(REPLACE ";" "\n  " shown "${problems}")
    message(FATAL_ERROR "doc-names FAILED: ${problem_count} problem(s):\n  ${shown}\n"
                        "A path a page names is a file or folder of this tree, read from the root, from "
                        "the page's folder or as the tail of a path, or a row of tests/doc_names.txt; a "
                        "suite, an entry or a gate it names is one the population files declare, and so "
                        "is a backticked name in parentheses after a path in the routing table; the "
                        "bullets under The checks are ZEN_TEXT_CHECKS; a range of laws names laws "
                        "docs/laws/ declares.")
endif()
message(STATUS "doc-names: PASSED -- every path the routers and the contributor's pages name is in the "
               "tree or in tests/doc_names.txt, every suite, entry and gate they name is declared, a "
               "backticked name in parentheses after a routing-table path is an entry that is too, the "
               "bullets under The checks are ZEN_TEXT_CHECKS, and every range of laws is whole")
