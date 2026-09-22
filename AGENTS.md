# AGENTS.md — Loom

Machine context router: **`docs/CONTEXT.md`** (topics → reference → laws →
tests). Human docs: `docs/README.md`. Normative truth is `docs/reference/` +
`docs/laws/` only; `docs/history/` is frozen record; `docs/evidence/` is not
the API contract.

## The supplied host, and who decides what a loaded artifact may do

`loom-host` (`src/host/`) is the one executable this project INSTALLS: a boot walk
from a file the person wrote, an operator console with authority in it, and two
per-install files (`loom-boot.json`, `loom-authority.json`). It is a replaceable
default assembled from ordinary participants — `ConsoleEngine`, `WeaveManager`,
`ControlWeave`, plus `loom::host::HostWarden` — and it holds no privilege the
package does not export. Human route: `docs/guides/running-loom.md`; prerequisites
`docs/guides/tools.md`.

**Three rules the host is built around, each replacing a defect a green lane could
not see.** (1) An ANSWER is something Loom attributed — the correlation the console
minted plus the bus-stamped sender (`loom::AskBook`) — never the newest entry in the
reply window, which is a value any admitted artifact can author under the ordinary
poke-answer baseline. (2) A loaded artifact is ADOPTED INSIDE THE LOAD, through
`loom::LifecycleAdoption` (`zen/kernel/control.hpp`): the one window between an
incarnation being committed and being told it is live. Because it lives in the door,
the boot walk, `start`, `reload` and an ordinary `zen.LoadWeave` all produce the same
governed participant, and no host call site adopts anything. (3) The loop is
`pump_pending()` plus a line read with a deadline (`src/host/line_input.hpp`), never
`drain_until_idle()` — one approved self-addressed message used to make `stop` and
`quit` unreadable forever. The deadline holds while a line is HALF TYPED: a Windows
console cannot be asked whether a line is finished, so it is read on its own thread.
What the reader holds and refuses is ONE rule on every platform (`loom::host::HeldInput`):
at most 64 KiB waiting — it waits for the host rather than reading a stream into memory —
and a line over 4000 bytes refused whole, never run shortened or split
(`docs/reference/bounds.md`). A
conversation that has not settled is reported PENDING and stays open; the host never
invents a completion. The console holds a conversation only for a caller that asks
(`ConsoleTracking::Tracked`), until that caller takes or forgets it.

**One host owns one decision store while it runs** (`src/host/store_lock.hpp`): the
store is written whole, so a second writer restores what the first one revoked. A
second host naming the same file exits **4** and says how to proceed. Exit codes: 0 ok,
2 bad command line, 3 a file would not parse, 4 store (or, serving, session directory) in use,
5 a session could not be opened.

**Building a capability on a session for some other application to drive** (a guest door, an
external-host tool package): `docs/guides/session-tools-map.md` routes a reader's actual
question — start/return, find a capability, act on the target, read a result, recover, extend
the harness — to the section of `docs/guides/sessions.md` that already answers it, so a
contributor writing that application's own companion entry knows what belongs there (that
target's admission and guest shapes) and what does not (the session and tool mechanics this
repository already documents once).

**`--serve <dir>` keeps the host alive for clients** (`docs/guides/sessions.md`): closed stdin
closes only the console. One loopback door (`src/host/session_door.hpp`) admits the owner's key
as a client with exactly the session/runs/history vocabularies, and a run manager's one-time
worker credential (only its SHA-256 crosses the bus) with no more than that manager's OWN
approved authority, whole or refused. Every session is compat-encoded. The scoped reader
(`history_reader.hpp`) reads the Recorder and is blacklisted from it. The run manager `loom-runs`
(`src/runs/`) is an ordinary loadable weave — task policy, not host root — with its Python
runtime beside it. **Every delivery is gated, answers included**: host wiring grants its own
answer shapes; the operator grants a loaded manager its typed answers. Python is optional:
gate `runs` carries the C++ `runs` suite, gate `python` the process journey `session_journey`
(DECLARED ABSENT without an interpreter).

**Five CTest entries drive the real thing**, because none of the above is visible to a
test of the deciding half. Two feed the host through a FILE (`tests/host_process/run.cmake`:
identities, counts and effective authority, never whole sentences, a canary first):
`host_console` (portable — recovery route, two real hosts on one store, repeated approval, a
failed write that must change nothing) and `host_process_weaves` (kernel gate —
attribution, activation, adoption by every route, responsiveness, a pin that cannot be
written, every late answer collected). Two TYPE into a real terminal — a hidden native
console on Windows, a pseudo-terminal on POSIX (`tests/host_terminal/witness.cpp`):
`host_line_input` (portable) and `host_terminal_weaves` (kernel). One feeds the reader a
PIPE and a FILE with a producer held back (`host_line_streams`, portable): the input limits,
asserted identically on every platform. **Redirected stdin is not evidence about a
console**: the Windows reader blocked on a half-typed line under a green file-fed lane.

