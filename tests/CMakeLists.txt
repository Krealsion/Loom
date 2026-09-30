# One test binary, zen-tests: doctest is compiled once (doctest_main.cpp) and each suite is its own
# CTest entry, registered from the TEST_SUITE(...) declarations of the sources compiled here. What
# holds that population honest is in docs/laws/population-laws.md; the lane is tests/verify.cmake.
# The kernel suites and the loadable fixtures ride `if(TARGET zen-kernel)` (Linux, and Windows
# under LOOM_ENABLE_WINDOWS_KERNEL); the sandbox suites ride `if(NOT WIN32)`. test_capabilities
# rides the kernel gate because it loads a real weave library.

# The test sources, by gate. They are the suite inventory: the CTest registration below reads the
# suite names out of exactly the sources compiled, so there is no second list to drift.
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
    test_dispatch_refusal.cpp
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
    test_ask_book.cpp
    test_role_request.cpp
    test_poke.cpp
    test_describe.cpp
    test_console.cpp
    test_history_recorder.cpp
    test_history_logger.cpp
    test_component.cpp
    test_pixel.cpp
    test_bridge.cpp
    test_host_policy.cpp
    test_host_input.cpp
    test_session.cpp
    test_observe.cpp)
# Kernel-dependent suites: compiled wherever the kernel target exists.
set(ZEN_TEST_SOURCES_KERNEL
    test_kernel.cpp
    test_admission.cpp
    test_dispatch_loaded.cpp
    test_handoff.cpp
    test_manager.cpp
    test_capabilities.cpp
    # Joint publication of latest claims (SENSE-06/07), with a real loaded
    # claimant -- kernel-gated because it loads one.
    test_joint.cpp)
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
# THE RUN MANAGER'S TWO GATES (docs/guides/sessions.md). `runs` rides the manager having been
# built at all -- a loadable weave, so the kernel, and LOOM_BUILD_SESSION_TOOLS -- and carries
# the `runs` suite, which needs no Python. `python` rides that AND a Python 3.8+ interpreter:
# the session journey starts real Python workers and clients. Python is optional tooling, never a
# baseline prerequisite: where it is absent the journey is DECLARED ABSENT, said at configure and
# by the population check, never passed. LOOM_SESSION_PYTHON names an interpreter explicitly.
set(LOOM_SESSION_PYTHON "" CACHE FILEPATH
    "Python 3.8+ for the session journey (empty: search for one)")
set(zen_session_python "")
if(TARGET zen-runs)
    set(ZEN_TEST_SOURCES_RUNS test_runs.cpp)
    target_sources(zen-tests PRIVATE ${ZEN_TEST_SOURCES_RUNS})
    list(APPEND ZEN_TEST_SOURCES_DIRECT ${ZEN_TEST_SOURCES_RUNS})
    target_link_libraries(zen-tests PRIVATE zen-runs)
    # A worker that leaves a child behind (tests/runs_leader/leader.cpp): the one arrangement the
    # `runs` suite cannot make for itself, since what the manager owns is the worker's execution
    # GROUP and only the worker can put a second process in it. An ordinary executable, not a
    # loadable weave, so it is on neither population roll. Its three constants are authored once,
    # here, and reach both the worker and the suite that reads what it left.
    set(zen_runs_leader_code 7)         # the LEADER's own exit code, which nothing may overwrite
    set(zen_runs_leader_seconds 120)    # how long the child it leaves behind keeps running
    set(zen_runs_leader_pidfile "child.pid")
    add_executable(zen-runs-leader runs_leader/leader.cpp)
    target_link_libraries(zen-runs-leader PRIVATE zen-warnings zen-sanitize)
    target_compile_definitions(zen-runs-leader PRIVATE
        ZEN_RUNS_LEADER_CODE=${zen_runs_leader_code}
        ZEN_RUNS_LEADER_SECONDS=${zen_runs_leader_seconds}
        ZEN_RUNS_LEADER_PIDFILE="${zen_runs_leader_pidfile}")
    add_dependencies(zen-tests zen-runs-leader)
    target_compile_definitions(zen-tests PRIVATE
        ZEN_TEST_CMAKE="${CMAKE_COMMAND}"
        ZEN_TEST_RUNS_LEADER="$<TARGET_FILE:zen-runs-leader>"
        ZEN_RUNS_LEADER_CODE=${zen_runs_leader_code}
        ZEN_RUNS_LEADER_PIDFILE="${zen_runs_leader_pidfile}"
        ZEN_TEST_PYTHON_RUNTIME="${CMAKE_SOURCE_DIR}/python")
    list(APPEND ZEN_POPULATION_GATES runs)
    if(LOOM_SESSION_PYTHON)
        set(zen_session_python ${LOOM_SESSION_PYTHON})
    else()
        find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            set(zen_session_python ${Python3_EXECUTABLE})
        endif()
    endif()
    if(zen_session_python)
        list(APPEND ZEN_POPULATION_GATES python)
    else()
        message(STATUS "loom: no Python 3.8+ interpreter found -- the session journey is "
                       "DECLARED ABSENT here (set LOOM_SESSION_PYTHON to run it)")
    endif()
