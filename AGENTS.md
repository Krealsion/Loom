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
- The isolation/policy/all suites need a delegated cgroup scope: run the whole
  binary via `tests/run-under-scope.sh ./build/tests/zen-tests` — a bare run
  fails ~33 enforcement cases **by design** (fail-hard beats silent skip).
- Windows/MinGW builds the portable subset only; kernel lanes are opt-in.

## The population contract — what a green run means (POP-01..04)

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
- **OS-enforcement tallies are per suite and EXACT**: `isolation == 15`,
  `policy == 11`, the same in a dedicated run and in `all`. A new enforcement
  proof means updating the expected number on purpose. A suite that includes
  `tests/enforcement_gate.hpp` must `#define ZEN_ENFORCEMENT_DOMAIN` first.
- **`ZEN_ALLOW_UNENFORCEABLE=1` / `ZEN_REQUIRE_ENFORCEMENT=0` prove nothing about
  containment.** They convert missing enforcement into marked-degraded skips so a
  host that cannot enforce can still run the rest; the coverage case then prints
  `*** NON-ENFORCEMENT MODE ***` and asserts no population, and the official lane
  refuses to start. Never quote such a run as enforcement evidence.
- **Declared absence is not a pass.** The default Windows build has 22 suites to
  Linux's 28 because the `kernel` and `posix` gates are off; the `population`
  check prints those as `DECLARED ABSENT`. Do not compare the two totals as
  though they should match, and do not read an absent suite as a passing one.
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
- Green means correct — "proven" means a regression test asserts it; check
  case *counts*, not just pass/fail.
- Docs claims over source: when they disagree, current source + tests win;
  record the contradiction rather than silently fixing prose.
