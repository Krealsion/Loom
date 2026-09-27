# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The required weave-contract population (POP-05), computed at configure time from the build
# graph, because target types and link closures exist only there. The roll
# LOOM_WEAVE_CONTRACT_TARGETS says which artifacts opted in; this file says which must have, and
# reads neither the roll nor LOOM_WEAVE_BUILD_CONTRACT, so emptying one leaves the other intact.
# Green requires the two to agree (docs/laws/population-laws.md).

# Required: every SHARED or MODULE library declared in tests/ (each is a fixture some suite
# dlopens, and a new one forces a decision: contract it, or exempt it in writing), every weave
# Loom itself ships, and the STATIC or OBJECT libraries in their link closures, since the contract
# covers a compilation, not a file (KERN-05). Not required: executables, interface libraries
# (walked through), and shared libraries in a closure, each its own image. A stranger's build is
# never listed here.

if(COMMAND zen_required_weave_population)
    return()
endif()

# Mark a target as deliberately outside the required population, with the reason recorded
# beside the artifact rather than in a list somewhere else -- so a rename or a deletion
# carries the exemption with it and can never go stale.
# The reason is joined from however many arguments follow the target, so it can be wrapped
# across lines at the call site the way every other explanation in this tree is, without a
# newline landing inside a manifest row.
function(zen_weave_contract_exempt target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "zen_weave_contract_exempt: '${target}' is not a target.")
    endif()
    string(JOIN "" reason ${ARGN})
    if(reason STREQUAL "")
        message(FATAL_ERROR
            "zen_weave_contract_exempt: '${target}' needs a reason. An exemption from the "
            "reloadable-weave build contract is a position this project takes on purpose; "
            "it is written down or it is not taken.")
    endif()
    # The reason travels to the -P check as one field of a `|`-delimited row, where a `;` would
    # split the row and part the reason from its target, so it is refused here. (A plain `a;b`
    # never arrives: the unquoted ${ARGN} expansion eats it. An escaped `a\;b` does.)
    if(reason MATCHES ";")
        message(FATAL_ERROR
            "zen_weave_contract_exempt: '${target}' has a reason containing a semicolon, "
            "which CMake would read as a list separator and this exemption's reason would "
            "stop belonging to this target. Rephrase it.")
    endif()
    set_property(TARGET ${target} PROPERTY ZEN_WEAVE_CONTRACT_EXEMPT "${reason}")
endfunction()

# Is this target exempt? Asked as "was the property SET", never as "is its value truthy":
# a reason that happened to read `0` or `NO` would otherwise silently stop being an
# exemption. (Detect an authored marker by asking whether it was authored -- the value here
# is prose for a human, not a flag.)
function(_zen_wp_exemption target out)
    get_target_property(value "${target}" TYPE)
    if(value STREQUAL "INTERFACE_LIBRARY")
        # An INTERFACE library has no compilation, so it is never in this population, and
        # arbitrary properties are not readable on one before CMake 3.19 anyway.
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    get_target_property(value "${target}" ZEN_WEAVE_CONTRACT_EXEMPT)
    if(value STREQUAL "value-NOTFOUND")
        set(value "")
    endif()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

# Resolve an ALIAS target to the real one; leave everything else alone.
function(_zen_wp_dealias name out)
    set(resolved "${name}")
    if(TARGET "${name}")
        get_target_property(aliased "${name}" ALIASED_TARGET)
        if(aliased)
            set(resolved "${aliased}")
        endif()
    endif()
    set(${out} "${resolved}" PARENT_SCOPE)
endfunction()

