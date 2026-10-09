# Best practices

What good work looks like in this repository, one practice to a bullet. Each practice links its
owner: the law, reference page, guide, root document or check that states the rule and the
reasons for it, which this page never restates. [Taking an issue](taking-an-issue.md) is the path
from an issue to a pull request, and [AGENTS.md](../../AGENTS.md) holds the build rules.

Each section belongs to one area of the repository and stands alone, naming its own owners, so
that a section can move into its area's own document. A section opens with its owners: the laws,
the reference pages, and the main suites and other CTest entries that witness it, which
[CONTEXT.md](../CONTEXT.md) lists in full. A suite runs alone with
`build/tests/zen-tests --test-suite=<suite>`, and any entry through the lane with
`-DZEN_SELECT='^<entry>$'`.

## Every change

### Scope and design

- **Read the owner before the code.** [CONTEXT.md](../CONTEXT.md) routes each topic to its
  reference page, its laws and its tests.
- **Intent decides what is right; a passing test shows what happens.**
  [Intent, evidence, and architectural fit](../../AGENTS.md#intent-evidence-and-architectural-fit)
- **Judge the whole result, and what it leaves the consumers that build on Loom.**
  [Intent, evidence, and architectural fit](../../AGENTS.md#intent-evidence-and-architectural-fit)
  and [consumers](../../AGENTS.md#consumers)
- **Read a law's DOES NOT MEAN before changing what it governs.**
  [Reading a law](../laws/README.md#reading-a-law)
- **Stay inside the issue's fences.** [Taking an issue](taking-an-issue.md#1-choose-a-ready-issue)

### The witness

- **The case comes first, red on the unchanged code for the issue's reason.**
  [Taking an issue](taking-an-issue.md#6-a-test-that-fails-before-the-change)
- **"Proven" means a regression test asserts it**; a property only read in the code is true by
  construction, not yet pinned. [Laws](../laws/README.md)
- **Zero is never a pass**: a run that selects no case exits 70, and the lane refuses an empty
  selection.
  [The population contract](../../AGENTS.md#the-population-contract--what-a-green-run-means-pop-0105)
- **A case's name says what it proves**, with no label and no private id.
  [Source comments](../../CONTRIBUTING.md#source-comments) and [the checks](../../CONTRIBUTING.md#the-checks)

### Verification

- **Quote `tests/verify.cmake`, never a bare `ctest`.**
  [The population contract](../../AGENTS.md#the-population-contract--what-a-green-run-means-pop-0105)
- **A Windows green names its compiler**, never a bare "Windows". [Build and test](../../AGENTS.md#build-and-test)
- **Declared absence is not a pass**, and a Windows total is never compared with a Linux one.
  [POP-03](../laws/population-laws.md#pop-03--unsupported-testability-is-declared-never-disguised)
- **A run under the enforcement opt-out proves nothing about containment.**
  [POP-04](../laws/population-laws.md#pop-04--an-opt-out-run-is-not-enforcement-evidence)
- **A build speedup waits until the whole lane has run under it.**
  [Build and test](../../AGENTS.md#build-and-test)
- **A hosted run is read job by job.** [Taking an issue](taking-an-issue.md#10-reading-the-hosted-run)

### Writing

- **Current-facing text states the present**, holds nothing private and names no path outside
  the repository. [Comments and documents](../../CONTRIBUTING.md#comments-and-documents)
- **A value the code owns is named by its owner, or held in a marker**:
  `cmake -DZEN_VALUES_WRITE=ON -P tests/check_code_values.cmake` rewrites the markers.
  [Values the code owns](../../CONTRIBUTING.md#values-the-code-owns)
- **A comment stays only where a reader would otherwise make a mistake**, and an installed header
  is documentation. [Source comments](../../CONTRIBUTING.md#source-comments)
- **A term is explained where a reader first meets it**, or linked to the
  [terminology index](../terminology.md). [Comments and documents](../../CONTRIBUTING.md#comments-and-documents)

### Delivery

- **One issue is one branch and one pull request, which closes it with `Fixes #n`.**
  [Taking an issue](taking-an-issue.md#1-choose-a-ready-issue) and
  [the pull request](taking-an-issue.md#9-the-pull-request)
- **No commit records an assistant as a co-author or credits one**:
  `cmake -P tests/check_commit_attribution.cmake`.
  [The attribution guard](../../tests/check_commit_attribution.cmake)
- **The whole diff is read before the push.**
  [Taking an issue](taking-an-issue.md#8-the-checks-whose-green-counts)
- **One push, and its run read to its end before the next.**
  [Taking an issue](taking-an-issue.md#10-reading-the-hosted-run)

## Values, schemas and the gate

Laws GATE-01 to GATE-04 in [admission](../laws/admission-laws.md), and LIFE-08; reference
[values and admission](../reference/values-and-admission.md); the suites `schema`, `value`,
`gate`, `registry`, `serialize`, `compat`, `fuzz`, `integration`, `weave_shape` and `weave`, and
the entry `optional_member_types`.

- **No fast path around admission**, and authorization is a distinct step.
  [GATE-01](../laws/admission-laws.md#gate-01--one-gate) and
  [GATE-03](../laws/admission-laws.md#gate-03--authorization-is-not-conformance)
- **A changed shape is a new version.**
  [GATE-04](../laws/admission-laws.md#gate-04--immutable-published-schemas)
- **An optional field is not a versioning tool.**
  [Required and optional fields](../reference/values-and-admission.md#required-and-optional-fields)
- **Parsing is not validation**, and conformance is not semantic validity.
  [GATE-02](../laws/admission-laws.md#gate-02--untrusted-until-proven) and
  [the gate](../reference/values-and-admission.md#the-gate)
- **A reclaimed schema is undiscoverable, not destroyed.**
  [LIFE-08](../laws/lifecycle-laws.md#life-08--a-schema-is-retained-by-a-live-claim-never-by-having-been-registered)

## Messaging, dispatch and answers

Laws MSG-01 to MSG-12 in [messaging](../laws/messaging-laws.md) and ANS-01 to ANS-07 in
[answer authority](../laws/answer-authority-laws.md); reference
[messaging](../reference/messaging.md); the suites `switchboard`, `provenance`,
`role_authorship`, `ask_book`, `poke`, `describe` and `dispatch_refusal`, and `kernel` and
`dispatch_loaded` where the kernel is built.

- **A host loop turns with `pump_pending()`**; the drain is unbounded by contract, and neither
  gains a cap or a synonym.
  [MSG-09](../laws/messaging-laws.md#msg-09--a-dispatch-turn-can-be-bounded-without-bending-fifo)
- **A correlation identifies and never authenticates.**
  [ANS-05](../laws/answer-authority-laws.md#ans-05--correlation-identifies-it-never-authenticates)
- **Addressing a role says nothing about the sender's office.**
  [MSG-04](../laws/messaging-laws.md#msg-04--role-addressing-is-destination-not-office)
- **A handler that throws costs its delivery, not the bus.**
  [MSG-10](../laws/messaging-laws.md#msg-10--a-callback-that-throws-costs-the-delivery-not-the-bus)
- **When something did not arrive, tap the bus and read the refusal's reason as a direction.**
  [Diagnostics](../guides/diagnostics.md)

## Lifecycle, replacement and handoff

Laws LIFE-01 to LIFE-08 in [lifecycle](../laws/lifecycle-laws.md), PR-01 to PR-09 in
[replacement](../laws/replacement-laws.md) and HANDOFF-01 to HANDOFF-03 in
[handoff](../laws/handoff-laws.md); reference [lifecycle](../reference/lifecycle.md),
[prepared replacement](../reference/prepared-replacement.md) and
[handoff](../reference/handoff.md); the suites `provenance`, `switchboard`, `weave` and `bridge`,
`kernel`, `manager` and `handoff` where the kernel is built, and `isolation` on POSIX.

- **A commit schedules; only the admission dispatch commits.**
  [PR-07](../laws/replacement-laws.md#pr-07--commit-schedules-only-the-dispatch-commits)
- **Replacement preserves no incumbent state**: continuity is authored.
  [PR-09](../laws/replacement-laws.md#pr-09--replacement-verifies-the-successor-not-continuity)
- **A committed activation is not answerable.**
  [LIFE-05](../laws/lifecycle-laws.md#life-05--a-committed-activation-is-not-answerable)
- **A weave outlives its own callback**: removing the running weave changes nothing, and the host
  may retry once the callback has exited.
  [LIFE-06](../laws/lifecycle-laws.md#life-06--a-weave-outlives-its-own-callback)
- **A different schema identity takes an authored transformation, never coercion in the gate.**
  [HANDOFF-01](../laws/handoff-laws.md#handoff-01--different-schema-identities-require-an-explicit-authored-transformation)
- **A replacement's truth is its transaction's outcome.**
  [Diagnostics](../guides/diagnostics.md#4-replacement-outcomes)

## The kernel, loading and the dynamic ABI

Laws KERN-01 to KERN-05 in [kernel](../laws/kernel-laws.md), and POP-05; reference
[kernel](../reference/kernel.md) and [dynamic ABI](../reference/dynamic-abi.md); guide
[dynamic weaves](../guides/dynamic-weaves.md); the suites `kernel`, `manager`, `dispatch_loaded`,
`joint`, `admission` and `capabilities` where the kernel is built and `schema_codec` everywhere,
and the entries `weave_contract` and `weave_population`.

- **Every loadable weave target, the test fixtures included, goes through
  `loom_weave_build_contract()`.** [Build and test](../../AGENTS.md#build-and-test) and
  [KERN-05](../laws/kernel-laws.md#kern-05--a-reloadable-artifacts-statics-live-and-die-with-it)
- **The linker is not changed without the whole lane.** [Build and test](../../AGENTS.md#build-and-test)
- **ABI tables are built with designated initializers, in declaration order.**
  [Constructing the tables](../reference/dynamic-abi.md#constructing-the-tables)
- **A version bump keeps its stale fixture and owes a genuine artifact from before the change.**
  [Compatibility discipline](../reference/dynamic-abi.md#compatibility-discipline)
- **A weave is loaded through the control door**, which activates it.
  [Load it](../guides/dynamic-weaves.md#load-it)

## Capabilities, isolation, policy and admission

Laws GATE-03 and GATE-05 in [admission](../laws/admission-laws.md), and POP-02 and POP-04;
reference [capabilities](../reference/capabilities.md); the suite `grant`, `capabilities` and
`admission` where the kernel is built, and on POSIX `isolation`, `policy` and the entry `all`,
which CTest runs through `tests/run-under-scope.sh`.

- **`Kernel::load` mints no grant**, and a Kernel nobody configured admits nothing.
  [Who decides what a loaded artifact may do](../../AGENTS.md#the-supplied-host-and-who-decides-what-a-loaded-artifact-may-do)
- **A grant is not mutable**: only delegated message authority changes live.
  [GATE-05](../laws/admission-laws.md#gate-05--baseline-authority-is-admission-time-delegated-authority-is-live-effective-authority-decides)
- **An OS-enforcement tally is exact, and its owner is the suite's closing population call.**
  [The population contract](../../AGENTS.md#the-population-contract--what-a-green-run-means-pop-0105)
- **A run under the opt-out is never containment evidence.**
  [POP-04](../laws/population-laws.md#pop-04--an-opt-out-run-is-not-enforcement-evidence)
- **The sanitizer lane's leak checking stops at the host**: a child's leaks are absent, not clean.
  [Leak checking stops at the sandbox boundary](../reference/known-seams.md#leak-checking-stops-at-the-sandbox-boundary)

## The bridge, sessions, the supplied host and runs

Reference [bridge](../reference/bridge.md) and [bounds](../reference/bounds.md#sessions-and-runs),
each naming its laws; guides [running Loom](../guides/running-loom.md),
[sessions](../guides/sessions.md) and [the session tools map](../guides/session-tools-map.md);
the suites `bridge`, `host_policy`, `host_input`, `session`, and `runs` where the session tools
are built; the entries `host_console`, `host_line_input`, `host_line_streams`,
`host_process_weaves` and `host_terminal_weaves`, and with Python, `session_journey`,
`session_client` and `observe_journey`.

- **An answer is what Loom attributed, never the newest entry in the reply window.**
  [Who decides what a loaded artifact may do](../../AGENTS.md#the-supplied-host-and-who-decides-what-a-loaded-artifact-may-do)
- **A reachable bridge socket is not admitted, and the transport is plain.**
  [The bridge carries no transport security](../reference/known-seams.md#the-bridge-carries-no-transport-security-and-a-credential-is-a-shared-secret)
- **Python is optional tooling**: without it the session journeys are declared absent, not passed.
  [Optional, and what each one buys you](../guides/tools.md#optional-and-what-each-one-buys-you)
- **A killed host's workers outlive it on POSIX.**
  [A worker does not end with a host that was killed](../reference/known-seams.md#a-worker-does-not-end-with-a-host-that-was-killed--except-on-windows-and-not-because-of-loom)
- **Tell a compiler's, CMake's, the loader's, a policy's and an authority's failure apart.**
  [When something goes wrong](../guides/running-loom.md#when-something-goes-wrong)

## The terminal, console, history, the Weaver and senses

Laws SENSE-01 to SENSE-07 in [senses](../laws/sense-laws.md), and the laws each reference page
names; reference [terminal](../reference/terminal.md), [weaver](../reference/weaver.md),
[history](../reference/history.md), [senses](../reference/senses.md),
[joint publication](../reference/joint-publication.md) and [observation](../reference/observation.md);
the suites `terminal`, `weaver`, `console`, `recorder`, `logger`, `sense` and `observe`, and
`joint` where the kernel is built; the entry `hook_return_types`.

- **A terminal session is an ordinary weave**, not a host and not root.
  [What it is, and what it is not](../reference/terminal.md#what-it-is-and-what-it-is-not)
- **"Submitted" is what was authored, never a delivery.**
  [Submitted, received, answered and dispatch refusal](../reference/terminal.md#submitted-received-answered-and-dispatch-refusal)
- **The Weaver changes authority and performs nothing**, and a grant is not a lease.
  [The flow](../reference/weaver.md#the-flow) and
  [a policy delegate's death does not revoke what it granted](../reference/known-seams.md#a-policy-delegates-death-does-not-revoke-what-it-granted)
- **Reading a Sense takes its own observe rule.**
  [SENSE-05](../laws/sense-laws.md#sense-05--reading-is-authorized-and-the-repository-is-bounded)
- **A publication shown is not a publication applied.**
  [SENSE-06](../laws/sense-laws.md#sense-06--a-publication-is-shown-and-what-the-showing-came-to-is-recorded-never-assumed)

## The installed package and CMake

Laws KERN-05 and POP-03; [consuming Loom](../../README.md#consuming-loom); witnessed by
`tests/package/run.cmake`, outside the build tree, and by the hosted run's job that builds
Zengine's `main` against the change.

- **Among Loom's own lanes, only the package witness reaches Loom through `find_package` alone**,
  so only it catches a requirement the package fails to carry:
  `cmake -DZEN_PREFIX=<prefix> -DZEN_WORK=<dir> -P tests/package/run.cmake`.
  [Build and test](../../AGENTS.md#build-and-test)
- **Hosting is gated on `if(TARGET loom::kernel)`**, and the exported surface stays smaller than
  the build tree. [Consuming Loom](../../README.md#consuming-loom)
- **`loom::core` carries the MSVC preprocessor flag**, so a consumer that links it passes nothing.
  [Platform differences](../guides/tools.md#platform-differences-that-will-actually-bite-you)
- **Use nothing only a compiler newer than the floor accepts.** [Build and test](../../AGENTS.md#build-and-test)
- **A kernel-less package fails Zengine's tests at configure, by design.**
  [Consumers](../../AGENTS.md#consumers)

## Populations and tests

Laws POP-01 to POP-05 in [population](../laws/population-laws.md); the manifests
`tests/suite_population.txt` and `tests/entry_population.txt`; the entries `population`,
`empty_population_refused` and `empty_population_says_so`.

- **A new or renamed suite is a line in `tests/suite_population.txt`**, and any other CTest entry
  a line in `tests/entry_population.txt`.
  [The population contract](../../AGENTS.md#the-population-contract--what-a-green-run-means-pop-0105)
- **A case floor is never lowered to make a deletion pass.**
  [POP-02](../laws/population-laws.md#pop-02--a-coverage-floor-belongs-to-the-population-it-counts)
- **Suite registration is derived from each test source's `TEST_SUITE`**, with no hand-kept
  list. [The population contract](../../AGENTS.md#the-population-contract--what-a-green-run-means-pop-0105)
- **Assertion totals are reported, never an acceptance oracle.**
  [The population contract](../../AGENTS.md#the-population-contract--what-a-green-run-means-pop-0105)

## Documentation and its checks

[Comments and documents](../../CONTRIBUTING.md#comments-and-documents) and
[the checks](../../CONTRIBUTING.md#the-checks); the entries `doc_links`, `source_comments`,
`doc_standard` and `code_values`, run together by `cmake -P tests/documentation_lane.cmake`.

- **A change that touches no compiled line is verified by the documentation lane.**
  [Build and test](../../AGENTS.md#build-and-test)
- **Normative truth is `docs/reference/` and `docs/laws/`**; history is frozen, and evidence is
  not the contract. [CONTEXT.md](../CONTEXT.md)
- **A law is cited by the id `docs/laws/` declares**, and any other reason is said in words.
  [Comments and documents](../../CONTRIBUTING.md#comments-and-documents)
- **What no check can see is a reviewer's**: history told in other words, and a comment's truth.
  [The checks](../../CONTRIBUTING.md#the-checks)
