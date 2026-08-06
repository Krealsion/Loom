# Verification-population laws (POP)

These are laws about **what a green result means**, not about what the runtime does.
Every other namespace in this garden constrains the Loom; this one constrains the
harness that claims to have checked it — the project's own ethos ("never claim an
enforcement, or a proof, you did not impose") turned on its own test suite.

They exist because external-audit findings kept landing on the same sentence from
different directions: a named suite could pass having executed nothing (COLD-1 F-2),
the counter designed to catch exactly that could be satisfied by a *different* suite
(F-24), a consumer repo could delete its whole test tree at configure time and
still print "100% tests passed" (F-25), and — one layer down, over built artifacts
rather than test cases — twenty-three loadable weaves could silently leave the
reloadable-weave build contract with every lane green (COLD-2 C-3, POP-05).

The one law under all of them:

> **A green Zen verification result names a population that actually ran. Missing
> tests are absence of evidence, never successful evidence.**

Harness: [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt) ·
[`tests/doctest_main.cpp`](../../tests/doctest_main.cpp) ·
[`tests/suite_population.txt`](../../tests/suite_population.txt) ·
[`tests/check_population.cmake`](../../tests/check_population.cmake) ·
[`tests/verify.cmake`](../../tests/verify.cmake) ·
[`tests/enforcement_gate.hpp`](../../tests/enforcement_gate.hpp) ·
[`tests/weave_population.cmake`](../../tests/weave_population.cmake) ·
[`tests/check_weave_population.cmake`](../../tests/check_weave_population.cmake).
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

## POP-05 — A population that must carry a build contract is declared apart from what took it

LAW — Where the project requires an artifact to be built under a contract, *which artifacts
are required* is established independently of *which artifacts opted in*. A green result
means the two agree; an artifact that quietly leaves the contract is named, not missed.

This is POP-01's reasoning applied one layer down, to a population of **built artifacts**
rather than of test cases. It is here because the same sentence failed again from a new
direction: COLD-2 finding C-3 removed one line from the fixture factory, twenty-three
loadable weaves left the reloadable-weave contract roll, thirteen `STB_GNU_UNIQUE` symbols
returned to `libzen_test_weave.so`, and the whole official lane stayed green — because the
only check over them iterated the roll, and a derived list cannot detect its own absences.

MEANS
- the requirement comes from the **build graph**: `tests/weave_population.cmake` classifies
  every `SHARED`/`MODULE` library declared in `tests/` as a loadable weave that owes the
  contract, and derives the `STATIC`/`OBJECT` libraries inside their link closures, because
  the contract covers a compilation and not a file ([KERN-05](kernel-laws.md));
- nothing on that side reads `LOOM_WEAVE_CONTRACT_TARGETS` or the verdict property, so the
  edit that empties the roll leaves the expectation intact — which is the entire mechanism;
- the `weave_population` CTest entry compares them and fails **both** ways, with the artifact
  named: *required but not contracted* is a live reload-safety hazard, *contracted but not
  declared required* is drift between two concepts that are supposed to describe one thing;
- an exclusion is a **written** position: `zen_weave_contract_exempt(<target> <reason>)`
  records it on the target itself (so it cannot outlive the artifact), the reason is
  mandatory and printed on every run, and an exempt target appearing *on* the roll is itself
  a failure. Today there is exactly one, the F-22 negative control;
- zero is not a pass on either side, and the sweep cannot go blind quietly: the contract's
  own applied/bypass pair is a fixed canary the gate checks in both directions, so a
  classifier that stops recognising weaves is a configure-time refusal rather than two
  silently deregistered CTest entries.

DOES NOT MEAN
- that a third-party build system is required to use Loom's contract, or is enumerated by
  any of this. KERN-05 defines a correct path and is not a cage; Loom's required population
  is Loom's own tree, and none of this metadata is installed with the package;
- that the population check replaces the artifact check. `weave_contract` still reads the
  built files' symbol tables, because a target can be required, present on the roll, and
  still compiled wrong;
- that "every shared library is a loadable weave" holds anywhere else. It is a measured
  claim about `tests/`, and a future non-weave shared library there does not slip past — it
  lands in the required set and forces a decision in writing;
- that the kernel unload/reload cases catch this. They do not and never could: they exercise
  the loader's bookkeeping, which is correct either way, while whether an image's statics
  actually died is a build-artifact fact no in-process case observes.

PROVEN BY — `tests/weave_population.cmake` (the sweep + the exemption declaration);
`tests/check_weave_population.cmake` and the `weave_population` CTest entry; the gate and its
canary in `tests/CMakeLists.txt`. Measured by re-running COLD-2's M6 against it: the roll
drops 42 → 17, the requirement stays 42, thirteen unique symbols return, and the lane fails
naming all twenty-five artifacts — while removing the contract from a *single* target names
exactly that one.