**`Kernel::load` no longer mints a grant.** The three-argument overload used to
bind `Grant{}.allow_any()` to every library it opened, through three doors (direct
load, the message-driven control door, `load_candidate`). Now a `Kernel` asks its
installed `AdmissionPolicy` (`zen/kernel/admission.hpp`), and a Kernel on which
nobody called `admit_with(...)` **admits nothing and says so**. A host that wants
the old authority asks for it by name — `trust_every_artifact("why")` — and the
`why` rides every verdict. The four-argument `load(..., Grant)` still bypasses the
policy, deliberately: naming the grant at the call site *is* the decision. Which BUILD it
is (`AdmissionRequest::build`) is read from the file only when a policy asks, once per
operation — never eagerly: hashing every image for a policy that never looked failed
Zengine's `timer` suite. Count the work (`file_content_id_scans()`), not the clock.
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

## Build / test (canonical: WSL Ubuntu, GCC ≥ 11.4, cmake ≥ 3.22)

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"
cmake -DZEN_BUILD_DIR=build -P tests/verify.cmake     # THE official lane
# sanitizer lane: same with -B build-san -DZEN_SANITIZE=ON
```

- `-Werror` is on; C++20; avoid GCC-12+-only features.
- **Every loadable weave target goes through `loom_weave_build_contract()`**
  (`cmake/loom-weave.cmake`, KERN-05) — including the fixtures in `tests/`, and
  including `loom`/`zen-switchboard` themselves, since their objects land inside
  the image. Never hand-write the compiler option; the `weave_contract` entry
  reads the built artifacts and will say so. Nor is it on your memory: the
  `weave_population` entry derives which artifacts *must* carry it from the build
  graph — every `SHARED`/`MODULE` library in `tests/` plus the static libraries in
  their link closures — and names any that left the roll (POP-05). A deliberate
  exception is `zen_weave_contract_exempt(<target> <reason>)`, in writing, and it
  fails if that target then turns up contracted.
- The isolation/policy/all suites need a delegated cgroup scope: run the whole
  binary via `tests/run-under-scope.sh ./build/tests/zen-tests` — outside such a
  scope every OS-enforcement case fails **by design** (fail-hard beats silent
  skip), naming the capability it could not enforce.
- **BL-VER-07's environment exception is RETIRED (BL-VER-08)** — do not classify a
  network-case failure as it. The `isolation` suite's *granted-network positive
  control* used to connect to the closed port 1 and require `ECONNREFUSED`, which
  made the result depend on host closed-port behaviour; on a WSL2 mirrored-networking
  host that SYN is black-holed (`ETIMEDOUT` after ~2 min) and the lane was **RED**.
  The case now establishes its own listener on `127.0.0.1:0` and requires a
  successful connect plus a token byte, so no closed port is consulted anywhere. If
  it fails now, it is a **new** failure — see
  [capabilities: the granted-network positive control](docs/reference/capabilities.md#the-granted-network-positive-control-and-the-endpoint-it-uses).
- Windows builds the portable subset only, on **MinGW-w64 and MSVC** alike;
  kernel lanes are opt-in (`LOOM_ENABLE_WINDOWS_KERNEL`). MSVC needs
  `/Zc:preprocessor` for Loom's public `__VA_OPT__` macros — `loom::core`
  carries it as an INTERFACE option, so say which compiler a Windows green was
  proven on, never a bare "Windows".
- The installed package has its own witness, outside the build tree:
  `cmake -DZEN_PREFIX=<prefix> -DZEN_WORK=<dir> -P tests/package/run.cmake`.
  It is the only lane that can catch a requirement the package fails to carry,
  because it is the only one that reaches Loom solely through `find_package`.
- **Never change Loom's linker without the full lane.** `-fuse-ld=gold` links
  faster and segfaults the weave suites (`kernel`, `handoff`, `all`): an
  exception propagating out of a `dlopen`ed weave dies, because gold resolves
  the ELF vague-linkage behaviour the reloadable-weave contract (KERN-05,
  `cmake/loom-weave.cmake`) depends on; the same tree under the default linker
  is green. The rule is general: a build speedup of any kind — linker, launcher,
  unity build, flag — is not adopted until `tests/verify.cmake` has run whole
  under it, and a faster red is not a result.

## The population contract — what a green run means (POP-01..05)

`tests/verify.cmake` is the lane to quote. A bare `ctest` is fine while working,
but it accepts `-R <matches nothing>` as success (exit 0, "No tests were
found!!!"); the lane refuses that, passes `--no-tests=error`, and refuses to run
at all under the enforcement opt-out. Full laws: `docs/laws/population-laws.md`.

- **Zero is never a pass.** `zen-tests --test-suite=<no match>` exits 70 with
  `EMPTY TEST POPULATION`; a selector matching no CTest entry fails the lane.
- **Suite registration is derived** from the `TEST_SUITE(...)` declarations in
  the compiled sources — there is no hand-kept name list to drift.
- **`tests/suite_population.txt`** is the inventory contract: exact suite set per
  gate (`portable`/`kernel`/`posix`), plus a per-suite case FLOOR set at the
  measured baseline. Adding cases is free; deleting one crosses its floor. Adding
  or renaming a *suite* needs a line here — deliberately. Enforced by the
  `population` CTest entry (query-mode only; it runs no cases).
- **`tests/entry_population.txt`** is the same idea one layer out, for the CTest
  entries that are **not** suites — `population`, the two empty-population
  witnesses, `weave_contract`, `weave_population`, `doc_links`, `all`. Expected
  set = the suites declared for the active gates **∪** these; compared to
  `ctest -N` by name, both directions (declared-and-missing is a red, and so is
  registered-and-undeclared). The same gates, one taxonomy. Adding a CTest entry
  of any kind is a line in one of the two files, and which file says what kind of
  thing it is.
- **The entry inventory is taken TWICE, by two doors that do not lean on each
  other** — in `tests/verify.cmake` before the lane runs anything, and inside the
  `population` entry as part of its own work. One implementation
  (`tests/check_entry_population.cmake`), one authored expectation, two
  executions. It cannot be only an entry (a deleted entry cannot complain about
  its own deletion, so the lane's door is what notices a missing `population`),
  and it cannot be only the lane (a lane cannot check itself). So that neither
  door can go missing quietly, the lane's inventory leaves a **receipt** naming
  the run, the build, the gates and the two identity sets it compared, and the
  entry — which measures all of that independently — refuses a run that carries
  the lane's token and shows no matching receipt. One ordinary deletion on either
  side is red. Removing the lane's call *and* its announcement of itself escapes
  the doors' notice of each other, and still does not escape the entry's own
  inventory.
- **OS-enforcement tallies are per suite and EXACT, not floors.** `isolation`
  and `policy` each expect an exact count of executed enforcement proofs, the
  same in a dedicated run and in `all`. Exact cuts both ways deliberately: a
  missing witness fails, and so does an unannounced extra one, so adding an
  enforcement proof means editing the expected number on purpose. Each number is
  owned by the `ZEN_ENFORCEMENT_POPULATION(...)` call at the end of
  `tests/test_isolation.cpp` and `tests/test_policy.cpp` — read the value there;
  it is deliberately not written down anywhere else. A suite that includes
  `tests/enforcement_gate.hpp` must `#define ZEN_ENFORCEMENT_DOMAIN` first.
