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
    src/content_id.cpp
    src/detail/json.cpp
    src/detail/base64.cpp
    src/detail/binary.cpp)

target_include_directories(loom
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(loom PUBLIC cxx_std_20)

# The public shape macros need a conforming preprocessor: ZEN_EXPOSE and ZEN_HIDE dispatch on
# C++20 `__VA_OPT__` (include/zen/weave/shape.hpp), which MSVC's default traditional preprocessor
# does not implement, and `/permissive-` does not imply it. It rides PUBLIC on `loom`, so a
# stranger gets it through find_package(loom), and not on zen-warnings or the weave contract,
# since every user of the macros needs it. `$<CXX_COMPILER_ID:MSVC>` is false for clang-cl, whose
# preprocessor already implements `__VA_OPT__`.
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
# A weave statically links these two, so their objects land inside the loadable image. Their
# libstdc++ inline variables and template statics bind STB_GNU_UNIQUE unless the contract
# applies, and one such symbol keeps the whole image resident after dlclose, whatever the weave
# author's own flags. It rides the installed libraries too: a stranger contracts only their own
# sources.
loom_weave_build_contract(loom)
loom_weave_build_contract(zen-switchboard)

# ---- The kernel and isolation (dynamic loading; namespaces and cgroups are Linux-only) ---------
# The kernel's home is Linux (dlopen, with the isolation stack beneath it). On Windows it is an
# explicit opt-in development backend (LoadLibrary) with no isolation, which it never claims
# (Kernel::containment_note() says so). The option is off by default on every platform.
option(LOOM_ENABLE_WINDOWS_KERNEL
       "Build the Windows kernel backend (LoadLibrary). Development/demo only — no isolation."
       OFF)

if(NOT WIN32 OR LOOM_ENABLE_WINDOWS_KERNEL)

# The kernel (dynamic library loading + the C ABI + hot-reload).
add_library(zen-kernel
    src/kernel/kernel.cpp
    src/kernel/admission.cpp)

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
# Isolation is the security story and is deliberately not ported: the OS sandbox, its enforced
# guarantees and the out-of-process host exist only where the Linux facilities do.
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

# ---- The UI vocabulary (zen-ui): the semantic widget tree, in its own target -----------
# The renderer-agnostic widget tree and the component vocabulary (typed slots serialized as gated
# Values, zen.ui.*). It depends only on the core, never on the console, the bus or a renderer: the
# console emits this tree, the TUI projects it, and the vocabulary owns no presentation.
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

# The PIXEL projection's logic — one renderer's layout brain (tree -> draw commands). It owns
# no window, no font library and no display: text metrics are INJECTED, so the layout is a pure
# function of tree + metrics and is provable in the ordinary suite on every platform this
# repository builds on. EXECUTING the commands is a presentation's job, and no presentation
# that does so lives here — which is the point: the brain outlives any one skin.
add_library(zen-ui-pixel src/ui/pixel.cpp)
target_compile_features(zen-ui-pixel PUBLIC cxx_std_20)
target_link_libraries(zen-ui-pixel
    PUBLIC  zen-ui zen-sanitize
    PRIVATE zen-warnings)
add_library(loom::ui-pixel ALIAS zen-ui-pixel)

# ---- The terminal core: an ordinary participant a presentation can drive --------
# A TerminalSession holds no Switchboard, no tap, no renderer and no frontend: an ordinary weave
# with one identity, one grant, a supplied vocabulary and its own transcript, over the typed
# composer. It is a library because more than one presentation drives it. It links the bus for
# one reason, in one file: the host-wiring channel (src/terminal/host_channel.cpp), where
# `Switchboard::send_as` lives out of every participant-side header's reach.
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

# Deliberately not loom_weave_build_contract(): nothing loadable links this target, and
# contracting an artifact the build-graph sweep does not require makes the roll and the
# requirement diverge, the drift `weave_population` names (POP-05). When a loadable weave links
# it, the sweep derives the requirement.

# ---- History: what the host knows, and what it keeps -----------
# Two owners, one library, neither reading the other: `loom::Recorder` is volatile working
# memory, `loom::Logger` the durable selected record, both tap consumers beside the console. It
# links the bus and the core and nothing else from this tree, so a history lens cannot become a
# presentation. Exported, because consumers reach it through find_package(loom).
add_library(zen-history
    src/history/record.cpp
    src/history/recorder.cpp
    src/history/logger.cpp
    src/history/dump.cpp)
target_include_directories(zen-history
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(zen-history PUBLIC cxx_std_20)
target_link_libraries(zen-history
    PUBLIC  loom zen-switchboard zen-sanitize
    PRIVATE zen-warnings)
add_library(loom::history ALIAS zen-history)

# DELIBERATELY NOT loom_weave_build_contract(), for zen-terminal's reason (POP-05): nothing
# loadable links this target, so contracting it would put an artifact on the roll that the
# build-graph sweep does not require, which is exactly the drift `weave_population` names.

# ---- Console: the engine, then a terminal ------------
# A message-native bus participant (discover, compose, gate-send, receive replies into an indexed
# buffer), built frontend-agnostic so any frontend reuses the engine. It emits its own interface
# as the zen-ui tree (ui.cpp), a consumer of the vocabulary and not its owner.
add_library(zen-console
    src/console/console.cpp
    src/console/ui.cpp)
target_include_directories(zen-console
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(zen-console PUBLIC cxx_std_20)
# ...and zen-terminal, for the one typed composer both use. The dependency runs this way only:
# the terminal core knows nothing about a console, a tap or a UI tree.
target_link_libraries(zen-console
    PUBLIC  loom zen-switchboard zen-terminal zen-ui zen-sanitize
    PRIVATE zen-warnings)
add_library(loom::console ALIAS zen-console)

# The terminal frontend: a thin REPL over the engine that exercises discovery, guided send,
# one-liners, the buffer and the tap as text.
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
# The renderer consumes the VOCABULARY, not the console — the one-way dependency every
# projection of this tree owes. (The console frontends link zen-tui AND zen-console side by side.)
target_link_libraries(zen-tui
    PUBLIC  zen-ui zen-sanitize
    PRIVATE zen-warnings)

# The first user seat, running: a host that boots an operator console, a Weaver
# holding one GrantAuthority, one governed session and one service — and then gets out of the
# way. It is a HOST, not a library: the durable artifacts are the header-only Weaver
# (include/zen/weaver/) and the console engine it drives, both of which this file only mounts.
# Its REPL is deliberately shape-agnostic — there is no approve/grant/revoke command, because
# the operator composes the ordinary registered policy shapes like any others.
add_executable(zen-weaver-demo src/weaver/weaver_demo.cpp)
target_link_libraries(zen-weaver-demo
    PRIVATE zen-console zen-switchboard loom zen-warnings zen-sanitize)

# The terminal core's console frontend: a REPL that presents the terminal core and owns none of
# its semantics. It boots its own Loom and mounts a governed session, a separate operator seat, a
# Weaver and a service, and lets a person drive whichever participant they name; the `debug:`
# host lens is the host's own tap and registry, labelled so. It links the terminal core and the
# bus and nothing else from this tree.
add_executable(zen-terminal-repl src/terminal/terminal_main.cpp)
target_link_libraries(zen-terminal-repl
    PRIVATE zen-terminal zen-switchboard loom zen-warnings zen-sanitize)

# The full-screen TUI frontend (in-process): local wiring + a demo responder over the shared renderer.
add_executable(zen-console-tui src/console/console_tui.cpp)
target_link_libraries(zen-console-tui
    PRIVATE zen-tui zen-console zen-switchboard loom zen-warnings zen-sanitize)

# ---- loom-host: the supplied host, and the only executable this project installs -----
# The answer to "I installed Loom, now what?": it reads a boot plan the person wrote, starts what
# it names in order, and gives them a console with authority in it. It links the console engine,
# the terminal core and, where there is one, the kernel; ZEN_HOST_HAS_KERNEL says which, and a
# host without one still runs and says why it can load nothing.

# The host's deciding half, as a library the suite reaches: the parsers of the two files a person
# writes, `line_input` (reading a command without stopping the bus) and `store_lock` (who owns
# these decisions). `main()` stays in the executable, driven as a real process by
# tests/host_process/.
add_library(zen-host-policy STATIC
    src/host/authority.cpp
    src/host/boot_plan.cpp
    src/host/config_file.cpp
    src/host/line_input.cpp
    src/host/store_lock.cpp)
target_include_directories(zen-host-policy
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/host>)
target_compile_features(zen-host-policy PUBLIC cxx_std_20)
target_link_libraries(zen-host-policy
    PUBLIC  loom zen-switchboard zen-sanitize
    PRIVATE zen-warnings)

add_executable(loom-host src/host/host_main.cpp)
# It links the history pair and the bridge: the host mounts Loom's own Recorder and Logger (from
# the plan's `history` section) and holds a LinkWeave per `links` row (src/host/link.hpp), host
# wiring over the exported client.
target_link_libraries(loom-host
    PRIVATE zen-host-policy zen-console zen-terminal zen-history zen-bridge zen-switchboard loom
            zen-warnings zen-sanitize)
if(TARGET zen-kernel)
    target_link_libraries(loom-host PRIVATE zen-kernel)
    target_compile_definitions(loom-host PRIVATE ZEN_HOST_HAS_KERNEL=1)
else()
    target_compile_definitions(loom-host PRIVATE ZEN_HOST_HAS_KERNEL=0)
endif()

# ---- loom-runs: the run manager a session host boots, and the Python session tooling ---------
# Optional: `loom-host --serve` serves clients with or without it. `loom-runs` is an ordinary
# loadable weave that starts each run's worker as a Python process; `python/loom_session` is the
# client library, the CLI and the worker's runtime. Building needs no Python, only running a tool
# does (docs/guides/sessions.md). A loadable weave needs the kernel, so this rides it.
option(LOOM_BUILD_SESSION_TOOLS
       "Build the run manager (loom-runs) and install the Python session tooling" ON)
if(LOOM_BUILD_SESSION_TOOLS AND TARGET zen-kernel)
    # The manager's own machinery -- the catalog and the worker process -- as a library the suite
    # reaches too. Its objects land inside the loadable image, so it takes the weave contract.
    add_library(zen-runs STATIC
        src/runs/catalog.cpp
        src/runs/process.cpp)
    target_include_directories(zen-runs
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/runs>)
    target_compile_features(zen-runs PUBLIC cxx_std_20)
    target_link_libraries(zen-runs
        PUBLIC  loom zen-switchboard zen-sanitize
        PRIVATE zen-warnings ${CMAKE_DL_LIBS})
    loom_weave_build_contract(zen-runs)

    add_library(loom-runs MODULE src/runs/runs_weave.cpp)
    target_link_libraries(loom-runs PRIVATE zen-runs loom zen-switchboard zen-warnings zen-sanitize)
    loom_weave_build_contract(loom-runs)
    # One directory holds the artifact and, beside it, the runtime it starts workers with --
    # `<dir>/python/loom_session` -- in the build tree exactly as in an installed prefix, so the
    # manager finds it the same way in both.
    set(loom_session_tools_dir ${CMAKE_BINARY_DIR}/session-tools)
    set_target_properties(loom-runs PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY ${loom_session_tools_dir}
        RUNTIME_OUTPUT_DIRECTORY ${loom_session_tools_dir})
    # Copied on EVERY build, not only when the artifact relinks: the Python is edited on its own,
    # and a runtime older than its sources is exactly the stale copy this must never serve.
    add_custom_target(loom-session-runtime ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/python/loom_session
                ${loom_session_tools_dir}/python/loom_session
        COMMENT "Placing the loom_session runtime beside loom-runs")
    add_dependencies(loom-runs loom-session-runtime)
endif()

# ---- The bridge: a bus reached over a socket ---------------------------------------------
# Portable: the bridge protocol crosses the host boundary over POSIX sockets or Winsock (ws2_32),
# on an AF_UNIX socket or loopback TCP; the bridge host listens on 127.0.0.1 only. `zen-bridge`
# is the crossing (the framed channel, the host side with its admission seam, the console-free
# client and the link envelope) and links only the bus and the core, so it is exported.
# `zen-bridge-console` is the operator's console over it, and stays home with the console.
add_library(zen-bridge
    src/bridge/channel.cpp
    src/bridge/server.cpp
    src/bridge/client.cpp)
target_include_directories(zen-bridge
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(zen-bridge PUBLIC cxx_std_20)
target_link_libraries(zen-bridge
    PUBLIC  loom zen-switchboard zen-sanitize
    PRIVATE zen-warnings)
if(WIN32)
    target_link_libraries(zen-bridge PUBLIC ws2_32)
endif()
add_library(loom::bridge ALIAS zen-bridge)

add_library(zen-bridge-console
    src/bridge/remote_console.cpp)
target_include_directories(zen-bridge-console
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(zen-bridge-console PUBLIC cxx_std_20)
target_link_libraries(zen-bridge-console
    PUBLIC  zen-bridge zen-console loom zen-switchboard zen-sanitize
    PRIVATE zen-warnings)
add_library(loom::bridge-console ALIAS zen-bridge-console)

# A host: a bus, one demo weave and the BridgeServer, listening for remote operators.
add_executable(zen-bridge-host src/bridge/bridge_host_main.cpp)
target_link_libraries(zen-bridge-host PRIVATE zen-bridge zen-switchboard loom zen-warnings zen-sanitize)

# A non-interactive remote operator that drives the protocol end to end and prints PASS or FAIL.
# Portable, so it can cross a real boundary (TCP localhost).
add_executable(zen-bridge-probe src/bridge/bridge_probe_main.cpp)
target_link_libraries(zen-bridge-probe PRIVATE zen-bridge-console zen-bridge zen-console zen-switchboard loom zen-warnings zen-sanitize)

# The interactive remote console: the same ConsoleUi and renderer as the in-process TUI, with the
# engine behind a socket (RemoteConsole) and a loop that multiplexes input and the socket.
add_executable(zen-console-remote src/console/console_remote.cpp)
target_link_libraries(zen-console-remote
    PRIVATE zen-tui zen-bridge-console zen-bridge zen-console zen-switchboard loom zen-warnings zen-sanitize)

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

# Every program defined above, the tests' and the examples' among them, runs in the UTF-8 code
# page on Windows.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/loom-code-page.cmake)
loom_program_code_page()

# ---- The consumer surface: install and export (find_package(loom)) ----------------------
# Exported: only the substrate that is Loom's for good and that a consumer outside this repository
# needs: the core, the switchboard, the terminal core, history, the bridge crossing and, where
# built, the kernel. The UI vocabulary, the console, the TUI and isolation stay home, so a
# consumer that leans on one fails on the default path. The UI pair is a relational tree, not an
# application canvas's model (no authored placement or absolute extent).

# EXPORT_NAME gives the installed targets the same spellings as the in-tree ALIASes (loom::core,
# loom::switchboard), so a build from source and a build against the package resolve the same
# names.

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
    set_target_properties(zen-history     PROPERTIES EXPORT_NAME history)
    set_target_properties(zen-bridge      PROPERTIES EXPORT_NAME bridge)
    set_target_properties(zen-sanitize    PROPERTIES EXPORT_NAME sanitize)
    set_target_properties(zen-warnings    PROPERTIES EXPORT_NAME warnings)

    # zen-sanitize is a PUBLIC dependency of both libraries and zen-warnings survives in a static
    # library's link interface as $<LINK_ONLY:>, so both ride the export set; they are INTERFACE
    # targets, and LINK_ONLY keeps Loom's -Werror and -Wconversion off a consumer's sources.
    # zen-terminal, zen-history and zen-bridge qualify on their contents: participant semantics,
    # a history lens and a crossing, with no console, UI vocabulary or renderer in any of them.
    install(TARGETS loom zen-switchboard zen-terminal zen-history zen-bridge zen-sanitize zen-warnings
        EXPORT loomTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # The one installed executable, not in the export set: a consumer links libraries, not a
    # host. The libraries it links and Loom does not export (zen-console, zen-ui) stay unexported;
    # a static host binary carries its own copy.
    install(TARGETS loom-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # THE SESSION TOOLING, WHERE IT WAS BUILT: the run manager and, beside it, the Python runtime
    # it starts workers with (`lib/loom/loom-runs.<so|dll>` and `lib/loom/python/loom_session`),
    # the human CLI's two launchers, and a small Loom-only tool package. None of it is part of
    # the export set: a consumer links nothing here, a session host loads the artifact by path.
    if(TARGET loom-runs)
        install(TARGETS loom-runs
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/loom
            RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/loom)
        install(DIRECTORY python/loom_session
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/loom/python
            PATTERN "__pycache__" EXCLUDE
            PATTERN "*.pyc" EXCLUDE)
        install(PROGRAMS tools/loom-session tools/loom-session.cmd
            DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(DIRECTORY tools/basics
            DESTINATION ${CMAKE_INSTALL_DATADIR}/loom/tools
            PATTERN "__pycache__" EXCLUDE
            PATTERN "*.pyc" EXCLUDE)
    endif()

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
            PATTERN "remote_console.hpp" EXCLUDE
            PATTERN "console"   EXCLUDE
            PATTERN "isolation" EXCLUDE
            PATTERN "ui"        EXCLUDE)

    install(EXPORT loomTargets
        FILE loomTargets.cmake
        NAMESPACE loom::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom)

    # The installed programs the package names (loomConfig.cmake.in says what each one is).
    set(LOOM_HOST_INSTALL_DIR ${CMAKE_INSTALL_BINDIR})
    set(LOOM_SESSION_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/loom)
    if(TARGET loom-runs)
        set(LOOM_SESSION_TOOLS_INSTALLED TRUE)
    else()
        set(LOOM_SESSION_TOOLS_INSTALLED FALSE)
    endif()
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/loomConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/loomConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom
        PATH_VARS LOOM_HOST_INSTALL_DIR LOOM_SESSION_INSTALL_DIR)

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
