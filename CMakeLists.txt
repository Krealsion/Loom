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
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

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
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(zen-isolation PUBLIC cxx_std_20)
target_link_libraries(zen-isolation
    PUBLIC  loom zen-switchboard zen-sanitize
    PRIVATE zen-warnings)

add_library(loom::isolation ALIAS zen-isolation)

endif() # NOT WIN32 (kernel + isolation)

# ---- Console (the first doing-layer component: the engine, then a terminal) ------------
#
# A message-native bus participant — discover, compose, gate-send, receive replies into an
# indexed buffer — built frontend-agnostic so the terminal (now) and a GUI (later) reuse
# this engine unchanged. Stage 3 adds the UI-as-data layer (ui.cpp): a renderer-agnostic
# semantic widget tree the engine emits and any renderer consumes. The UI-Builder vocabulary
# (component.cpp) evolves that SAME tree into components-with-typed-slots that serialize as
# gated Values (zen.ui.*). Depends only on the bus + core.
add_library(zen-console
    src/console/console.cpp
    src/console/ui.cpp
    src/console/component.cpp)
target_include_directories(zen-console
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(zen-console PUBLIC cxx_std_20)
target_link_libraries(zen-console
    PUBLIC  loom zen-switchboard zen-sanitize
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
target_link_libraries(zen-tui
    PUBLIC  zen-console zen-sanitize
    PRIVATE zen-warnings)

# The full-screen TUI frontend (in-process): local wiring + a demo responder over the shared renderer.
add_executable(zen-console-tui src/console/console_tui.cpp)
target_link_libraries(zen-console-tui
    PRIVATE zen-tui zen-console zen-switchboard loom zen-warnings zen-sanitize)

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
