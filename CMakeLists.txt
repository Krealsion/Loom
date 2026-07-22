cmake_minimum_required(VERSION 3.16)

project(loom
    VERSION 0.1.0
    DESCRIPTION "Self-describing-value-and-gate core for Zen"
    LANGUAGES CXX)

# ---- Project-wide policy --------------------------------------------------

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The kernel loads Weaves from shared libraries, which statically link the core.
# Build everything position-independent so the static libraries can be pulled
# into a .so.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Standard install layout (lib/, include/, bin/). Pulled in here rather than beside the install
# rules because the exported targets below carry an INSTALL_INTERFACE include path.
include(GNUInstallDirs)

option(ZEN_WERROR    "Treat warnings as errors (CI default)"        ON)
option(ZEN_SANITIZE  "Build with -fsanitize=address,undefined"     OFF)
option(ZEN_BUILD_TESTS    "Build the test suite"                    ON)
option(ZEN_BUILD_EXAMPLES "Build the examples"                      ON)

# Warnings that every Zen target inherits. The library earns trust by being
# clean under the strictest reasonable flags.
add_library(zen-warnings INTERFACE)
target_compile_options(zen-warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<BOOL:${ZEN_WERROR}>>:-Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<BOOL:${ZEN_WERROR}>>:/WX>)

