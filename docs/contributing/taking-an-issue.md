# Taking an issue

From an issue labelled `ready` to a pull request ready to merge, for a contributor whose only
source is a clone of this repository. Each step says what to do and routes to the page that owns
the detail. [Best practices](best-practices.md) says what good work looks like along the way. The
commands use GitHub's command-line client, `gh`, signed in and run inside the clone; the GitHub
web pages do the same.

## 1. Choose a `ready` issue

Bounded work is an issue written as a small brief. Only the `ready` label means the work is
verified, bounded and open to take; `gh label list` says what every other label means.

```sh
gh issue list --label ready
gh issue view <n> --comments
gh issue view <n> --json closedByPullRequestsReferences     # a pull request that already fixes it
```

A `ready` issue has four parts, and each one binds:

- **What's wrong**: the defect, and how it was found.
- **Done when**: the outcome and the evidence that shows it. The pull request answers each line.
- **Fences**: what the change must not do or reach. They bind as firmly as the outcome. Work
  beyond them belongs to another issue, raised in a comment, never to a wider change.
- **Checked against**: the commit the issue was checked on.

One issue is one branch and one pull request. Terms for contributions from outside the project
are in [CONTRIBUTING.md](../../CONTRIBUTING.md#code-contributions).

## 2. Start from current `main`

```sh
git fetch origin
git switch -c <area>/<what> origin/main
```

Reproduce the issue on that commit before changing anything: *checked against* may name an older
one. If it no longer reproduces, nothing needs repairing; say so on the issue, naming the commit
you checked, and stop. If it rests on work that has not merged, say which, and build on it only
as the issue says. Name the branch for its area and its outcome, as `weave/optional-shape-fields`
or `docs/state-the-present`.

Without push access to this repository, fork it with `gh repo fork --remote`, which makes the
fork `origin` and this repository `upstream`. Then read `origin/main` below as `upstream/main`,
fetched with `git fetch upstream`, and give the change-kind check and the documentation lane
`-DZEN_BASE=$(git merge-base HEAD upstream/main)`. The pull request still targets this
repository's `main`, and a first pull request from a fork runs its checks once a maintainer
approves them.

## 3. Read what owns the subsystem

[AGENTS.md](../../AGENTS.md) holds the rules a change keeps and the traps to avoid, and
[CONTEXT.md](../CONTEXT.md) routes each topic to its reference page, its laws and its tests.
Read those, and the pages the issue links, before designing the change;
[CONTRIBUTING.md](../../CONTRIBUTING.md#getting-a-loom-you-can-work-with) asks that you have run
Loom first. Normative truth is `docs/reference/` and `docs/laws/`. Where the issue and a law
disagree, trace both and say in the pull request which is wrong, rather than bending one to fit
the other ([intent, evidence, and architectural fit](../../AGENTS.md#intent-evidence-and-architectural-fit)).

## 4. The environment

[The tools you need](../guides/tools.md) says which compilers, CMake and build tools Loom builds
with, and where to get them; Linux is the reference platform, and Windows has
[a route of its own](../guides/tools.md#the-windows-route). Beyond those:

- **A CMake new enough for the whole lane**, which [build and test](../../AGENTS.md#build-and-test)
  names; the library alone configures with an older one.
- **`nm`**, which the weave-contract check and the package witness refuse to run without
  ([optional, and what each one buys you](../guides/tools.md#optional-and-what-each-one-buys-you)).
- **On Linux, a delegated cgroup scope** for the `isolation`, `policy` and `all` entries, which
  CTest runs through `tests/run-under-scope.sh`. A machine has one when
  `systemd-run --user --scope -p Delegate=yes true` succeeds. Without one, their OS-enforcement
  cases fail by design and the lane cannot pass there: report that run as red, for that reason,
  and read those entries in the hosted run. A host that restricts unprivileged user namespaces, as
  a stock Ubuntu 24.04 does, fails them too; the hosted run's own setting for that is in
  `.github/workflows/ci.yml`.
- **Optionally, Python**, at the version [the tools page](../guides/tools.md#optional-and-what-each-one-buys-you)
  names. With the session tools Loom builds by default, it opens the `python` gate; without it the
  session journeys are declared absent, not passed.

Loom fetches nothing at configure. A change to documentation alone needs only CMake and Git.

## 5. Build

The commands, and the rules a build keeps, are in [AGENTS.md](../../AGENTS.md#build-and-test).
Configure a tree once; after each edit, `cmake --build build` rebuilds what changed. Suites are
read from their sources at configure, so a new or renamed `TEST_SUITE` reaches CTest when the
tree is configured again ([`tests/CMakeLists.txt`](../../tests/CMakeLists.txt)).

## 6. A test that fails before the change

The case comes first, and it fails on the unchanged code for the reason the issue gives.

1. **Put it with its subject's cases**: [CONTEXT.md](../CONTEXT.md) names the tests for each
   topic. A new test source joins its gate's list in `tests/CMakeLists.txt` and declares its
   `TEST_SUITE`, and a new suite or CTest entry is a line in a population manifest
   ([populations and tests](best-practices.md#populations-and-tests)).
2. **Run it alone.** Every suite is in one binary:

   ```sh
   build/tests/zen-tests --test-suite=<suite> --list-test-cases --test-case='<pattern>'
   build/tests/zen-tests --test-suite=<suite> --test-case='<pattern>'
   ```

   A comma splits a filter in two, so list what a pattern selects before trusting a run, and a
   run that selects nothing exits 70. By hand, the `isolation` and `policy` suites run inside
   `tests/run-under-scope.sh`. The lane runs one entry with `-DZEN_SELECT='^<entry>$'`, and says
   it ran a subset, which is never the delivery green.
3. **Keep what the red run printed, and the commit it ran on**: the pull request quotes both.
4. **Make the change**, and run the same case again.

A case asserts what *done when* requires, not what the code prints today: a test can pin an
incorrect expectation ([laws](../laws/README.md)). And it goes red on the defect, never for a
reason that would let a simpler case pass
([source comments](../../CONTRIBUTING.md#source-comments)). For an issue about a page, the red is
the page as it reads before the change, quoted, or a text check that fails on it.

## 7. The change

The smallest change that meets *done when* inside the fences. Everything it writes meets
[the standard for comments and documents](../../CONTRIBUTING.md#comments-and-documents), and
[best practices](best-practices.md) routes each of the repository's other standards to its owner.

## 8. The checks whose green counts

```sh
cmake -P tests/check_change_kind.cmake
```

says each changed file's kind against `origin/main`, and whether the change is
documentation-only. It reads the working tree and its untracked files too, so a pull request's
body file, an install prefix or a work directory belongs outside the clone, or under `build-*/`,
which Git ignores. If the change is documentation-only, the documentation lane is its
verification:

```sh
cmake -P tests/documentation_lane.cmake
```

Otherwise the official lane is its verification, and [AGENTS.md](../../AGENTS.md#build-and-test)
holds its commands. The sanitizer lane, the same build with `-DZEN_SANITIZE=ON`, is among the
checks the merge waits for, so a change it could fail runs it too. A change to what the installed
package carries, an installed header, an exported target or the package's CMake files, runs the
package witness as well, which no hosted Linux job runs:

```sh
cmake -DZEN_BUILD_DIR=build -P tests/verify.cmake
cmake --install build --prefix <prefix>
cmake -DZEN_PREFIX=<prefix> -DZEN_WORK=<dir> -P tests/package/run.cmake
```

Then, before the push:

```sh
cmake -P tests/check_commit_attribution.cmake
git diff origin/main...HEAD
```

Read the whole diff, every file, as review will read it. What that read finds is fixed and
verified again before the push. A green you report quotes its lane, never a bare `ctest`
([the population contract](../../AGENTS.md#the-population-contract--what-a-green-run-means-pop-0105)),
and a Windows green names its compiler ([build and test](../../AGENTS.md#build-and-test)).

## 9. The pull request

```sh
git push -u origin <area>/<what>
gh pr create --base main --title "<what the change does>" --body-file <file outside the clone>
```

- **The title** says in one sentence what the change does or makes true, such as "A `ZEN_SHAPE`
  member of type `std::optional<T>` is an optional field of `T`'s type". The merge makes it the
  subject of a commit on `main`.
- **The body** says what was wrong and what changed, in a few sentences. Then the evidence: the
  case, the commit it was red on and what it printed; the lanes that passed, each with its
  configuration and compiler; what did not run, and why. Then screenshots of a visible result, or
  the sentence that screenshots do not apply. Last, on a line of its own, `Fixes #<n>`, which
  closes the issue when the pull request merges.
- **Each commit is one coherent step**, its subject a sentence saying what it does or makes true,
  as the title does. The merge keeps every commit on `main`.
- **No assistant credit** in a commit or in the body, in any form: not an assistant as a
  co-author, and not a line crediting one. The body becomes the merge commit's message, which
  [the attribution guard](../../tests/check_commit_attribution.cmake) reads on `main`.
- **You do not merge.** Leave the pull request open once it is ready.

## 10. Reading the hosted run

CI runs on every pull request, never on a branch alone, and again on `main` after the merge.
Read the run job by job until every job has finished:

```sh
gh pr checks <n> --watch            # until none is pending; fails if any check failed, advisory too
gh pr checks <n> --required         # the checks the merge waits for; exits 8 while one is pending
gh run list --branch <area>/<what> --limit 1
gh run view <run-id>                # the jobs, each with its conclusion
gh run view <run-id> --log-failed   # a failed step's log, once the whole run has finished
gh api --allow-escape-sequences repos/Krealsion/Loom/actions/jobs/<job-id>/logs   # one job's log before then
```

A check's link ends in its job's id.

- **The change's kind decides what runs.** A documentation-only change runs the attribution,
  change-kind and documentation jobs, and its build jobs show as skipped.
- **Zengine's current `main` is built against a compiled change**: the job
  `Loom -> canonical Zengine stranger` installs this Loom and runs Zengine's own lane against it,
  and takes longest. Its red fails the run, and the pull request says what the change leaves its
  consumers ([intent, evidence, and architectural fit](../../AGENTS.md#intent-evidence-and-architectural-fit)).
- **The three Windows jobs are advisory**, as their names say: their red does not fail the run,
  and it is still reported.
- **The merge waits only for the checks `gh pr checks <n> --required` lists.**
- **A new push cancels the run in progress.** Push once, and read that run to its end before the
  next push.
- **A red is diagnosed before the next push**: name the job, the step and the case, reproduce it
  where the platform allows, and fix it on the branch.

The pull request is ready to merge when every required check has passed on its last commit and
the body's evidence names that commit.

## 11. Review, and `main` moving

A finding from review is fixed on the branch, verified as in step 8, and pushed once. When `main`
moves under an open pull request, for instance because another issue's pull request merged
first, rebase onto it, run the lane again, and update the body's evidence to the new tip:

```sh
git fetch origin
git rebase origin/main
git push --force-with-lease
```
