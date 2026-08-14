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
    test_grant.cpp
    test_weaver.cpp
    test_terminal.cpp
    test_harness.cpp
    test_breathing.cpp
    test_schema_codec.cpp
    test_weave_shape.cpp
    test_weave.cpp
    test_poke.cpp
    test_console.cpp
    test_recorder.cpp
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
    PRIVATE loom zen-switchboard zen-ui zen-ui-pixel zen-console zen-recorder zen-bridge zen-tui
            zen-warnings zen-sanitize)
if(TARGET zen-kernel)
    target_link_libraries(zen-tests PRIVATE zen-kernel)
endif()
if(NOT WIN32)
    target_link_libraries(zen-tests PRIVATE zen-isolation)
endif()

# The independent required-weave-contract population (POP-05, C3): the build-graph sweep
# and the exemption declaration. It reads target types and link closures, never the
# contract roll — which is exactly what lets the `weave_population` entry at the bottom of
# this file notice an artifact that quietly stopped taking the contract.
include(${CMAKE_CURRENT_SOURCE_DIR}/weave_population.cmake)

# ---- Loadable test Weave libraries + the child host ----
# The PORTABLE fixture set is loaded by the kernel/manager/capabilities suites and follows the
# kernel target (Linux always; Windows under the opt-in backend, where they build as .dll).
# The probe/bomb variants, the policy mods, and the child host are isolation/policy ground —
# Linux-only, as ever.
if(TARGET zen-kernel)

# The weave-library law — a loadable weave's statics must live and die with its own
# library (KERN-05) — comes from loom_weave_build_contract(), the same exported function
# a stranger gets from find_package(loom). It was a local variable holding a literal
# -fno-gnu-unique here until R2F-E; the house had the fix and the guest did not (COLD-1
# F-22). Calling the exported surface is what stops the two paths meaning different
# things: if the contract ever stops applying on the stranger's path, these fixtures stop
# getting it too.
#
# WHAT SAYS SO WHEN A FIXTURE LOSES IT — corrected by COLD-2 finding C-3. This comment
# used to claim "the kernel suite's own unload/reload cases say so". They do not, and the
# audit measured it: deleting the one contract line below dropped twenty-three loadable
# weaves off the roll, thirteen STB_GNU_UNIQUE symbols came back into
# libzen_test_weave.so, and every kernel unload/reload case still passed. Those cases
# exercise the LOADER's bookkeeping — exact-once teardown, refused reload, the handle
# chain — which is correct either way; whether the image's statics actually died is a
# build-artifact fact no in-process case observes. The truthful statement is narrower and
# is now a mechanism rather than a hope: the `weave_population` entry compares this
# directory's loadable weaves against the contract roll and NAMES any that left it, and
# `weave_contract` then re-reads the built artifacts' symbol tables. Neither is a test
# case; both run in every lane.
#
# One configurable source compiled into several variants, plus a bad-ABI lib.
function(zen_add_test_weave target)
    add_library(${target} SHARED weavelib/test_weave.cpp)
    target_link_libraries(${target} PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
    loom_weave_build_contract(${target})
    target_compile_definitions(${target} PRIVATE ${ARGN})
    # C4702 (unreachable code) is an MSVC /W4 warning with no GCC counterpart, and here it
    # is CORRECT rather than wrong: this one source is compiled into ~25 variants selected
    # by #if, and a variant whose branch ends in an unconditional `return` (ZEN_WEAVE_DEFERS)
    # genuinely leaves the shared Ping tail below it unreachable. That is the fixture's
    # construction, not a defect in it, and rearranging a lifecycle fixture to satisfy a
    # reachability warning would change test code for a compiler's benefit. Scoped to these
    # variant targets only -- Zen's warning policy is untouched everywhere else.
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/wd4702>)
endfunction()

zen_add_test_weave(zen_test_weave)
zen_add_test_weave(zen_test_weave_b) # an identical "rebuild", for hot-reload
zen_add_test_weave(zen_test_weave_badsnap ZEN_WEAVE_MALFORMED_SNAPSHOT)
zen_add_test_weave(zen_test_weave_badmsg ZEN_WEAVE_MALFORMED_MESSAGE)
zen_add_test_weave(zen_test_weave_v2 ZEN_WEAVE_STATE_V2)
# RTH-1: a loaded handler that THROWS. Its exception is caught at the ABI seam and
# returns as a status, which used to be discarded -- so the bus recorded and announced a
# plain Delivered for a handler that never finished.
zen_add_test_weave(zen_test_weave_throws ZEN_WEAVE_THROW_ON_MAGIC)
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
    loom_weave_build_contract(${target})
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
    loom_weave_build_contract(${target})
    target_compile_definitions(${target} PRIVATE ${ARGN})
