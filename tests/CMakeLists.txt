# A single test binary; the doctest framework is compiled exactly once in
# doctest_main.cpp. Each suite is registered with CTest so `ctest` reports them
# individually and `-R <suite>` can target one.
#
# Population honesty (R2F-D). Three mechanisms, none of which trusts the other two:
#   * registration is DERIVED from the TEST_SUITE(...) declarations in the compiled
#     sources, so there is no second list to drift out of sync with them;
#   * doctest_main.cpp refuses a run that selected zero cases, so a registered name that
#     no longer matches anything fails instead of passing in 0.00 s;
#   * suite_population.txt + check_population.cmake pin the exact suite inventory per
#     platform gate and a per-suite case floor, so a suite that VANISHES is caught even
#     though a vanished suite registers nothing to fail.
# The official lane that ties them together is tests/verify.cmake (see AGENTS.md).
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

# The test sources, kept in named lists rather than inline: they are the AUTHORITATIVE
# suite inventory, and the CTest registration below is derived from them by reading the
# TEST_SUITE(...) declarations out of exactly the sources this configuration compiles.
# (R2F-D / POP-01. Before it, a hand-maintained ZEN_TEST_SUITES list sat beside them, and
# a renamed suite left the old CTest name registered against nothing: `ctest -R switchboard`
# reported Passed in 0.00 s having run 0 cases. Two lists that must agree, with nothing
# making them agree, is the whole defect.)
set(ZEN_TEST_SOURCES_PORTABLE
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
    test_role_authorship.cpp
    test_sense.cpp
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
set(ZEN_TEST_SOURCES_KERNEL
    test_kernel.cpp
    test_handoff.cpp
    test_manager.cpp
    test_capabilities.cpp)
# Isolation-dependent suites: Linux-only (the sandbox exists only there).
set(ZEN_TEST_SOURCES_POSIX
    test_policy.cpp
    test_isolation.cpp)

add_executable(zen-tests doctest_main.cpp ${ZEN_TEST_SOURCES_PORTABLE})

# The sources actually compiled here, split by how their suites must be LAUNCHED (the
# sandbox suites need a delegated cgroup scope), and the gates that are actually on. Both
# feed the derived registration and the population contract; a gate that is OFF makes its
# suites DECLARED ABSENT, which is a different thing from "ran and passed".
set(ZEN_TEST_SOURCES_DIRECT ${ZEN_TEST_SOURCES_PORTABLE})
set(ZEN_TEST_SOURCES_SCOPED "")
set(ZEN_POPULATION_GATES portable)

if(TARGET zen-kernel)
    target_sources(zen-tests PRIVATE ${ZEN_TEST_SOURCES_KERNEL})
    list(APPEND ZEN_TEST_SOURCES_DIRECT ${ZEN_TEST_SOURCES_KERNEL})
    list(APPEND ZEN_POPULATION_GATES kernel)
endif()
if(NOT WIN32)
    target_sources(zen-tests PRIVATE ${ZEN_TEST_SOURCES_POSIX})
    list(APPEND ZEN_TEST_SOURCES_SCOPED ${ZEN_TEST_SOURCES_POSIX})
    list(APPEND ZEN_POPULATION_GATES posix)
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
# R2B-2: the DEFERRING STEWARD — a dynamically loaded weave that takes an ask's
# answer right away with it, returns without answering, and answers from a LATER
# handler. It is the central proof, and it must be a real .so: the whole question
# is whether the right survives the handler that earned it, and a native lambda
# holding the old Mail would answer a different question.
zen_add_test_weave(zen_test_weave_defers ZEN_WEAVE_DEFERS)
# R2B-3b-1a: the immediate-answer parity fixture, and its answer-twice twin.
zen_add_test_weave(zen_test_weave_answers ZEN_WEAVE_ANSWERS)
zen_add_test_weave(zen_test_weave_answers_twice ZEN_WEAVE_ANSWERS ZEN_WEAVE_ANSWERS_TWICE)
# R2E-0 / Night Lab III P-011: the silent-seam reproducer. Emits a shape only this
# artifact knows, so the host seam cannot resolve it.
zen_add_test_weave(zen_test_weave_seam_emit ZEN_WEAVE_SEAM_EMIT)
# R2E-0/v6: the Sense parity fixture — claims, forged-office refusal, and a
# synchronous read-back, all from the far side of the seam.
zen_add_test_weave(zen_test_weave_senses ZEN_WEAVE_SENSES)

# R2B-3b-3: the `versioned.service` pair — ONE source, two real artifacts. v1 is
# the incumbent that answers "v1" throughout; v2 (ZEN_VERSIONED_CANDIDATE) is the
# same production service plus the preparation conversation. Built from the same
# source on purpose: the artifact that prepares must be the artifact that goes live.
function(zen_add_versioned_service target)
    add_library(${target} SHARED weavelib/versioned_service.cpp)
    target_link_libraries(${target} PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
    target_compile_options(${target} PRIVATE ${ZEN_WEAVE_LIB_FLAGS})
    target_compile_definitions(${target} PRIVATE ${ARGN})
endfunction()

zen_add_versioned_service(zen_test_versioned_v1)
zen_add_versioned_service(zen_test_versioned_v2 ZEN_VERSIONED_CANDIDATE)

# R2E-0: the Handoff Garden. Two REAL ledger artifacts whose state schemas are
# genuinely incompatible (flat scalars vs nested messages + a list), plus the
# temporary migrator that authors v1 -> v2 meaning and is unloaded afterwards.
function(zen_add_handoff_ledger target)
    add_library(${target} SHARED weavelib/handoff_ledger.cpp)
    target_link_libraries(${target} PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
    target_compile_options(${target} PRIVATE ${ZEN_WEAVE_LIB_FLAGS})
    target_compile_definitions(${target} PRIVATE ${ARGN})
endfunction()

zen_add_handoff_ledger(zen_test_handoff_v1)
zen_add_handoff_ledger(zen_test_handoff_v2 ZEN_HANDOFF_V2)

add_library(zen_test_handoff_migrator SHARED weavelib/handoff_migrator.cpp)
target_link_libraries(zen_test_handoff_migrator
    PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
target_compile_options(zen_test_handoff_migrator PRIVATE ${ZEN_WEAVE_LIB_FLAGS})

# R2D-0: the office worker — dynamic office-authorship parity. Loaded in-process
# under the role "worker.a" (kernel suite) and out-of-process (isolation suite),
# where the pipe's fail-closed law is the thing under test.
add_library(zen_test_office_worker SHARED weavelib/office_worker.cpp)
target_link_libraries(zen_test_office_worker PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
target_compile_options(zen_test_office_worker PRIVATE ${ZEN_WEAVE_LIB_FLAGS})

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
    zen_test_staleabi zen_test_weave_defers zen_test_weave_answers
    zen_test_weave_answers_twice zen_test_weave_seam_emit zen_test_weave_senses
    zen_test_versioned_v1 zen_test_versioned_v2
    zen_test_handoff_v1 zen_test_handoff_v2 zen_test_handoff_migrator
    zen_test_office_worker)
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
    "ZEN_SO_STALEABI=\"$<TARGET_FILE:zen_test_staleabi>\""
    "ZEN_SO_DEFERS=\"$<TARGET_FILE:zen_test_weave_defers>\""
    "ZEN_SO_ANSWERS=\"$<TARGET_FILE:zen_test_weave_answers>\""
    "ZEN_SO_ANSWERS_TWICE=\"$<TARGET_FILE:zen_test_weave_answers_twice>\""
    "ZEN_SO_SEAM_EMIT=\"$<TARGET_FILE:zen_test_weave_seam_emit>\""
    "ZEN_SO_SENSES=\"$<TARGET_FILE:zen_test_weave_senses>\""
    "ZEN_SO_VERSIONED_V1=\"$<TARGET_FILE:zen_test_versioned_v1>\""
    "ZEN_SO_VERSIONED_V2=\"$<TARGET_FILE:zen_test_versioned_v2>\""
    "ZEN_SO_HANDOFF_V1=\"$<TARGET_FILE:zen_test_handoff_v1>\""
    "ZEN_SO_HANDOFF_V2=\"$<TARGET_FILE:zen_test_handoff_v2>\""
    "ZEN_SO_HANDOFF_MIGRATOR=\"$<TARGET_FILE:zen_test_handoff_migrator>\""
    "ZEN_SO_OFFICE_WORKER=\"$<TARGET_FILE:zen_test_office_worker>\"")

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
    zen_test_storage_broker zen_test_net_client zen_test_net_broker zen_test_forge_client
    # R2B-2: the deferring steward is mounted out-of-process too, to pin that the
    # capability FAILS CLOSED across the pipe rather than being faked across it.
    zen_test_weave_defers)
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

# ---- CTest registration, DERIVED from the suite declarations in the sources ----------
#
# doctest returns the failed-assertion count as its exit code, and doctest_main.cpp turns a
# zero-case run into exit 70, so CTest's exit-status check is the source of truth for both
# "did it fail" and "was there anything to run at all" — no output-scraping needed.
#
# Reading the names out of the sources is what removes the F-2 drift by construction: there
# is no second place to keep in sync, so a rename MOVES the registered test instead of
# orphaning it. The three things a derivation cannot see on its own are covered elsewhere,
# deliberately:
#   * a suite compiled out by an #if, or one whose cases all vanish — the source still names
#     it, so it still registers, and the run selects 0 cases → doctest_main.cpp fails it;
#   * a source that disappears from the lists above — its suite leaves the built binary, and
#     tests/suite_population.txt no longer matches → the `population` test fails;
#   * a suite absent because its platform gate is off — declared absent in the manifest,
#     reported as absent, never reported as passed.
#
# The derivation runs at CONFIGURE time, and deliberately does not add the sources to
# CMAKE_CONFIGURE_DEPENDS: on the WSL/drvfs lane that would re-configure for minutes on every
# test edit, and it buys nothing, because the stale window fails closed from both ends. Rename
# a suite and merely rebuild: the old name is still registered, selects zero cases, and
# doctest_main.cpp fails it. Re-configure: the registration moves to the truthful new name and
# the `population` contract fails until the manifest is updated on purpose. Neither path can
# produce a green.
function(zen_suites_from_sources out_var)
    set(discovered "")
    foreach(src IN LISTS ARGN)
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${src}" declarations
             REGEX "^[ ]*TEST_SUITE(_BEGIN)?[ ]*\\(\"[A-Za-z0-9_]+\"")
        set(in_this_source "")
        foreach(declaration IN LISTS declarations)
            string(REGEX MATCH "\"([A-Za-z0-9_]+)\"" _matched "${declaration}")
            list(APPEND in_this_source "${CMAKE_MATCH_1}")
        endforeach()
        if(NOT in_this_source)
            message(FATAL_ERROR
                "zen tests: ${src} is compiled into zen-tests but declares no TEST_SUITE(...). "
                "Suite registration is derived from those declarations, so this source's cases "
                "would run only in the aggregate `all` lane and no named CTest entry could ever "
                "select them.")
        endif()
        list(APPEND discovered ${in_this_source})
    endforeach()
    list(REMOVE_DUPLICATES discovered)
    set(${out_var} "${discovered}" PARENT_SCOPE)
endfunction()

zen_suites_from_sources(ZEN_TEST_SUITES_DIRECT ${ZEN_TEST_SOURCES_DIRECT})
foreach(suite IN LISTS ZEN_TEST_SUITES_DIRECT)
    add_test(NAME ${suite} COMMAND zen-tests --test-suite=${suite} --no-skipped-summary)
endforeach()

# The guard's own witnesses, and the reason they are two entries rather than one: CTest
# ignores the exit code when PASS_REGULAR_EXPRESSION is set, so a single test could pin
# either the refusal or the diagnostic but not both. This IS the F-2 control, run every
# lane: a filter naming a suite that no compiled case belongs to (the shape a rename, an
# #if or a deletion produces) must exit nonzero AND say why.
add_test(NAME empty_population_refused
    COMMAND zen-tests --test-suite=zen_no_such_suite_R2FD --no-skipped-summary)
set_tests_properties(empty_population_refused PROPERTIES WILL_FAIL TRUE)

add_test(NAME empty_population_says_so
    COMMAND zen-tests --test-suite=zen_no_such_suite_R2FD --no-skipped-summary)
set_tests_properties(empty_population_says_so PROPERTIES
    PASS_REGULAR_EXPRESSION "EMPTY TEST POPULATION")

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

# The cgroup-scoped suites are Linux-only, and their NAMES are derived exactly like the
# direct ones — only the launcher differs. The isolation and policy suites mount
# out-of-process Weaves under the default resource containment, which needs cgroup-v2
# delegation a plain invocation lacks (it lands in the root cgroup). run-under-scope.sh
# launches the binary inside an unprivileged delegated systemd scope when one is available.
if(NOT WIN32)
    zen_suites_from_sources(ZEN_TEST_SUITES_SCOPED ${ZEN_TEST_SOURCES_SCOPED})
    foreach(suite IN LISTS ZEN_TEST_SUITES_SCOPED)
        add_test(NAME ${suite} COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run-under-scope.sh
            $<TARGET_FILE:zen-tests> --test-suite=${suite} --no-skipped-summary)
    endforeach()

    # A catch-all that runs everything (also under a scope, since it includes isolation).
    add_test(NAME all COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run-under-scope.sh
        $<TARGET_FILE:zen-tests>)
endif() # NOT WIN32 (isolation/policy/all suites)

# ---- The population contract (POP-01 / POP-02) ---------------------------------------
#
# The registration above cannot see its own absences: a suite that is gone is simply not
# registered, and a CTest listing that is short by six entries looks exactly like a
# platform where six entries were never meant to exist. tests/suite_population.txt is
# where the difference is written down, and this entry is what enforces it — exact suite
# inventory per active gate, plus a per-suite case floor. It runs no cases (doctest query
# modes only), so it costs a fraction of a second and can sit in every lane.
set(zen_sdl_tests_exe "")
if(TARGET zen-sdl-tests)
    list(APPEND ZEN_POPULATION_GATES sdl)
    set(zen_sdl_tests_exe "$<TARGET_FILE:zen-sdl-tests>")
endif()
# Comma-separated: a ;-list would be split into separate arguments on the command line.
string(REPLACE ";" "," zen_population_gates_arg "${ZEN_POPULATION_GATES}")

add_test(NAME population COMMAND ${CMAKE_COMMAND}
    -DZEN_TESTS_EXE=$<TARGET_FILE:zen-tests>
    -DZEN_SDL_TESTS_EXE=${zen_sdl_tests_exe}
    -DZEN_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/suite_population.txt
    -DZEN_GATES=${zen_population_gates_arg}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_population.cmake)
