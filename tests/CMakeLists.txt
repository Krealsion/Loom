# A single test binary; the doctest framework is compiled exactly once in
# doctest_main.cpp. Each suite is registered with CTest so `ctest` reports them
# individually and `-R <suite>` can target one.
#
# Platform gating: the kernel/isolation-dependent test sources, the loadable Weave .so's, the
# child host, and the cgroup-scoped suites are Linux-only and live behind `if(NOT WIN32)`, so the
# portable subset builds and runs on Windows. On Linux every gate is taken — the full source set,
# the full weavelib set, and the full suite list are exactly as before (the gating is invisible).
#
# Note: test_capabilities is gated Linux-only because it includes <zen/kernel/kernel.hpp> and loads
# a real .so via ZEN_SO_WEAVE (it exercises capability enforcement through the kernel loader), so it
# cannot build without the kernel + dlopen — despite "capabilities" reading as portable.

add_executable(zen-tests
    doctest_main.cpp
    test_schema.cpp
    test_value.cpp
    test_gate.cpp
    test_registry.cpp
    test_serialize.cpp
    test_compat.cpp
    test_integration.cpp
    test_fuzz.cpp
    test_switchboard.cpp
    test_harness.cpp
    test_breathing.cpp
    test_schema_codec.cpp
    test_weave_shape.cpp
    test_weave.cpp
    test_poke.cpp
    test_console.cpp
    test_component.cpp
    test_pixel.cpp
    test_bridge.cpp)

# Kernel/isolation-dependent suites: compiled only where the kernel + sandbox exist.
if(NOT WIN32)
    target_sources(zen-tests PRIVATE
        test_kernel.cpp
        test_capabilities.cpp
        test_policy.cpp
        test_isolation.cpp)
endif()

target_include_directories(zen-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/console) # tui_render.hpp / terminal.hpp for the tui_map_key pin

target_link_libraries(zen-tests
    PRIVATE loom zen-switchboard zen-ui zen-ui-pixel zen-console zen-bridge zen-tui
            zen-warnings zen-sanitize)
if(NOT WIN32)
    target_link_libraries(zen-tests PRIVATE zen-kernel zen-isolation)
endif()

# ---- Loadable test Weave libraries (.so) + the child host (Linux-only) ----
# These are dlopen-loaded by the kernel/isolation/capabilities/policy suites; none exist on Windows.
if(NOT WIN32)