# Sanitizers, applied to compile and link of everything that opts in.
add_library(zen-sanitize INTERFACE)
if(ZEN_SANITIZE)
    target_compile_options(zen-sanitize INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(zen-sanitize INTERFACE
        -fsanitize=address,undefined)
endif()

# ---- The library ----------------------------------------------------------

add_library(loom
    src/schema.cpp
    src/value.cpp
    src/gate.cpp
    src/registry.cpp
    src/serialize.cpp
    src/detail/json.cpp
    src/detail/base64.cpp
    src/detail/binary.cpp)

target_include_directories(loom
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(loom PUBLIC cxx_std_20)
target_link_libraries(loom
    PUBLIC  zen-sanitize
    PRIVATE zen-warnings)

add_library(loom::core ALIAS loom)

# ---- The switchboard (the first live boundary) ----------------------------

add_library(zen-switchboard
    src/switchboard/switchboard.cpp)

target_include_directories(zen-switchboard
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(zen-switchboard PUBLIC cxx_std_20)
target_link_libraries(zen-switchboard
    PUBLIC  loom zen-sanitize
    PRIVATE zen-warnings)

add_library(loom::switchboard ALIAS zen-switchboard)

# ---- Linux-only substrate: the kernel + isolation (POSIX dlopen, namespaces, cgroups) ----------
#
# These depend on POSIX/Linux facilities (dlfcn dynamic loading, fork, mount/pid/net namespaces,
# cgroup-v2) and are gated behind a platform check so the portable subset (core, switchboard,
# console, both terminal frontends) configures on Windows. SEAM APPRECIATION: the gate makes the
# "Linux-only targets" a visible seam — native Windows .dll Weave loading (dlopen -> LoadLibrary)
# is a clean future addition hooked right here, but its trigger (wanting native Windows execution
# of sandboxed Weaves) likely never fires, because WSL-hosting is the security story. Hooked,
# deferred to a probably-never trigger. On Linux every gate below is taken — nothing changes.
if(NOT WIN32)

# The kernel (DLL loading + the C ABI + hot-reload).
add_library(zen-kernel
    src/kernel/kernel.cpp)

target_include_directories(zen-kernel
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(zen-kernel PUBLIC cxx_std_20)
target_link_libraries(zen-kernel
    PUBLIC  loom zen-switchboard zen-sanitize
    PRIVATE zen-warnings ${CMAKE_DL_LIBS})

add_library(loom::kernel ALIAS zen-kernel)

# Isolation (out-of-process Weave hosting + crash supervision). The host-side library: the IPC
# channel, the proxy Weave, and the supervisor. It reuses the kernel's header-only schema codec to
# reconstruct a child's schemas; it does not depend on the kernel's compiled code.
add_library(zen-isolation
    src/isolation/channel.cpp
    src/isolation/grant_record.cpp
    src/isolation/host.cpp
    src/isolation/sandbox.cpp)

target_include_directories(zen-isolation
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src) # detail/sha256.hpp (grant-key digest), like loom

target_compile_features(zen-isolation PUBLIC cxx_std_20)
target_link_libraries(zen-isolation
    PUBLIC  loom zen-switchboard zen-sanitize
    PRIVATE zen-warnings)

add_library(loom::isolation ALIAS zen-isolation)

endif() # NOT WIN32 (kernel + isolation)

# ---- The UI vocabulary (zen-ui) — the Loom's semantic tree, in its own target -----------
#
# The renderer-agnostic widget tree + the UI-Builder component vocabulary (components with
# typed slots serializing as gated Values, zen.ui.*). Lifted out of the console (Phase B):
# the vocabulary depends ONLY on the core (schemas/values/gate) — never on the console, the
# bus, or any renderer. The console EMITS this tree; the TUI and the SDL renderer PROJECT it;
# the outline walk proves it carries no medium. Everyone consumes; nobody owns.
add_library(zen-ui
    src/ui/tree.cpp
    src/ui/component.cpp)
target_include_directories(zen-ui
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(zen-ui PUBLIC cxx_std_20)
target_link_libraries(zen-ui
    PUBLIC  loom zen-sanitize
    PRIVATE zen-warnings)
add_library(loom::ui ALIAS zen-ui)

# The PIXEL projection's logic — one renderer's layout brain (tree -> draw commands), kept
# SDL-free so it is provable in the ordinary suite on every platform (incl. Windows/MinGW,
# where the SDL skin does not build). The SDL target below merely executes these commands.
add_library(zen-ui-pixel src/ui/pixel.cpp)
target_compile_features(zen-ui-pixel PUBLIC cxx_std_20)
target_link_libraries(zen-ui-pixel
    PUBLIC  zen-ui zen-sanitize
    PRIVATE zen-warnings)
add_library(loom::ui-pixel ALIAS zen-ui-pixel)

# ---- Console (the first doing-layer component: the engine, then a terminal) ------------
#
# A message-native bus participant — discover, compose, gate-send, receive replies into an
# indexed buffer — built frontend-agnostic so the terminal (now) and a GUI (later) reuse
# this engine unchanged. Stage 3 adds the UI-as-data layer (ui.cpp): the console emits its
# own interface as the zen-ui semantic tree (a consumer of the vocabulary, not its owner).
# Depends on the bus + core + the UI vocabulary.
add_library(zen-console
    src/console/console.cpp
    src/console/ui.cpp)
target_include_directories(zen-console
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(zen-console PUBLIC cxx_std_20)
target_link_libraries(zen-console
    PUBLIC  loom zen-switchboard zen-ui zen-sanitize
    PRIVATE zen-warnings)
add_library(loom::console ALIAS zen-console)

# The terminal frontend — a thin, throwaway REPL over the engine (Stage 1-2; a GUI replaces
# only this later). Exercises discovery / guided send / one-liner / buffer / tap as text.
add_executable(zen-console-term src/console/console_term.cpp)
target_link_libraries(zen-console-term
    PRIVATE zen-console zen-switchboard loom zen-warnings zen-sanitize)

# The shared TUI renderer + the TerminalBackend seam, as a small library BOTH consoles link: the
# in-process TUI and the remote console differ ONLY in transport + I/O loop, never in rendering. The
# renderer (tui_render.cpp) lays the SAME renderer-agnostic widget tree out to cells — the only place
# positions and cells exist; the render/controller logic lives in zen-console so it stays headlessly
# testable. All platform terminal control lives behind the seam (terminal.hpp): a POSIX backend on
# non-Windows, a Win32 Console backend on Windows; make_terminal() is the only per-platform symbol.
add_library(zen-tui STATIC src/console/tui_render.cpp)
if(WIN32)
    target_sources(zen-tui PRIVATE src/console/terminal_windows.cpp)
else()
    target_sources(zen-tui PRIVATE src/console/terminal_posix.cpp)
endif()
# The renderer consumes the VOCABULARY, not the console — the same one-way dependency the SDL
# renderer has. (The console frontends link zen-tui AND zen-console side by side.)
target_link_libraries(zen-tui
    PUBLIC  zen-ui zen-sanitize
    PRIVATE zen-warnings)

# The full-screen TUI frontend (in-process): local wiring + a demo responder over the shared renderer.
add_executable(zen-console-tui src/console/console_tui.cpp)
target_link_libraries(zen-console-tui
    PRIVATE zen-tui zen-console zen-switchboard loom zen-warnings zen-sanitize)

# ---- The SDL2 projection (the second real renderer: same tree, to pixels) ---------------
#
# GATED: only the SDL renderer target needs SDL2 — the vocabulary, the pixel-layout logic,
# the console, the bus, and the core stay SDL-free and portable. WSL/Linux-first (default ON
# there, OFF on Windows — the flagged platform decision; the vocabulary + pixel logic still
# build and test on Windows). Prefers a system SDL2; otherwise fetches PINNED release
# tarballs (static SDL2 + SDL2_ttf with its vendored FreeType) so nothing is installed on the
# host and the checksum is part of the build.
if(WIN32)
    set(zen_sdl_default OFF)
else()
    set(zen_sdl_default ON)
endif()
option(ZEN_SDL "Build the SDL2 renderer, its demo, and its dummy-driver tests" ${zen_sdl_default})

if(ZEN_SDL)
    find_package(SDL2 CONFIG QUIET)
    find_package(SDL2_ttf CONFIG QUIET)
    if(NOT (TARGET SDL2::SDL2 OR TARGET SDL2::SDL2-static) OR
       NOT (TARGET SDL2_ttf::SDL2_ttf OR TARGET SDL2_ttf::SDL2_ttf-static))
        # On WSL building from a Windows-mounted checkout (/mnt/...), the 9p filesystem cannot
        # hold the symlinks inside SDL's tarball (and compiles dependencies slowly) — keep the
        # fetched trees on the WSL-native filesystem instead. Only the MODULE DEFAULT is
        # replaced (the FetchContent module pre-defines FETCHCONTENT_BASE_DIR when included,
        # so a bare NOT-DEFINED check never fires); a user's own choice is respected. Keyed
        # per build dir: build/ and build-san/ must not share dependency OBJECTS (different
        # flags), only re-download the same pinned tarball.
        if(CMAKE_SOURCE_DIR MATCHES "^/mnt/"
           AND (CMAKE_HOST_SYSTEM_VERSION MATCHES "[Mm]icrosoft" OR DEFINED ENV{WSL_DISTRO_NAME}))
            get_filename_component(zen_bindir_name "${CMAKE_BINARY_DIR}" NAME)
            if(NOT DEFINED FETCHCONTENT_BASE_DIR
               OR FETCHCONTENT_BASE_DIR STREQUAL "${CMAKE_BINARY_DIR}/_deps")
                set(FETCHCONTENT_BASE_DIR "$ENV{HOME}/.cache/zen-fetch/${zen_bindir_name}"
                    CACHE PATH
                    "FetchContent trees (WSL-native: drvfs cannot hold SDL's symlinks)" FORCE)
                message(STATUS "zen: FetchContent trees under ${FETCHCONTENT_BASE_DIR} "
                               "(drvfs symlink workaround)")
            endif()
        endif()
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL2TTF_VENDORED ON CACHE BOOL "" FORCE)
        set(SDL2TTF_SAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL2TTF_INSTALL OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF)
        FetchContent_Declare(sdl2_fetched
            URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-2.30.11.tar.gz
            URL_HASH SHA256=8b8d4aef2038533da814965220f88f77d60dfa0f32685f80ead65e501337da7f)
        FetchContent_Declare(sdl2_ttf_fetched
            URL https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.22.0/SDL2_ttf-2.22.0.tar.gz
            URL_HASH SHA256=d48cbd1ce475b9e178206bf3b72d56b66d84d44f64ac05803328396234d67723)
        FetchContent_MakeAvailable(sdl2_fetched sdl2_ttf_fetched)
    endif()
    if(TARGET SDL2::SDL2-static)
        set(zen_sdl2_lib SDL2::SDL2-static)
    else()
        set(zen_sdl2_lib SDL2::SDL2)
    endif()
    if(TARGET SDL2_ttf::SDL2_ttf-static)
        set(zen_sdl2_ttf_lib SDL2_ttf::SDL2_ttf-static)
    else()
        set(zen_sdl2_ttf_lib SDL2_ttf::SDL2_ttf)
    endif()

    # The skin: executes the pixel layout's commands, maps raw SDL events to semantic
    # InputEvents. The ONLY target that sees SDL.
    add_library(zen-ui-sdl src/ui/sdl/sdl_ui.cpp)
    target_link_libraries(zen-ui-sdl
        PUBLIC  zen-ui-pixel ${zen_sdl2_lib} ${zen_sdl2_ttf_lib} zen-sanitize
        PRIVATE zen-warnings)

    # The preview window (Josh's visual verify): stress-placeholder trees, input-as-messages.
    add_executable(zen-ui-sdl-demo src/ui/sdl/sdl_demo.cpp)
    target_link_libraries(zen-ui-sdl-demo
        PRIVATE zen-ui-sdl zen-ui zen-warnings zen-sanitize)
endif()

# ---- The remote-operator bridge (operator-protocol transport; server + remote console land here) --
#
# PORTABLE on purpose: the operator-protocol crosses the host boundary, so the transport builds on
# BOTH the WSL host (POSIX sockets) and the Windows client (Winsock — ws2_32). AF_UNIX is the fast
# WSL<->WSL inner loop; AF_INET 127.0.0.1 is the real Windows->WSL crossing (WSL2 forwards localhost).
# The engine runs CLIENT-side (RemoteConsole, a Console impl over the wire); the host registers a
# proxy-participant per operator and answers discovery / streams the tap / stamps + re-admits sends.
add_library(zen-bridge
    src/bridge/channel.cpp
    src/bridge/server.cpp
    src/bridge/remote_console.cpp)
target_include_directories(zen-bridge
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(zen-bridge PUBLIC cxx_std_20)
target_link_libraries(zen-bridge
    PUBLIC  loom zen-switchboard zen-console zen-sanitize
    PRIVATE zen-warnings)
if(WIN32)
    target_link_libraries(zen-bridge PUBLIC ws2_32)
endif()
add_library(loom::bridge ALIAS zen-bridge)

# The WSL-side host: a bus + one demo weave + the BridgeServer, listening for remote operators. The
# WSL end of the Windows->WSL crossing. Portable (it is the bus side, all in-process), but in practice
# run on the WSL host.
add_executable(zen-bridge-host src/bridge/bridge_host_main.cpp)
target_link_libraries(zen-bridge-host PRIVATE zen-bridge zen-switchboard loom zen-warnings zen-sanitize)

# A NON-interactive remote operator that drives the protocol end-to-end and prints PASS/FAIL — the
# crossing proof. PORTABLE on purpose: built via MinGW on Windows, it connects to a WSL-hosted
# zen-bridge-host across the real boundary (TCP localhost).
add_executable(zen-bridge-probe src/bridge/bridge_probe_main.cpp)
target_link_libraries(zen-bridge-probe PRIVATE zen-bridge zen-console zen-switchboard loom zen-warnings zen-sanitize)

# The interactive remote-operator console: the SAME ConsoleUi + shared renderer as the in-process
# TUI, with the engine behind a socket (RemoteConsole) and the loop an event-driven multiplexer over
# {input, socket}. PORTABLE — built via MinGW on Windows, it is the Windows console driving a
# WSL-hosted bus across the boundary.
add_executable(zen-console-remote src/console/console_remote.cpp)
target_link_libraries(zen-console-remote
    PRIVATE zen-tui zen-bridge zen-console zen-switchboard loom zen-warnings zen-sanitize)

# The child executable: runs one Weave out-of-process via the kernel C ABI. It is a byte shuttler —
# it deliberately does NOT link loom; it speaks the wire format only through the protocol header
# and lets the host gate everything. Linux-only (the sandbox/hosting is POSIX; see the gate above).
if(NOT WIN32)
add_executable(zen-weave-host
    src/isolation/weave_host_main.cpp)

target_include_directories(zen-weave-host
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(zen-weave-host PRIVATE cxx_std_20)
target_link_libraries(zen-weave-host
    PRIVATE zen-warnings zen-sanitize ${CMAKE_DL_LIBS})
endif() # NOT WIN32 (zen-weave-host)

# ---- Tests ----------------------------------------------------------------

if(ZEN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ZEN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---- The consumer surface: install + export (find_package(loom)) ----------------------
#
# Zengine is the Loom's first external consumer, and it consumes by the STRANGER'S path — a
# find_package() against an installed package, never a sibling include. That is the point of
# the split: every rough edge in this surface hits the house before it hits a guest.
#
# Exported deliberately SMALLER than what the build tree offers: only the substrate that is
# Loom's forever. The UI trio, the console, the TUI, the bridge and the SDL skin are
# Zengine-destined (each moves in its own port phase), so exporting them now would publish a
# surface we are about to relocate. The kernel and isolation ARE Loom's, but they are
# Linux-only; the rule was that each joins the export when a hosting consumer appears — and
# for the KERNEL that trigger has now fired (Zengine's snake slice hosts .so weaves), so
# loom::kernel rides the export on Linux. Isolation still waits for its first out-of-process
# consumer. SEAM APPRECIATION: the export set is the honest answer to "what is public", and
# the gap between it and the build tree is a visible, intentional seam — a consumer that
# leans on an unexported target fails on the DEFAULT path, which is exactly where we want
# that mistake to surface.
#
# EXPORT_NAME gives the installed targets the SAME spellings as the in-tree ALIASes
# (loom::core, loom::switchboard). That identity is what makes a consumer's sibling-source
# override a true drop-in: the same names resolve either way, so the two paths cannot silently
# diverge in what they mean.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(loom_default_install ON)
else()
    set(loom_default_install OFF)
endif()
option(LOOM_INSTALL "Generate the install/export rules for find_package(loom)"
       ${loom_default_install})

if(LOOM_INSTALL)
    include(CMakePackageConfigHelpers)

    set_target_properties(loom            PROPERTIES EXPORT_NAME core)
    set_target_properties(zen-switchboard PROPERTIES EXPORT_NAME switchboard)
    set_target_properties(zen-sanitize    PROPERTIES EXPORT_NAME sanitize)
    set_target_properties(zen-warnings    PROPERTIES EXPORT_NAME warnings)

    # zen-sanitize is a PUBLIC dependency of both libraries, and zen-warnings survives in a
    # static library's link interface as $<LINK_ONLY:>; both must therefore ride in the same
    # export set or the export fails to generate. They are INTERFACE targets, so nothing is
    # installed for them beyond the target definition — and LINK_ONLY is what keeps Loom's
    # -Werror/-Wconversion from leaking onto a consumer's own sources.
    install(TARGETS loom zen-switchboard zen-sanitize zen-warnings
        EXPORT loomTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # The kernel target is Linux-only (it does not exist on WIN32), so it joins the same
    # export set conditionally; a consumer therefore gates on `if(TARGET loom::kernel)`,
    # which is also the honest question ("can this install host .so weaves?").
    if(NOT WIN32)
        set_target_properties(zen-kernel PROPERTIES EXPORT_NAME kernel)
        install(TARGETS zen-kernel
            EXPORT loomTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    # The exported header closure, verified self-contained: the core, the switchboard, the
    # weave-authoring headers, and the kernel's (kernel/abi.h is the reason "*.h" matches —
    # the C ABI header is deliberately not a .hpp). The kernel headers ship on every platform
    # (they are just text); the TARGET is what a consumer must gate on. The other unexported
    # targets' headers stay home.
    install(DIRECTORY include/zen/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zen
        FILES_MATCHING
            PATTERN "*.hpp"
            PATTERN "*.h"
            PATTERN "bridge"    EXCLUDE
            PATTERN "console"   EXCLUDE
            PATTERN "isolation" EXCLUDE
            PATTERN "ui"        EXCLUDE)

    install(EXPORT loomTargets
        FILE loomTargets.cmake
        NAMESPACE loom::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom)

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/loomConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/loomConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/loomConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/loomConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/loomConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom)
endif()