endif()

target_include_directories(zen-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/console) # tui_render.hpp / terminal.hpp for the tui_map_key pin

target_link_libraries(zen-tests
    PRIVATE loom zen-switchboard zen-ui zen-ui-pixel zen-console zen-history zen-bridge zen-bridge-console zen-tui
            zen-host-policy zen-warnings zen-sanitize)
if(TARGET zen-kernel)
    target_link_libraries(zen-tests PRIVATE zen-kernel)
endif()
if(NOT WIN32)
    target_link_libraries(zen-tests PRIVATE zen-isolation)
endif()

# The required-contract population (POP-05): the build-graph sweep and the exemption declaration.
# It reads target types and link closures, never the contract roll, which is what lets the
# `weave_population` entry notice an artifact that stopped taking the contract.
include(${CMAKE_CURRENT_SOURCE_DIR}/weave_population.cmake)

# ---- Loadable test weave libraries and the child host ----
# The portable fixtures are loaded by the kernel, manager and capabilities suites and follow the
# kernel target (built as .dll on Windows). The probe and bomb variants, the policy mods and the
# child host are sandbox ground, and Linux-only.
if(TARGET zen-kernel)

# A loadable weave's statics live and die with its own library (KERN-05). The fixtures take that
# from loom_weave_build_contract(), the function a stranger gets from find_package(loom), so the
# two paths cannot mean different things. No in-process case sees a fixture lose it (the kernel's
# unload cases test the loader's bookkeeping, right either way), so `weave_population` and
# `weave_contract` below say so from the build graph and the built symbol tables.

# One configurable source compiled into several variants, plus a bad-ABI lib.
function(zen_add_test_weave target)
    add_library(${target} SHARED weavelib/test_weave.cpp)
    target_link_libraries(${target} PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
    loom_weave_build_contract(${target})
    target_compile_definitions(${target} PRIVATE ${ARGN})
    # MSVC's C4702 (unreachable code) is right here and harmless: one source is compiled into
    # many variants by #if, and a variant that returns early leaves the shared tail unreachable.
    # Silenced for these variant targets only.
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/wd4702>)
endfunction()

zen_add_test_weave(zen_test_weave)
zen_add_test_weave(zen_test_weave_b) # an identical "rebuild", for hot-reload
zen_add_test_weave(zen_test_weave_badsnap ZEN_WEAVE_MALFORMED_SNAPSHOT)
zen_add_test_weave(zen_test_weave_badmsg ZEN_WEAVE_MALFORMED_MESSAGE)
zen_add_test_weave(zen_test_weave_v2 ZEN_WEAVE_STATE_V2)
# A loaded handler that throws: its exception is caught at the ABI seam and returns as a status,
# which the bus must record rather than announce a plain Delivered.
zen_add_test_weave(zen_test_weave_throws ZEN_WEAVE_THROW_ON_MAGIC)
# The letter: a predecessor that declares zen.PrepareShutdown and writes a Bequest, and a
# differently shaped heir (Counter v2) that claims by role on wake.
zen_add_test_weave(zen_test_weave_bequeaths ZEN_WEAVE_BEQUEATHS)
zen_add_test_weave(zen_test_weave_heir ZEN_WEAVE_HEIR)
# ...and one that declares the ceremony then never answers: the honest wedge.
zen_add_test_weave(zen_test_weave_wedged ZEN_WEAVE_WEDGED)
# Activation: a participant that declares zen.Activated and records what it was told, its
# identical "rebuild" for reload in place, and a drift twin whose state schema is identical but
# whose door contract has one extra shape: reload compares the whole contract, not just the state.
zen_add_test_weave(zen_test_weave_activates ZEN_WEAVE_ACTIVATES)
zen_add_test_weave(zen_test_weave_activates_b ZEN_WEAVE_ACTIVATES)
zen_add_test_weave(zen_test_weave_activates_drift ZEN_WEAVE_ACTIVATES ZEN_WEAVE_ACTIVATES_DRIFT)
# A drift twin whose extra door disagrees about Greet v1, so the agreement wall makes a rejected
# candidate's schema admission observable from outside.
zen_add_test_weave(zen_test_weave_activates_conflict
    ZEN_WEAVE_ACTIVATES ZEN_WEAVE_ACTIVATES_CONFLICT)
