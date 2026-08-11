# AGENTS.md — Loom

Machine context router: **`docs/CONTEXT.md`** (topics → reference → laws →
tests). Human docs: `docs/README.md`. Normative truth is `docs/reference/` +
`docs/laws/` only; `docs/history/` is frozen record; `docs/evidence/` is not
the API contract.

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
  gate (`portable`/`kernel`/`posix`/`sdl`), plus a per-suite case FLOOR set at the
  measured baseline. Adding cases is free; deleting one crosses its floor. Adding
  or renaming a *suite* needs a line here — deliberately. Enforced by the
  `population` CTest entry (query-mode only; it runs no cases).
- **`tests/entry_population.txt`** is the same idea one layer out, for the CTest
  entries that are **not** suites — `population`, the two empty-population
  witnesses, `weave_contract`, `weave_population`, `doc_links`, `all`. Expected
  set = the suites declared for the active gates **∪** these; compared to
  `ctest -N` by name, both directions (declared-and-missing is a red, and so is
  registered-and-undeclared). Same four gates, one taxonomy. Adding a CTest entry
  of any kind is a line in one of the two files, and which file says what kind of
  thing it is.
- **The entry inventory runs in `tests/verify.cmake`, not as a CTest entry**, and
  it cannot be: an entry that has been deleted cannot complain about its own
  deletion. It sits on the lane's critical path — it owns the count and the
  zero-refusal — so removing the call leaves a lane with no answer rather than a
  lane that quietly runs less. The other half of the lock is that the
  `population` entry reads the lane and fails if it has stopped calling the
  inventory. Neither can be deleted alone. Two coordinated deletions still
  escape; a file cannot be defended against an edit to itself.
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

- A transaction id, a correlation, or a payload field is ever authority.
- `Committed` at commit-call time — commit *schedules*; `AdmissionPending` is
  real (PR-07).
- Prepared replacement preserves incumbent state — it does not (PR-09).
- `send_to_role` says anything about the sender's office (MSG-04) — office
  speech is its own explicit, verified act (`mail.as_role(...)`, MSG-07), and
  merely holding a role attaches nothing.
- A committed activation is answerable (LIFE-05).
- A reachable bridge socket is authenticated — it is not; every connection gets
  a full operator grant (`docs/reference/bridge.md`).
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
  **SUBMITTED**, never "delivered" — an ordinary sender is not told its send's
  fate — and `loom::ConsoleEngine` remains a separate, deliberately trusted
  host/debug lens that *can* say delivered (`docs/reference/terminal.md`).
- That grants are mutable (GATE-05). A subject's *delegated* message authority
  can be replaced live, by a holder of a host-minted `GrantAuthority`, within
  that capability's ceiling; its admission **baseline** never changes, and its
  OS/filesystem/resource containment was consumed into kernel state before the
  child ran — `LiveAuthority` has no word for any of it.
- Green means correct — "proven" means a regression test asserts it; check
  case *counts*, not just pass/fail.
- Docs claims over source: when they disagree, current source + tests win;
  record the contradiction rather than silently fixing prose.
