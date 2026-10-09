# AGENTS.md — Loom

For a machine collaborator working in this repository. The router is **`docs/CONTEXT.md`**
(topic → reference → laws → tests); the human router is `docs/README.md`. Normative truth is
`docs/reference/` and `docs/laws/` only; `docs/history/` is a frozen record; `docs/evidence/` is
not the API contract. What a comment or a page may carry is in
[CONTRIBUTING.md](CONTRIBUTING.md#comments-and-documents). A `ready` issue goes to a pull request
by [taking an issue](docs/contributing/taking-an-issue.md), and
[best practices](docs/contributing/best-practices.md) names the owner of each practice.

## Build and test

Linux is the reference platform. GCC 11.4 or newer and CMake 3.22 or newer run the whole lane (the
library alone configures with CMake 3.16).

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"
cmake -DZEN_BUILD_DIR=build -P tests/verify.cmake     # THE official lane
# sanitizer lane: the same with -B build-san -DZEN_SANITIZE=ON
cmake -P tests/documentation_lane.cmake               # the documentation lane, no build
```

- **A change that touches no compiled line is verified by the documentation lane.** Markdown, an
  image under `docs/`, a text check or the files only text checks read, and a C/C++ file whose
  tokens are unchanged and only its comments differ: `tests/check_change_kind.cmake` says each
  changed file's kind against `origin/main` and why, and the lane runs every text check
  `tests/text_checks.cmake` lists, in seconds, passing only for a documentation-only change. CI
  runs those checks on every change and skips the build and test jobs for a documentation-only
  one. Anything else — a compiled line, a CMake file, CI, the lane itself — is the official
  lane's.

- `-Werror` is on; C++20. GCC 11.4 is the floor, so use nothing only a newer compiler accepts.
- **Every loadable weave target goes through `loom_weave_build_contract()`**
  (`cmake/loom-weave.cmake`, KERN-05), including the fixtures in `tests/` and `loom` and
  `zen-switchboard` themselves, since their objects land inside the image. Never hand-write the
  compiler option: the `weave_contract` entry reads the built artifacts and will say so. Which
  artifacts *must* carry it is derived from the build graph by the `weave_population` entry
  (every `SHARED`/`MODULE` library in `tests/` and the static libraries in their link closures),
  and one that left the roll is named (POP-05). A deliberate exception is
  `zen_weave_contract_exempt(<target> <reason>)`, in writing, and it fails if that target then
  turns up contracted.
- The `isolation`, `policy` and `all` suites need a delegated cgroup scope: run the whole binary
  through `tests/run-under-scope.sh ./build/tests/zen-tests`. Outside such a scope every
  OS-enforcement case fails **by design** (failing hard beats skipping silently) and names the
  capability it could not enforce.
- The `isolation` suite's granted-network positive control opens its own listener on
  `127.0.0.1:0` and requires a successful connect and a token byte, so no host's closed-port
  behaviour is consulted anywhere: a failure there is a real failure
  ([capabilities](docs/reference/capabilities.md#the-granted-network-positive-control-and-the-endpoint-it-uses)).
- Windows builds the portable subset only, on **MinGW-w64 and MSVC** alike; the kernel lanes are
  opt-in (`LOOM_ENABLE_WINDOWS_KERNEL`). MSVC needs `/Zc:preprocessor` for Loom's public
  `__VA_OPT__` macros, and `loom::core` carries it as an interface option, so say which compiler a
  Windows green was proven on, never a bare "Windows".
- The installed package has its own witness, outside the build tree:
  `cmake -DZEN_PREFIX=<prefix> -DZEN_WORK=<dir> -P tests/package/run.cmake`. It is the only lane
  that can catch a requirement the package fails to carry, because it is the only one that reaches
  Loom solely through `find_package`.
- **Never change Loom's linker without the full lane.** `-fuse-ld=gold` links faster and
  segfaults the weave suites (`kernel`, `handoff`, `all`): an exception propagating out of a
  `dlopen`ed weave dies, because gold resolves the ELF vague-linkage behaviour the
  reloadable-weave contract (KERN-05, `cmake/loom-weave.cmake`) depends on. The rule is general:
  a build speedup of any kind — linker, launcher, unity build, flag — is not adopted until
  `tests/verify.cmake` has run whole under it, and a faster red is not a result.

## The population contract — what a green run means (POP-01..05)

`tests/verify.cmake` is the lane to quote. A bare `ctest` is fine while working, but it accepts
`-R <matches nothing>` as success (exit 0, "No tests were found!!!"); the lane refuses that,
passes `--no-tests=error`, and refuses to run at all under the enforcement opt-out. The laws are
in `docs/laws/population-laws.md`.

- **Zero is never a pass.** `zen-tests --test-suite=<no match>` exits 70 with
  `EMPTY TEST POPULATION`; a selector matching no CTest entry fails the lane.
- **Suite registration is derived** from the `TEST_SUITE(...)` declarations in the compiled
  sources; there is no hand-kept name list to drift.
- **`tests/suite_population.txt`** is the inventory contract: the exact suite set per gate
  (`portable`, `kernel`, `posix`, `runs`) and a case floor per suite. Adding cases is
  free; deleting one crosses its floor. Adding or renaming a *suite* needs a line there. The
  `population` CTest entry enforces it without running any case.
- **`tests/entry_population.txt`** does the same for the CTest entries that are **not** suites:
  the population checks, the documentation checks, the weave-artifact checks, the host-process
  witnesses and the rest. The expected set is the suites declared for the active gates together
  with these, compared with `ctest -N` by name in both directions: declared and missing is a red,
  and so is registered and undeclared. Adding a CTest entry of any kind is a line in one of the
  two files, and which file says what kind of thing it is.
- **The entry inventory is taken twice, by two doors that do not lean on each other**: in
  `tests/verify.cmake` before the lane runs anything, and inside the `population` entry as part of
  its own work. One implementation (`tests/check_entry_population.cmake`), one expectation, two
  executions: a deleted entry cannot complain about its own deletion, so the lane's door notices a
  missing `population`; and a lane cannot check itself, so the entry's door asks the build what it
  registered. The lane's inventory leaves a **receipt** (the run, the build, the gates and the two
  identity sets it compared), and the entry refuses a run that carries the lane's token and shows
  no matching receipt. One ordinary deletion on either side is red; removing the lane's call *and*
  its announcement of itself escapes the doors' notice of each other, and still not the entry's
  own inventory.
- **OS-enforcement tallies are per suite and EXACT, not floors.** `isolation` and `policy` each
  expect an exact count of executed enforcement proofs, the same in a dedicated run and in `all`.
  A missing witness fails, and so does an unannounced extra one. Each number is owned by the
  `ZEN_ENFORCEMENT_POPULATION(...)` call at the end of `tests/test_isolation.cpp` and
  `tests/test_policy.cpp`; read it there, it is written nowhere else. A suite that includes
  `tests/enforcement_gate.hpp` must `#define ZEN_ENFORCEMENT_DOMAIN` first.
- **`ZEN_ALLOW_UNENFORCEABLE=1` / `ZEN_REQUIRE_ENFORCEMENT=0` prove nothing about containment.**
  They turn missing enforcement into marked, degraded skips so a host that cannot enforce can still
  run the rest; the coverage case then prints `*** NON-ENFORCEMENT MODE ***` and asserts no
  population, and the official lane refuses to start. Never quote such a run as enforcement
  evidence.
- **Declared absence is not a pass.** The default Windows build carries fewer suites than Linux
  because the `kernel` and `posix` gates are off; the `population` check prints each of those as
  `DECLARED ABSENT`. Do not compare the two totals, and do not read an absent suite as a passing
  one.
- **The same doctrine covers built artifacts** (POP-05): the `weave_population` entry pins which
  artifacts must carry the reloadable-weave build contract, derived from the build graph, so an
  artifact that stops taking it is named instead of silently leaving.
- **Populations are not interchangeable**: cases, suites, OS-enforcement proofs, assertions.
  Assertion totals are reported, never an acceptance oracle.
- **Documentation is checked too.** `doc_links` resolves every relative link in a current-facing
  `*.md` and its `#anchor`, and every repository-relative `docs/...md` path in a first-party C/C++
  or CMake comment (read from the repository root, because a comment moves with its code), and
  refuses a path outside the repository, whether spelled out or reached by climbing above it
  with `../`. `source_comments` and `doc_standard` hold the comment and document standard, and
  `code_values` the values the code owns, each read from its owner. A broken reference, a
  private id, a page telling its own history or the development process in a form
  `doc_standard` names, or a copied value out of step or unmarked is a red in the official
  lane. `docs/history/`, `archive/`, vendored trees and build trees are excluded by written
  rule.

## The supplied host, and who decides what a loaded artifact may do

`loom-host` (`src/host/`) is the one executable this project installs: a boot walk from a file
the person wrote, an operator console with authority in it, and two per-install files
(`loom-boot.json`, `loom-authority.json`). It is a replaceable default assembled from ordinary
participants — `ConsoleEngine`, `WeaveManager`, `ControlWeave`, and `loom::host::HostWarden` — and
holds no privilege the package does not export. Human route: `docs/guides/running-loom.md`;
prerequisites: `docs/guides/tools.md`.

**Three rules the host is built around, each for a defect a green lane could not see:**

1. An **answer** is something Loom attributed — the correlation the console minted plus the
   bus-stamped sender (`loom::AskBook`) — never the newest entry in the reply window, which any
   admitted artifact can author under the ordinary answer baseline.
2. A loaded artifact is **adopted inside the load**, through `loom::LifecycleAdoption`
   (`zen/kernel/control.hpp`): the one window between an incarnation being committed and being told
   it is live. Because it lives in the door, the boot walk, `start`, `reload` and an ordinary
   `zen.LoadWeave` all produce the same governed participant, and no host call site adopts
   anything.
3. The loop is `pump_pending()` plus a line read with a deadline (`src/host/line_input.hpp`), never
   `drain_until_idle()`: one approved self-addressed message would otherwise make `stop` and `quit`
   unreadable forever. The deadline holds while a line is **half typed**: a Windows console cannot
   be asked whether a line is finished, so it is read on its own thread. What the reader holds and
   refuses is one rule on every platform (`loom::host::HeldInput`): at most `kHeldInputBytes`
   waiting, and a line over `kMaxCommandBytes` refused whole, never run shortened or split
   (`docs/reference/bounds.md`).

A conversation that has not settled is reported PENDING and stays open; the host never invents a
completion. The console holds a conversation only for a caller that asks
(`ConsoleTracking::Tracked`), until that caller takes or forgets it.

**One host owns one decision store while it runs** (`src/host/store_lock.hpp`): the store is
written whole, so a second writer would restore what the first one revoked. A second host naming
the same file exits **4** and says how to proceed. Exit codes: 0 ok, 2 bad command line, 3 a file
would not parse, 4 store (or, serving, session directory) in use, 5 a session could not be opened.

**Building a capability on a session for some other application to drive** (a guest door, an
external-host tool package): `docs/guides/session-tools-map.md` routes each question — start and
return, find a capability, act on the target, read a result, recover, extend the harness — to the
section of `docs/guides/sessions.md` that answers it, so that application's own documentation
holds only what is its own (its admission and guest shapes).

**`--serve <dir>` keeps the host alive for clients** (`docs/guides/sessions.md`): closed stdin
closes only the console. One loopback door (`src/host/session_door.hpp`) admits the owner's key
as a client with exactly the session, runs and history vocabularies, and a run manager's one-time
worker credential (only its SHA-256 crosses the bus) with no more than that manager's own approved
authority, whole or refused. Every session is compat-encoded. The scoped reader
(`src/host/history_reader.hpp`) reads the Recorder and is blacklisted from it. The run manager
`loom-runs` (`src/runs/`) is an ordinary loadable weave — task policy, not host root — with its
Python runtime beside it. **Every delivery is gated, answers included**: host wiring grants its
own answer shapes; the operator grants a loaded manager its typed answers. Python is optional:
gate `runs` carries the C++ `runs` suite, gate `python` the process journey `session_journey`
(declared absent without an interpreter).

**Five CTest entries drive the real host**, because none of the above is visible to a test of the
deciding half. Two feed it through a file (`tests/host_process/run.cmake`: identities, counts and
effective authority, never whole sentences, a canary first): `host_console` (portable) and
`host_process_weaves` (kernel gate). Two type into a real terminal — a hidden native console on
Windows, a pseudo-terminal on POSIX (`tests/host_terminal/witness.cpp`): `host_line_input`
(portable) and `host_terminal_weaves` (kernel). One feeds the reader a pipe and a file with a
producer held back (`host_line_streams`, portable): the input limits, asserted identically on
every platform. **Redirected stdin is not evidence about a console.**

**`Kernel::load` mints no grant.** A `Kernel` asks its installed `AdmissionPolicy`
(`zen/kernel/admission.hpp`), and a Kernel on which nobody called `admit_with(...)` **admits
nothing and says so**. A host that wants every artifact trusted asks for it by name —
`trust_every_artifact("why")` — and the `why` rides every verdict. The four-argument
`load(..., Grant)` bypasses the policy deliberately: naming the grant at the call site *is* the
decision. Which build an artifact is (`AdmissionRequest::build`) is read from the file only when a
policy asks, once per operation, never eagerly; measure that by the work
(`file_content_id_scans()`), not the clock.
`docs/reference/capabilities.md#admitting-a-loaded-artifact`; suite `admission`.

## Intent, evidence, and architectural fit

Approved purpose, requirements, and architectural contracts determine whether a change is right.
Code, observations, and tests establish current behavior under the conditions examined. A passing
test cannot by itself settle intended behavior or justify weakening a contract. If they disagree,
trace the requirement and actual path, identify whether code, test, or prose is wrong, and correct
that part within the authorized scope. Escalate changes to intended contracts to their owner.

Before implementation and again at review, assess the complete result: does it serve the intended
capability, preserve coherent ownership/authority/composition, and leave selected next work and
known consumers able to build on it? Local green does not excuse avoidable coupling, duplicate
truth, special cases, or migration burdens imposed on future owners. Revise that design; make
necessary tradeoffs explicit for acceptance. Keep the argument proportional and grounded in
real consumers, not speculative abstractions. Required checks still have to pass.

## Consumers

This repository owns substrate truth. [Zengine](https://github.com/Krealsion/Zengine) owns its
packages (Timer and the others) and consumes Loom as an installed package (`find_package(loom)`).
Its tests need a Loom that exports `loom::kernel` (always on Linux; on Windows only under
`LOOM_ENABLE_WINDOWS_KERNEL`); against a kernel-less package its `tests/` **fails configuration**
rather than silently emptying itself, and `-DBUILD_TESTING=OFF` is its supported library-only
configuration. [Night Lab](https://github.com/Krealsion/zen-night-lab) is an independent
application; what it found is evidence (`docs/evidence/night-lab.md`), not contract.

## Do not assume

- That `Switchboard` has a `pump()` or a `run()`. Neither exists: both read, to an ordinary C++
  reader, as the bounded turn while meaning drain-to-idle. The ordinary host-loop operation is
  `pump_pending()`; the drain is `drain_until_idle()`, and it is unbounded by contract (MSG-09).
  Do not introduce either as a synonym, and do not give the drain a turn or time cap.
- A transaction id, a correlation, or a payload field is ever authority.
- That a request queued after an answer is dispatched after the work that answer's sender set in
  motion. FIFO orders envelopes as they are queued, and a consumer's follow-ups are queued as they
  happen; the bus's own record of what one send set in motion is a fence
  (`Switchboard::send_as_fenced`, `docs/reference/messaging.md`), and across a link, `settle`.
- `Committed` at commit-call time — commit *schedules*; `AdmissionPending` is real (PR-07).
- Prepared replacement preserves incumbent state — it does not (PR-09).
- `send_to_role` says anything about the sender's office (MSG-04) — office speech is its own
  explicit, verified act (`mail.as_role(...)`, MSG-07), and merely holding a role attaches nothing.
- A committed activation is answerable (LIFE-05).
- A reachable bridge socket is admitted — it is not; a connection has no proxy and acts on
  nothing until the host's `BridgeAdmission` policy answers its Hello with a grant, a refusal or a
  deferred decision. The operator policy admits everything that reaches it, and no policy adds
  transport security (`docs/reference/bridge.md`).
- A grant bounds what in-process native code can *touch* — it bounds speech only; a `dlopen`ed
  weave shares this address space (`docs/guides/dynamic-weaves.md`).
- That the Weaver acts for the session it governs. It changes authority and performs nothing: the
  session retries its own action, and the target sees the **session** as `mail.sender()`. Nor
  does a Weaver's death revoke what it installed — a grant is not a lease
  (`docs/reference/weaver.md`).
- That a `TerminalSession` is powerful because it is a terminal. It is an ordinary weave: no
  `Switchboard&`, no tap, no registry read, no `allow_any`, and a vocabulary its host supplied
  rather than discovered. Its transcript says **SUBMITTED**, never "delivered"; an authenticated
  later dispatch-refusal record explains a refused addressed send. Absence proves nothing, and
  `loom::ConsoleEngine` remains a separate, deliberately trusted host and debug lens that *can*
  say delivered (`docs/reference/terminal.md`).
- That grants are mutable (GATE-05). A subject's *delegated* message authority can be replaced
  live, by a holder of a host-minted `GrantAuthority`, within that capability's ceiling; its
  admission **baseline** never changes, and its OS, filesystem and resource containment was
  consumed into kernel state before the child ran — `LiveAuthority` has no word for any of it.
- That `Emit<...>` is informational, or that declaring a shape anywhere grants anything. Every
  list a weave declares — accepted, claimed, emitted, persisted — and every component those shapes
  nest is claimed through one agreement wall at registration (natively) and at load
  (in the manifest, since ABI v9), so a divergent definition refuses at the door in either order and a
  self-contradicting declaration never registers; and none of it is authority — a declared
  emitter with no send rule is still `CapabilityDenied`, and `Emit` is not an exhaustive send list
  (`docs/decisions/declared-vocabulary-is-agreed-at-admission.md`).
- Green means correct — a regression test pins an assertion under its arranged conditions, not
  the fitness of the whole architecture. Check the population too.
- A discrepancy means prose is automatically wrong. Code and tests establish observed behavior;
  intended contracts still require judgment. Follow
  [intent, evidence, and architectural fit](#intent-evidence-and-architectural-fit).