# The deferring steward: a loaded weave that takes an ask's answer with it, returns without
# answering, and answers from a later handler. It must be a real .so: the question is whether the
# right survives the handler that earned it, and a native lambda holding the old Mail would answer
# a different question.
zen_add_test_weave(zen_test_weave_defers ZEN_WEAVE_DEFERS)
# The immediate-answer fixture, and its twin that answers twice.
zen_add_test_weave(zen_test_weave_answers ZEN_WEAVE_ANSWERS)
zen_add_test_weave(zen_test_weave_answers_twice ZEN_WEAVE_ANSWERS ZEN_WEAVE_ANSWERS_TWICE)
# An artifact that emits a shape only it knows, so the host seam cannot resolve it (a silent
# seam).
zen_add_test_weave(zen_test_weave_seam_emit ZEN_WEAVE_SEAM_EMIT)
# The same unresolvable shape with no address on it: publishes SeamOnly v1 (unheard, so not a
# refusal) and then a resolvable Pong whose bytes fail the gate (undelivered, so still one).
zen_add_test_weave(zen_test_weave_seam_publish ZEN_WEAVE_SEAM_PUBLISH)
# The Sense fixture: claims, a forged office refused, and a synchronous read-back, all from the
# far side of the seam.
zen_add_test_weave(zen_test_weave_senses ZEN_WEAVE_SENSES)
# Schema admission (since ABI v9): the declared emitter and its divergent twin, the loaded half of the
# case where a stale application was admitted against a newer peer's emitted shape.
zen_add_test_weave(zen_test_weave_emits ZEN_WEAVE_EMITS_GREET)
zen_add_test_weave(zen_test_weave_emits_conflict ZEN_WEAVE_EMITS_GREET ZEN_WEAVE_GREET_CONFLICT)
# ...the nested-component cases: a disagreement only the component closure can see
# (Box vs Box2 over two Part v1 definitions), one artifact contradicting itself in both
# declaration orders, and the agreeing control that shares one Part.
zen_add_test_weave(zen_test_weave_nest_old ZEN_WEAVE_NEST_OLD)
zen_add_test_weave(zen_test_weave_nest_new ZEN_WEAVE_NEST_NEW)
zen_add_test_weave(zen_test_weave_nest_mixed ZEN_WEAVE_NEST_MIXED)
zen_add_test_weave(zen_test_weave_nest_mixed_rev ZEN_WEAVE_NEST_MIXED ZEN_WEAVE_NEST_MIXED_REVERSED)
zen_add_test_weave(zen_test_weave_nest_agree ZEN_WEAVE_NEST_OLD ZEN_WEAVE_NEST_AGREE)
# ...and the guide's copied weave that kept `Ping v1`'s name and added a field.
zen_add_test_weave(zen_test_weave_ping_drift ZEN_WEAVE_PING_DRIFT)

