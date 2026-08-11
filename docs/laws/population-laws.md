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
[`tests/entry_population.txt`](../../tests/entry_population.txt) ·
[`tests/check_entry_population.cmake`](../../tests/check_entry_population.cmake) ·
[`tests/verify.cmake`](../../tests/verify.cmake) ·
[`tests/enforcement_gate.hpp`](../../tests/enforcement_gate.hpp) ·
[`tests/weave_population.cmake`](../../tests/weave_population.cmake) ·
[`tests/check_weave_population.cmake`](../../tests/check_weave_population.cmake).
Running them: [`AGENTS.md`](../../AGENTS.md).

**These laws are Zen's, and Zen is two repositories.** POP-01, POP-02 and POP-03 bind the
Loom's harness *and* Zengine's; POP-05 is about the Loom's own test tree today. That was not
true when this file was written, and the gap was easy to miss precisely because the heading
says "what a green result means" and POP-03 already reached across — a reader could
reasonably conclude the rest did too. It did not: until C4, Zengine had stock doctest mains
(zero selected cases printed `Status: SUCCESS!` and exited 0), no inventory of its expected
CTest entries, and no case floors, so deleting a whole test case left `ctest` reporting 10 of
10 (COLD-2 C-4). Zengine now owns the equivalent mechanism, **as its own**:
`Zengine/tests/verify.cmake` · `Zengine/tests/check_population.cmake` ·
`Zengine/tests/test_population.txt` · `Zengine/tests/doctest_main.cpp`, with the registration
helpers in `Zengine/CMakeLists.txt`. It is a second implementation and not a shared one on
purpose — Zengine is consumed as a stranger against an *installed* Loom package, which ships
headers and libraries and no test metadata, so a population contract that needed the
substrate's source tree would be a contract Zengine does not own.

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
  entries, and passes `--no-tests=error` so CTest refuses it too;
- **the entries themselves are a declared population, not only the suites** (VOLATILE-2a).
  `tests/entry_population.txt` names the CTest entries that are not suites — the population
  checks, the empty-population witnesses, the weave-artifact checks, the documentation-link
  check, the aggregate runner — with the gate each rides; the expected set is that union
  with the gate-resolved suites, and the lane compares it to `ctest -N` **by name, both
  directions**. Until it existed, deleting one `add_test` left the lane registering one
  fewer entry, running everything that remained, and reporting green at the smaller number
  — including for the entries whose whole job is to notice absences;
- **in Zengine, the same four sentences with its own nouns** (C4): every runtime suite links
  `Zengine/tests/doctest_main.cpp`, so a filter matching nothing exits **70** saying `EMPTY
  TEST POPULATION`; `Zengine/tests/test_population.txt` pins the exact CTest-entry inventory
  per gate, plus a floor for each doctest surface and the diagnostic each compile-negative
  test must be judged on; and `Zengine/tests/verify.cmake` refuses a build tree registering
  zero entries, refuses an inventory that does not match, and re-proves the empty-population
  refusal on every doctest binary on every run before it trusts a single case count.

DOES NOT MEAN
- that suite names can never change. A rename is fine; it must move the registration
  truthfully or fail the inventory contract — what is forbidden is the stale green
  phantom left behind;
- that the two harnesses are one mechanism, or that either may lean on the other. They are
  deliberately independent implementations of the same law, in repositories that verify each
  other at arm's length. Neither repository's population contract may be satisfied by the
  other's evidence;
- that the population check may live inside the population. Both repositories' official
  lanes are scripts run *outside* CTest for exactly that reason: a check registered as a
  CTest entry stops asking its question the moment that entry is the thing deleted. The
  Loom's entry inventory is the sharpest case — it is what notices a deleted entry, so it
  could not be one — and it is placed on the lane's critical path, owning the count and the
  zero-refusal, so deleting the call leaves a lane with no answer rather than a smaller
  green;
