# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# What a private id is, for the two checks that refuse one: `source_comments` reads comments with
# it and `doc_standard` reads documents (CONTRIBUTING.md#comments-and-documents). Included only.

# Standards whose names are shaped like an id.
set(ZEN_NOT_IDS UTF-8 UTF-16 UTF-32 MPL-2 FNV-1a SHA-1 SHA-256 ISO-8601 IEEE-754
    TEST-NET-1 TEST-NET-2 TEST-NET-3)

# The laws docs/laws/ declares, from each register's `## MSG-09 — ...` headings: the only ids a
# comment or a document may cite.
function(zen_declared_laws repo out)
    file(GLOB registers "${repo}/docs/laws/*.md")
    set(laws "")
    foreach(register IN LISTS registers)
        file(STRINGS "${register}" headings REGEX "^## [A-Z]+-[0-9]+ ")
        foreach(heading IN LISTS headings)
            string(REGEX MATCH "^## ([A-Z]+-[0-9]+) " _ "${heading}")
            list(APPEND laws "${CMAKE_MATCH_1}")
        endforeach()
    endforeach()
    set(${out} "${laws}" PARENT_SCOPE)
endfunction()

# The private ids in a text whose `;`, `[`, `]` and `\` the caller has swapped out: upper-case parts
# joined by dashes, the last maybe followed by one lower-case letter, bounded by anything but a
# letter, digit or underscore, holding a digit somewhere, that are neither a declared law nor a
# standard's name. A capitalised compound with no digit (`READ-ONLY`) is a word; the self-test
# spells examples.
function(zen_private_ids text laws out)
    set(found "")
    string(REGEX MATCHALL "[A-Za-z0-9_]*[A-Z][A-Z0-9]*(-[A-Z0-9]+)+[a-z]?[A-Za-z0-9_]*" hits "${text}")
    foreach(hit IN LISTS hits)
        if(hit MATCHES "^[A-Z][A-Z0-9]*(-[A-Z0-9]+)+[a-z]?$" AND
           hit MATCHES "[0-9]" AND
           NOT hit IN_LIST ZEN_NOT_IDS AND NOT hit IN_LIST laws)
            list(APPEND found "${hit}")
        endif()
    endforeach()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# Every includer runs this before it reads the tree: each predicate says no where it must and yes
# where it must, so a grammar that stopped matching cannot pass a tree by finding nothing.
function(zen_private_ids_self_test who laws)
    if(NOT "MSG-09" IN_LIST laws OR NOT "POP-01" IN_LIST laws)
        message(FATAL_ERROR "${who}: self-test -- docs/laws/ declared no MSG-09 or POP-01; the "
                            "register headings are no longer read")
    endif()
    zen_private_ids("see R2E-0a, BL-VER-07, EDIT-W1, B1-B5, R2F-E and pre-WUX-12's note" "${laws}" got)
    if(NOT got STREQUAL "R2E-0a;BL-VER-07;EDIT-W1;B1-B5;R2F-E;WUX-12")
        message(FATAL_ERROR "${who}: self-test -- private ids found '${got}', want "
                            "'R2E-0a;BL-VER-07;EDIT-W1;B1-B5;R2F-E;WUX-12'")
    endif()
    zen_private_ids("MSG-09, POP-01..05, UTF-8, SHA-256, READ-ONLY, x86-64, a-1" "${laws}" got)
    if(NOT got STREQUAL "")
        message(FATAL_ERROR "${who}: self-test -- laws, standards and words read as ids: '${got}'")
    endif()
    zen_private_ids("MSG-0 and MSG-99 and MSG-9" "${laws}" got)
    if(NOT got STREQUAL "MSG-0;MSG-99;MSG-9")
        message(FATAL_ERROR "${who}: self-test -- a law family's phase tag or an undeclared law "
                            "was accepted: found '${got}'")
    endif()
endfunction()
