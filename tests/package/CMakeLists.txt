# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# The installed-package witness: a standalone project, configured against an install prefix,
# that reaches Loom only through find_package(loom), since a build tree can satisfy a target the
# export set never published. What it must not contain is the point: no compile option, no
# /Zc:preprocessor, no include path into Loom, no CMAKE_CXX_STANDARD. If one becomes necessary
# the package has stopped carrying its requirements: fix Loom's exported targets, not this file.

# Driven by tests/package/run.cmake against a freshly installed prefix.

cmake_minimum_required(VERSION 3.16)
project(loom-package-witness LANGUAGES CXX)

find_package(loom REQUIRED)

# ---- 0. the programs the package names ---------------------------------------------
#
# A consumer's own tests find the installed host -- and, where it was built, the session
# tooling -- through the package's variables, never by knowing the install layout. The package
# checks each path as it sets it; this states what it said, and fails on a claim without its
# thing, or on the tooling claimed where no kernel could host its run manager.
if(NOT DEFINED LOOM_HOST_PROGRAM OR NOT EXISTS "${LOOM_HOST_PROGRAM}")
    message(FATAL_ERROR "package witness: the package names no installed host (LOOM_HOST_PROGRAM)")
endif()
if(NOT DEFINED LOOM_SESSION_TOOLS)
    message(FATAL_ERROR
        "package witness: the package does not say whether it carries the session tooling "
        "(LOOM_SESSION_TOOLS)")
endif()
if(LOOM_SESSION_TOOLS)
    if(NOT TARGET loom::kernel)
        message(FATAL_ERROR
            "package witness: the package claims the session tooling, and has no kernel to host "
            "its run manager")
    endif()
    if(NOT EXISTS "${LOOM_RUNS_ARTIFACT}" OR
       NOT EXISTS "${LOOM_SESSION_RUNTIME}/loom_session/__init__.py")
        message(FATAL_ERROR
            "package witness: the session tooling the package claims is not all there: "
            "'${LOOM_RUNS_ARTIFACT}', '${LOOM_SESSION_RUNTIME}/loom_session'")
    endif()
    message(STATUS "package witness: host ${LOOM_HOST_PROGRAM}; session tooling "
                   "${LOOM_RUNS_ARTIFACT}, runtime ${LOOM_SESSION_RUNTIME}")
else()
    message(STATUS "package witness: host ${LOOM_HOST_PROGRAM}; this install carries no "
                   "session tooling, and says so")
endif()

# ---- 1. the public macro surface -------------------------------------------------
#
# Links loom::core ONLY. That is the narrowest claim: the shape macros must work for a
# consumer who never touches the bus, the kernel, or a weave.
add_executable(witness-macros macro_surface.cpp)
target_link_libraries(witness-macros PRIVATE loom::core)

# ---- 2. the terminal core, reached the way a second presentation reaches it -------
# Links loom::terminal and loom::switchboard and nothing else: no console, UI vocabulary or
# renderer, none of which is exported.
add_executable(witness-terminal stranger_terminal.cpp)
target_link_libraries(witness-terminal PRIVATE loom::terminal loom::switchboard loom::core)

# ---- 2b. the history pair, reached the way an application outside this repository reaches it --
# Links loom::history, loom::switchboard and loom::core and nothing else: the Recorder and the
# Logger are reachable, and no console, UI vocabulary or terminal is.
add_executable(witness-history stranger_history.cpp)
target_link_libraries(witness-history PRIVATE loom::history loom::switchboard loom::core)

# ---- 2c. the crossing, reached the way a linking host reaches it -------------------
# Links loom::bridge, loom::switchboard and loom::core and nothing else: no console and no remote
# console, neither of which is exported.
add_executable(witness-bridge stranger_bridge.cpp)
target_link_libraries(witness-bridge PRIVATE loom::bridge loom::switchboard loom::core)

if(NOT TARGET loom::kernel)
    # Honest, and not a skip-in-disguise: on a build of Loom with no kernel there is no
    # dynamic-weave path to witness, and saying so beats silently proving half of this.
    message(STATUS
        "package witness: loom::kernel is absent from this install -- the macro-surface "
        "witness is built, the dynamic-weave witness is DECLARED ABSENT. (On Windows the "
        "kernel is the opt-in LOOM_ENABLE_WINDOWS_KERNEL development/demo backend.)")
    return()
endif()

# ---- 2. a real loadable weave ----------------------------------------------------
add_library(witness-weave SHARED stranger_weave.cpp)
target_link_libraries(witness-weave PRIVATE loom::core loom::switchboard)

# The weave build contract, reached through the installed package exactly as a stranger
# reaches it. It is a DIFFERENT law from the preprocessor requirement above: this one is
# about a loadable image (its static lifetime on ELF/GNU, its own runtime on Windows) and
# is platform-specific; that one is about compiling Loom's public headers at all and is
# MSVC-specific. A consumer calls this one by hand; the other arrives on its own.
loom_weave_build_contract(witness-weave)

# ---- 3. the host that loads it through the REAL kernel ----------------------------
add_executable(witness-host stranger_host.cpp)
# loom::terminal joins the link line for the SELF-DESCRIPTION half of this witness: a
# stranger that learns a vocabulary by message, then generates a form and composes a
# message from it, reaches for the composer -- and the composer has to be reachable
# through find_package exactly as the kernel is. Nothing here adds a compile OPTION.
target_link_libraries(witness-host
    PRIVATE loom::kernel loom::switchboard loom::terminal loom::core)
target_compile_definitions(witness-host PRIVATE
    "ZEN_WITNESS_WEAVE=\"$<TARGET_FILE:witness-weave>\"")
add_dependencies(witness-host witness-weave)
