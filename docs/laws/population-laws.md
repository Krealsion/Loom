# Verification-population laws (POP)

These are laws about **what a green result means**, not about what the runtime does.
Every other namespace in this garden constrains the Loom; this one constrains the
harness that claims to have checked it — the project's own ethos ("never claim an
enforcement, or a proof, you did not impose") turned on its own test suite.

They exist because four external-audit findings landed on the same sentence from
different directions: a named suite could pass having executed nothing (COLD-1 F-2),
the counter designed to catch exactly that could be satisfied by a *different* suite
(F-24), and a consumer repo could delete its whole test tree at configure time and
still print "100% tests passed" (F-25).

The one law under all three:

> **A green Zen verification result names a population that actually ran. Missing
> tests are absence of evidence, never successful evidence.**

Harness: [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt) ·
[`tests/doctest_main.cpp`](../../tests/doctest_main.cpp) ·
[`tests/suite_population.txt`](../../tests/suite_population.txt) ·
[`tests/check_population.cmake`](../../tests/check_population.cmake) ·
[`tests/verify.cmake`](../../tests/verify.cmake) ·
[`tests/enforcement_gate.hpp`](../../tests/enforcement_gate.hpp).
Running them: [`AGENTS.md`](../../AGENTS.md).

## POP-01 — A named verification target may not pass on an empty population

LAW — Invoking a target named for a test population has exactly two truthful
outcomes: the population executes at a size the project declared, or the invocation
fails loudly. Zero is a failure at every layer — zero doctest cases selected by a
suite filter, and zero CTest entries selected by a selector.

MEANS
- `zen-tests --test-suite=<anything that matches nothing>` exits **70** and prints
  `EMPTY TEST POPULATION`, so a renamed, deleted or `#if`-excluded suite fails its
  CTest entry instead of reporting `Passed 0.00 sec`;
- CTest suite registration is **derived** from the `TEST_SUITE(...)` declarations in
  the sources actually compiled, so there is no second list to drift away from them —
  a rename *moves* the registered test rather than orphaning it;
- `tests/suite_population.txt` pins the exact suite inventory per platform gate plus a
  per-suite case floor, and the `population` CTest entry enforces both, so a suite that
  vanished (and therefore registers nothing that could fail) is still caught;
- the official lane `tests/verify.cmake` refuses a selector that matched zero CTest
  entries, and passes `--no-tests=error` so CTest refuses it too.

DOES NOT MEAN
- that suite names can never change. A rename is fine; it must move the registration
  truthfully or fail the inventory contract — what is forbidden is the stale green
  phantom left behind;
- that every platform runs identical suites. Gates are real (see POP-03);
- that CTest's own global semantics changed. `ctest -R <nothing>` still exits 0 for
  anyone who runs it bare; the project-owned lane is what refuses it;
- that case totals measure test quality, or that assertion totals measure coverage.
  The four populations — cases, suites, OS-enforcement proofs, assertions — are
  reported separately and only the first three are ever an oracle.

PROVEN BY — `tests/doctest_main.cpp` (the run-census listener); CTest entries
`empty_population_refused` (nonzero exit) and `empty_population_says_so` (the
diagnostic — two entries because `PASS_REGULAR_EXPRESSION` makes CTest ignore the exit
code, so one test could pin either but not both); the `population` entry; the derived
`foreach` registration in `tests/CMakeLists.txt`.

## POP-02 — A coverage floor belongs to the population it counts

LAW — A count named for one suite is computed from that suite's own witnesses, and it
is **exact**. No suite's executions may satisfy another suite's proof, in any lane.

MEANS
- `zenh::enforced_case_count(domain)` and `degraded_run(domain)` are keyed by
  `ZEN_ENFORCEMENT_DOMAIN`, which each suite's translation unit must define before
  including the gate — there is no default, so a new enforcement suite is a compile
  error until it names its own population;
- the expected populations are `isolation == 15` and `policy == 11`, identical in a
  dedicated run and in the aggregate `all` lane;
- exact, not `>=`: a missing witness fails, and a *new* witness is a deliberate edit
  to the expected number.

