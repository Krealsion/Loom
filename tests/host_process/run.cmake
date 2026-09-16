# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
#
# THE SUPPLIED HOST, AS A REAL PROCESS.
#
# WHY THIS LANE EXISTS. `loom-host`'s deciding half has a suite (`host_policy`), and it
# could not have caught a single one of the defects this file now pins. Every one of them
# lived in the GLUE — which arriving message answers which question, when an approved
# permission becomes effective, whether the operator can still be heard — and glue is only
# observable from outside, with a real process, a real console and a real loaded artifact.
# The alternative that was tried and is not enough is "a REPL is driven by hand".
#
# WHAT IT ASSERTS, AND WHAT IT DELIBERATELY DOES NOT. It asserts identities (which weave is
# governed), lifecycle facts (how many authenticated activations a participant saw),
# effective authority (what the bus actually permits, read back through the warden), and
# responsiveness (that commands typed during sustained traffic are executed and the host
# exits 0). It does NOT pin the host's sentences: every match below is a distinguishing
# fragment or a number, never a whole line of prose, so ordinary rewording does not fail
# this lane and a changed MEANING does.
#
# TWO GROUPS, because two different configurations can run them:
#   console   needs no kernel: the recovery route, decision ownership between two real
#             processes, repeated approval, and a failed write that must change nothing.
#   weaves    needs loadable artifacts: attribution, activation, adoption by every route.
#
# Parameters: ZEN_HOST_EXE, ZEN_WORK, ZEN_GROUP, and for `weaves` also ZEN_PROBE_LIB and
# ZEN_PROBE_FORGE_LIB.

cmake_minimum_required(VERSION 3.22)

foreach(v ZEN_HOST_EXE ZEN_WORK ZEN_GROUP)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "host_process: ${v} is required")
    endif()
endforeach()
if(NOT EXISTS "${ZEN_HOST_EXE}")
    message(FATAL_ERROR "host_process: no host executable at ${ZEN_HOST_EXE}")
endif()

set(zen_failures "")
set(zen_checks 0)

# ---- the two primitives every scenario is written in -------------------------

# A fresh, EMPTY directory per scenario. Scenarios write decision files and lock files;
# one leaking into the next would make a pass mean nothing.
function(zen_scenario_dir name out)
    set(dir "${ZEN_WORK}/${name}")
    file(REMOVE_RECURSE "${dir}")
    file(MAKE_DIRECTORY "${dir}")
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()

# Run the host with a scripted stdin. Captures stdout and stderr TOGETHER (the host writes
# refusals to stderr and answers to stdout, and a person sees one stream), and the real
# exit code — never a pipeline's.
#
# TIMEOUT IS A FAILURE, NOT A CONVENIENCE. The responsiveness scenario's whole claim is
# that the host comes back; a run that had to be killed is the defect, so the timeout is
# reported as one rather than swallowed.
function(zen_run_host dir script args out_text out_code)
    set(in "${dir}/stdin.txt")
    file(WRITE "${in}" "${script}")
    execute_process(
        COMMAND "${ZEN_HOST_EXE}" ${args}
        WORKING_DIRECTORY "${dir}"
        INPUT_FILE "${in}"
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        RESULT_VARIABLE code
        TIMEOUT 60)
    set(${out_text} "${out}${err}" PARENT_SCOPE)
    set(${out_code} "${code}" PARENT_SCOPE)
    # APPENDED, so a scenario that runs the host several times keeps every transcript; the
    # scenario directory is emptied when the scenario starts.
    file(APPEND "${dir}/output.txt"
         "--- stdin:\n${script}--- output:\n${out}${err}\n--- exit: ${code}\n\n")
endfunction()

# The condition is taken as ARGN and handed to if() as separate arguments. Written the
# obvious way first -- one `condition` parameter -- it arrived at if() as a single string
# with spaces in it, which CMake reads as a variable NAME: every check then tested whether
# some undefined variable was set, and a run where nothing worked reported eleven passes.
# A check harness gets its own canary below for exactly that reason.
macro(zen_check what)
    math(EXPR zen_checks "${zen_checks} + 1")
    if(${ARGN})
        message(STATUS "  ok: ${what}")
    else()
        list(APPEND zen_failures "${what}")
        message(STATUS "  FAILED: ${what}")
    endif()
