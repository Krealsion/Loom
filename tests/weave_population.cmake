# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# THE REQUIRED WEAVE-CONTRACT POPULATION (POP-05, C3) -- computed at CONFIGURE time,
# because target types and link graphs only exist there.
#
# WHY THIS EXISTS. `loom_weave_build_contract()` records every target it touches on the
# global LOOM_WEAVE_CONTRACT_TARGETS roll, and check_weave_contract.cmake then proves each
# of those artifacts really is unique-symbol-free. That is a complete answer to "is every
# contracted artifact reload-safe" and NO answer at all to "is every artifact that must be
# reload-safe contracted". COLD-2 finding C-3 measured the gap: deleting one line from
# zen_add_test_weave() dropped twenty-three loadable weaves off the roll, thirteen
# STB_GNU_UNIQUE symbols came back into libzen_test_weave.so, and the official lane stayed
# green at 33/33 -- because a derived list cannot detect its own absences (POP-01).
#
# WHAT MAKES THIS INDEPENDENT. Nothing in this file reads LOOM_WEAVE_CONTRACT_TARGETS or
# LOOM_WEAVE_BUILD_CONTRACT. It reads the build graph: a target's TYPE, and the STATIC and
# OBJECT libraries reachable through its link closure. Those are facts about what the
# artifact IS, authored where the artifact is declared; the roll is evidence of what opted
# in. Green requires the two to agree, and the mutation that empties one leaves the other
# untouched -- which is the whole point.
#
# THE SEMANTIC POPULATION, and why it is not "all shared libraries" in general:
#
#   loadable weave artifact   a SHARED or MODULE library declared in Loom's tests/
#       REQUIRED                  directory. This is a measured claim about THIS
#                                 directory, not a universal one: every SHARED library
#                                 in tests/ is a weavelib fixture that some suite
#                                 dlopen's, and there is no other kind here. A future
#                                 non-weave shared library in tests/ does not slip
#                                 through -- it lands in the required set and forces a
#                                 deliberate decision (contract it, or exempt it in
#                                 writing), which is the correct outcome either way.
#
#   static/object library     STATIC or OBJECT libraries in a required weave's link
#   inside a weave image      closure (loom, zen-switchboard). The contract covers a
#       REQUIRED                  COMPILATION, not a file (KERN-05): R2F-E measured
#                                 libloom.a dragging 20 unique symbols into every weave
#                                 that linked it while the weave's own sources were
#                                 spotless. Deriving these from the closure rather than
#                                 naming them is what keeps that lesson a mechanism.
#
#   host binary               EXECUTABLE targets (zen-tests, zen-weave-host). Never
#       NOT REQUIRED              dlopen'ed; loom_weave_build_contract() refuses them.
#
#   interface library         zen-warnings, zen-sanitize: no compilation of their own,
#       NOT REQUIRED              and the contract function refuses them too. Walked
#                                 THROUGH, so a static library reachable only via an
#                                 interface target is still found.
#
#   shared library in the     a separate image whose statics live and die with it. Its
#   closure                   own reload-safety is its own question, not this image's.
#       NOT REQUIRED (here)
#
#   declared exemption        a target carrying ZEN_WEAVE_CONTRACT_EXEMPT. Today exactly
#       EXEMPT, IN WRITING        one: zen_test_contract_bypass, the negative control
#                                 whose uncontracted-ness IS the proof. The reason string
#                                 is mandatory, it is printed on every run, and the check
#                                 fails if an exempt target ever appears on the roll --
#                                 an exemption is a documented position, never a mute.
#
# A stranger is not enumerated by any of this. Loom's required population is Loom's own
# tree; a third-party build system opts into the installed contract function and is never
# listed here (KERN-05: it defines a correct path, it is not a cage).

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
    # The reason travels to the -P check as one field of a `|`-delimited row, and a
    # semicolon in it would become a CMake list separator on the way -- splitting the row
    # and desynchronising the reason from the target it belongs to. Refused at the source
    # rather than repaired downstream. (Measured: a plain `a;b` argument never gets this
    # far -- the unquoted ${ARGN} expansion above eats the separator and the join loses the
    # character. An ESCAPED `a\;b` does survive into `reason`, which is the shape this
    # refuses.)
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
# survives in its interface as $<LINK_ONLY:x>. Unwrap that one shape, and REFUSE anything
# else still carrying a generator expression rather than skipping it quietly -- a link
# entry this sweep cannot read could be exactly the static library whose objects land
# inside the image, and a silent skip there is the failure mode C3 exists to end.
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
        list(APPEND rows "REQUIRED|${weave}|loadable weave library declared in tests/")
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