DOES NOT MEAN
- that every population contract in the tree is exact. Suite CASE floors are minimums
  anchored to a measured baseline, because that population is meant to grow every
  phase; the enforcement populations are small, security-relevant and intentionally
  stable, which is what makes exactness the right price there. Two populations, two
  policies, each argued in `tests/suite_population.txt` and `enforcement_gate.hpp`;
- that the tally proves containment. It proves the containment proofs *ran*; the
  proofs themselves are what prove containment.

PROVEN BY — `tests/enforcement_gate.hpp` (`ZEN_REQUIRE_ENFORCEABLE`,
`ZEN_ENFORCEMENT_POPULATION`); the coverage cases at the end of suites `isolation` and
`policy`; observable in any verbose run as
`OS-enforcement cases executed for '<domain>': N of N expected`.

## POP-03 — Unsupported testability is declared, never disguised

LAW — When a configuration cannot host the tests it was asked to build, it fails at
configure time. A population that is absent by platform or by option is *reported as
absent*; it is never allowed to look like a population that ran.

MEANS
- Zengine's suites need a Loom exporting `loom::kernel`. With testing enabled and no
  kernel, configuration **fails** with a message saying what is missing, why the tests
  cannot be trusted without it, and how to get a Loom that can host weaves. It used to
  `return()` quietly, after which `ctest` printed "100% tests passed" over one
  surviving smoke test — reachable from the supported default Windows Loom package;
- the requirement belongs to the tests, not the package: `-DBUILD_TESTING=OFF`
  configures and builds against a kernel-less Loom, and with a kernel-full Loom it
  still builds every weave library and registers no tests;
- absences inside the Loom's own tree are declared in `suite_population.txt` by gate
  (`portable` / `kernel` / `posix` / `sdl`) and the `population` check prints them:
  `DECLARED ABSENT in this configuration (not run, and not passed)`.

DOES NOT MEAN
- that Linux-only suites should be compiled on Windows. The default Windows build
  legitimately has 22 suites where Linux has 28 — that is a declared absence, not a
  regression, and the two totals are never to be compared as though they should match;
- that every optional feature must be enabled everywhere;
- that Zengine itself requires a kernel. Only its suites do.

PROVEN BY — `Zengine/tests/CMakeLists.txt` (the `FATAL_ERROR`), `Zengine/CMakeLists.txt`
(`BUILD_TESTING`); `tests/check_population.cmake`'s gate handling, exercised for real by
the native Windows lane, where the `kernel` and `posix` gates are off.

## POP-04 — An opt-out run is not enforcement evidence

LAW — `ZEN_ALLOW_UNENFORCEABLE=1` (or `ZEN_REQUIRE_ENFORCEMENT=0`) buys the ability to
run on a host that genuinely cannot enforce. It buys nothing else. A run made under it
is NON-ENFORCEMENT MODE and may never be quoted as proof that containment was imposed.

MEANS
- the coverage case says so in words, in the run's own output:
  `*** NON-ENFORCEMENT MODE *** the OS-enforcement population for '<domain>' did NOT
  execute`, and asserts no population at all;
- the refusal is unconditional in that mode — it does not depend on whether the proofs
  happened to run anyway on a capable host, because the opt-out means the run *could*
  have skipped them;
- the official lane `tests/verify.cmake` refuses to start when either variable is set,
  so the lane whose job is to mint quotable evidence cannot mint it in that mode.

DOES NOT MEAN
- that the opt-out is deprecated, or that a run under it fails. It passes, loudly
  labelled;
- that a developer opt-out mode is ever security evidence;
- that a bare `ctest` **summary line** distinguishes the two. It does not, and cannot:
  CTest shows a passing test's output only under `-V` or on failure, and making the run
  fail would destroy the very portability the opt-out exists for. Every other surface
  says so — the run's own output, the assertion count, and the official lane's refusal to
  start — which is exactly why the lane, not a bare summary line, is what gets quoted.

PROVEN BY — `ZEN_ENFORCEMENT_POPULATION` in `tests/enforcement_gate.hpp`; the refusal in
`tests/verify.cmake`; observable by running `ZEN_ALLOW_UNENFORCEABLE=1 ctest -V -R policy`.
