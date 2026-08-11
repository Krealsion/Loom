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

# The reloadable-weave build contract (KERN-05). ONE definition, included here for the
# build tree and installed beside loomConfig.cmake for the stranger, so the two paths
# cannot come to mean different things -- the same identity argument the EXPORT_NAMEs
# make for the targets. Loom's own dynamic-weave fixtures call it too: the house builds
# its weaves through exactly the surface it hands a guest.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/loom-weave.cmake)

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

# THE PUBLIC MACRO SURFACE REQUIRES A CONFORMING PREPROCESSOR (MSVC-0).
#
# ZEN_EXPOSE / ZEN_HIDE dispatch on argument presence with C++20 `__VA_OPT__`
# (include/zen/weave/shape.hpp). MSVC's TRADITIONAL preprocessor is still the
# default in C++20 mode and does not implement it: `__VA_OPT__` survives as an
# ordinary identifier and gets pasted, so `ZEN_EXPOSE(rate)` expands to a call to
# `ZEN_DETAIL_EXPOSE___VA_OPT__(FIELD)(rate)` and the maker is told their own
# field name is a syntax error. Measured on MSVC 19.50: without the flag the
# probe fails with exactly that diagnostic family; with it, it compiles clean.
# `/permissive-` (already on zen-warnings) does NOT imply it -- both arms of that
# experiment had `/permissive-`.
#
# THIS IS PUBLIC, so it rides PUBLIC on `loom` -- the target whose public include
# directory carries shape.hpp -- and therefore reaches a stranger through
# find_package(loom) as INTERFACE_COMPILE_OPTIONS. A consumer must never have to
# discover a Loom-private compiler requirement to use Loom's ordinary headers.
# It deliberately does NOT go on zen-warnings (PRIVATE, and $<LINK_ONLY:> in the
# export precisely so Loom's warning policy does not leak onto a consumer's
# sources) nor in loom_weave_build_contract() (weave-only; this requirement binds
# every consumer of the shape macros, whether or not they ever build a weave).
#
# `$<CXX_COMPILER_ID:MSVC>` is false for clang-cl, whose CMake compiler id is
# Clang -- correct rather than incidental: clang-cl's preprocessor already
# implements __VA_OPT__, so it needs nothing here. (The `MSVC` *variable* would
# have been true for clang-cl; the generator expression is the precise one.)
target_compile_options(loom PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

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

# ---- The substrate itself is built under the weave contract (KERN-05) ------------------
#
# These two are what a weave statically links, so their objects land INSIDE the loadable
# image and their symbol bindings are the image's. Compiling them without the contract
# was enough, on its own, to keep every weave in this tree resident after dlclose: R2F-E
# measured 20 STB_GNU_UNIQUE symbols in libloom.a (std::piecewise_construct, the variant
# vtables, _Sp_make_shared_tag's tag, the to_chars digit tables — libstdc++ inline
# variables and template statics, instantiated here and vague-linkage by nature), and a
# single one of those marks the whole image NODELETE. The flag on the weave author's own
# translation unit could never have reached them.
#
# It rides the INSTALLED libraries too, which is the part that matters most: a stranger's
# weave gets a unique-free substrate from find_package(loom) without doing anything, and
# only has to contract their OWN sources. Applying it here is also strictly more correct
# for hosts — the design already says a host and a library must never interpose (the
# kernel opens with RTLD_LOCAL for exactly that reason), and unique binding is the one
# thing that was quietly making them share anyway.
loom_weave_build_contract(loom)
loom_weave_build_contract(zen-switchboard)

# ---- The kernel + isolation (dynamic loading; namespaces/cgroups are Linux-only) ---------------
#
# The kernel's PRIMARY home is Linux (dlopen + the isolation stack beneath it). On Windows it is
# available as an EXPLICIT OPT-IN development/demo backend (LoadLibrary) — the hook this gate
# always appreciated, now pulled by its real trigger: makers on Windows wanting to run the Zengine
# demos (the snake slice's three moments need live load/swap/reload). The Windows backend carries
# NO isolation and never claims any (Kernel::containment_note() says so; the sandbox, namespaces,
# cgroups, and the honesty-lattice guarantees remain Linux-only, and the security story remains
# WSL-hosting). Default builds are unchanged on every platform: the option is OFF.
option(LOOM_ENABLE_WINDOWS_KERNEL
       "Build the Windows kernel backend (LoadLibrary). Development/demo only — no isolation."
       OFF)

if(NOT WIN32 OR LOOM_ENABLE_WINDOWS_KERNEL)

# The kernel (dynamic library loading + the C ABI + hot-reload).
add_library(zen-kernel
    src/kernel/kernel.cpp)

target_include_directories(zen-kernel
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(zen-kernel PUBLIC cxx_std_20)
target_link_libraries(zen-kernel
    PUBLIC  loom zen-switchboard zen-sanitize
    PRIVATE zen-warnings ${CMAKE_DL_LIBS})

add_library(loom::kernel ALIAS zen-kernel)

if(WIN32)
    # Impossible to miss, on purpose: this configuration hosts weaves with no sandbox.
    # (ASCII only, like every runtime honesty string: it must render on any console.)
    message(STATUS "loom: WINDOWS KERNEL BACKEND ENABLED - development/demo only. "
                   "Containment: unisolated; process-level only; no sandbox.")
endif()

endif() # kernel (Linux always; Windows under LOOM_ENABLE_WINDOWS_KERNEL)

# ---- Linux-only substrate: isolation (fork, mount/pid/net namespaces, cgroup-v2) ---------------
#
# Isolation is the security story and it is deliberately NOT ported: the OS sandbox, the honesty
# lattice's enforced rungs, and the out-of-process host exist only where the Linux facilities do.
if(NOT WIN32)

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

endif() # NOT WIN32 (isolation)

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

# ---- The terminal core (TERM-0): an ordinary participant a presentation can drive --------
#
# The durable half of "a terminal". It holds NO Switchboard, NO tap, NO renderer and no
# frontend: a TerminalSession is an ordinary weave with one identity, one grant, one supplied
# vocabulary and its own transcript, and the typed composer underneath it is the console's
# assumption ladder stopped one step before anything is sent. Two presentations already want
# it (the zen-terminal REPL below, and a future Workshop pane), which is exactly why it is a
# library rather than a frontend's private machinery.
#
# It links the bus for ONE reason and in ONE translation unit: the host-wiring channel
# (src/terminal/host_channel.cpp) is where `Switchboard::send_as` lives, and keeping that in
# a file no participant-side header includes is the same discipline grant_wiring.hpp keeps.
add_library(zen-terminal
    src/terminal/composer.cpp
    src/terminal/session.cpp
    src/terminal/host_channel.cpp)
target_include_directories(zen-terminal
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(zen-terminal PUBLIC cxx_std_20)
target_link_libraries(zen-terminal
    PUBLIC  loom zen-switchboard zen-sanitize
    PRIVATE zen-warnings)
add_library(loom::terminal ALIAS zen-terminal)

# DELIBERATELY NOT loom_weave_build_contract(). This target took it in TERM-0's first draft, by
# analogy with `loom` and `zen-switchboard` above, and the `weave_population` check refused it
# (POP-05): those two are contracted because loadable weaves in tests/ statically link them, so
# their objects land inside a reloadable image; nothing loadable links the terminal core today.
# Contracting an artifact the required-population sweep does not require makes the roll and the
# requirement diverge, which is precisely the drift that check exists to name. When a loadable
# weave first links this target, the sweep will derive the requirement from the build graph and
# say so — which is a better mechanism than a line here somebody has to remember.

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
# ...and zen-terminal, for the ONE typed composer both use (TERM-0). The dependency runs this
# way and never the other: the terminal core knows nothing about a console, a tap or a UI tree.
target_link_libraries(zen-console
    PUBLIC  loom zen-switchboard zen-terminal zen-ui zen-sanitize
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

# The first user seat, running (WEAVER-1): a host that boots an operator console, a Weaver
# holding one GrantAuthority, one governed session and one service — and then gets out of the
# way. It is a HOST, not a library: the durable artifacts are the header-only Weaver
# (include/zen/weaver/) and the console engine it drives, both of which this file only mounts.
# Its REPL is deliberately shape-agnostic — there is no approve/grant/revoke command, because
# the operator composes the ordinary registered policy shapes like any others.
add_executable(zen-weaver-demo src/weaver/weaver_demo.cpp)
target_link_libraries(zen-weaver-demo
    PRIVATE zen-console zen-switchboard loom zen-warnings zen-sanitize)

# TERM-0's console frontend: a REPL that is a PRESENTATION of the terminal core rather than the
# owner of any terminal semantics. It boots its own Loom — it does not attach to another process —
# mounts a governed session, a separate operator seat, a Weaver and a service, and lets a person
# drive whichever participant they name. The host lens (`debug:`) is the host's own tap and
# registry read, labelled as such, and deliberately NOT wired into either participant.
# It links the terminal core and the bus, and NOTHING ELSE from this tree — no console engine,
# no TUI renderer, no UI vocabulary. That absence is a claim being made in the build graph.
add_executable(zen-terminal-repl src/terminal/terminal_main.cpp)
target_link_libraries(zen-terminal-repl
    PRIVATE zen-terminal zen-switchboard loom zen-warnings zen-sanitize)

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
# THE UI TRIO'S TRIGGER HAS ALSO FIRED, AND THE ANSWER WAS STILL NO. W-1 sent a real hosting
# consumer at it (Zengine's Workshop wanted an authored tree, a pixel resolution and a hit
# test) and measured two independent blockers. Recording them here because the sentence above
# reads like a promise that one packaging decision is all that stands in the way, and it is
# not — see reportbacks/W-1-RB.md in the workspace for the full measurement.
#
#   1. THE CONSOLE IS A PRIOR HOSTING CONSUMER THAT NEVER LEFT. zen-ui is not waiting for its
#      first host: zen-console emits its own interface as this tree (include/zen/console/
#      ui.hpp), zen-tui renders it, zen-bridge reaches it through the console, and
#      zen-ui-sdl executes zen-ui-pixel's commands. Live suites: console 27, component 15,
#      pixel 9, bridge 19+8, sdl 3. The Loom cannot depend on Zengine, so relocating the tree
#      means relocating the console, the TUI and the bridge with it. "Moves in its own port
#      phase" is true; "moves when a hosting consumer appears" is not the trigger.
#   2. IT IS A DIFFERENT MODEL FROM THE ONE AN APPLICATION CANVAS NEEDS, and would not have
#      served the consumer even if it had moved. loom::Widget fences OUT the authored
#      placement Workshop authors, `weight` is documented as a relative grow hint and never
#      an absolute size (so neither "14 cells" nor "60% of the workspace" is expressible),
#      stacks partition where authored elements overlap, and px_layout emits a PxTarget only
#      for activatable rows, activatable Text and Fields — a Region would be unhittable.
#      Zengine grew its own zengine::ui for the placement/extent model; this tree stays the
#      relational one, and the two are not competitors.
#
# So nothing here changed in W-1: no target joined the export set, no PATTERN was dropped,
# and the surface a stranger sees is byte-identical.
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
    set_target_properties(zen-terminal    PROPERTIES EXPORT_NAME terminal)
    set_target_properties(zen-sanitize    PROPERTIES EXPORT_NAME sanitize)
    set_target_properties(zen-warnings    PROPERTIES EXPORT_NAME warnings)

    # zen-sanitize is a PUBLIC dependency of both libraries, and zen-warnings survives in a
    # static library's link interface as $<LINK_ONLY:>; both must therefore ride in the same
    # export set or the export fails to generate. They are INTERFACE targets, so nothing is
    # installed for them beyond the target definition — and LINK_ONLY is what keeps Loom's
    # -Werror/-Wconversion from leaking onto a consumer's own sources.
    # zen-terminal JOINS THE EXPORT, and it is the first target to do so on the strength of a
    # SECOND PRESENTATION rather than a second host (TERM-0). The rule this export set follows is
    # "only the substrate that is Loom's forever", and the terminal core qualifies on its
    # contents: it is participant semantics — identity, vocabulary, typed composition, ask/answer
    # correlation, a truthful transcript — with no renderer, no UI vocabulary and no frontend
    # anywhere in it. Its whole purpose is to be presented by something that is not in this
    # repository, and a core a stranger cannot reach is a core with exactly one presentation.
    #
    # The UI trio, the console, the TUI, the bridge and the SDL skin still stay home for the
    # reasons written at the top of this section; nothing about that changed, and the terminal
    # core deliberately depends on NONE of them.
    install(TARGETS loom zen-switchboard zen-terminal zen-sanitize zen-warnings
        EXPORT loomTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # The kernel joins the export wherever it was built: always on Linux; on Windows only
    # under LOOM_ENABLE_WINDOWS_KERNEL (the opt-in development/demo backend — no isolation).
    # A consumer therefore gates on `if(TARGET loom::kernel)`, which is also the honest
    # question ("can this install host loadable weaves?").
    if(TARGET zen-kernel)
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

    # loom-weave.cmake rides along verbatim -- the SAME file the build tree included
    # above, not a generated copy of it, so the stranger's contract and the house's
    # cannot drift apart. loomConfig.cmake includes it from beside itself.
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/loomConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/loomConfigVersion.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/loom-weave.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom)
endif()
