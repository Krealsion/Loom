# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The `doc_standard` entry: does each current-facing document meet the documentation standard
# (CONTRIBUTING.md#comments-and-documents)? A private id, the development process as a clock or a
# page's history in a form `zen_doc_standard_told` names is a red that names its line; an empty
# population is a red. `doc_links` holds the paths; neither check reads a page for truth.
#   cmake -P tests/check_doc_standard.cmake    (from the repository root, or -DZEN_REPO=<repo>)

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ZEN_REPO OR ZEN_REPO STREQUAL "")
    set(ZEN_REPO "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
get_filename_component(ZEN_REPO "${ZEN_REPO}" ABSOLUTE)
if(NOT EXISTS "${ZEN_REPO}/AGENTS.md")
    message(FATAL_ERROR "doc-standard: '${ZEN_REPO}' is not this repository's root "
                        "(no AGENTS.md). Pass -DZEN_REPO=<repository root>.")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/private_ids.cmake")

# ---- scope -----------------------------------------------------------------------------
# Every *.md from the repository root, so a new folder is held the moment it exists; anything
# narrowing is a written rule. Not current-facing: build and editor state, the store, frozen
# history, rehomed source and vendored trees; and zen-vision.md, the founder's own statement of
# the project, which is kept as written.
set(ZEN_DOC_STANDARD_EXCLUDE
    "^build(-[^/]*)?/" "^cmake-build" "^_install" "^out/" "^[.]git(/|$)" "^[.]idea/" "^[.]vscode/"
    "^[.]claude/" "^docs/history/" "^archive/" "third_party/" "^zen-vision[.]md$")

string(ASCII 1 ZEN_SOH)
string(ASCII 2 ZEN_STX)
string(ASCII 3 ZEN_ETX)
string(ASCII 4 ZEN_EOT)

# A document as a CMake list of its lines, the four characters a list cannot hold swapped out.
function(zen_doc_standard_lines path out)
    file(READ "${path}" text)
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE ";" "${ZEN_SOH}" text "${text}")
    string(REPLACE "[" "${ZEN_STX}" text "${text}")
    string(REPLACE "]" "${ZEN_ETX}" text "${text}")
    string(REPLACE "\\" "${ZEN_EOT}" text "${text}")
    string(REPLACE "\n" ";" lines "${text}")
    set(${out} "${lines}" PARENT_SCOPE)
endfunction()