# A link entry is not always a bare target name: a static library's PRIVATE dependency
# survives in its interface as $<LINK_ONLY:x>. Unwrap that one shape, and refuse anything else
# still carrying a generator expression rather than skipping it: an unreadable link entry could
# be exactly the static library whose objects land inside the image.
function(_zen_wp_link_name entry owner out)
    set(name "${entry}")
    if(name MATCHES "^\\$<LINK_ONLY:(.+)>$")
        set(name "${CMAKE_MATCH_1}")
    endif()
    if(name MATCHES "\\$<")
        message(FATAL_ERROR
            "weave population: '${owner}' links `${entry}`, a generator expression this "
            "sweep cannot resolve to a target name. It may hide a STATIC library whose "
            "objects land inside a loadable weave image, so this is a refusal, not a skip. "
            "Teach tests/weave_population.cmake to read this shape.")
    endif()
    set(${out} "${name}" PARENT_SCOPE)
endfunction()

# LINKING is not the only way another target's objects get into an image: a source list may
# name them directly with $<TARGET_OBJECTS:x>, which the link walk below cannot see. Loom
# does not do this today, and if it ever starts, the sweep must be taught rather than
# quietly under-report -- the one thing this whole file exists to prevent.
function(_zen_wp_refuse_unreadable_objects target)
    get_target_property(sources "${target}" SOURCES)
    if(NOT sources)
        return()
    endif()
    foreach(source IN LISTS sources)
        if(source MATCHES "\\$<TARGET_OBJECTS:")
            message(FATAL_ERROR
                "weave population: loadable weave '${target}' takes objects from another "
                "target directly (`${source}`). Those compilations land inside its image "
                "and owe the build contract just as a linked static library does, but they "
                "arrive by a route the link-closure walk cannot see. Teach "
                "tests/weave_population.cmake to read this shape before using it.")
        endif()
    endforeach()
endfunction()

# Every STATIC/OBJECT library whose objects end up inside `root`'s image, found by walking
# the link closure and passing THROUGH interface targets.
function(_zen_wp_static_closure root out)
    set(pending "${root}")
    set(seen "${root}")
    set(found "")
    while(pending)
        list(POP_FRONT pending current)
        set(entries "")
        get_target_property(current_kind "${current}" TYPE)
        # LINK_LIBRARIES is not a readable property of an INTERFACE library on the CMake
        # this project supports (3.16); its interface list is.
        if(NOT current_kind STREQUAL "INTERFACE_LIBRARY")
            get_target_property(direct "${current}" LINK_LIBRARIES)
            if(direct)
                list(APPEND entries ${direct})
            endif()
        endif()
        get_target_property(interface "${current}" INTERFACE_LINK_LIBRARIES)
        if(interface)
            list(APPEND entries ${interface})
        endif()

        foreach(entry IN LISTS entries)
            _zen_wp_link_name("${entry}" "${current}" name)
            _zen_wp_dealias("${name}" name)
            if(NOT TARGET "${name}")
                continue() # a system library (dl, ws2_32) or a raw link flag
            endif()
            list(FIND seen "${name}" already)
            if(NOT already EQUAL -1)
                continue()
            endif()
            list(APPEND seen "${name}")
            get_target_property(kind "${name}" TYPE)
            if(kind STREQUAL "STATIC_LIBRARY" OR kind STREQUAL "OBJECT_LIBRARY")
                list(APPEND found "${name}")
                list(APPEND pending "${name}")
            elseif(kind STREQUAL "INTERFACE_LIBRARY")
                list(APPEND pending "${name}")
            endif()
        endforeach()
    endwhile()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# Compute the required population for the CURRENT directory's loadable weaves.