endmacro()

# THE HARNESS'S OWN CANARY, run before anything it is supposed to judge. It proves that a
# false condition is actually reported as a failure -- which the first version of
# `zen_check` did not do, and could not have been noticed from a green run.
set(zen_canary_failures "")
set(zen_canary_saved "${zen_failures}")
zen_check("canary: THIS LINE MUST READ 'FAILED' -- a false condition" 1 EQUAL 2)
list(LENGTH zen_failures zen_canary_len)
if(NOT zen_canary_len EQUAL 1)
    message(FATAL_ERROR
        "host_process: the check harness did not report a deliberately false condition as a "
        "failure. Every result below would be meaningless, so nothing else runs.")
endif()
zen_check("canary: a true condition passes" 1 EQUAL 1)
list(LENGTH zen_failures zen_canary_len)
if(NOT zen_canary_len EQUAL 1)
    message(FATAL_ERROR
        "host_process: the check harness reported a true condition as a failure.")
endif()
set(zen_failures "${zen_canary_saved}")
set(zen_checks 0)

# ---- the console group -------------------------------------------------------

if(ZEN_GROUP STREQUAL "console")

    # 1. THE ADVERTISED RECOVERY ROUTE REACHES A CONSOLE.
    #
    # `--no-boot` is documented as the way back in when the boot plan is what is broken.
    # It used to parse and validate the plan BEFORE applying --no-boot, so malformed JSON
    # exited 3 without ever opening the console — the one surface from which a person
    # could have fixed the file. `--check` must still fail on the same file: validating is
    # what --check is for, and a recovery route that softened it would have removed the
    # only command that answers "is my file right?".
    message(STATUS "host_process/console: broken boot plan")
    zen_scenario_dir(broken-plan dir)
    file(WRITE "${dir}/loom-boot.json" "{broken")
    zen_run_host("${dir}" "status\nquit\n" "" text code)
    zen_check("a broken plan with --check is an error" NOT code EQUAL 0)

    zen_run_host("${dir}" "status\nquit\n" "--no-boot" text code)
    zen_check("--no-boot over a broken plan exits 0" code EQUAL 0)
    zen_check("--no-boot over a broken plan says the plan was not run"
              text MATCHES "NOT run")
    zen_check("--no-boot over a broken plan reaches the console"
              text MATCHES "loom>")
    zen_check("status reports the unreadable plan rather than an empty one"
              text MATCHES "COULD NOT BE READ")
    # The same file, with --check, is still an error — and that is a SEPARATE run so the
    # two answers cannot come from one code path that happens to satisfy both.
    zen_run_host("${dir}" "" "--check" text code)
    zen_check("--check still refuses the same broken plan" NOT code EQUAL 0)

    # 2. TWO REAL HOSTS CANNOT BOTH OWN ONE DECISION STORE.
    #
    # Decisions are written whole, so a second host holding a stale copy does not merely
    # lose its own approval: it RESTORES what the first host revoked. Reproduced by one
    # person with two terminals, so it is pinned with two real processes.
    #
    # THE PIPELINE IS THE TRICK AND IT IS HONEST. CMake's execute_process starts every
    # COMMAND of a pipeline at once; `cmake -E sleep` holds the first host's stdin open
    # (and produces nothing) so it stays alive, and the second host runs beside it. Which
    # of the two wins the claim is a race and is NOT asserted — what is asserted is that
    # exactly one of them owns it, which is the property.
    message(STATUS "host_process/console: two hosts, one decision store")
    zen_scenario_dir(shared-store dir)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E sleep 4
        COMMAND "${ZEN_HOST_EXE}" --no-boot --authority "${dir}/shared.json"
        COMMAND "${ZEN_HOST_EXE}" --no-boot --authority "${dir}/shared.json"
        WORKING_DIRECTORY "${dir}"
        OUTPUT_VARIABLE pair_out
        ERROR_VARIABLE pair_err
        RESULTS_VARIABLE pair_codes
        TIMEOUT 60)
    file(WRITE "${dir}/pair.txt" "${pair_out}${pair_err}\n--- codes: ${pair_codes}\n")
    list(GET pair_codes 1 first_code)
    list(GET pair_codes 2 second_code)
    set(refused 0)
    foreach(c ${first_code} ${second_code})
        if(c EQUAL 4)
            math(EXPR refused "${refused} + 1")
        endif()
    endforeach()
    # THE WINNER'S EXIT CODE IS NOT ASSERTED, and the reason is the plumbing rather than
    # the host: the winner writes its banner into the loser's stdin, the loser has already
    # exited, and the winner dies of SIGPIPE. That is an artifact of chaining two consoles
    # together to get them running at the same time. What matters, and what is asserted,
    # is that exactly ONE of them was refused -- so exactly one holds the store, whichever
    # one won the race.
    zen_check("exactly one of two concurrent hosts is refused the decision store"
              refused EQUAL 1)
    zen_check("...so a claim really was taken" EXISTS "${dir}/shared.json.lock")
    zen_check("the refusal names the store and a way forward"
              "${pair_err}" MATCHES "already owns the decision store")

    # ...AND THE CLAIM IS RELEASED BY THE OS WHEN A HOST ENDS. This is why the mechanism
    # is a lock and not a PID file: nothing has to clean up after a host that died.
    zen_run_host("${dir}" "quit\n" "--no-boot;--authority;${dir}/shared.json" text code)
    zen_check("a later host takes the store over once the owner is gone" code EQUAL 0)

    # 3. A FAILED WRITE CHANGES NOTHING — LIVE OR REMEMBERED.
    #
    # The store used to mutate its map and then try to write, so an unwritable file
    # printed `cannot write` and changed what the host permitted anyway: the approval was
    # good for that session and the next boot had never heard of it. The candidate is
    # written first now, so a failed command is a command that did not happen.
    #
    # The injection is the temp path being a DIRECTORY, which no rename can replace.
    message(STATUS "host_process/console: a failed write changes nothing")
    zen_scenario_dir(write-failure dir)
    file(MAKE_DIRECTORY "${dir}/decisions.json.tmp")
    zen_run_host("${dir}"
                 "authority trust probe\nauthority\nquit\n"
                 "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("an unwritable store reports the write failure" text MATCHES "cannot write")
    zen_check("...and says the decision is unchanged" text MATCHES "unchanged")
    zen_check("...and the live policy did NOT take the approval"
              NOT text MATCHES "run +probe")
    zen_check("...and nothing was written" NOT EXISTS "${dir}/decisions.json")

    # 4. REPEATING AN APPROVAL DOES NOT DEFEAT TAKING IT BACK.
    #
    # `allow` twice used to append two copies of one permission; one `revoke` removed one,
    # reported a revocation, and left the permission delegated and remembered.
    message(STATUS "host_process/console: repeated approval, one revoke")
    zen_scenario_dir(duplicate dir)
    zen_run_host("${dir}"
                 "authority trust probe
authority allow probe Greet v1 -> any target
authority allow probe Greet v1 -> any target
authority
authority revoke probe Greet v1 -> any target
authority
authority show probe
quit
" "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("a repeated approval says it is already granted" text MATCHES "already allowed")
    # The store's own count, which is the fact -- not a regex over spliced output. Two
    # allows of one permission leave ONE send rule; one revoke leaves none.
    zen_check("the permission is stored once, not twice" text MATCHES "1 send, 0 observe")
    zen_check("one revoke takes it back for good" text MATCHES "0 send, 0 observe")
    string(REGEX MATCHALL "may say:" says "${text}")
    list(LENGTH says say_count)
    zen_check("...and nothing is left to say afterwards" say_count EQUAL 0)

    # ...and the same rule written twice BY HAND, which is the case a console-side guard
    # alone cannot reach: the file is a surface this product invites people to edit.
    zen_scenario_dir(duplicate-file dir)
    file(WRITE "${dir}/decisions.json"
         "{\"rules\":[{\"artifact\":\"probe\",\"content_id\":\"\",\"may_run\":true,"
         "\"trust_rebuilds\":false,"
         "\"send\":[\"Greet v1 -> any target\",\"Greet v1 -> any target\"],"
         "\"observe\":[],\"note\":\"\"}]}\n")
    zen_run_host("${dir}"
                 "authority
authority revoke probe Greet v1 -> any target
authority
authority show probe
quit
" "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("a hand-written duplicate is collapsed and said out loud"
              text MATCHES "repeated permission")
    zen_check("...and read as one permission" text MATCHES "1 send, 0 observe")
    zen_check("one revoke clears a hand-written duplicate too"
              text MATCHES "0 send, 0 observe")
    string(REGEX MATCHALL "may say:" says "${text}")
    list(LENGTH says say_count)
    zen_check("...with nothing left to say" say_count EQUAL 0)

    # 4b. AND A REVOKE UNDER A BROADER STANDING RULE SAYS SO.
    #
    # Taking back `Greet v1 -> any target` while `any shape -> any target` is still
    # granted changes the list and changes nothing about what the weave may do. A bare
    # "revoked" there would be true about the file and misleading about the world.
    zen_scenario_dir(revoke-under-broad dir)
    zen_run_host("${dir}"
                 "authority trust probe
authority allow probe any shape -> any target
authority allow probe Greet v1 -> any target
authority revoke probe Greet v1 -> any target
quit
" "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("a revoke under a broader rule reports the honest remainder"
              text MATCHES "STILL PERMITTED")

    # 5. AND A DECISION THAT WAS WRITTEN SURVIVES A RESTART — the positive control for
    # scenario 3, so "nothing was remembered" there is a real distinction and not a store
    # that never remembers anything.
    message(STATUS "host_process/console: a written decision survives a restart")
    zen_scenario_dir(restart dir)
    zen_run_host("${dir}" "authority trust probe\nquit\n"
                 "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("the approval is written" EXISTS "${dir}/decisions.json")
    zen_run_host("${dir}" "authority\nquit\n"
                 "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("a restarted host still has it" text MATCHES "run +probe")

endif()

# ---- the weaves group --------------------------------------------------------

if(ZEN_GROUP STREQUAL "weaves")
    foreach(v ZEN_PROBE_LIB ZEN_PROBE_FORGE_LIB)
        if(NOT EXISTS "${${v}}")
            message(FATAL_ERROR "host_process: ${v} names no file (${${v}})")
        endif()
    endforeach()

    # A decision file that approves the probe to run and to make its startup send. Written
    # by hand in the person's own format, which is also a check that the format a guide
    # tells people to write is the format the host reads.
    function(zen_write_decisions path rules)
        file(WRITE "${path}"
             "{\"rules\":[{\"artifact\":\"probe\",\"content_id\":\"\",\"may_run\":true,"
             "\"trust_rebuilds\":true,\"send\":[${rules}],\"observe\":[],\"note\":\"\"}]}\n")
    endfunction()

    function(zen_write_plan path lib)
        file(WRITE "${path}"
             "{\"boot\":[{\"name\":\"probe\",\"path\":\"${lib}\",\"role\":\"probe\","
             "\"enabled\":true,\"on_failure\":\"stop\"}]}\n")
    endfunction()

    # 1. A BOOT-STARTED PARTICIPANT IS ACTIVATED, AND ITS APPROVED AUTHORITY IS ALREADY
    #    IN FORCE WHEN IT USES IT.
    #
    # Two numbers, and each one is a separate finding:
    #   activations=1  the boot walk goes through the same door `start` does, so an
    #                  authenticated `zen.Activated` is announced. It called Kernel::load
    #                  directly before, and a participating probe reported activations=0
    #                  under a boot this host printed as COMPLETE.
    #   startups=1     the permission the person had already approved was installed
    #                  BEFORE the weave was told it was live. Installed one turn later —
    #                  which is where adoption-on-the-answer necessarily puts it — this is
    #                  0 and `refusals` is 1 instead.
    message(STATUS "host_process/weaves: boot activates, and approved authority is in force")
    zen_scenario_dir(boot-activation dir)
    zen_write_decisions("${dir}/decisions.json" "\"Startup v1 -> role probe\"")
    zen_write_plan("${dir}/plan.json" "${ZEN_PROBE_LIB}")
    zen_run_host("${dir}"
                 "authority show probe\nsend 5 Inspect 1\nquit\n"
                 "--boot;${dir}/plan.json;--authority;${dir}/decisions.json" text code)
    zen_check("the host comes back" code EQUAL 0)
    zen_check("the boot row started the probe" text MATCHES "probe")
    # THE IDENTITY, CHECKED IN THE SAME RUN THAT USES IT. `send 5` above names a weave by
    # number because the console addresses weaves by number; this line is what makes that
    # number an assertion instead of an assumption. If the host's construction order ever
    # changes, this fails and says so rather than silently sending somewhere else.
    zen_check("the administered subject is weave 5, which is the weave the sends name"
              text MATCHES "LIVE, weave 5")
    zen_check("an authenticated activation reached the boot-started participant"
              text MATCHES "activations=1")
    zen_check("the approved startup send was permitted at the first breath"
              text MATCHES "startups=1")
    zen_check("...and nothing of ours was refused" text MATCHES "refusals=0")
    zen_check("the remembered permission is delegated, not baseline"
              text MATCHES "revocable.: Startup v1 -> role probe")

    # 2. AN UNRELATED `zen.Result` IS NOT THIS HOST'S ANSWER.
    #
    # The forge variant authors a `zen.Result{"1"}` at the console, unasked, timed to land
    # after the genuine answer. It needs no extra approval to do it. What must hold: the
    # operator's `start` reports the weave the steward actually loaded, and the subject
    # this host administers is that weave — not weave 1, the console, whose baseline is
    # `any shape -> any target` and whose adoption would have handed a loaded artifact the
    # operator's own reach.
    message(STATUS "host_process/weaves: an unrelated Result is not an answer")
    zen_scenario_dir(unrelated-reply dir)
    zen_write_decisions("${dir}/decisions.json" "\"Startup v1 -> role probe\"")
    zen_run_host("${dir}"
                 "start probe ${ZEN_PROBE_FORGE_LIB} probe
authority show probe
buffer
quit
" "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("the forging probe starts" code EQUAL 0)
    zen_check("the administered subject is the loaded weave, not the console"
              text MATCHES "LIVE, weave 5")
    zen_check("the console's own wildcard baseline is not what got described"
              NOT text MATCHES "LIVE, weave 1")
    zen_check("the host reported no subject mismatch" NOT text MATCHES "WARNING")
    # THE FORGERY DID ARRIVE, otherwise this scenario proves nothing about ignoring it.
    # `buffer` prints each retained reply with its bus-stamped author: the genuine answer
    # is a `zen.Result` carrying "5" from weave 4 (the steward), and the forgery is a
    # `zen.Result` carrying "1" from weave 5 (the artifact). Both are in the window; only
    # one of them was ever anybody's answer.
    zen_check("the genuine answer came from the steward"
              text MATCHES "zen.Result v1  from weave 4  5")
    zen_check("the unrelated Result really was delivered to the console"
              text MATCHES "zen.Result v1  from weave 5  1")

    # 3. AN ORDINARY `zen.LoadWeave` PRODUCES THE SAME GOVERNED PARTICIPANT.
    #
    # A weave (or an operator) sending the steward's own shape used to load the artifact
    # and bypass adoption entirely: `authority show` then said nothing was loaded under
    # that name and the remembered permissions were never installed. Adoption lives in the
    # door now, so there is no route that can miss it.
    message(STATUS "host_process/weaves: a message-driven load is governed too")
    zen_scenario_dir(message-load dir)
    zen_write_decisions("${dir}/decisions.json" "\"Startup v1 -> role probe\"")
    zen_run_host("${dir}"
                 "send 4 zen.LoadWeave 1 name=probe path=${ZEN_PROBE_LIB} role=probe
authority show probe
send 5 Inspect 1
quit
" "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("a message-driven load comes back" code EQUAL 0)
    zen_check("a message-driven load is under administration" text MATCHES "LIVE, weave 5")
    zen_check("...with the remembered permission installed"
              text MATCHES "revocable.: Startup v1 -> role probe")
    zen_check("...and it was activated and its startup send permitted"
              text MATCHES "activations=1 startups=1 refusals=0")

    # 4. THE CONSOLE STAYS REACHABLE WHILE THE BUS IS BUSY FOREVER.
    #
    # `Spin` re-arms itself in its own handler, so the queue is never empty again. Under an
    # unbounded drain the host never read another command: `stop probe` and `quit` were
    # already in the pipe and were never executed, and the review had to kill it. Every
    # handler returned; nothing was stuck in native code.
    #
    # The `Inspect` between them is the other half: useful progress DID happen — the ticks
    # are real work — while the operator kept being served.
    message(STATUS "host_process/weaves: the console survives an endlessly busy bus")
    zen_scenario_dir(busy-weave dir)
    zen_write_decisions("${dir}/decisions.json"
                        "\"Startup v1 -> role probe\",\"Spin v1 -> role probe\"")
    zen_write_plan("${dir}/plan.json" "${ZEN_PROBE_LIB}")
    zen_run_host("${dir}"
                 "send 5 Spin 1
send 5 Inspect 1
authority show probe
stop probe
quit
" "--boot;${dir}/plan.json;--authority;${dir}/decisions.json" text code)
    zen_check("the host returns rather than having to be killed" code EQUAL 0)
    zen_check("a command typed after the spin started was executed"
              text MATCHES "'probe' stopped")
    zen_check("inspection stayed reachable during sustained traffic"
              text MATCHES "activations=1")
    zen_check("...and the weave really was doing work" text MATCHES "ticks=[1-9]")
    zen_check("authority inspection stayed reachable too" text MATCHES "LIVE, weave 5")

    # 5. UNLOAD RETIRES THE ADMINISTRATION, BY THE DOOR THAT DID IT.
    #
    # After `stop`, there is no subject to administer and `authority show` must say so
    # rather than describing a weave that no longer exists. The person's standing decision
    # is untouched, which is why starting it again restores exactly what they approved.
    message(STATUS "host_process/weaves: unload retires the administration")
    zen_scenario_dir(unload dir)
    zen_write_decisions("${dir}/decisions.json" "\"Startup v1 -> role probe\"")
    zen_write_plan("${dir}/plan.json" "${ZEN_PROBE_LIB}")
    zen_run_host("${dir}"
                 "stop probe
authority show probe
start probe ${ZEN_PROBE_LIB} probe
authority show probe
quit
" "--boot;${dir}/plan.json;--authority;${dir}/decisions.json" text code)
    zen_check("stopping an application leaves the host running" text MATCHES "still running")
    zen_check("a stopped artifact is no longer administered"
              text MATCHES "nothing is loaded under 'probe'")
    zen_check("the standing decision survived the unload" text MATCHES "probe: may run")
    zen_check("starting it again restores the administration and the approval"
              text MATCHES "revocable.: Startup v1 -> role probe")

    # 6. A RELOAD IS NEW CODE BEHIND A STABLE ID, AND IS ADOPTED AGAIN.
    message(STATUS "host_process/weaves: reload keeps the id and re-adopts")
    zen_scenario_dir(reload dir)
    zen_write_decisions("${dir}/decisions.json" "\"Startup v1 -> role probe\"")
    zen_write_plan("${dir}/plan.json" "${ZEN_PROBE_LIB}")
    zen_run_host("${dir}"
                 "reload probe ${ZEN_PROBE_LIB}
authority show probe
send 5 Inspect 1
quit
" "--boot;${dir}/plan.json;--authority;${dir}/decisions.json" text code)
    zen_check("a reload comes back" code EQUAL 0)
    zen_check("the reloaded incarnation keeps its id and stays administered"
              text MATCHES "LIVE, weave 5")
    zen_check("a reload earns its own activation, so the count grows"
              text MATCHES "activations=2")
    zen_check("...and the startup send was permitted on the new incarnation too"
              text MATCHES "startups=2 refusals=0")

    # 7. REVOKING A PERMISSION TAKES IT AWAY FROM THE RUNNING WEAVE.
    #
    # The remembered half is delegated live authority precisely so this is possible: a
    # baseline could not be taken back. After the revoke the probe's next startup send is
    # refused, and Loom's own notice says so.
    message(STATUS "host_process/weaves: revocation reaches the running weave")
    zen_scenario_dir(revoke-live dir)
    zen_write_decisions("${dir}/decisions.json" "\"Startup v1 -> role probe\"")
    zen_write_plan("${dir}/plan.json" "${ZEN_PROBE_LIB}")
    zen_run_host("${dir}"
                 "authority revoke probe Startup v1 -> role probe
reload probe ${ZEN_PROBE_LIB}
send 5 Inspect 1
quit
" "--boot;${dir}/plan.json;--authority;${dir}/decisions.json" text code)
    zen_check("a revoke reaches the bus" text MATCHES "may now say: nothing")
    zen_check("the revoked send is refused on the next activation"
              text MATCHES "startups=1 refusals=1")

    # 8. A BUILD WHOSE PIN CANNOT BE WRITTEN IS REFUSED, AND SO IS EVERY OTHER BUILD.
    #
    # An approval without `--rebuilds` means "this build; ask me again when it changes", and
    # the pin is the only record that can notice a change. With the store's temp path blocked
    # the host used to admit build A with a note nobody saw until `status`, leave the rule
    # unpinned, and then admit a DIFFERENT build B the same way. The two builds are the two
    # probe variants: one artifact name, different bytes. The approval is made at the console,
    # which is the person's route and the one that leaves the rule unpinned.
    message(STATUS "host_process/weaves: a pin that cannot be written refuses the build")
    zen_scenario_dir(failed-pin dir)
    zen_run_host("${dir}" "authority trust probe\nquit\n"
                 "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("the approval is remembered, and a rebuild will ask again"
              text MATCHES "a rebuild of it will ask again")
    file(MAKE_DIRECTORY "${dir}/decisions.json.tmp")
    zen_run_host("${dir}"
                 "start probe ${ZEN_PROBE_LIB} probe
start probe ${ZEN_PROBE_LIB} probe
start probe ${ZEN_PROBE_FORGE_LIB} probe
authority show probe
quit
" "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("the host comes back" code EQUAL 0)
    string(REGEX MATCHALL "could not record which build" pin_refusals "${text}")
    list(LENGTH pin_refusals pin_refusal_count)
    zen_check("build A, a retry of A, and a different build B are each refused"
              pin_refusal_count EQUAL 3)
    zen_check("...naming the storage failure" text MATCHES "cannot write")
    zen_check("...so nothing was loaded or administered"
              text MATCHES "nothing is loaded under 'probe'")
    zen_check("...no pin was invented" text MATCHES "build pinned: .not yet.")
    zen_check("...and the approval did not widen to any rebuild"
              NOT text MATCHES "any rebuild is accepted")

    # The recovery route is the ordinary one: make the store writable and start it again.
    file(REMOVE_RECURSE "${dir}/decisions.json.tmp")
    zen_run_host("${dir}"
                 "start probe ${ZEN_PROBE_LIB} probe
authority show probe
stop probe
start probe ${ZEN_PROBE_FORGE_LIB} probe
quit
" "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("once the store is writable the approved build runs" text MATCHES "LIVE, weave")
    zen_check("...pinned by its bytes" text MATCHES "build pinned: [0-9a-f]+")
    zen_check("...and a different build asks again, as the person chose"
              text MATCHES "changed since it was approved")
    zen_run_host("${dir}"
                 "start probe ${ZEN_PROBE_FORGE_LIB} probe
start probe ${ZEN_PROBE_LIB} probe
authority show probe
quit
" "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("a restarted host still refuses the different build"
              text MATCHES "changed since it was approved")
    zen_check("...and runs the pinned one" text MATCHES "LIVE, weave")

    # ...AND `--rebuilds` IS CONSENT TO ANY BUILD, SO A DISK DOES NOT WITHDRAW IT. The missing
    # record is said when the start happens, not only when `status` is asked.
    message(STATUS "host_process/weaves: --rebuilds is not withdrawn by an unwritable pin")
    zen_scenario_dir(failed-pin-rebuilds dir)
    zen_run_host("${dir}" "authority trust probe --rebuilds\nquit\n"
                 "--no-boot;--authority;${dir}/decisions.json" text code)
    file(MAKE_DIRECTORY "${dir}/decisions.json.tmp")
    zen_run_host("${dir}"
                 "start probe ${ZEN_PROBE_LIB} probe
stop probe
start probe ${ZEN_PROBE_FORGE_LIB} probe
authority show probe
quit
" "--no-boot;--authority;${dir}/decisions.json" text code)
    zen_check("under --rebuilds the first build runs" text MATCHES "'probe' stopped")
    zen_check("...and so does a different one" text MATCHES "LIVE, weave")
    zen_check("...and the record that could not be made is said as the start happens"
              text MATCHES "note: probe: admitted because trust_rebuilds is on")
    zen_check("...naming what could not be written" text MATCHES "could not be recorded")

    # 9. EVERY ANSWER THE HOST WAS OWED IS COLLECTED, INCLUDING LATE ONES, AND NOTHING PILES UP.
    #
    # The console holds a slot for each conversation from the ask until its answer is TAKEN.
    # Forty conversations answered only after the host has stopped waiting (`Countdown` answers
    # 64 turns later), then three hundred ordinary ones: a host that left a single late answer
    # untaken would run out of its 32 slots, and its later sends would say "untracked".
    message(STATUS "host_process/weaves: sustained and delayed answers are all collected")
    zen_scenario_dir(sustained dir)
    zen_write_decisions("${dir}/decisions.json"
                        "\"Startup v1 -> role probe\",\"Tock v1 -> role probe\"")
    zen_write_plan("${dir}/plan.json" "${ZEN_PROBE_LIB}")
    set(script "")
    foreach(i RANGE 1 40)
        string(APPEND script "send 5 Countdown 1 turns=64\n")
    endforeach()
    foreach(i RANGE 1 300)
        string(APPEND script "send 5 Inspect 1\n")
    endforeach()
    string(APPEND script "asks\nquit\n")
    zen_run_host("${dir}" "${script}"
                 "--boot;${dir}/plan.json;--authority;${dir}/decisions.json" text code)
    zen_check("the host comes back" code EQUAL 0)
    string(REGEX MATCHALL "counted down" late "${text}")
    list(LENGTH late late_count)
    zen_check("every one of forty delayed answers was reported when it landed"
              late_count EQUAL 40)
    string(REGEX MATCHALL "reply -> m[0-9]+  activations=" replies "${text}")
    list(LENGTH replies reply_count)
    zen_check("all three hundred ordinary answers were attributed" reply_count EQUAL 300)
    zen_check("no send ever ran out of slots" NOT text MATCHES "untracked")
    zen_check("and nothing is left held at the end" text MATCHES "no open conversations")

endif()

# ---- the verdict -------------------------------------------------------------

list(LENGTH zen_failures failed)
if(failed GREATER 0)
    message(STATUS "")
    foreach(f IN LISTS zen_failures)
        message(STATUS "FAILED: ${f}")
    endforeach()
    message(FATAL_ERROR
        "host_process(${ZEN_GROUP}): ${failed} of ${zen_checks} checks FAILED. "
        "Each scenario's captured stdin/stdout is under ${ZEN_WORK}.")
endif()
message(STATUS "host_process(${ZEN_GROUP}): PASSED -- ${zen_checks} checks")