endfunction()

zen_add_handoff_ledger(zen_test_handoff_v1)
zen_add_handoff_ledger(zen_test_handoff_v2 ZEN_HANDOFF_V2)

add_library(zen_test_handoff_migrator SHARED weavelib/handoff_migrator.cpp)
target_link_libraries(zen_test_handoff_migrator
    PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
loom_weave_build_contract(zen_test_handoff_migrator)

# R2D-0: the office worker — dynamic office-authorship parity. Loaded in-process
# under the role "worker.a" (kernel suite) and out-of-process (isolation suite),
# where the pipe's fail-closed law is the thing under test.
add_library(zen_test_office_worker SHARED weavelib/office_worker.cpp)
target_link_libraries(zen_test_office_worker PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
loom_weave_build_contract(zen_test_office_worker)

add_library(zen_test_badabi SHARED weavelib/bad_abi.cpp)
target_link_libraries(zen_test_badabi PRIVATE loom zen-warnings zen-sanitize)
loom_weave_build_contract(zen_test_badabi)

# R2B-1: the artifact built against the PREVIOUS ABI. bad_abi declares a FUTURE
# version (a host too old for it); this one declares version 1 — the real stale
# artifact the provenance break creates — so "paid explicitly" has a witness.
add_library(zen_test_staleabi SHARED weavelib/stale_abi.cpp)
target_link_libraries(zen_test_staleabi PRIVATE loom zen-warnings zen-sanitize)
loom_weave_build_contract(zen_test_staleabi)

# R2F-E: the build contract's own pair. ONE source, two artifacts — one through
# loom_weave_build_contract(), one deliberately bypassing it. The bypass twin is not a
# defect: it is the positive control that keeps the question askable, exactly the way
# the stale-ABI fixture keeps "the previous ABI refuses" askable after every bump. It
# is also the only artifact in this tree built to be reload-UNSAFE on purpose, so it
# links nothing — its one symbol cannot alias anything else in the process.
add_library(zen_test_contract_applied SHARED weavelib/contract_sentinel.cpp)
loom_weave_build_contract(zen_test_contract_applied)

add_library(zen_test_contract_bypass SHARED weavelib/contract_sentinel.cpp)
# (no loom_weave_build_contract here — that absence IS the control)
#
# ...and the required-population sweep must be told so IN WRITING, because it classifies
# every shared library in this directory as a loadable weave that owes the contract, and
# it is right to. The exemption lives on the target rather than in a list elsewhere, so it
# cannot outlive the artifact; the reason is mandatory; it is printed on every run; and
# check_weave_population.cmake fails if an exempt target ever turns up ON the roll — which
# is the one way this control could rot into a second contracted twin proving nothing.
zen_weave_contract_exempt(zen_test_contract_bypass
    "the F-22 negative control: same source as zen_test_contract_applied, contract "
    "deliberately withheld, so the unique-symbol sentinel it still carries is what keeps "
    "claim 1 of weave_contract falsifiable")

set(ZEN_KERNEL_WEAVE_LIBS zen_test_weave zen_test_weave_b zen_test_weave_badsnap
    zen_test_weave_badmsg zen_test_weave_v2 zen_test_weave_bequeaths zen_test_weave_heir
    zen_test_weave_wedged zen_test_weave_activates zen_test_weave_activates_b
    zen_test_weave_activates_drift zen_test_weave_activates_conflict zen_test_badabi
    zen_test_staleabi zen_test_weave_defers zen_test_weave_answers
    zen_test_weave_answers_twice zen_test_weave_seam_emit zen_test_weave_senses
    zen_test_versioned_v1 zen_test_versioned_v2
    zen_test_handoff_v1 zen_test_handoff_v2 zen_test_handoff_migrator
    zen_test_office_worker
    zen_test_contract_applied zen_test_contract_bypass)
add_dependencies(zen-tests ${ZEN_KERNEL_WEAVE_LIBS})

# Hand the portable fixtures' paths to the tests as string macros.
target_compile_definitions(zen-tests PRIVATE
    "ZEN_SO_WEAVE=\"$<TARGET_FILE:zen_test_weave>\""
    "ZEN_SO_WEAVE_B=\"$<TARGET_FILE:zen_test_weave_b>\""
    "ZEN_SO_BADSNAP=\"$<TARGET_FILE:zen_test_weave_badsnap>\""
    "ZEN_SO_BADMSG=\"$<TARGET_FILE:zen_test_weave_badmsg>\""
    "ZEN_SO_V2=\"$<TARGET_FILE:zen_test_weave_v2>\""
    "ZEN_SO_THROWS=\"$<TARGET_FILE:zen_test_weave_throws>\""
    "ZEN_SO_BEQUEATHS=\"$<TARGET_FILE:zen_test_weave_bequeaths>\""
    "ZEN_SO_HEIR=\"$<TARGET_FILE:zen_test_weave_heir>\""
    "ZEN_SO_WEDGED=\"$<TARGET_FILE:zen_test_weave_wedged>\""
    "ZEN_SO_ACTIVATES=\"$<TARGET_FILE:zen_test_weave_activates>\""
    "ZEN_SO_ACTIVATES_B=\"$<TARGET_FILE:zen_test_weave_activates_b>\""
    "ZEN_SO_ACTIVATES_DRIFT=\"$<TARGET_FILE:zen_test_weave_activates_drift>\""
    "ZEN_SO_ACTIVATES_CONFLICT=\"$<TARGET_FILE:zen_test_weave_activates_conflict>\""
    "ZEN_SO_BADABI=\"$<TARGET_FILE:zen_test_badabi>\""
    "ZEN_SO_STALEABI=\"$<TARGET_FILE:zen_test_staleabi>\""
    "ZEN_SO_CONTRACT_APPLIED=\"$<TARGET_FILE:zen_test_contract_applied>\""
    "ZEN_SO_CONTRACT_BYPASS=\"$<TARGET_FILE:zen_test_contract_bypass>\""
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
# C-2: a child that inventories the descriptors it actually inherited, tries to USE the
# one the host parked, and independently attempts a fresh socket — so "the namespace is
# real" and "ambient host descriptors were removed" are two answers, not one.
zen_add_test_weave(zen_test_weave_fdprobe ZEN_WEAVE_FD_PROBE)
# C-2a: a child that reports its COMPLETE environment, so "the host's ambient environment
# did not cross" is an assertion about the whole set rather than about the one variable
# somebody remembered to plant.
zen_add_test_weave(zen_test_weave_envprobe ZEN_WEAVE_ENV_PROBE)
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
    loom_weave_build_contract(${t})
endforeach()

set(ZEN_ISOLATION_WEAVE_LIBS zen_test_weave_crasher zen_test_weave_silent
    zen_test_weave_netprobe zen_test_weave_fsprobe zen_test_weave_fdprobe
    zen_test_weave_envprobe zen_test_weave_membomb
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
    "ZEN_SO_FDPROBE=\"$<TARGET_FILE:zen_test_weave_fdprobe>\""
    "ZEN_SO_ENVPROBE=\"$<TARGET_FILE:zen_test_weave_envprobe>\""
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

# HOW THIS PROJECT CONSUMES DOCTEST, in one place because there are two test binaries
# and two places that must agree is the drift this directory already refuses elsewhere.
#
#   * doctest's own header trips -Wsign-conversion etc.; it is third-party, so we do not
#     hold it to Zen's warning policy. Mark its include dir as system.
#   * DOCTEST_CONFIG_USE_STD_HEADERS makes doctest INCLUDE <ostream> and friends instead
#     of forward-declaring them inside namespace std. Its forward declarations leave
#     std::basic_ostream INCOMPLETE where doctest stringifies a value, so the class's
#     MEMBER operator<<(const void*) is invisible and only free operators are candidates.
#     With MSVC's STL that is fatal for any shared_ptr: std::operator<<(ostream&, const
#     shared_ptr<T>&) is found by ADL, doctest's detector accepts it, and its body
#     (`_Out << _Px.get()`) then hard-errors -- C2676 on eight ordinary
#     `REQUIRE(some_schema_ptr != nullptr)` lines that libstdc++ compiles fine. doctest
#     ships this macro for exactly this class of standard-library difference; it changes
#     no assertion's meaning, only which declarations doctest relies on.
add_library(zen-doctest INTERFACE)
target_include_directories(zen-doctest SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(zen-doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)

target_link_libraries(zen-tests PRIVATE zen-doctest)

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
    target_include_directories(zen-sdl-tests PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(zen-sdl-tests
        PRIVATE zen-doctest zen-ui-sdl zen-ui-pixel zen-ui zen-warnings zen-sanitize)
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

# The same gate set, written where the OFFICIAL LANE can read it (VOLATILE-2a). The lane's
# CTest-entry inventory needs to know which configuration this is, and it runs standalone
# with nothing but a build directory. Written from the one variable the `population` entry
# reads on the next line, so the two cannot come to disagree about which gates are on.
#
# It DESCRIBES the build; it never requires anything. suite_population.txt and
# entry_population.txt are what require, and a description that got this wrong makes a
# missing entry look declared-absent -- which is why the inventory refuses a gate set that
# is empty or that has lost `portable`.
file(WRITE ${CMAKE_BINARY_DIR}/zen-test-gates.cmake
     "# Written by tests/CMakeLists.txt at configure time. A description of this build\n"
     "# tree's active gates, read by tests/check_entry_population.cmake. Not an expectation.\n"
     "set(ZEN_ACTIVE_GATES \"${ZEN_POPULATION_GATES}\")\n")

# ZEN_BUILD_DIR and ZEN_BUILD_CONFIG are what let this entry take the CTest-ENTRY inventory
# for itself (VOLATILE-B1) rather than reading the lane's source text for the name of a
# function. It asks `ctest -N` which entries this configured build registered, which is a
# question about the build and not about this run's selection -- so it answers the same under
# `ctest`, under the official lane, and under a subset run that happens to include it.
# $<CONFIG> is empty under a single-configuration generator and is handled as such.
add_test(NAME population COMMAND ${CMAKE_COMMAND}
    -DZEN_TESTS_EXE=$<TARGET_FILE:zen-tests>
    -DZEN_SDL_TESTS_EXE=${zen_sdl_tests_exe}
    -DZEN_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/suite_population.txt
    -DZEN_GATES=${zen_population_gates_arg}
    -DZEN_BUILD_DIR=${CMAKE_BINARY_DIR}
    -DZEN_BUILD_CONFIG=$<CONFIG>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_population.cmake)

# ---- The documentation-link contract (VOLATILE-2) -------------------------------------
#
# The only entry that interrogates the SOURCE TREE rather than a build: every repo-local
# documentation reference this repository makes must still resolve -- markdown links and
# their #anchors, and the repository-relative `docs/...md` paths written in first-party
# C/C++ comments. It reads no build output, so it registers in every configuration and on
# every platform, which is the point: the lanes most likely to notice a path that stopped
# resolving are the ones a hand-run script never reaches.
#
# Pure CMake, like every other repository-owned check here, because a verifier may not
# depend on a tool that merely happens to be installed. Its predecessor
# `tools/check-doc-links.py` could not run at all on the native Windows host this
# repository supports.
add_test(NAME doc_links COMMAND ${CMAKE_COMMAND}
    -DZEN_REPO=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_doc_links.cmake)

# ---- The weave build contract: two populations, two entries (KERN-05 / POP-05) --------
#
# What the `population` entry does for the test population, these do for the loadable
# weave artifacts. TWO questions, deliberately not one:
#
#   weave_population   is every artifact that MUST carry the contract on the roll?
#                      Expectation from the BUILD GRAPH (weave_population.cmake: target
#                      types + link closures), evidence from the roll. C3/COLD-2 C-3.
#   weave_contract     does every artifact on the roll actually have the properties?
#                      Read out of the built files' symbol tables. R2F-E.
#
# Neither subsumes the other and neither derives from the other. The roll of contracted
# targets is what loom_weave_build_contract() recorded about itself — the right thing to
# check compile options and symbols against, and precisely the wrong thing to ask whether
# somebody left. So the required side never touches it (POP-01: a derived list cannot
# detect its own absences), and the compile options still come out of each target's real
# COMPILE_OPTIONS rather than from the function's verdict string.
#
# THE GATE IS THE SWEEP'S OWN ANSWER, not a named fixture. `if(TARGET
# zen_test_contract_applied)` would have deregistered both entries the day that fixture
# was renamed — a check that silently stops existing is the failure mode one directory up
# from the one C3 closes. So: if this configuration builds loadable weaves at all, both
# entries register; if it builds none (the default Windows package, where the kernel gate
# is off and tests/ has no shared libraries), there is no population and their absence is
# declared, not disguised (POP-03).
#
# ...WHICH MOVES THE SAME HAZARD ONTO THE SWEEP, so the gate is asked in BOTH directions
# against a fixed canary. zen_test_contract_applied and zen_test_contract_bypass are the
# contract's own fixtures: where they exist, this configuration builds loadable weaves by
# construction, one required and one exempt. Asking only "weaves imply the pair" left a
# blinded sweep — one that stops recognising SHARED libraries at all — quietly
# deregistering BOTH entries and passing the lane at 32 of 32. Measured during C3: the
# first version of this guard had exactly that hole.
zen_required_weave_population(zen_weave_required_rows zen_weave_targets zen_weave_exempt)

if(zen_weave_targets OR TARGET zen_test_contract_applied OR TARGET zen_test_contract_bypass)
    # The R2F-E pair is load-bearing for the ARTIFACT check — claim 2 asks whether the
    # bypass twin still reproduces F-22, and without the twin the contracted artifacts
    # prove nothing. Losing it must therefore be loud rather than quiet.
    foreach(zen_control IN ITEMS zen_test_contract_applied zen_test_contract_bypass)
        if(NOT TARGET ${zen_control})
            message(FATAL_ERROR
                "zen tests: this configuration builds loadable weaves but not "
                "'${zen_control}'. That target is half of the weave build contract's "
                "positive/negative pair; without both, `weave_contract` can no longer tell "
                "'the contract works' from 'the sentinel stopped expressing the hazard'.")
        endif()
    endforeach()

    # ...and the canary in the other direction: the sweep must have SEEN them, each on the
    # side it belongs to.
    list(FIND zen_weave_targets zen_test_contract_applied zen_canary)
    if(zen_canary EQUAL -1)
        message(FATAL_ERROR
            "zen tests: the build-graph sweep did not classify 'zen_test_contract_applied' "
            "as a loadable weave, though it is a shared library in this directory and "
            "exists for no other purpose. tests/weave_population.cmake has gone blind, and "
            "a blind sweep declares an empty requirement that everything satisfies.")
    endif()
    list(FIND zen_weave_exempt zen_test_contract_bypass zen_canary)
    if(zen_canary EQUAL -1)
        message(FATAL_ERROR
            "zen tests: 'zen_test_contract_bypass' is not among the declared weave-contract "
            "exemptions. Either its zen_weave_contract_exempt() declaration is gone — in "
            "which case the required population now demands the contract of the one "
            "artifact whose whole job is to lack it — or the sweep stopped reading "
            "exemptions at all.")
    endif()

    # The required population, written where the -P check can read it. Plain file(WRITE):
    # it holds target NAMES and prose, no generator expressions, and it is settled the
    # moment the sweep above ran.
    string(REPLACE ";" "\n" zen_weave_required_text "${zen_weave_required_rows}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/weave_population_required.txt
         "${zen_weave_required_text}\n")

    get_property(zen_contract_targets GLOBAL PROPERTY LOOM_WEAVE_CONTRACT_TARGETS)
    set(zen_contract_rows "")
    foreach(t IN LISTS zen_contract_targets)
        get_target_property(zen_contract_verdict ${t} LOOM_WEAVE_BUILD_CONTRACT)
        get_target_property(zen_contract_kind ${t} TYPE)
        string(APPEND zen_contract_rows
            "${t}|$<TARGET_FILE:${t}>|${zen_contract_kind}|${zen_contract_verdict}|"
            "$<JOIN:$<TARGET_PROPERTY:${t},COMPILE_OPTIONS>,+>\n")
    endforeach()
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/weave_contract_manifest.txt
         CONTENT "${zen_contract_rows}")

    # binutils is the authority on a symbol's binding; llvm-nm answers identically.
    # An absent tool is a FAILURE inside the script, never a silent skip here.
    find_program(ZEN_NM NAMES nm llvm-nm)
    if(NOT ZEN_NM)
        set(ZEN_NM "")
    endif()

    add_test(NAME weave_contract COMMAND ${CMAKE_COMMAND}
        -DZEN_MANIFEST=${CMAKE_CURRENT_BINARY_DIR}/weave_contract_manifest.txt
        -DZEN_HOST_EXE=$<TARGET_FILE:zen-tests>
        "-DZEN_HOST_OPTIONS=$<JOIN:$<TARGET_PROPERTY:zen-tests,COMPILE_OPTIONS>,+>"
        -DZEN_BYPASS_LIB=$<TARGET_FILE:zen_test_contract_bypass>
        -DZEN_PLATFORM=${CMAKE_SYSTEM_NAME}
        -DZEN_NM=${ZEN_NM}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_weave_contract.cmake)

    # The required-vs-actual comparison. It reads the SAME roll manifest weave_contract
    # reads — one measurement, two consumers — against an expectation computed without
    # ever looking at it.
    add_test(NAME weave_population COMMAND ${CMAKE_COMMAND}
        -DZEN_REQUIRED=${CMAKE_CURRENT_BINARY_DIR}/weave_population_required.txt
        -DZEN_CONTRACTED=${CMAKE_CURRENT_BINARY_DIR}/weave_contract_manifest.txt
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_weave_population.cmake)
endif()