# The `versioned.service` pair: one source, two real artifacts. v1 is the incumbent that answers
# "v1" throughout; v2 (ZEN_VERSIONED_CANDIDATE) is the same production service plus the
# preparation conversation. One source on purpose: the artifact that prepares must be the one
# that goes live.
function(zen_add_versioned_service target)
    add_library(${target} SHARED weavelib/versioned_service.cpp)
    target_link_libraries(${target} PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
    loom_weave_build_contract(${target})
    target_compile_definitions(${target} PRIVATE ${ARGN})
endfunction()

zen_add_versioned_service(zen_test_versioned_v1)
zen_add_versioned_service(zen_test_versioned_v2 ZEN_VERSIONED_CANDIDATE)

# The handoff ledgers: two real artifacts whose state schemas are genuinely incompatible (flat
# scalars against nested messages and a list), and the temporary migrator that authors the v1 to
# v2 meaning and is unloaded afterwards.
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

# Sender-visible refusal: a real loaded author and authenticated recipient.
add_library(zen_test_dispatch SHARED weavelib/dispatch_probe.cpp)
target_link_libraries(zen_test_dispatch PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
loom_weave_build_contract(zen_test_dispatch)
target_compile_definitions(zen-tests PRIVATE "ZEN_SO_DISPATCH=\"$<TARGET_FILE:zen_test_dispatch>\"")

# Joint publication (ABI v8): a real loaded claimant that offers into a joint publication across
# the seam and hears `claim_published` back across it, answering all three ways: Applied, Failed,
# Declined.
add_library(zen_test_joint SHARED weavelib/joint_probe.cpp)
target_link_libraries(zen_test_joint PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
loom_weave_build_contract(zen_test_joint)
target_compile_definitions(zen-tests PRIVATE "ZEN_SO_JOINT=\"$<TARGET_FILE:zen_test_joint>\"")

# The supplied host's probe, two variants from one source, for the `host_process` entries' real
# loom-host: it counts authenticated activations, sends itself a message on activation (a send an
# approved permission must already cover), re-arms a Spin forever (the cooperative busy weave),
# and in the FORGE variant authors an unasked `zen.Result` the console must not take for an
# answer.
add_library(zen_host_probe SHARED weavelib/host_probe.cpp)
target_link_libraries(zen_host_probe PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
loom_weave_build_contract(zen_host_probe)
add_library(zen_host_probe_forge SHARED weavelib/host_probe.cpp)
target_link_libraries(zen_host_probe_forge PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
target_compile_definitions(zen_host_probe_forge PRIVATE ZEN_PROBE_FORGE)
loom_weave_build_contract(zen_host_probe_forge)

# The office worker: office authorship across the seam, loaded in process under the role
# "worker.a" (kernel suite) and out of process (isolation suite), where the pipe's fail-closed
# rule is what is under test.
add_library(zen_test_office_worker SHARED weavelib/office_worker.cpp)
target_link_libraries(zen_test_office_worker PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
loom_weave_build_contract(zen_test_office_worker)

add_library(zen_test_badabi SHARED weavelib/bad_abi.cpp)
target_link_libraries(zen_test_badabi PRIVATE loom zen-warnings zen-sanitize)
loom_weave_build_contract(zen_test_badabi)

# An artifact built against the previous ABI: bad_abi declares a future version (a host too old
# for it), this one declares version 1, a real stale artifact, so refusing it has a witness.
add_library(zen_test_staleabi SHARED weavelib/stale_abi.cpp)
target_link_libraries(zen_test_staleabi PRIVATE loom zen-warnings zen-sanitize)
loom_weave_build_contract(zen_test_staleabi)

# The build contract's own pair: one source, two artifacts, one through
# loom_weave_build_contract() and one deliberately bypassing it. The bypass twin is the positive
# control that keeps the question askable, as the stale-ABI fixture keeps "the previous ABI
# refuses" askable. It is the only artifact here built to be reload-unsafe on purpose, so it links
# nothing: its one symbol cannot alias anything else in the process.
add_library(zen_test_contract_applied SHARED weavelib/contract_sentinel.cpp)
loom_weave_build_contract(zen_test_contract_applied)

add_library(zen_test_contract_bypass SHARED weavelib/contract_sentinel.cpp)
# (no loom_weave_build_contract here: that absence is the control)
# The required-population sweep counts every shared library here as owing the contract, so the
# exemption is written on the target with its reason, printed every run; and
# check_weave_population.cmake fails if an exempt target ever turns up contracted.
zen_weave_contract_exempt(zen_test_contract_bypass
    "the negative control: same source as zen_test_contract_applied, contract "
    "deliberately withheld, so the unique-symbol sentinel it still carries is what keeps "
    "claim 1 of weave_contract falsifiable")

set(ZEN_KERNEL_WEAVE_LIBS zen_test_weave zen_test_weave_b zen_test_weave_badsnap
    zen_test_weave_badmsg zen_test_weave_v2 zen_test_weave_bequeaths zen_test_weave_heir
    zen_test_weave_wedged zen_test_weave_activates zen_test_weave_activates_b
    zen_test_weave_activates_drift zen_test_weave_activates_conflict zen_test_badabi
    zen_test_staleabi zen_test_weave_defers zen_test_weave_answers
    zen_test_weave_answers_twice zen_test_weave_seam_emit zen_test_weave_seam_publish
    zen_test_weave_senses
    zen_test_weave_emits zen_test_weave_emits_conflict
    zen_test_weave_nest_old zen_test_weave_nest_new zen_test_weave_nest_mixed
    zen_test_weave_nest_mixed_rev zen_test_weave_nest_agree zen_test_weave_ping_drift
    zen_test_versioned_v1 zen_test_versioned_v2
    zen_test_handoff_v1 zen_test_handoff_v2 zen_test_handoff_migrator
    zen_test_office_worker zen_test_dispatch zen_test_joint
    zen_host_probe zen_host_probe_forge
    zen_test_contract_applied zen_test_contract_bypass zen_test_weave_asks)
add_dependencies(zen-tests ${ZEN_KERNEL_WEAVE_LIBS})

# Hand the portable fixtures' paths to the tests as string macros.
target_compile_definitions(zen-tests PRIVATE
    "ZEN_SO_WEAVE=\"$<TARGET_FILE:zen_test_weave>\""
    "ZEN_SO_WEAVE_B=\"$<TARGET_FILE:zen_test_weave_b>\""
    "ZEN_SO_ASKS=\"$<TARGET_FILE:zen_test_weave_asks>\""
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
    "ZEN_SO_SEAM_PUBLISH=\"$<TARGET_FILE:zen_test_weave_seam_publish>\""
    "ZEN_SO_SENSES=\"$<TARGET_FILE:zen_test_weave_senses>\""
    "ZEN_SO_VERSIONED_V1=\"$<TARGET_FILE:zen_test_versioned_v1>\""
    "ZEN_SO_VERSIONED_V2=\"$<TARGET_FILE:zen_test_versioned_v2>\""
    "ZEN_SO_HANDOFF_V1=\"$<TARGET_FILE:zen_test_handoff_v1>\""
    "ZEN_SO_HANDOFF_V2=\"$<TARGET_FILE:zen_test_handoff_v2>\""
    "ZEN_SO_HANDOFF_MIGRATOR=\"$<TARGET_FILE:zen_test_handoff_migrator>\""
    "ZEN_SO_OFFICE_WORKER=\"$<TARGET_FILE:zen_test_office_worker>\""
    "ZEN_SO_EMITS=\"$<TARGET_FILE:zen_test_weave_emits>\""
    "ZEN_SO_EMITS_CONFLICT=\"$<TARGET_FILE:zen_test_weave_emits_conflict>\""
    "ZEN_SO_NEST_OLD=\"$<TARGET_FILE:zen_test_weave_nest_old>\""
    "ZEN_SO_NEST_NEW=\"$<TARGET_FILE:zen_test_weave_nest_new>\""
    "ZEN_SO_NEST_MIXED=\"$<TARGET_FILE:zen_test_weave_nest_mixed>\""
    "ZEN_SO_NEST_MIXED_REV=\"$<TARGET_FILE:zen_test_weave_nest_mixed_rev>\""
    "ZEN_SO_NEST_AGREE=\"$<TARGET_FILE:zen_test_weave_nest_agree>\""
    "ZEN_SO_PING_DRIFT=\"$<TARGET_FILE:zen_test_weave_ping_drift>\"")

# The admission suite's declaring artifact: the same fixture, with a CapabilityAsk in
# its manifest. It rides the kernel gate rather than the posix one, because what it
# proves -- that a declaration reaches the host's policy and grants nothing -- is true
# of the in-process kernel on every platform that has one.
zen_add_test_weave(zen_test_weave_asks ZEN_WEAVE_ASKS)

endif() # TARGET zen-kernel (portable fixtures)

if(NOT WIN32)

# Isolation harness: a child that crashes (mid-handle + on revive, with a small
# reload budget) and one that is deliberately silent.
zen_add_test_weave(zen_test_weave_crasher
    ZEN_WEAVE_CRASH_ON_MAGIC ZEN_WEAVE_CRASH_ON_REVIVE ZEN_WEAVE_LOW_RELOADS)
zen_add_test_weave(zen_test_weave_silent ZEN_WEAVE_SILENT)
# A child that reaches for the network directly, to prove the OS sandbox blocks it.
zen_add_test_weave(zen_test_weave_netprobe ZEN_WEAVE_NET_PROBE)
# A child that reaches for the filesystem directly, to prove the mount-namespace view.
zen_add_test_weave(zen_test_weave_fsprobe ZEN_WEAVE_FS_PROBE)
# A child that inventories the descriptors it inherited, tries to use the one the host parked, and
# independently attempts a fresh socket, so "the namespace is real" and "ambient host descriptors
# were removed" are two answers, not one.
zen_add_test_weave(zen_test_weave_fdprobe ZEN_WEAVE_FD_PROBE)
# A child that reports its complete environment, so "the host's ambient environment did not
# cross" is an assertion about the whole set rather than about one planted variable.
zen_add_test_weave(zen_test_weave_envprobe ZEN_WEAVE_ENV_PROBE)
# A child that bombs memory (OOM-killed under memory.max, with a small reload budget so it
# quarantines) and one that fork-bombs (bounded by pids.max).
zen_add_test_weave(zen_test_weave_membomb ZEN_WEAVE_MEM_BOMB ZEN_WEAVE_LOW_RELOADS)
zen_add_test_weave(zen_test_weave_forkbomb ZEN_WEAVE_FORK_BOMB)

# A mod that asks for network and filesystem write (through ZEN_ASK), to prove an ask is not a
# grant: it still lands on the floor.
add_library(zen_test_mod_storage SHARED weavelib/mod_storage.cpp)
target_link_libraries(zen_test_mod_storage PRIVATE loom zen-switchboard zen-warnings zen-sanitize)

# A storage-client mod that stores and retrieves purely by messaging the "storage" broker (no
# disk), and the StorageBroker itself.
add_library(zen_test_storage_client SHARED weavelib/storage_client.cpp)
target_link_libraries(zen_test_storage_client PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
add_library(zen_test_storage_broker SHARED weavelib/storage_broker.cpp)
target_link_libraries(zen_test_storage_broker PRIVATE loom zen-switchboard zen-warnings zen-sanitize)

# A net-client mod that reaches the network only through the NetworkBroker, and the broker itself
# (raw TCP, an allow-list, no dependency).
add_library(zen_test_net_client SHARED weavelib/net_client.cpp)
target_link_libraries(zen_test_net_client PRIVATE loom zen-switchboard zen-warnings zen-sanitize)
add_library(zen_test_net_broker SHARED weavelib/net_broker.cpp)
target_link_libraries(zen_test_net_broker PRIVATE loom zen-switchboard zen-warnings zen-sanitize)

# The confused deputy: a mod that forges a role-send's wire reply_to to point at a different
# weave; the host must ignore it and reply to the stamped sender.
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
    # The deferring steward mounted out of process too, to pin that the capability fails closed
    # across the pipe rather than being faked across it.
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

# How this project consumes doctest, in one place for both test binaries. Its include directory is
# a system one, so its own warnings are not held to Zen's policy. DOCTEST_CONFIG_USE_STD_HEADERS
# makes it include <ostream> rather than forward-declare it: with MSVC's STL a forward-declared
# ostream hides the member operator<<(const void*), and `REQUIRE(ptr != nullptr)` on a
# shared_ptr fails to compile (C2676). The macro changes no assertion's meaning.
add_library(zen-doctest INTERFACE)
target_include_directories(zen-doctest SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(zen-doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)

target_link_libraries(zen-tests PRIVATE zen-doctest)

# ---- CTest registration, derived from the suite declarations in the sources ----------
# doctest exits with the failed-assertion count and doctest_main.cpp turns a zero-case run into
# exit 70, so CTest's exit status answers both "did it fail" and "was there anything to run". A
# rename moves the registered entry instead of orphaning it; a suite compiled out, a source
# dropped from the lists or a gate turned off is caught by doctest_main.cpp, the `population`
# entry or a declared absence (docs/laws/population-laws.md).

# Read at configure time, deliberately without the sources in CMAKE_CONFIGURE_DEPENDS: a stale
# registration fails closed either way (an old name selects zero cases; a new one fails
# `population` until the manifest says so), and re-configuring on every test edit is slow.
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

# The guard's own witnesses, two entries because CTest ignores the exit code when
# PASS_REGULAR_EXPRESSION is set: a filter naming a suite no compiled case belongs to (what a
# rename, an #if or a deletion produces) must exit nonzero and say why.
add_test(NAME empty_population_refused
    COMMAND zen-tests --test-suite=zen_suite_that_does_not_exist --no-skipped-summary)
set_tests_properties(empty_population_refused PROPERTIES WILL_FAIL TRUE)

add_test(NAME empty_population_says_so
    COMMAND zen-tests --test-suite=zen_suite_that_does_not_exist --no-skipped-summary)
set_tests_properties(empty_population_says_so PROPERTIES
    PASS_REGULAR_EXPRESSION "EMPTY TEST POPULATION")

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

# ---- The population contract (POP-01, POP-02) ------------------------------------------
# A suite that is gone is simply not registered, and a short CTest listing looks like a platform
# where those entries were never meant to exist. suite_population.txt writes the difference down
# and this entry enforces it, the exact suites per active gate and a case floor for each, in
# doctest's query modes without running a case.
# Comma-separated: a ;-list would be split into separate arguments on the command line.
string(REPLACE ";" "," zen_population_gates_arg "${ZEN_POPULATION_GATES}")

# The same gate set, written where the official lane can read it: its entry inventory runs with
# nothing but a build directory. It describes the build and requires nothing, so the inventory
# refuses a gate set that is empty or has lost `portable`.
file(WRITE ${CMAKE_BINARY_DIR}/zen-test-gates.cmake
     "# Written by tests/CMakeLists.txt at configure time. A description of this build\n"
     "# tree's active gates, read by tests/check_entry_population.cmake. Not an expectation.\n"
     "set(ZEN_ACTIVE_GATES \"${ZEN_POPULATION_GATES}\")\n")

# ZEN_BUILD_DIR and ZEN_BUILD_CONFIG let this entry take the CTest-entry inventory itself, by
# asking `ctest -N` what this build registered, so it answers the same under `ctest`, the lane or
# a subset run. $<CONFIG> is empty under a single-configuration generator and is handled so.
add_test(NAME population COMMAND ${CMAKE_COMMAND}
    -DZEN_TESTS_EXE=$<TARGET_FILE:zen-tests>
    -DZEN_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/suite_population.txt
    -DZEN_GATES=${zen_population_gates_arg}
    -DZEN_BUILD_DIR=${CMAKE_BINARY_DIR}
    -DZEN_BUILD_CONFIG=$<CONFIG>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_population.cmake)

# ---- The documentation checks -----------------------------------------------------------
# They read the source tree, not a build, so they register in every configuration: `doc_links`
# resolves every repository-relative reference, `source_comments` and `doc_standard` hold the
# comment and document standard (CONTRIBUTING.md#comments-and-documents), and `code_values` the
# values the code owns. Pure CMake, as every check here is: a verifier may not depend on a tool
# that merely happens to be installed.
add_test(NAME doc_links COMMAND ${CMAKE_COMMAND}
    -DZEN_REPO=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_doc_links.cmake)
add_test(NAME source_comments COMMAND ${CMAKE_COMMAND}
    -DZEN_REPO=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_source_comments.cmake)
add_test(NAME doc_standard COMMAND ${CMAKE_COMMAND}
    -DZEN_REPO=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_doc_standard.cmake)
add_test(NAME code_values COMMAND ${CMAKE_COMMAND}
    -DZEN_REPO=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_code_values.cmake)

# ---- The hook-return contract (SENSE-06) -------------------------------------------------
# Compile-judged: `on_claim_published` may return `void`, `bool` or `loom::Weave::PublishedClaim`
# and nothing else. Four positive controls and one negative, each a `try_compile` against this
# tree's public headers in a project and build tree of its own (tests/hook_return/CMakeLists.txt),
# since a compile test writes the tree it names.

# MSVC's cl.exe finds its headers through INCLUDE, which this configure's shell has and the lane's
# fresh shell does not, so INCLUDE and LIB travel with the entry, `;` spelled `|` to stay one
# argument (empty on other toolchains). The witness links nothing.
string(REPLACE ";" "|" zen_hook_env_include "$ENV{INCLUDE}")
string(REPLACE ";" "|" zen_hook_env_lib "$ENV{LIB}")
add_test(NAME hook_return_types COMMAND ${CMAKE_COMMAND}
    -S ${CMAKE_CURRENT_SOURCE_DIR}/hook_return
    -B ${CMAKE_BINARY_DIR}/hook_return-witness
    -G ${CMAKE_GENERATOR}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DZEN_LOOM_INCLUDE=${CMAKE_SOURCE_DIR}/include
    "-DZEN_HOOK_ENV_INCLUDE=${zen_hook_env_include}"
    "-DZEN_HOOK_ENV_LIB=${zen_hook_env_lib}")

# ---- The supplied host, as a real process (GATE-05, LIFE-04, MSG-09) --------------------
# A defect in the host's glue (which message answered which question, when an approved permission
# took effect, whether the operator is heard while a weave keeps the bus busy) is invisible to a
# test of the deciding half and visible in one console transcript. `host_console` needs no kernel;
# `host_process_weaves` rides the kernel gate. tests/host_process/run.cmake says what each holds.
add_test(NAME host_console COMMAND ${CMAKE_COMMAND}
    -DZEN_HOST_EXE=$<TARGET_FILE:loom-host>
    -DZEN_WORK=${CMAKE_BINARY_DIR}/host-process-work/console
    -DZEN_GROUP=console
    -P ${CMAKE_CURRENT_SOURCE_DIR}/host_process/run.cmake)

# ---- The persistent session, as independent processes (docs/guides/sessions.md) ---------
# A real `loom-host --serve`, the `loom-runs` artifact it boots, real Python workers, and clients
# that attach, act and leave: runs found finished, edited tools, overlapping runs, failures,
# cancellation, a lost answer recovered by name, pinned approval, the door's attenuation and a
# restarted host refusing old handles. Rides the `python` gate.
if(zen_session_python)
    add_test(NAME session_journey COMMAND ${zen_session_python}
        ${CMAKE_CURRENT_SOURCE_DIR}/session/journey.py
        --host $<TARGET_FILE:loom-host>
        --runs $<TARGET_FILE:loom-runs>
        --runtime ${CMAKE_BINARY_DIR}/session-tools/python
        --tools ${CMAKE_SOURCE_DIR}/tools/basics
        --work ${CMAKE_BINARY_DIR}/session-journey-work
        --evidence ${CMAKE_BINARY_DIR}/session-journey-work.json)
    set_tests_properties(session_journey PROPERTIES TIMEOUT 600)
    # The Python client's answer rule, where no host can stage a forged answer: a conversation
    # settles on Loom's attested answer or its refusal, never on a word under the same number.
    add_test(NAME session_client COMMAND ${zen_session_python}
        ${CMAKE_CURRENT_SOURCE_DIR}/session/test_client.py
        --runtime ${CMAKE_BINARY_DIR}/session-tools/python)
    set_tests_properties(session_client PROPERTIES TIMEOUT 60)
    # The observation journey: a subscription across two real hosts. The session host links to a
    # far host (observe_far/far_host.cpp: a bridge server, a relay admitting one guest to two
    # shapes, a producer it can drive), boots the probe vocabulary and runs the probe package
    # (session/observe_probe) as real workers, reading each outcome from the other process.
    add_executable(zen-observe-far-host observe_far/far_host.cpp)
    target_link_libraries(zen-observe-far-host PRIVATE loom zen-switchboard zen-bridge zen-warnings
                                                       zen-sanitize)
    add_library(zen_observe_probe_vocab SHARED weavelib/observe_probe_vocab.cpp)
    target_link_libraries(zen_observe_probe_vocab PRIVATE loom zen-switchboard zen-warnings
                                                          zen-sanitize)
    loom_weave_build_contract(zen_observe_probe_vocab)
    add_test(NAME observe_journey COMMAND ${zen_session_python}
        ${CMAKE_CURRENT_SOURCE_DIR}/session/observe_journey.py
        --host $<TARGET_FILE:loom-host>
        --runs $<TARGET_FILE:loom-runs>
        --runtime ${CMAKE_BINARY_DIR}/session-tools/python
        --far $<TARGET_FILE:zen-observe-far-host>
        --vocab $<TARGET_FILE:zen_observe_probe_vocab>
        --probe ${CMAKE_CURRENT_SOURCE_DIR}/session/observe_probe
        --work ${CMAKE_BINARY_DIR}/observe-journey-work
        --evidence ${CMAKE_BINARY_DIR}/observe-journey-work.json)
    set_tests_properties(observe_journey PROPERTIES TIMEOUT 600)
endif()

# ---- The supplied host's input, through a real terminal --------------------------------
# A file is the one stdin whose reader cannot show a console's defects, so
# tests/host_terminal/witness.cpp builds the terminal a person types into (a hidden console on
# Windows, a pseudo-terminal on POSIX) and types: `host_line_input` (portable) and
# `host_terminal_weaves` (kernel, a real loom-host). `host_line_streams` (portable) feeds a pipe
# and a file: the input limits, the same on every platform, and no evidence about a terminal.
find_package(Threads REQUIRED)
add_executable(zen-host-terminal-witness host_terminal/witness.cpp)
target_link_libraries(zen-host-terminal-witness
    PRIVATE zen-host-policy Threads::Threads zen-warnings zen-sanitize)
add_test(NAME host_line_input COMMAND zen-host-terminal-witness reader)
set_tests_properties(host_line_input PROPERTIES TIMEOUT 240)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/host-process-work/streams)
add_test(NAME host_line_streams COMMAND zen-host-terminal-witness streams
    ${CMAKE_BINARY_DIR}/host-process-work/streams)
set_tests_properties(host_line_streams PROPERTIES TIMEOUT 240)

# ---- The weave build contract: two populations, two entries (KERN-05, POP-05) --------
# `weave_population`: is every artifact that must carry the contract on the roll? Its expectation
# comes from the build graph (weave_population.cmake), never from the roll. `weave_contract`: does
# every artifact on the roll have the properties, read from the built symbol tables? Both
# register when this configuration builds loadable weaves and are declared absent when it builds
# none (POP-03); the contract's own two fixtures are the canary, in both directions.
zen_required_weave_population(zen_weave_required_rows zen_weave_targets zen_weave_exempt)

if(zen_weave_targets OR TARGET zen_test_contract_applied OR TARGET zen_test_contract_bypass)
    # The contract's own pair is load-bearing for `weave_contract`: without the bypass twin still
    # carrying the unique binding, the contracted artifacts prove nothing. Losing it is loud.
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

    # The half of the host-process lane that needs loadable artifacts: attribution,
    # activation, adoption by every route that can load anything, and responsiveness while
    # a cooperative weave keeps the bus non-empty forever.
    add_test(NAME host_process_weaves COMMAND ${CMAKE_COMMAND}
        -DZEN_HOST_EXE=$<TARGET_FILE:loom-host>
        -DZEN_PROBE_LIB=$<TARGET_FILE:zen_host_probe>
        -DZEN_PROBE_FORGE_LIB=$<TARGET_FILE:zen_host_probe_forge>
        -DZEN_WORK=${CMAKE_BINARY_DIR}/host-process-work/weaves
        -DZEN_GROUP=weaves
        -P ${CMAKE_CURRENT_SOURCE_DIR}/host_process/run.cmake)

    # The same host, typed at through a real terminal: a delayed answer reported while a command
    # is half typed, work that continues through a pause mid-command, and inspection, authority,
    # stop and quit reachable across pauses and non-character input (witness.cpp's header).
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/host-process-work/terminal)
    add_test(NAME host_terminal_weaves COMMAND zen-host-terminal-witness host
        $<TARGET_FILE:loom-host> $<TARGET_FILE:zen_host_probe>
        ${CMAKE_BINARY_DIR}/host-process-work/terminal)
    set_tests_properties(host_terminal_weaves PROPERTIES TIMEOUT 240)
endif()
