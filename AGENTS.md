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
- **One recurring red has a documented environment exception (BL-VER-07)**: on a
  WSL2 host in mirrored networking mode, the `isolation` suite's *granted-network
  positive control* fails because a loopback `connect()` to a closed port is
  black-holed there (`ETIMEDOUT` after ~2 min) instead of refused. The lane stays
  **RED** — it is not skipped, xfailed or excused, and the enforcement populations
  still execute (17/17, 11/11). Before citing it, check every line of the
  signature in
  [capabilities: a known environment exception](docs/reference/capabilities.md#a-known-environment-exception-the-granted-network-positive-control);
  if any differs, it is a new failure. An unexpected **green** there is a trigger too.
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
- **OS-enforcement tallies are per suite and EXACT**: `isolation == 17`,
  `policy == 11`, the same in a dedicated run and in `all`. Those two numbers
  are owned by the `ZEN_ENFORCEMENT_POPULATION(...)` call at the end of
  `tests/test_isolation.cpp` and `tests/test_policy.cpp` — read them there
  rather than from prose. A new enforcement proof means updating the expected
  number on purpose. A suite that includes `tests/enforcement_gate.hpp` must
  `#define ZEN_ENFORCEMENT_DOMAIN` first.
- **`ZEN_ALLOW_UNENFORCEABLE=1` / `ZEN_REQUIRE_ENFORCEMENT=0` prove nothing about
  containment.** They convert missing enforcement into marked-degraded skips so a
  host that cannot enforce can still run the rest; the coverage case then prints
  `*** NON-ENFORCEMENT MODE ***` and asserts no population, and the official lane
  refuses to start. Never quote such a run as enforcement evidence.
- **Declared absence is not a pass.** The default Windows build has 22 suites to
  Linux's 28 because the `kernel` and `posix` gates are off; the `population`
  check prints those as `DECLARED ABSENT`. Do not compare the two totals as
  though they should match, and do not read an absent suite as a passing one.
- **The same doctrine covers built artifacts, not only tests** (POP-05). The
  `weave_population` entry pins which artifacts must carry the reloadable-weave
  build contract, derived from the build graph rather than from the roll of
  targets that took it — so an artifact that stops taking it is named instead of
  silently leaving. Exclusions are written on the target with a mandatory reason.
- **Populations are not interchangeable**: cases, suites, OS-enforcement proofs,
  assertions. Assertion totals are reported, never an acceptance oracle.

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
