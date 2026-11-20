# Contributing

The guide for everyone who changes Loom, a person or an agent: one reader with one path. Read
[what any task needs](#what-any-task-needs) before any task; [the reference](#reference) after it
is read when a question needs it. Every page and comment is written to
[the documentation's design](docs/contributing/documentation-design.md), and
[the recommended setup](docs/contributing/recommended-setup.md) helps you choose a machine, a
clone and a harness.

[Zengine](https://github.com/Krealsion/Zengine) shares part of this guide and links it from its
own: [what a change is judged against](#what-a-change-is-judged-against),
[comments and documents](#comments-and-documents), [making a change](#making-a-change), and in the
reference [attribution](#attribution), [attacking the result](#attacking-the-result),
[what a boundary means](#what-a-boundary-means), [tools for the next contributor](#tools-for-the-next-contributor)
and [line width](#line-width). Those sections say nothing that is Loom's alone.

## What any task needs

### Getting a Loom you can work with

Before anything else: [the tools you need](docs/guides/tools.md) — a C++20 compiler and CMake,
which are real prerequisites and not assumed knowledge — and then
[from nothing to a running weave](docs/guides/running-loom.md), which installs Loom, starts the
supplied host and builds something against the installed package. Do that first even if you
intend to change Loom itself: the shortest way to understand what a change breaks is to have used
the thing it breaks.

### Where things live

[`docs/CONTEXT.md`](docs/CONTEXT.md) routes each topic to its reference page, its laws and its
tests, and [`docs/README.md`](docs/README.md) is the documentation's front page. What Loom
promises is `docs/reference/` and `docs/laws/` only; `docs/history/` is a frozen record, and
`docs/evidence/` is not the API contract. Loom's own words are in
[the terminology](docs/terminology.md). Read the owner of what your task touches before its code.

### What a change is judged against

The purpose a change was asked for, and the contracts already agreed, decide whether it is right.
Code, tests and what you observe show what it does now, under the conditions you examined. A
passing test does not settle what was intended, and it never justifies weakening a contract. When
they disagree, trace the requirement and the actual path, find whether the code, the test or the
prose is wrong, and correct that one within the change's scope; a change to an intended contract
goes to its owner.

Before you build and again at review, judge the whole result: does it serve the capability it is
for, keep ownership, authority and composition coherent, and leave the next piece of work and the
known consumers able to build on it? A local green does not excuse avoidable coupling, a fact said
twice, a special case, or a migration left for a later owner; revise the design, and make a
necessary tradeoff explicit for review. Build a piece for the uses the direction sets, whole and
with care: nothing waits for its absence to hurt, and nothing is argued down for having no
consumer yet. What the direction does not name is still a guess, so say what a piece is for
before building it. The required checks still have to pass.

### Build and test

Linux is the reference platform. GCC 11.4 or newer and CMake 3.22 or newer run the whole lane (the
library alone configures with CMake 3.16).

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"
cmake -DZEN_BUILD_DIR=build -P tests/verify.cmake     # the official lane
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
- **Say which green you proved**: this repository's, in which configuration, with which compiler.
  A Windows green names its compiler, never a bare "Windows".
- [More build rules](#more-build-rules) cover the weave contract, the cgroup scope the enforcement
  suites need, Windows, the package witness and the linker.

### What a green means

`tests/verify.cmake` is the lane to quote. A bare `ctest` is fine while working, but it accepts
`-R <matches nothing>` as success; the lane refuses that, and refuses to run at all under the
enforcement opt-out. Zero is never a pass. Adding a suite or a CTest entry of any kind is a line in
`tests/suite_population.txt` or `tests/entry_population.txt`, and deleting a case can cross a
suite's floor. Assertion totals are reported, never an acceptance oracle.
[The population contract](#the-population-contract--what-a-green-run-means-pop-0105) has the
detail.

### Traps

- `Switchboard` has no `pump()` and no `run()`. The host-loop operation is `pump_pending()`; the
  drain is `drain_until_idle()`, unbounded by contract (MSG-09), and it gets no turn or time cap.
- A transaction id, a correlation or a payload field is never authority.
- `Committed` at commit-call time only schedules; `AdmissionPending` is real (PR-07).
- Green means nothing complained: a regression test pins an assertion under its arranged
  conditions, not the fitness of the whole architecture. Check the population too.
- [More traps](#more-traps) are in the reference, each with its owner.

### Comments and documents

Everything here — a source comment, a page under `docs/`, this file — is written to
[the documentation's design](docs/contributing/documentation-design.md), for a reader who has
ordinary programming knowledge and this repository, and nothing else. It states the present fact.

- **Names first.** Names are verbs that say what happens, and parameters say what they hold; a
  renamed thing's files change with it. A comment that exists only because a name is poor is a
  sign the name should change.
- **A comment says why, what the code must not become, or what cannot be seen from its line**, in
  complete sentences a reader understands from the line and the code around it. When they cannot,
  the comment says more. A comment that restates the code goes. A label that only names a unit,
  an argument or a numbered step may stay a noun phrase. A rewritten comment is a claim about the
  code: check it against the code, branch by branch, before polishing its words.
- **A rule about one declaration** is written in that declaration's comment, and no page repeats
  it. A rule that other files rely on is a numbered law on its folder's law page, and a comment
  names it inside a sentence that says the rule ("the drain has no turn or time cap (MSG-09)"). A
  law is cited by the id [`docs/laws/`](docs/laws/README.md) declares; any other reason is said in
  words. A bare pointer, an id and a path alone, is not written anew.
- **Installed headers are documentation.** `include/zen/` is the installed API, read by strangers
  while they use it. A header says what a thing is and what it deliberately is not (so a reader
  does not infer a capability from an architecture), its contract, and what fires when that
  contract is broken: a guard whose trigger is misdescribed is worse than none.
- **Tests are witnesses.** A case's name says what it proves, in a sentence, one claim a name. Its
  comments say what its name and code cannot: why a setup, a bound, a repeat or an oracle has its
  shape, and what a mutation found that the case now guards, wherever a reader would otherwise
  simplify the case into one that passes for the wrong reason.
- **Nothing private.** No development-phase name, plan code or private id, and nothing about the
  maintainers' own machine, workspace, process, roles or reports.
- **No history.** What was removed, what a retired piece did, how the code came to be this way:
  that story is the commit's and the pull request's, and Git's. History kept as written belongs to
  [`docs/history/`](docs/history/README.md).
- **No path outside the repository** — not the workspace it is checked out in, not a sibling, not
  a drive or a scratch directory, not a home; say the thing in words.
- **Terms in order.** A page explains a term where a reader first meets it, or links
  [the terminology](docs/terminology.md), before relying on it.
- **A value the code owns is named, not copied** ([values the code owns](#values-the-code-owns)).
- **Documentation is done** when what the change made false is corrected, in place, and no more.

### Making a change

Bounded work is a GitHub issue labelled `ready`.
[Taking an issue](docs/contributing/taking-an-issue.md) goes from one to a pull request ready to
merge, and [best practices](docs/contributing/best-practices.md) says what good work looks like
here, subsystem by subsystem. Open an issue before preparing a large change to the core.

- **Start from the current `main`**, one branch for one issue, and name the commit you started
  from. A `main` that moved meanwhile is rebased onto, never reset away.
- **Reproduce first.** A defect is shown red before its repair and green after; a case that never
  went red has not shown it catches anything.
- **Before the push**, run what the change touches and its own cases, and read the whole diff
  against `main` by hand, every file, as review will; fix what that read finds.
- **After the push**, the pull request's hosted run carries the full lanes. Follow it job by job
  to its end, and fix a red on the branch by a new push. Every check names the commit it ran at.
- **One commit is one coherent step**, and its message says what changed and why.
- **The pull request shows a visible result** in screenshots from the tested state, captioned with
  what each proves; a change with none says so.
- **A maintainer merges.** Leave the pull request open once it is ready.
- **Your own account authors and commits it**, with no co-author line and no AI credit
  ([attribution](#attribution)), and you answer for the work, whatever ran it: an agent's choices,
  its subagents and its budget are your own.
- **Delete only what can be made again**: a clone whose work a remote holds, a tree its build makes
  again. Push or save unpushed work first, and keep what a run produced (its logs, its exits, its
  pictures) apart from the clone and its trees, so a rebuild does not take it with it. What a change
  made goes once its pull request merges.
- **An agent's session that is summarized or compressed** reads the start file, this guide and the
  owner of its part again before going on, and measures again every number it will report: a summary
  keeps conclusions and drops the evidence behind them. Its memory holds routes, never facts: how
  the maintainers work where no page says it, and where each fact's owner is, since a fact copied
  into memory goes stale while its owner moves. Work that spans sessions keeps a short note of where
  it stands: the branch and its last tip, what is uncommitted, the open question and the next
  action, what is still running, and the evidence so far with the commit each piece ran at.

---

## Reference

### Attribution

A commit is authored under the account of the person who makes it, never the AI agent they use
as its author or its committer. No commit and no pull request carries a co-author line or an
AI credit, for anyone. The reason:

> You will be the one held accountable for the code you open a pull request for, not the agent
> you used. Take care in the work you do, and consider the reasons and outcomes of the choices
> you make.

`tests/check_commit_attribution.cmake` refuses, on every commit it reads, an author or
committer that is an AI agent, every co-author line, and an assistant's credit: Claude
Code's line, a footer of links to an assistant's own pages, and the marker Cursor opens
a description with, the comment named `CURSOR_AGENT_PR_BODY_BEGIN`. Prose that names an
assistant, or names that marker, passes. On a pull request the same check reads the title
and the description, the text the merge copies into a commit. Its refusal names the
commit and the identity, and gives the same reason. A refusal of the pull request's title
or description says to edit that text and then push a commit or close and reopen the
pull request, since a re-run reads the text its run began with.

### Attacking the result

A change with behavior at run time gets one pass by hand before it is called done, against the
defects review finds most often. Say in the pull request which of these you tried and which did not
apply:

- **Which answer is this?** Two requests in flight, a late or duplicate answer, a stale one after a
  retry: an outcome belongs to the exact request that caused it.
- **Replacement and reload.** Replace, reload or reopen the owner mid-operation: a held choice,
  draft, lease or subscription survives or is refused out loud, and never lands on another subject.
- **Failure paths.** Make the write fail, the peer refuse, the process die, the wait expire: the
  result says what happened, keeps control, and never claims a success it did not get.
- **Which "done"?** Each word an operation says — queued, accepted, done, shown, saved — names the
  step its evidence reached, never the next: an accepted send is not a delivery, a delivery is not
  a picture, and a write is not a save until it is read back.
- **The world after the write.** Anything judged before a write is judged again on what the write
  leaves: the check that admitted a change says nothing about the state the change made.
- **Bounds.** Flood it and leave it running: every buffer, queue, retained answer and traversal has
  a bound, and reaching it is refused out loud.
- **Two of them.** Two processes, launches or hosts on one store or runtime: one wins by design,
  and the other is told.
- **The witness.** The case goes red on the defect, exercises the real boundary rather than a
  stand-in, and asserts the requirement rather than today's output.
- **The words.** Every claim in a message, a page and the pull request is one the evidence shows,
  and a blocker is diagnosed before it is reported.

A verdict is the check's own exit code, captured and tested, never read through a pipe. The
evidence that a case catches a defect is a mutation that made it red. A temporary edit to a source
tree and a build of that tree run one after the other, never together, and the source is restored
and checked before the result is believed.

### What a boundary means

- **Trace the exact mode an API runs in before relying on its name.** Matching a correlation,
  routing by role and an authenticated answer are different guarantees. If a guard blocks the
  happy path, repair the missing mechanism; removing the guard also needs a witness that the
  unwanted actor or outcome is still excluded.
- **Turn a consequential requirement into something observable.** A class called a weave does not
  prove it can be loaded or replaced; an acknowledgement does not identify a later read from shared
  storage. Exercise the real loading boundary, and a competing writer where that matters.
- **Keep reusable proof usable.** A new public capability joins the installed consumer witness, and
  reading it generically travels in a maintained tool or example, not a helper left uncommitted.

### Tools for the next contributor

A command, an example, a test input or a setup that the work showed is worth keeping is left with
the page or the tool that owns it, with its purpose, what it needs, how to reset it and what a
reader should expect. Look for an existing one to improve before building another, check a saved
one in a fresh copy before calling it reusable, and leave out credentials. A repository keeps only
the tools a lane, a check, a guide or the product runs; one change's own scripts stay in your own
space.

### Line width

Text written by hand (prose, comments, code and pages) fills its lines to 100 columns. A line of up
to 125 stands as it is, whether written so or carried there by an edit, and is not rewrapped for
its length alone; a line past 125 is wrapped back to 100. A table row, a link or a command that
cannot break is excepted, and a formatter's output stands as the formatter leaves it
(`.clang-format` sets 100).

### More build rules

- **Every loadable weave target goes through `loom_weave_build_contract()`**
  (`cmake/loom-weave.cmake`, KERN-05), including the fixtures in `tests/` and every static
  library the package installs for a weave to link (`LOOM_PACKAGE_LIBRARIES`), since their
  objects land inside the image. Never hand-write its options: the `weave_contract` entry reads
  the built artifacts and will say so. Which artifacts must carry it is derived by the
  `weave_population` entry (every `SHARED`/`MODULE` library in `tests/`, the static libraries in
  their link closures, and `LOOM_PACKAGE_LIBRARIES`), and one that left the roll is named
  (POP-05). A deliberate exception is `zen_weave_contract_exempt(<target> <reason>)`, in writing,
  and it fails if that target then turns up contracted.
- The `isolation`, `policy` and `all` suites need a delegated cgroup scope: run the whole binary
  through `tests/run-under-scope.sh ./build/tests/zen-tests`. Outside such a scope every
  OS-enforcement case fails **by design** (failing hard beats skipping silently) and names the
  capability it could not enforce.
- Windows builds the portable subset only, on **MinGW-w64 and MSVC** alike; the kernel lanes are
  opt-in (`LOOM_ENABLE_WINDOWS_KERNEL`). MSVC needs `/Zc:preprocessor` for Loom's public
  `__VA_OPT__` macros, and `loom::core` carries it as an interface option; on the MSVC ABI the whole
  build, and a project finding the package, uses the static C++ runtime a weave carries (KERN-05).
  Under MinGW-w64 each weave also links `cmake/loom-weave-tls-return.cpp`, which returns at unload
  the TLS and FLS indexes its runtime took; `weave_reload` loads one past the process's TLS ceiling.
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

### The population contract — what a green run means (POP-01..05)

`tests/verify.cmake` passes `--no-tests=error` and refuses to run under the enforcement opt-out.
The laws are in `docs/laws/population-laws.md`.

- **Zero is never a pass.** `zen-tests --test-suite=<no match>` exits 70 with
  `EMPTY TEST POPULATION`; a selector matching no CTest entry fails the lane.
- **Suite registration is derived** from the `TEST_SUITE(...)` declarations in the compiled
  sources; there is no hand-kept name list to drift.
- **`tests/suite_population.txt`** is the inventory contract: the exact suite set per gate
  (`portable`, `kernel`, `posix`, `runs`) and a case floor per suite. Adding cases is free;
  deleting one crosses its floor. Adding or renaming a suite needs a line there. The `population`
  CTest entry enforces it without running any case.
- **`tests/entry_population.txt`** does the same for the CTest entries that are not suites: the
  population checks, the documentation checks, the weave-artifact checks, the host-process
  witnesses and the rest. Which of the two files holds an entry says what kind of thing it is.
- **OS-enforcement tallies are per suite and exact, not floors.** `isolation` and `policy` each
  expect an exact count of executed enforcement proofs, the same in a dedicated run and in `all`.
  A missing witness fails, and so does an unannounced extra one. Each number is owned by the
  `ZEN_ENFORCEMENT_POPULATION(...)` call at the end of `tests/test_isolation.cpp` and
  `tests/test_policy.cpp`; read it there, it is written nowhere else. A suite that includes
  `tests/enforcement_gate.hpp` must `#define ZEN_ENFORCEMENT_DOMAIN` first.
- **Populations are not interchangeable**: cases, suites, OS-enforcement proofs, assertions.
- **Documentation is checked too.** The checks `tests/text_checks.cmake` lists (`ZEN_TEXT_CHECKS`)
  hold [the standard above](#comments-and-documents) ([the checks](#the-checks)).
  `docs/history/`, `archive/`, vendored trees and build trees are excluded by written rule.

### The supplied host, and who decides what a loaded artifact may do

`loom-host` (`src/host/`) is the one executable this project installs: a boot walk from a file
the person wrote, a weaver's console with authority in it, and two per-install files
(`loom-boot.json`, `loom-authority.json`). It is a replaceable default assembled from ordinary
participants — `ConsoleEngine`, `WeaveManager`, `ControlWeave`, and `loom::host::HostWarden` — and
holds no privilege the package does not export. Its guide is `docs/guides/running-loom.md`, and
its prerequisites `docs/guides/tools.md`.

**Three rules the host is built around, each for a defect a green lane could not see:**

1. An **answer** is something Loom attributed — the correlation the console minted plus the
   bus-stamped sender (`loom::AskBook`) — never the newest entry in the reply window, which any
   admitted artifact can author under the ordinary answer baseline.
2. A loaded artifact is **adopted inside the load**, through `loom::LifecycleAdoption`
   (`zen/kernel/control.hpp`): the one window between an incarnation being committed and being told
   it is live. Because it lives in the door, the boot walk, `start`, `reload` and an ordinary
   `zen.LoadWeave` all produce the same governed participant, and no host call site adopts
   anything.
3. The loop is `pump_pending()` plus the one wait (`include/zen/host/reactor.hpp`) over the console
   (`include/zen/host/line_input.hpp`) and, serving, the door, never `drain_until_idle()`: one
   approved self-addressed message would otherwise make `stop` and `quit` unreadable forever.
   What the reader holds and refuses is one rule on every platform (`loom::host::HeldInput`): at
   most `kHeldInputBytes` waiting, and a line over `kMaxCommandBytes` refused whole, never run
   shortened or split (`docs/reference/bounds.md`).

A conversation that has not settled is reported PENDING and stays open; the host never invents a
completion. The console holds a conversation only for a caller that asks
(`ConsoleTracking::Tracked`), until that caller takes or forgets it.

**One host owns one decision store while it runs** (`include/zen/host/store_lock.hpp`): the store is
written whole, so a second writer would restore what the first one revoked.

**Building a capability on a session for some other application to drive** (a guest door, an
external-host tool package): `docs/guides/session-tools-map.md` routes each question — start and
return, find a capability, act on the target, read a result, recover, extend the harness — to the
section of `docs/guides/sessions.md` that answers it, so that application's own documentation
holds only what is its own (its admission and guest shapes).

**`--serve <dir>` keeps the host alive for clients** (`docs/guides/sessions.md`): closed stdin
closes only the console. One loopback door (`src/host/session_door.hpp`) admits the owner's key
as a client with exactly the session, runs and history vocabularies, and a run manager's one-time
worker credential (only its SHA-256 crosses the bus) with no more than that manager's own approved
authority, whole or refused. The run manager `loom-runs` (`src/runs/`) is an ordinary loadable
weave — task policy, not host root — with its Python runtime beside it. **Every delivery is gated,
answers included**: host wiring grants its own answer shapes; the weaver grants a loaded manager
its typed answers.

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
`load(..., Grant)` bypasses the policy deliberately: naming the grant at the call site is the
decision. `docs/reference/capabilities.md#admitting-a-loaded-artifact`; suite `admission`.

### Consumers

This repository owns substrate truth. [Zengine](https://github.com/Krealsion/Zengine) owns its
packages (Timer and the others) and consumes Loom as an installed package (`find_package(loom)`).
Its tests need a Loom that exports `loom::kernel` (always on Linux; on Windows only under
`LOOM_ENABLE_WINDOWS_KERNEL`); against a kernel-less package its `tests/` **fails configuration**
rather than silently emptying itself, and `-DBUILD_TESTING=OFF` is its supported library-only
configuration. [Night Lab](https://github.com/Krealsion/zen-night-lab) is an independent
application; what it found is evidence (`docs/evidence/night-lab.md`), not contract.

### More traps

- That `Switchboard` has a `pump()` or a `run()`. Neither exists: both read, to an ordinary C++
  reader, as the bounded turn while meaning drain-to-idle. Do not introduce either as a synonym
  for `pump_pending()` or `drain_until_idle()`.
- That a request queued after an answer is dispatched after the work that answer's sender set in
  motion. FIFO orders envelopes as they are queued, and a consumer's follow-ups are queued as they
  happen; the bus's own record of what one send set in motion is a fence
  (`Switchboard::send_as_fenced`, `docs/reference/messaging.md`), and across a link, `settle`.
- That prepared replacement preserves incumbent state — it does not (PR-09).
- That `send_to_role` says the sender speaks as a role (MSG-04) — speaking as one is its own
  explicit, verified act (`mail.as_role(...)`, MSG-07), and merely holding a role attaches nothing.
- That a committed activation is answerable (LIFE-05).
- That a reachable bridge socket is admitted — it is not; a connection has no proxy and acts on
  nothing until the host's `BridgeAdmission` policy answers its Hello with a grant, a refusal or a
  deferred decision. The weaver policy admits everything that reaches it, and no policy adds
  transport security (`docs/reference/bridge.md`).
- That a grant bounds what in-process native code can touch — it bounds speech only; a `dlopen`ed
  weave shares this address space (`docs/guides/dynamic-weaves.md`).
- That the Weaver acts for the session it governs. It changes authority and performs nothing: the
  session retries its own action, and the target sees the **session** as `mail.sender()`. Nor
  does a Weaver's death revoke what it installed — a grant is not a lease
  (`docs/reference/weaver.md`).
- That a `TerminalSession` is powerful because it is a terminal. It is an ordinary weave: no
  `Switchboard&`, no tap, no registry read, no `allow_any`, and a vocabulary its host supplied
  rather than discovered. Its transcript says **SUBMITTED**, never "delivered"; an authenticated
  later dispatch-refusal record explains a refused addressed send. Absence proves nothing, and
  `loom::ConsoleEngine` remains a separate, deliberately trusted host and debug lens that can
  say delivered (`docs/reference/terminal.md`).
- That grants are mutable (GATE-05). A subject's delegated message authority can be replaced
  live, by a holder of a host-minted `GrantAuthority`, within that capability's ceiling; its
  admission **baseline** never changes, and its OS, filesystem and resource containment was
  consumed into kernel state before the child ran — `LiveAuthority` has no word for any of it.
- That `Emit<...>` is informational, or that declaring a shape anywhere grants anything. Every
  list a weave declares — accepted, claimed, emitted, persisted — and every component those shapes
  nest is claimed through one agreement wall at registration (natively) and at load
  (in the manifest, since ABI v9), so a divergent definition refuses at the door in either order
  and a self-contradicting declaration never registers; and none of it is authority — a declared
  emitter with no send rule is still `CapabilityDenied`, and `Emit` is not an exhaustive send list
  (`docs/decisions/declared-vocabulary-is-agreed-at-admission.md`).
- That a discrepancy means the prose is wrong. Code and tests establish observed behavior;
  intended contracts still need judgment ([what a change is judged against](#what-a-change-is-judged-against)).

### Values the code owns

A version, a limit, a count or a default the code owns is named by its owner — the constant,
the macro, the shape — wherever a sentence needs it: "at most `kMaxFences` fences", "the current
version is `ZEN_ABI_VERSION`". A comment names the owner and no more. Where a page's reader
needs the number itself, it stands in a marker that renders as nothing:

```markdown
| `kMaxJointOfferBytes` | <!-- value kMaxJointOfferBytes KiB -->64<!-- /value --> KiB | … |
<!-- value kMaxCommandBytes in "longer than {} bytes" -->
```

The first holds the number between its two comments, written as the word after the id says
(`KiB`, `MiB`, `GiB`, `s`, `pow2`, `grouped`, or nothing for the integer); two ids joined by a
comma are one value two owners share, and they must agree. The second holds a spelling on its
own line, or, standing alone, the next line or the fenced block after it: the form for a
number inside code or a transcript. A marker shown inside a fenced block, as here, is an
example and holds nothing.

`code_values` (`tests/check_code_values.cmake`) reads each value
[`tests/code_values.txt`](tests/code_values.txt) registers from its owner, and a marker that
says otherwise is a red; `cmake -DZEN_VALUES_WRITE=ON -P tests/check_code_values.cmake` rewrites
the markers to the owners' values. It also refuses an unmarked copy of a value it knows: the
owner's name with a number beside it, in a page or a comment, and a spelling the registry names
— a claim of the current value (`C ABI (version **{}**)`) always, and a mention (`ABI v{}`) at
the current value unless it states that version's own fact ("since ABI v9", "v5 adds").

A test count outside the population files names, beside it, the commit and the command that
measured it, or it is not written; `code_values` refuses one that names neither. No change
sweeps for copies by hand: when review finds a moving value the registry does not know, it is
registered in the change that found it.

### The checks

Each check `tests/text_checks.cmake` names holds this, one bullet a check, and names the
file and line of what it refuses:

- `source_comments` (`tests/check_source_comments.cmake`) reads first-party C/C++, CMake, the
  two population manifests, Python and shell, and the session tooling's launchers: a removal note
  or a private id. A Python, shell or batch file is read whole for an id, docstrings and strings
  included. Every C/C++ string literal and the code of every CMake file are read for a private id
  too, as their comments are; the three checks whose self-tests must spell ids are spared that
  reading. It reads each `TEST_CASE` and `SUBCASE` name, every one on a line: one holding a
  private id or a label (`J1:`, `S3b`), or a `TEST_CASE` name another case already has in the one
  test binary, is refused. Its limit on a comment block's lines is paused
  ([the documentation's design](docs/contributing/documentation-design.md#the-checks)).
- `doc_standard` (`tests/check_doc_standard.cmake`) reads every current-facing Markdown file for
  a private id, for the development process used as a unit of time, and for a page's history in
  the few forms that told it every time they were read by hand: a heading marked retired, a note
  of what a thing was before or what it was called, and a bold note opening on the past. The
  check names each form in its own words.
- `doc_links` (`tests/check_doc_links.cmake`) resolves every repository-relative link, and every
  `.md` path in a C/C++ or CMake comment, and refuses a path outside the repository in any
  current-facing text file, whether it is spelled out or reached by climbing above it with `../`.
- `doc_names` (`tests/check_doc_names.cmake`) reads the start file and the contributor's pages --
  `AGENTS.md`, `docs/CONTEXT.md`, `docs/README.md`, this page and `docs/contributing/` -- for the
  names no link holds. A path in backticks, or written bare in the routing table, is a file or
  folder of the tree: read from the root, from the page's folder, or, when it opens with no
  top-level folder, as the tail of a path (`test_gate.cpp`, `zen/kernel/control.hpp`). A name the
  tree does not hold, a file the host writes or a build's tree, is a row of
  [`tests/doc_names.txt`](tests/doc_names.txt), and a row no page needs is a red. A suite, an
  entry or a gate named beside that word is one the population files declare, and so is a
  backticked name in parentheses after a path in the routing table, which is an entry. Every law
  in a range such as `POP-01..05` is declared. The bullets under this heading name the checks
  `tests/text_checks.cmake` lists, and a missing or an extra name is a red.
- `code_values` (`tests/check_code_values.cmake`) holds the values the code owns, as the
  section above says.
- `census` (`tests/check_census.cmake`) holds every row of
  [`tests/census.txt`](tests/census.txt) exactly, in either direction: a file's lines of code, a
  class's members, a table's doors. A line that holds only a comment, or nothing, is no line of
  code. A row marked `falls` is refused when it has risen against the base.

What a private id is, `tests/private_ids.cmake` says once for both. Every current-facing
document and every first-party source file is held to this standard; vendored code is not. No
check can see history or a private process written in other words, a label shape in a string (in
data `c2a` looks like `r1` or `Ping2`), or a comment's truth; that is a reviewer's.

### Code contributions

Issues, testing, design discussion, reproductions, and feedback are welcome.

Before merging substantial third-party code contributions to the core
repository, the project will publish explicit contributor terms so ownership
and licensing remain clear for contributors and maintainers alike.

Please open an issue before preparing a large core contribution.

This note is deliberately minimal: it is not a CLA, it requires no copyright
assignment, and it sets no terms beyond asking that big core changes start
with a conversation. Experimentation, packages, and weaves of your own need
no permission at all — they are yours.