# One configurable source compiled into several variants, plus a bad-ABI lib.
function(zen_add_test_weave target)
    add_library(${target} SHARED weavelib/test_weave.cpp)
    target_link_libraries(${target} PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
    target_compile_definitions(${target} PRIVATE ${ARGN})
endfunction()

zen_add_test_weave(zen_test_weave)
zen_add_test_weave(zen_test_weave_b) # an identical "rebuild", for hot-reload
zen_add_test_weave(zen_test_weave_badsnap ZEN_WEAVE_MALFORMED_SNAPSHOT)
zen_add_test_weave(zen_test_weave_badmsg ZEN_WEAVE_MALFORMED_MESSAGE)
zen_add_test_weave(zen_test_weave_v2 ZEN_WEAVE_STATE_V2)
# Isolation harness: a child that crashes (mid-handle + on revive, with a small
# reload budget) and one that is deliberately silent.
zen_add_test_weave(zen_test_weave_crasher
    ZEN_WEAVE_CRASH_ON_MAGIC ZEN_WEAVE_CRASH_ON_REVIVE ZEN_WEAVE_LOW_RELOADS)
zen_add_test_weave(zen_test_weave_silent ZEN_WEAVE_SILENT)
# B3: a child that reaches for the network directly, to prove the OS sandbox blocks it.
zen_add_test_weave(zen_test_weave_netprobe ZEN_WEAVE_NET_PROBE)
# B4: a child that reaches for the filesystem directly, to prove the mount-ns view.
zen_add_test_weave(zen_test_weave_fsprobe ZEN_WEAVE_FS_PROBE)
# B5: a child that bombs memory (OOM-kill under memory.max, with a small reload budget so
# it quarantines) and one that fork-bombs (bounded by pids.max).
zen_add_test_weave(zen_test_weave_membomb ZEN_WEAVE_MEM_BOMB ZEN_WEAVE_LOW_RELOADS)
zen_add_test_weave(zen_test_weave_forkbomb ZEN_WEAVE_FORK_BOMB)

add_library(zen_test_badabi SHARED weavelib/bad_abi.cpp)
target_link_libraries(zen_test_badabi PRIVATE loom zen-warnings zen-sanitize)

# Policy phase (P1): a woven mod that *asks* for network + filesystem write (via
# ZEN_ASK), used to prove the ask is not a grant — it still lands on the floor.
add_library(zen_test_mod_storage SHARED weavelib/mod_storage.cpp)
target_link_libraries(zen_test_mod_storage PRIVATE loom zen-switchboard zen-warnings zen-sanitize)

# Policy phase (P1, Part B): a storage-client mod that persists/retrieves purely by
# messaging the "storage" broker (zero disk), and the StorageBroker itself.
add_library(zen_test_storage_client SHARED weavelib/storage_client.cpp)
target_link_libraries(zen_test_storage_client PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
add_library(zen_test_storage_broker SHARED weavelib/storage_broker.cpp)
target_link_libraries(zen_test_storage_broker PRIVATE loom zen-switchboard zen-warnings zen-sanitize)

# Policy phase (P2, the NetworkBroker): a net-client mod that reaches the network only
# through the broker, and the NetworkBroker itself (raw TCP, allow-list, no dependency).
add_library(zen_test_net_client SHARED weavelib/net_client.cpp)
target_link_libraries(zen_test_net_client PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
add_library(zen_test_net_broker SHARED weavelib/net_broker.cpp)
target_link_libraries(zen_test_net_broker PRIVATE loom zen-switchboard zen-warnings zen-sanitize)

# Confused-deputy proof (Part 3): a malicious mod that forges a role-send's wire reply_to to point
# at a different Weave; the host must ignore it and reply to the stamped sender.
add_library(zen_test_forge_client SHARED weavelib/forge_client.cpp)
target_link_libraries(zen_test_forge_client PRIVATE loom zen-switchboard zen-warnings zen-sanitize)

set(ZEN_WEAVE_LIBS zen_test_weave zen_test_weave_b zen_test_weave_badsnap
    zen_test_weave_badmsg zen_test_weave_v2 zen_test_weave_crasher
    zen_test_weave_silent zen_test_weave_netprobe zen_test_weave_fsprobe
    zen_test_weave_membomb zen_test_weave_forkbomb zen_test_mod_storage
    zen_test_storage_client zen_test_storage_broker zen_test_net_client
    zen_test_net_broker zen_test_forge_client zen_test_badabi)
add_dependencies(zen-tests ${ZEN_WEAVE_LIBS})

# The isolation tests spawn the real child-host executable.
add_dependencies(zen-tests zen-weave-host)

# Hand the .so paths and the child-host path to the tests as string macros.
target_compile_definitions(zen-tests PRIVATE
    "ZEN_SO_WEAVE=\"$<TARGET_FILE:zen_test_weave>\""
    "ZEN_SO_WEAVE_B=\"$<TARGET_FILE:zen_test_weave_b>\""
    "ZEN_SO_BADSNAP=\"$<TARGET_FILE:zen_test_weave_badsnap>\""
    "ZEN_SO_BADMSG=\"$<TARGET_FILE:zen_test_weave_badmsg>\""
    "ZEN_SO_V2=\"$<TARGET_FILE:zen_test_weave_v2>\""
    "ZEN_SO_CRASHER=\"$<TARGET_FILE:zen_test_weave_crasher>\""
    "ZEN_SO_SILENT=\"$<TARGET_FILE:zen_test_weave_silent>\""
    "ZEN_SO_NETPROBE=\"$<TARGET_FILE:zen_test_weave_netprobe>\""
    "ZEN_SO_FSPROBE=\"$<TARGET_FILE:zen_test_weave_fsprobe>\""
    "ZEN_SO_MEMBOMB=\"$<TARGET_FILE:zen_test_weave_membomb>\""
    "ZEN_SO_FORKBOMB=\"$<TARGET_FILE:zen_test_weave_forkbomb>\""
    "ZEN_SO_MOD_STORAGE=\"$<TARGET_FILE:zen_test_mod_storage>\""
    "ZEN_SO_STORAGE_CLIENT=\"$<TARGET_FILE:zen_test_storage_client>\""
    "ZEN_SO_STORAGE_BROKER=\"$<TARGET_FILE:zen_test_storage_broker>\""
    "ZEN_SO_NET_CLIENT=\"$<TARGET_FILE:zen_test_net_client>\""
    "ZEN_SO_NET_BROKER=\"$<TARGET_FILE:zen_test_net_broker>\""
    "ZEN_SO_FORGE_CLIENT=\"$<TARGET_FILE:zen_test_forge_client>\""
    "ZEN_SO_BADABI=\"$<TARGET_FILE:zen_test_badabi>\""
    "ZEN_WEAVE_HOST_EXE=\"$<TARGET_FILE:zen-weave-host>\"")

endif() # NOT WIN32 (loadable Weave libraries + child host + .so macros)

# doctest's own header trips -Wsign-conversion etc.; it is third-party, so we do
# not hold it to Zen's warning policy. Mark its include dir as system.
target_include_directories(zen-tests SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# doctest returns the failed-assertion count as its exit code, so CTest's default
# exit-status check is the source of truth — no output-scraping needed. The portable
# suites register on every platform.
set(ZEN_TEST_SUITES schema value gate registry serialize compat integration fuzz
    switchboard harness breathing schema_codec weave_shape weave poke console component
    pixel bridge)
foreach(suite IN LISTS ZEN_TEST_SUITES)
    add_test(NAME ${suite} COMMAND zen-tests --test-suite=${suite} --no-skipped-summary)
endforeach()

# The SDL skin's smoke: a SEPARATE binary (zen-tests stays SDL-free) that runs the full
# pipeline headless under SDL's dummy video driver — real SDL, real TTF metrics, no display.
# Registered only when the gated SDL renderer is built; the projection LOGIC is pinned
# SDL-free in the `pixel` suite above, which runs everywhere.
if(TARGET zen-ui-sdl)
    add_executable(zen-sdl-tests doctest_main.cpp test_sdl.cpp)
    target_include_directories(zen-sdl-tests SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    target_include_directories(zen-sdl-tests PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(zen-sdl-tests
        PRIVATE zen-ui-sdl zen-ui-pixel zen-ui zen-warnings zen-sanitize)
    add_test(NAME sdl COMMAND zen-sdl-tests --no-skipped-summary)
    set_tests_properties(sdl PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
endif()

# The kernel/capabilities suites and the cgroup-scoped isolation/policy/all suites are Linux-only.
if(NOT WIN32)
    foreach(suite IN ITEMS kernel capabilities)
        add_test(NAME ${suite} COMMAND zen-tests --test-suite=${suite} --no-skipped-summary)
    endforeach()

    # The isolation and policy suites mount out-of-process Weaves under the default resource
    # containment, which needs cgroup-v2 delegation a plain invocation lacks (it lands in the root
    # cgroup). run-under-scope.sh launches the binary inside an unprivileged delegated systemd scope
    # when one is available.
    add_test(NAME isolation COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run-under-scope.sh
        $<TARGET_FILE:zen-tests> --test-suite=isolation --no-skipped-summary)
    add_test(NAME policy COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run-under-scope.sh
        $<TARGET_FILE:zen-tests> --test-suite=policy --no-skipped-summary)

    # A catch-all that runs everything (also under a scope, since it includes isolation).
    add_test(NAME all COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run-under-scope.sh
        $<TARGET_FILE:zen-tests>)
endif() # NOT WIN32 (kernel/capabilities/isolation/policy/all suites)
