# A single test binary; the doctest framework is compiled exactly once in
# doctest_main.cpp. Each suite is registered with CTest so `ctest` reports them
# individually and `-R <suite>` can target one.
#
# Gating: KERNEL-dependent pieces (the kernel/manager/capabilities suites and the portable
# loadable-weave fixtures) ride `if(TARGET zen-kernel)` — the kernel exists on Linux always and
# on Windows only under LOOM_ENABLE_WINDOWS_KERNEL (the opt-in development/demo backend), so the
# suites follow the target wherever it is. ISOLATION-dependent pieces (the sandbox suites, the
# probe/bomb fixtures, the policy mods, the child host, the cgroup-scoped registrations) stay
# behind `if(NOT WIN32)` — the sandbox exists only there. On Linux every gate is taken — the full
# source set, the full weavelib set, and the full suite list are exactly as before.
#
# Note: test_capabilities rides the kernel gate because it loads a real weave library via
# ZEN_SO_WEAVE (it exercises capability enforcement through the kernel loader) — despite
# "capabilities" reading as portable, it cannot build without the loader.

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
    test_provenance.cpp
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

# Kernel-dependent suites: compiled wherever the kernel target exists.
if(TARGET zen-kernel)
    target_sources(zen-tests PRIVATE
        test_kernel.cpp
        test_manager.cpp
        test_capabilities.cpp)