- **`ZEN_ALLOW_UNENFORCEABLE=1` / `ZEN_REQUIRE_ENFORCEMENT=0` prove nothing about
  containment.** They convert missing enforcement into marked-degraded skips so a
  host that cannot enforce can still run the rest; the coverage case then prints
  `*** NON-ENFORCEMENT MODE ***` and asserts no population, and the official lane
  refuses to start. Never quote such a run as enforcement evidence.
- **Declared absence is not a pass.** The default Windows build carries fewer
  suites than Linux because the `kernel` and `posix` gates are off; the
  `population` check prints each of those as `DECLARED ABSENT`. Do not compare
  the two totals as though they should match, and do not read an absent suite as
  a passing one. Which suites ride which gate is in `tests/suite_population.txt`;
  a *count* of them is a consequence of that file and is not written down twice.
- **The same doctrine covers built artifacts, not only tests** (POP-05). The
  `weave_population` entry pins which artifacts must carry the reloadable-weave
  build contract, derived from the build graph rather than from the roll of
  targets that took it — so an artifact that stops taking it is named instead of
  silently leaving. Exclusions are written on the target with a mandatory reason.
- **Populations are not interchangeable**: cases, suites, OS-enforcement proofs,
  assertions. Assertion totals are reported, never an acceptance oracle.