# What a line tells that a current-facing page never tells: the maintainers' process used as a
# clock, or the page's own history in the forms that narrated a change every time they were read
# by hand -- a heading marked RETIRED or *retired*, "used to be", "was renamed", "formerly", and a
# bold note opening "It used to" or "Retired with". A behaviour that ends ("a holder that no longer
# holds its office") and a technical phase ("the two-phase shutdown", "a phase of its own") are
# neither. Sets ${out} to the words found, or "".
function(zen_doc_standard_told line out)
    set(${out} "" PARENT_SCOPE)
    if(line MATCHES "^#+ .*(RETIRED|[*]retired[*])")
        set(${out} "a heading marked retired" PARENT_SCOPE)
        return()
    endif()
    string(TOLOWER "${line}" low)
    set(end "([^a-z'-]|$)")
    # One pattern at a time: every MATCHES in an OR chain is evaluated, and a later miss would
    # reset CMAKE_MATCH_0.
    foreach(pattern
            "(^|[^a-z-])(this|the next|a later|an earlier|the previous) phase('s)?${end}"
            "(^|[^a-z-])(that|one) phase's${end}"
            "(^|[^a-z-])a phase('s)? (that|which|whose|adds|edits|changes|touches|wrote|removed|records?)${end}"
            "(^|[^a-z-])phase (records?|reports?|prompts?)${end}"
            "(^|[^a-z])(used to be|(was|were|has been|have been) renamed|formerly)${end}"
            "[*][*](it|there|this|they) used to${end}"
            "[*][*]retired with${end}")
        if(low MATCHES "${pattern}")
            string(REGEX REPLACE "^[^a-z*]+|[^a-z']+$" "" found "${CMAKE_MATCH_0}")
            set(${out} "${found}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# Every finding in one document's lines.
function(zen_doc_standard_scan rel lines laws out)
    set(findings "")
    set(n 0)
    foreach(line IN LISTS lines)
        math(EXPR n "${n} + 1")
        zen_private_ids("${line}" "${laws}" ids)
        foreach(id IN LISTS ids)
            list(APPEND findings "${rel}:${n}: `${id}` is a development-phase name or private id -- say the fact in words, and cite a law only by an id docs/laws/ declares")
        endforeach()
        zen_doc_standard_told("${line}" told)
        if(NOT told STREQUAL "")
            list(APPEND findings "${rel}:${n}: \"${told}\" tells the maintainers' process or the page's history -- state the present fact, and leave history to docs/history/ and Git")
        endif()
    endforeach()
    set(${out} "${findings}" PARENT_SCOPE)
endfunction()

function(zen_doc_standard_matches rel patterns out)
    set(hit FALSE)
    foreach(pattern IN LISTS patterns)
        if(rel MATCHES "${pattern}")
            set(hit TRUE)
        endif()
    endforeach()
    set(${out} ${hit} PARENT_SCOPE)
endfunction()

# ---- the population ----------------------------------------------------------------------
# Excluded top-level entries are pruned before the walk, so no build tree is enumerated.
file(GLOB top RELATIVE "${ZEN_REPO}" "${ZEN_REPO}/*")
set(documents "")
foreach(entry IN LISTS top)
    set(probe "${entry}")
    if(IS_DIRECTORY "${ZEN_REPO}/${entry}")
        set(probe "${entry}/")
    endif()
    zen_doc_standard_matches("${probe}" "${ZEN_DOC_STANDARD_EXCLUDE}" skip)
    if(skip)
        continue()
    endif()
    if(IS_DIRECTORY "${ZEN_REPO}/${entry}")
        file(GLOB_RECURSE found RELATIVE "${ZEN_REPO}" "${ZEN_REPO}/${entry}/*.md")
    elseif(entry MATCHES "[.]md$")
        set(found "${entry}")
    else()
        continue()
    endif()
    foreach(rel IN LISTS found)
        zen_doc_standard_matches("${rel}" "${ZEN_DOC_STANDARD_EXCLUDE}" skip)
        if(NOT skip)
            list(APPEND documents "${rel}")
        endif()
    endforeach()
endforeach()
list(SORT documents)
list(LENGTH documents document_count)
zen_declared_laws("${ZEN_REPO}" laws)

# ---- the self-test ---------------------------------------------------------------------
zen_private_ids_self_test("doc-standard" "${laws}")
set(planted "# A page\n\nLoom routes it (MSG-09; see [x](y.md)).\nSince R2E-0 it does.\n")
string(REPLACE ";" "${ZEN_SOH}" planted "${planted}")
string(REPLACE "[" "${ZEN_STX}" planted "${planted}")
string(REPLACE "]" "${ZEN_ETX}" planted "${planted}")
string(REPLACE "\n" ";" planted "${planted}")
zen_doc_standard_scan("planted.md" "${planted}" "${laws}" got)
if(NOT got MATCHES "^planted[.]md:4: `R2E-0` " OR got MATCHES "planted[.]md:3:")
    message(FATAL_ERROR "doc-standard: self-test -- a planted page answered '${got}', want one "
                        "finding on its line 4 and none on its line 3")
endif()
# What a page never tells, and what it may: every told form is found, every ordinary sentence
# that shares a word with one passes.
foreach(told_line
        "## GATE-05 -- RETIRED: grants were mutable"
        "### Reading a value the pane had to cut -- *retired*"
        "> **It used to be `Ctrl`+`a`.** The list was an overlay a chord opened."
        "> **Retired with the object canvas.** The document this record decided is gone."
        "`Order >` was renamed from `Arrange` when a row one level up made it ambiguous."
        "The package's one process verb used to be run, wait, result, and it blocked."
        "It is formerly the host's own row."
        "Floors are minimums: a phase that adds cases raises the floor."
        "The harnesses live with the phase records, outside this repository."
        "Identity is a later phase's work."
        "Its table is in that phase's record.")
    zen_doc_standard_told("${told_line}" told)
    if(told STREQUAL "")
        message(FATAL_ERROR "doc-standard: self-test -- '${told_line}' was not found telling "
                            "history or the process; the told forms have stopped matching")
    endif()
endforeach()
foreach(plain_line
        "A holder that no longer holds its office is forgotten, and its lease with it."
        "The two-phase shutdown observes the whole group before it claims the end."
        "A run waits in a phase of its own, with the session still open."
        "Each phase of the shutdown has its own bound, and in that phase nothing is sent."
        "A retired spelling may appear in exactly one file, the checker that declares it."
        "The value used to key the map is the content id."
        "## MSG-09 -- The drain is unbounded by contract")
    zen_doc_standard_told("${plain_line}" told)
    if(NOT told STREQUAL "")
        message(FATAL_ERROR "doc-standard: self-test -- '${plain_line}' was read as telling "
                            "history or the process ('${told}'); an ordinary sentence would be refused")
    endif()
endforeach()
foreach(probe "docs/history/x.md" "zen-vision.md" "build/x.md" "build-san/x.md" "tests/third_party/x.md")
    zen_doc_standard_matches("${probe}" "${ZEN_DOC_STANDARD_EXCLUDE}" skip)
    if(NOT skip)
        message(FATAL_ERROR "doc-standard: self-test -- '${probe}' would be held")
    endif()
endforeach()
foreach(probe "README.md" "docs/README.md" "builder/x.md" "docs/historyx.md")
    zen_doc_standard_matches("${probe}" "${ZEN_DOC_STANDARD_EXCLUDE}" skip)
    if(skip)
        message(FATAL_ERROR "doc-standard: self-test -- '${probe}' would be excluded")
    endif()
endforeach()
if(document_count EQUAL 0 OR NOT "CONTRIBUTING.md" IN_LIST documents)
    message(FATAL_ERROR "doc-standard: the population (${document_count} documents) does not hold "
                        "CONTRIBUTING.md, which states the standard; the walk has stopped reading the tree")
endif()

# ---- the tree --------------------------------------------------------------------------
set(findings "")
foreach(rel IN LISTS documents)
    zen_doc_standard_lines("${ZEN_REPO}/${rel}" lines)
    zen_doc_standard_scan("${rel}" "${lines}" "${laws}" found)
    list(APPEND findings ${found})
endforeach()
list(LENGTH findings finding_count)
list(LENGTH laws law_count)
message(STATUS "doc-standard: ${document_count} documents held; ${law_count} laws declared "
               "under docs/laws/; self-test passed")
if(finding_count GREATER 0)
    string(REPLACE "${ZEN_SOH}" "\;" findings "${findings}")
    string(REPLACE "${ZEN_STX}" "[" findings "${findings}")
    string(REPLACE "${ZEN_ETX}" "]" findings "${findings}")
    string(REPLACE "${ZEN_EOT}" "\\" findings "${findings}")
    string(REPLACE ";" "\n  " shown "${findings}")
    message(FATAL_ERROR "doc-standard: ${finding_count} findings:\n  ${shown}")
endif()
message(STATUS "doc-standard: PASSED -- no private id, process clock or told history in a held document")