endif()
# Isolation-dependent suites: Linux-only (the sandbox exists only there).
if(NOT WIN32)
    target_sources(zen-tests PRIVATE
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
if(TARGET zen-kernel)
    target_link_libraries(zen-tests PRIVATE zen-kernel)
endif()
if(NOT WIN32)
    target_link_libraries(zen-tests PRIVATE zen-isolation)
endif()

# ---- Loadable test Weave libraries + the child host ----
# The PORTABLE fixture set is loaded by the kernel/manager/capabilities suites and follows the
# kernel target (Linux always; Windows under the opt-in backend, where they build as .dll).
# The probe/bomb variants, the policy mods, and the child host are isolation/policy ground —
# Linux-only, as ever.
if(TARGET zen-kernel)

# The -fno-gnu-unique weave-library law (ELF-only; DESIGN "Weaving stays Zen-invisible"):
# a loadable weave's statics must live and die with its own library. PE has no gnu-unique
# mechanism — Windows DLL statics are per-DLL by construction — so the flag is ELF/GCC ground.
set(ZEN_WEAVE_LIB_FLAGS
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<NOT:$<PLATFORM_ID:Windows>>>:-fno-gnu-unique>)

# One configurable source compiled into several variants, plus a bad-ABI lib.
function(zen_add_test_weave target)
    add_library(${target} SHARED weavelib/test_weave.cpp)
    target_link_libraries(${target} PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
    target_compile_options(${target} PRIVATE ${ZEN_WEAVE_LIB_FLAGS})
    target_compile_definitions(${target} PRIVATE ${ARGN})
endfunction()

zen_add_test_weave(zen_test_weave)
zen_add_test_weave(zen_test_weave_b) # an identical "rebuild", for hot-reload
zen_add_test_weave(zen_test_weave_badsnap ZEN_WEAVE_MALFORMED_SNAPSHOT)
zen_add_test_weave(zen_test_weave_badmsg ZEN_WEAVE_MALFORMED_MESSAGE)
zen_add_test_weave(zen_test_weave_v2 ZEN_WEAVE_STATE_V2)
# 1b (the letter): a predecessor that declares zen.PrepareShutdown and writes a
# Bequest, and a DIFFERENTLY-SHAPED heir (Counter v2) that claims by role on wake.
zen_add_test_weave(zen_test_weave_bequeaths ZEN_WEAVE_BEQUEATHS)
zen_add_test_weave(zen_test_weave_heir ZEN_WEAVE_HEIR)
# ...and one that declares the ceremony then never answers: the honest wedge.
zen_add_test_weave(zen_test_weave_wedged ZEN_WEAVE_WEDGED)
# R2A-1 (activation): a participant that declares zen.Activated and records what it
# was told, its identical "rebuild" for reload-in-place, and a DRIFT twin whose state
# schema is identical but whose door contract has one extra shape — the negative that
# proves reload compares the whole contract, not just the state.
zen_add_test_weave(zen_test_weave_activates ZEN_WEAVE_ACTIVATES)
zen_add_test_weave(zen_test_weave_activates_b ZEN_WEAVE_ACTIVATES)
zen_add_test_weave(zen_test_weave_activates_drift ZEN_WEAVE_ACTIVATES ZEN_WEAVE_ACTIVATES_DRIFT)
# R2A-1a: the drift twin whose extra door DISAGREES about Greet v1, so the agreement
# wall makes a rejected candidate's schema admission observable from outside.
zen_add_test_weave(zen_test_weave_activates_conflict
    ZEN_WEAVE_ACTIVATES ZEN_WEAVE_ACTIVATES_CONFLICT)

add_library(zen_test_badabi SHARED weavelib/bad_abi.cpp)
target_link_libraries(zen_test_badabi PRIVATE loom zen-warnings zen-sanitize)
target_compile_options(zen_test_badabi PRIVATE ${ZEN_WEAVE_LIB_FLAGS})

# R2B-1: the artifact built against the PREVIOUS ABI. bad_abi declares a FUTURE
# version (a host too old for it); this one declares version 1 — the real stale
# artifact the provenance break creates — so "paid explicitly" has a witness.
add_library(zen_test_staleabi SHARED weavelib/stale_abi.cpp)
target_link_libraries(zen_test_staleabi PRIVATE loom zen-warnings zen-sanitize)
target_compile_options(zen_test_staleabi PRIVATE ${ZEN_WEAVE_LIB_FLAGS})

set(ZEN_KERNEL_WEAVE_LIBS zen_test_weave zen_test_weave_b zen_test_weave_badsnap
    zen_test_weave_badmsg zen_test_weave_v2 zen_test_weave_bequeaths zen_test_weave_heir
    zen_test_weave_wedged zen_test_weave_activates zen_test_weave_activates_b
    zen_test_weave_activates_drift zen_test_weave_activates_conflict zen_test_badabi
    zen_test_staleabi)
add_dependencies(zen-tests ${ZEN_KERNEL_WEAVE_LIBS})

# Hand the portable fixtures' paths to the tests as string macros.
target_compile_definitions(zen-tests PRIVATE
    "ZEN_SO_WEAVE=\"$<TARGET_FILE:zen_test_weave>\""
    "ZEN_SO_WEAVE_B=\"$<TARGET_FILE:zen_test_weave_b>\""
    "ZEN_SO_BADSNAP=\"$<TARGET_FILE:zen_test_weave_badsnap>\""
    "ZEN_SO_BADMSG=\"$<TARGET_FILE:zen_test_weave_badmsg>\""
    "ZEN_SO_V2=\"$<TARGET_FILE:zen_test_weave_v2>\""
    "ZEN_SO_BEQUEATHS=\"$<TARGET_FILE:zen_test_weave_bequeaths>\""
    "ZEN_SO_HEIR=\"$<TARGET_FILE:zen_test_weave_heir>\""
    "ZEN_SO_WEDGED=\"$<TARGET_FILE:zen_test_weave_wedged>\""
    "ZEN_SO_ACTIVATES=\"$<TARGET_FILE:zen_test_weave_activates>\""
    "ZEN_SO_ACTIVATES_B=\"$<TARGET_FILE:zen_test_weave_activates_b>\""
    "ZEN_SO_ACTIVATES_DRIFT=\"$<TARGET_FILE:zen_test_weave_activates_drift>\""
    "ZEN_SO_ACTIVATES_CONFLICT=\"$<TARGET_FILE:zen_test_weave_activates_conflict>\""
    "ZEN_SO_BADABI=\"$<TARGET_FILE:zen_test_badabi>\""
    "ZEN_SO_STALEABI=\"$<TARGET_FILE:zen_test_staleabi>\"")

endif() # TARGET zen-kernel (portable fixtures)

if(NOT WIN32)

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

# The policy/forge mods are loadable weave libraries too — same linkage law.
foreach(t IN ITEMS zen_test_mod_storage zen_test_storage_client zen_test_storage_broker
                   zen_test_net_client zen_test_net_broker zen_test_forge_client)
    target_compile_options(${t} PRIVATE ${ZEN_WEAVE_LIB_FLAGS})
endforeach()

set(ZEN_ISOLATION_WEAVE_LIBS zen_test_weave_crasher zen_test_weave_silent
    zen_test_weave_netprobe zen_test_weave_fsprobe zen_test_weave_membomb
    zen_test_weave_forkbomb zen_test_mod_storage zen_test_storage_client
    zen_test_storage_broker zen_test_net_client zen_test_net_broker zen_test_forge_client)
add_dependencies(zen-tests ${ZEN_ISOLATION_WEAVE_LIBS})

# The isolation tests spawn the real child-host executable.
add_dependencies(zen-tests zen-weave-host)

# Hand the Linux-only fixture paths and the child-host path to the tests.
target_compile_definitions(zen-tests PRIVATE
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
    "ZEN_WEAVE_HOST_EXE=\"$<TARGET_FILE:zen-weave-host>\"")

endif() # NOT WIN32 (isolation/policy fixtures + child host)

# doctest's own header trips -Wsign-conversion etc.; it is third-party, so we do
# not hold it to Zen's warning policy. Mark its include dir as system.
target_include_directories(zen-tests SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# doctest returns the failed-assertion count as its exit code, so CTest's default
# exit-status check is the source of truth — no output-scraping needed. The portable
# suites register on every platform.
set(ZEN_TEST_SUITES schema value gate registry serialize compat integration fuzz
    switchboard provenance harness breathing schema_codec weave_shape weave poke console component
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

# The kernel-family suites register wherever the kernel target exists.
if(TARGET zen-kernel)
    foreach(suite IN ITEMS kernel manager capabilities)
        add_test(NAME ${suite} COMMAND zen-tests --test-suite=${suite} --no-skipped-summary)
    endforeach()
endif()

# The cgroup-scoped isolation/policy/all suites are Linux-only.
if(NOT WIN32)
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
endif() # NOT WIN32 (isolation/policy/all suites)