- **Documentation references are checked too** (`doc_links`). Every relative link
  in a current-facing `*.md` and its `#anchor`, plus every repository-relative
  `docs/...md` path written in a first-party C/C++ comment under `include/`,
  `src/`, `tests/` or `examples/`, must resolve — a broken one is a RED in the
  official lane, not something an executor has to remember to look for. A
  comment's reference is resolved against the **repository root** (`see
  docs/reference/capabilities.md`), because a comment moves with its code.
  Excluded by written rule: `docs/history/` and `docs/audits/` (frozen, and they
  describe the tree they were written against), `archive/`, vendored trees, build
  trees. A reference above the repository root is counted and declined — a
  standalone clone has no sibling to look at. `tests/check_doc_links.cmake`.

## Testing Zengine against this Loom

`../Zengine`'s suites need a Loom that exports `loom::kernel` (always on Linux;
on Windows only under the opt-in `LOOM_ENABLE_WINDOWS_KERNEL`). Against a
kernel-less package its `tests/` **fails configuration** rather than silently
emptying itself; `-DBUILD_TESTING=OFF` is the supported library-only
configuration.

## Ownership

This repo owns substrate truth. `../Zengine` owns package truth (Timer etc.)
and consumes Loom as an installed package (`find_package(loom)`). Night Lab
(github.com/Krealsion/zen-night-lab) is read-only evidence.

## Do not assume

- That `Switchboard` has a `pump()` or a `run()`. Neither exists: both spelled
  drain-to-idle in words an ordinary C++ reader takes for the bounded turn, which
  is how a first-contact host chose the call that never returns. The ordinary
  host-loop operation is `pump_pending()`; the drain is `drain_until_idle()`, and
  it is unbounded by contract (MSG-09, FRIC-1). Do not reintroduce either as a
  synonym, and do not give the drain a turn or time cap.
- A transaction id, a correlation, or a payload field is ever authority.
- That a request queued after an answer is dispatched after the work that answer's sender set
  in motion. FIFO orders envelopes as they are QUEUED, and a consumer's follow-ups are queued as
  they happen; the bus's own record of what one send set in motion is a fence
  (`Switchboard::send_as_fenced`, `docs/reference/messaging.md`), and across a link, `settle`.
- `Committed` at commit-call time — commit *schedules*; `AdmissionPending` is
  real (PR-07).
- Prepared replacement preserves incumbent state — it does not (PR-09).
- `send_to_role` says anything about the sender's office (MSG-04) — office
  speech is its own explicit, verified act (`mail.as_role(...)`, MSG-07), and
  merely holding a role attaches nothing.
- A committed activation is answerable (LIFE-05).
- A reachable bridge socket is admitted — it is not; a connection has no proxy
  and acts on nothing until the host's `BridgeAdmission` policy answers its
  Hello with a grant, a refusal or a deferred decision. The operator policy
  admits everything that reaches it, and no policy adds transport security
  (`docs/reference/bridge.md`).
- A grant bounds what in-process native code can *touch* — it bounds speech
  only; a `dlopen`ed weave shares this address space
  (`docs/guides/dynamic-weaves.md`).
- That the Weaver acts for the session it governs. It changes authority and
  performs nothing: the session retries its own action, and the target sees the
  **session** as `mail.sender()`. Nor does a Weaver's death revoke what it
  installed — a grant is not a lease (`docs/reference/weaver.md`).
- That a `TerminalSession` is powerful because it is a terminal. It is an
  ordinary weave: no `Switchboard&`, no tap, no registry read, no `allow_any`,
  and a vocabulary its host supplied rather than discovered. Its transcript says
  **SUBMITTED**, never "delivered"; an authenticated later dispatch-refusal
  record explains a refused addressed send. Absence proves nothing, and
  `loom::ConsoleEngine` remains a separate, deliberately trusted
  host/debug lens that *can* say delivered (`docs/reference/terminal.md`).
- That grants are mutable (GATE-05). A subject's *delegated* message authority
  can be replaced live, by a holder of a host-minted `GrantAuthority`, within
  that capability's ceiling; its admission **baseline** never changes, and its
  OS/filesystem/resource containment was consumed into kernel state before the
  child ran — `LiveAuthority` has no word for any of it.
- That `Emit<...>` is informational, or that declaring a shape anywhere grants
  anything. Every list a weave declares — accepted, claimed, emitted, persisted —
  and every component those shapes nest is claimed through one agreement wall at
  registration (natively) and at load (`zen.Manifest` v5, ABI v9), so a divergent
  definition refuses at the door in either order and a self-contradicting
  declaration never registers; and none of it is authority — a declared emitter
  with no send rule is still `CapabilityDenied`, and `Emit` is not an exhaustive
  send list (`docs/decisions/declared-vocabulary-is-agreed-at-admission.md`).
- Green means correct — a regression test pins an assertion under its arranged
  conditions, not the fitness of the whole architecture. Check the population too.
- A discrepancy means prose is automatically wrong. Code and tests establish
  observed behavior; intended contracts still require judgment. Follow
  [intent, evidence, and architectural fit](#intent-evidence-and-architectural-fit).
