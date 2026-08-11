# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# THE INSTALLED-PACKAGE WITNESS (MSVC-0).
#
# A STANDALONE project -- not add_subdirectory'd by Loom, never part of Loom's own
# build tree. It is configured against an INSTALL PREFIX and reaches Loom only through
# find_package(loom), which is the whole point: the build tree can satisfy a target the
# export set never published, and that is exactly the mistake that reaches a guest
# instead of the house.
#
# THE LOAD-BEARING PROPERTY OF THIS FILE IS WHAT IT DOES NOT CONTAIN.
# No target_compile_options. No /Zc:preprocessor. No -I into a Loom source tree. No
# CMAKE_CXX_STANDARD. If any of those become necessary to build these three targets,
# the package has stopped carrying its own requirements and this witness must go red --
# so do not "fix" a failure here by adding a flag. Fix it in Loom's exported targets.
#
# Drive it with tests/package/run.cmake, which configures, builds and runs it against a
# freshly installed prefix.

cmake_minimum_required(VERSION 3.16)
project(loom-package-witness LANGUAGES CXX)

find_package(loom REQUIRED)

# ---- 1. the public macro surface -------------------------------------------------
#
# Links loom::core ONLY. That is the narrowest claim: the shape macros must work for a
# consumer who never touches the bus, the kernel, or a weave.
add_executable(witness-macros macro_surface.cpp)
target_link_libraries(witness-macros PRIVATE loom::core)

# ---- 2. the terminal core, reached the way a second presentation reaches it -------
#
# TERM-0 exported `loom::terminal` on the strength of a second PRESENTATION rather than a
# second host, so the claim that a stranger can build one has to be measured here rather
# than asserted in a report. Links loom::terminal + loom::switchboard and nothing else --
# no console, no UI vocabulary, no renderer, none of which is exported.
add_executable(witness-terminal stranger_terminal.cpp)
target_link_libraries(witness-terminal PRIVATE loom::terminal loom::switchboard loom::core)

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

# The reloadable-weave build contract, reached through the installed package exactly as
# a stranger reaches it. It is a DIFFERENT law from the preprocessor requirement above:
# this one is about a loadable image's static lifetime and is platform-specific (ELF/GNU
# only); that one is about compiling Loom's public headers at all and is MSVC-specific.
# A consumer calls this one by hand; the other arrives on its own.
loom_weave_build_contract(witness-weave)

# ---- 3. the host that loads it through the REAL kernel ----------------------------
add_executable(witness-host stranger_host.cpp)
target_link_libraries(witness-host PRIVATE loom::kernel loom::switchboard loom::core)
target_compile_definitions(witness-host PRIVATE
    "ZEN_WITNESS_WEAVE=\"$<TARGET_FILE:witness-weave>\"")
add_dependencies(witness-host witness-weave)