- that a lane can defend itself against an edit to itself. It cannot, and the arrangement
  says so rather than implying otherwise: the entry inventory keeps the `population` entry
  registered, and the `population` entry requires the lane to still call the inventory, so
  neither goes alone — but two coordinated deletions still escape;
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
`foreach` registration in `tests/CMakeLists.txt`. In Zengine, by
`Zengine/tests/doctest_main.cpp` and the canary in
`Zengine/tests/check_population.cmake` (`zengine_assert_refuses_empty_population`, run
against every doctest surface on every verification); measured by C4's controls — a filter
matching nothing goes from 0-cases-exit-0 to exit 70, deleting one case from
`test_input.cpp` goes from a fully green run to `input selected N cases, below declared
floor M`, and deleting a whole CTest entry goes from a fully green run to a named `MISSING`.
The floors those controls crossed are in `Zengine/tests/test_population.txt`, which is where
they can be current; a run's own numbers belong to the report that measured it.

## POP-02 — A coverage floor belongs to the population it counts

LAW — A count named for one suite is computed from that suite's own witnesses, and it
is **exact**. No suite's executions may satisfy another suite's proof, in any lane.

MEANS
- `zenh::enforced_case_count(domain)` and `degraded_run(domain)` are keyed by
  `ZEN_ENFORCEMENT_DOMAIN`, which each suite's translation unit must define before
  including the gate — there is no default, so a new enforcement suite is a compile
  error until it names its own population;
- the `isolation` and `policy` suites each expect an exact number of executed
  OS-enforcement proofs, identical in a dedicated run and in the aggregate `all` lane, and
  each number is written at exactly one place — the `ZEN_ENFORCEMENT_POPULATION(...)` call
  closing that suite. Read the value there; no document holds a second copy of it, because
  a second copy is what goes stale while the enforced one moves;
- exact, not `>=`: **both directions cost something on purpose.** A missing witness fails,
  and so does an unannounced extra one — adding an OS-enforcement proof means editing the
  owning declaration, which is the correct price for a population that is small,
  security-relevant and intentionally stable. That is the opposite of the suite case floors
  below, and the difference is the point rather than an inconsistency;
- **in Zengine, the counting unit is the binary rather than the suite** (C4): its five
  runtime surfaces are five separate executables with no `TEST_SUITE` declarations, so each
  floor is read out of that surface's own `--count` and no other surface's growth can cover
  its loss. One aggregate floor over all of them was refused for exactly that reason — it
  would let a whole domain disappear behind another domain's additions. Its conditional
  subpopulations — the cases behind `#if defined(SURFACE_HAS_SDL)` — are their own manifest
  rows, so a surface reads one number where the SDL skin is built and a smaller one where it
  is not, with no slack in either. Both are in `Zengine/tests/test_population.txt`.

DOES NOT MEAN
- that every population contract in the tree is exact. Suite CASE floors are minimums
  anchored to a measured baseline, because that population is meant to grow every
  phase; the enforcement populations are small, security-relevant and intentionally
  stable, which is what makes exactness the right price there. Two populations, two
  policies, each argued in `tests/suite_population.txt` and `enforcement_gate.hpp`.
  Zengine draws the same line in the same place: its CTest-entry inventory is exact, its
  case floors are minimums;
- that a floor may be lowered to make a deletion green. A deliberate decrease in evidence is
  a reviewed edit to the manifest, and it should read like one;
- that a case floor is a coverage number, or that assertion totals are a population at all.
  Neither repository's assertion total is an oracle, and neither is written into a contract
  file: nothing enforces such a figure, so it travels dated and lane-named with the report
  that measured it, or not at all;
- that the tally proves containment. It proves the containment proofs *ran*; the
  proofs themselves are what prove containment.

PROVEN BY — `tests/enforcement_gate.hpp` (`ZEN_REQUIRE_ENFORCEABLE`,
`ZEN_ENFORCEMENT_POPULATION`); the coverage cases at the end of suites `isolation` and
`policy`; observable in any verbose run as
`OS-enforcement cases executed for '<domain>': N of N expected`. In Zengine, by
`Zengine/tests/test_population.txt` and the per-entry floor report the official lane prints
(`<entry>: doctest, <n> cases (floor <m> = <the rows whose gates are active>)`); measured by
C4's control B, where deleting one case from `test_input.cpp` fails `input` alone and names
it.

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
  legitimately carries fewer suites than Linux — that is a declared absence, not a
  regression, and the two totals are never to be compared as though they should match. The
  gate each suite rides is in `tests/suite_population.txt`; the totals are its consequence
  and are not restated here, because a restated total is the thing that goes stale;
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