#
#   out_rows    `<REQUIRED|EXEMPT>|<target>|<why>` lines for check_weave_population.cmake
#   out_weaves  just the loadable weave targets found, so the caller can tell "this
#               configuration builds none" from "this configuration lost them"
#   out_exempt  the targets that carried a written exemption, for the same reason
function(zen_required_weave_population out_rows out_weaves out_exempt)
    get_property(dir_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    # ...AND THE LOADABLE WEAVES LOOM ITSELF SHIPS, declared at the top level (the run manager,
    # `loom-runs`). A weave built beside the suite is still a weave, and an artifact this project
    # INSTALLS is the last one that may quietly lose the contract. Its own statics join the
    # population through the same closure walk below.
    get_property(top_targets DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
    set(shipped "")
    foreach(target IN LISTS top_targets)
        get_target_property(kind "${target}" TYPE)
        if(kind STREQUAL "SHARED_LIBRARY" OR kind STREQUAL "MODULE_LIBRARY")
            list(APPEND dir_targets "${target}")
            list(APPEND shipped "${target}")
        endif()
    endforeach()

    set(weaves "")
    set(exempt_targets "")
    set(exempt_reasons "")
    foreach(target IN LISTS dir_targets)
        get_target_property(kind "${target}" TYPE)
        _zen_wp_exemption("${target}" exemption)
        if(kind STREQUAL "SHARED_LIBRARY" OR kind STREQUAL "MODULE_LIBRARY")
            _zen_wp_refuse_unreadable_objects("${target}")
            if(NOT exemption STREQUAL "")
                list(APPEND exempt_targets "${target}")
                list(APPEND exempt_reasons "${exemption}")
            else()
                list(APPEND weaves "${target}")
            endif()
        elseif(NOT exemption STREQUAL "")
            message(FATAL_ERROR
                "weave population: '${target}' is a ${kind} carrying a weave-contract "
                "exemption (\"${exemption}\"), but it was never in the required population "
                "-- so the exemption silences nothing and misdescribes the artifact. Remove "
                "it, or find out why this target stopped being a loadable weave.")
        endif()
    endforeach()

    set(rows "")
    foreach(weave IN LISTS weaves)
        list(FIND shipped "${weave}" is_shipped)
        if(is_shipped EQUAL -1)
            list(APPEND rows "REQUIRED|${weave}|loadable weave library declared in tests/")
        else()
            list(APPEND rows "REQUIRED|${weave}|loadable weave library Loom builds and installs")
        endif()
    endforeach()

    # The statics, derived from the same weaves rather than named. `linked_by` keeps the
    # diagnostic concrete: naming ONE image the library lands in beats "something links it".
    set(statics "")
    foreach(weave IN LISTS weaves)
        _zen_wp_static_closure("${weave}" reachable)
        foreach(lib IN LISTS reachable)
            list(FIND statics "${lib}" already)
            if(already EQUAL -1)
                list(APPEND statics "${lib}")
                set(_zen_wp_linked_by_${lib} "${weave}")
                set(_zen_wp_linkers_${lib} 1)
            else()
                math(EXPR _zen_wp_linkers_${lib} "${_zen_wp_linkers_${lib}} + 1")
            endif()
        endforeach()
    endforeach()
    foreach(lib IN LISTS statics)
        get_target_property(kind "${lib}" TYPE)
        _zen_wp_exemption("${lib}" exemption)
        set(why "${kind} inside loadable weave '${_zen_wp_linked_by_${lib}}'")
        if(_zen_wp_linkers_${lib} GREATER 1)
            string(APPEND why " (and ${_zen_wp_linkers_${lib}} weaves in total)")
        endif()
        if(NOT exemption STREQUAL "")
            list(APPEND exempt_targets "${lib}")
            list(APPEND exempt_reasons "${exemption}")
        else()
            list(APPEND rows "REQUIRED|${lib}|${why}")
        endif()
    endforeach()

    list(LENGTH exempt_targets exempt_count)
    if(exempt_count GREATER 0)
        math(EXPR last "${exempt_count} - 1")
        foreach(i RANGE ${last})
            list(GET exempt_targets ${i} target)
            list(GET exempt_reasons ${i} reason)
            list(APPEND rows "EXEMPT|${target}|${reason}")
        endforeach()
    endif()

    set(${out_rows} "${rows}" PARENT_SCOPE)
    set(${out_weaves} "${weaves}" PARENT_SCOPE)
    set(${out_exempt} "${exempt_targets}" PARENT_SCOPE)
endfunction()
