# CONTEXT — the machine router

Routing, not prose. Retrieve the files for your topic; do not preload
everything. Vocabulary is canonical in [terminology.md](terminology.md);
**normative** = `reference/` + `laws/` (current truth); `decisions/` = why;
`history/` = frozen record (never normative unless a law cites it);
`evidence/` = what applications found (never the API contract).

**Ownership:** Loom (this repository) owns substrate truth.
[Zengine](https://github.com/Krealsion/Zengine) owns its packages (Timer, Input, Surface) and
documents them. [Night Lab](https://github.com/Krealsion/zen-night-lab) is an independent
application whose findings are recorded in [evidence/night-lab.md](evidence/night-lab.md); it
is evidence, never the contract.

**Recommended order for a cold start:** this file → the topic row below →
the named laws → the reference page → tests when exactness matters.

| Topic | Read | Laws | Tests | Why / evidence |
|---|---|---|---|---|
| values, schemas, the gate (required and optional fields, `std::optional` members of a `ZEN_SHAPE`) | reference/values-and-admission.md, guides/writing-a-weave.md | GATE-01..04 | tests/test_gate.cpp, test_schema.cpp, test_registry.cpp, test_serialize.cpp, test_weave_shape.cpp, optional_members/ | decisions/one-gate-at-every-boundary.md |
| how long a schema stays resolvable (what a claim is, who holds one, what a rejected candidate leaves, why a remote console's mirror is different) | reference/values-and-admission.md#registry, reference/bounds.md#schema-registry-bounded-by-claims-not-by-a-number | LIFE-08 | tests/test_registry.cpp, test_switchboard.cpp, test_kernel.cpp | registries that only ever grew, found by an outside review; [known-seams](reference/known-seams.md#a-remote-consoles-learned-schema-mirror-is-still-append-only) |
| what a participant DECLARES and what declaring does (the four lists — accepted, claimed, emitted, persisted — and every component they nest, claimed through one wall at the door for native and loaded alike; why a self-contradicting declaration never registers; why an emitted-only shape now resolves and why that grants nothing; the manifest since ABI v9; the reload candidate judged against the bus before rebind) | reference/values-and-admission.md#registry, reference/kernel.md#loading, reference/dynamic-abi.md, guides/writing-a-weave.md | GATE-04, LIFE-08, SENSE-04, KERN-04 | tests/test_schema.cpp (collect_referenced), test_registry.cpp, test_switchboard.cpp and test_kernel.cpp (the "schema admission" cases), test_schema_codec.cpp (the manifest), test_weave.cpp (the copied weave), test_isolation.cpp, tests/package/stranger_host.cpp | decisions/declared-vocabulary-is-agreed-at-admission.md (a stale application admitted against a newer peer's emitted shape) |
| message authority, dispatch, roles | reference/messaging.md | MSG-01..07 | tests/test_switchboard.cpp | history/pre-r2c/DESIGN.md §Switchboard |
| native callback boundaries (a handler or observer that throws; mutating the tap list mid-notification) | reference/messaging.md#dispatch-model, reference/messaging.md#observation | MSG-10, MSG-11 | tests/test_switchboard.cpp | — |
| sender-visible dispatch refusal, exact attempts, life/incarnation disposal | reference/messaging.md#sender-visible-dispatch-refusal, reference/dynamic-abi.md, reference/terminal.md | MSG-12 | tests/test_dispatch_refusal.cpp, test_dispatch_loaded.cpp, test_terminal.cpp | decisions/dispatch-refusal-returns-to-its-author.md |
| answers, deferral, provenance | reference/messaging.md#answers | ANS-01..07 | tests/test_provenance.cpp | decisions/readiness-is-authenticated-conversation.md |
| what an ASKER remembers about its own outstanding conversations (which of mine an arrival settles, why correlation alone is not enough, an ask to an office, capacity, forgetting locally) | reference/messaging.md#the-askers-own-book | ANS-05 | tests/test_ask_book.cpp, tests/test_terminal.cpp | the bookkeeping is the asker's, not the terminal's ([terminal](reference/terminal.md)) |
| what may be SAID to one weave (the self-description door, why the target owns the answer, why a descriptor package carries its dependency closure, why discovering a shape grants no authority to send it, and why the answer is a snapshot) | reference/messaging.md#self-description--what-may-be-said-to-this-weave | MSG-02, GATE-01 | tests/test_describe.cpp, tests/package/stranger_host.cpp | — |
| office authorship (speaking as a role) | reference/messaging.md#office-authorship-role-authored-provenance | MSG-07, MSG-04 | tests/test_role_authorship.cpp, test_kernel.cpp (the loaded-weave office cases), test_isolation.cpp (pipe) | decisions/office-authorship-is-deliberate.md, evidence/night-lab.md |
| lifecycle, zen.Activated | reference/lifecycle.md | LIFE-01..05 | tests/test_provenance.cpp, test_manager.cpp | decisions/lifecycle-authority-is-loom-owned.md, decisions/committed-activation-is-not-answerable.md |
| permanent removal during a callback (a weave, or its host, unregistering the weave whose handler is running) | reference/lifecycle.md#permanent-removal-and-the-active-callback | LIFE-06 | tests/test_switchboard.cpp, test_kernel.cpp | — |
| long-lived transport channels (a framed channel's retained send/receive buffers; a peer that keeps up but never lets the socket run dry) | reference/bounds.md#transport-channels-framed-byte-channels | LIFE-07 | tests/test_bridge.cpp, test_isolation.cpp | — |
| what a console retains (the tap window, the m1/m2/... reply buffer, why an `mN` label can refuse, how eviction is surfaced) and what it HOLDS for a caller (a tracked conversation's slot, from the ask until its answer is taken or forgotten; why `Console` sends are untracked) | reference/bounds.md#console-operator-history | — | tests/test_console.cpp, test_bridge.cpp | 1,000 answers retained by a caller that composes and pumps |
| what the HOST knows about the bus RIGHT NOW (the recorder: what one record holds, how delivery truth and retention truth are kept apart, the four honest answers to "what became of seq N", the last-call store vs. the recent FIFO vs. the protected window, per-shape retention policy, the two budgets, the structural blacklist) | reference/history.md, reference/bounds.md#recorder-host-working-memory | MSG-10 | tests/test_history_recorder.cpp | — |
| what the host CHOSE NOT TO FORGET (the logger: whitelist selection, the conservative default, per-shape caps and why there is no global one, the three durable origins and why a diagnostic may never pretend to be a message) | reference/history.md#logger--the-durable-selected-record, reference/bounds.md#logger-durable-record | MSG-10 | tests/test_history_logger.cpp | — |
| grants, isolation, powerbox | reference/capabilities.md | GATE-03, MSG-02 | tests/test_capabilities.cpp, test_isolation.cpp, test_policy.cpp | [history](history/README.md) |
| changing a LIVE subject's message authority — what baseline/delegated/effective mean, who may administer one (a host-minted `GrantAuthority`), the ceiling and how attenuation is decided, and why OS/filesystem/resource reach is not in it | reference/capabilities.md#live-delegation | GATE-05, GATE-03 | tests/test_grant.cpp | — |
| putting a HUMAN in that loop — the Weaver (the first policy delegate), the operator seat, the authority-request vocabulary, what approval does and deliberately does not do, and why the Weaver keeps no permission store | reference/weaver.md | GATE-05, MSG-02, ANS-01..07 | tests/test_weaver.cpp | — |
| giving that human HANDS — a terminal session (an ordinary participant with one identity, a supplied vocabulary and its own transcript), what "submitted" does and does not mean, how several outstanding asks are told apart by Loom's own correlation, why the transcript is not fed by the tap, and how one presentation shows two identities without merging them | reference/terminal.md | MSG-01..07, ANS-01..07, GATE-05 | tests/test_terminal.cpp | — |
| the sandbox exec boundary — what a child inherits vs. what Zen authors: ambient descriptors (a host socket, file, pipe or terminal already open at mount) and the child's environment (`LD_*`, tokens, session addresses) | reference/capabilities.md#the-exec-boundary-three-independent-facts | — | tests/test_isolation.cpp | a sandboxed child that inherited a host socket and the host's environment |
| the crossing between two hosts — who admits a connection and when (the admission seam: admit under a grant, refuse in words, or DEFER for a later decision), what a claimed name is and is not, what a session is told beside the bytes (`answers_ask`, `dispatch_refused`, the correlation), which outcomes of a send are kept apart, `Settled` for a send that asked to follow what it set in motion, the console-free client and the link envelope an ordinary weave asks through | reference/bridge.md | MSG-02, GATE-01, MSG-09, MSG-12, LIFE-07 | tests/test_bridge.cpp, tests/package/stranger_bridge.cpp | an application hosting a guest over a link: crossed answers under equal correlations, and ordinary far speech handed over as an answer |
| WHEN WHAT ONE SEND SET IN MOTION HAS BEEN DISPATCHED (a host's fence: the send and everything queued from inside the dispatch of anything it names; Open and Settled; what Settled does not say — answers, deferred work, the rest of the bus; the bound, and why a fenced send past it queues nothing) | reference/messaging.md#fences-when-what-one-send-set-in-motion-has-been-dispatched | MSG-01, MSG-10 | tests/test_switchboard.cpp (the `fence:` cases), tests/test_bridge.cpp (settlement) | an agent's picture taken before the key it typed was painted: a consumer that paints after several deliveries of its own is overtaken by a request that followed the injection's answer |
| a PERSISTENT SESSION: the host kept alive for clients that attach and leave (`loom-host --serve`, the lifetime, `session.json`/`session.key`, the door `loom.session` and who it admits as what, owner-key clients and one-time run credentials attenuated to their registrar's own approved authority, Shutdown), the run manager `loom-runs` (catalog `loom-tools.json`, package manifests `loom-tool.json`, approval by revision, snapshots, named runs and their states, cancel vs detach vs host end, `run.json`, reconciliation by name, stale lifetimes), the Python client/CLI/worker (`python/loom_session`), and the scoped history reader `loom.history` | guides/sessions.md, ../include/zen/session/vocabulary.hpp, ../include/zen/runs/vocabulary.hpp, ../src/host/session_door.hpp, ../src/host/history_reader.hpp, ../src/runs/run_manager.hpp, reference/bounds.md#sessions-and-runs | GATE-05, MSG-02, ANS-02, MSG-10 | tests/test_session.cpp, tests/test_runs.cpp, tests/session/journey.py (`session_journey`, the `python` gate) | an agent that must come back to the same host |
| a session's PAYLOAD ENCODING (a compat session speaks Zen's JSON envelope through the same gate; a link encodes a compat ask natively) | reference/bridge.md#the-payload-encoding-a-session-speaks | GATE-01 | tests/test_bridge.cpp (the `encoding:` cases and the compat-envelope link case) | a Python peer should not have to reproduce the canonical binary |
| how poke/self-description answers are authenticated; old artifacts and redirection | reference/messaging.md#substrate-answer-attribution | ANS-01, ANS-05 | poke | `WeaveBase::answer_substrate` |
| the supplied host LINKING to another host (a `links` row, the office `loom.link.<name>`, what a weave asks and how the far answer comes back — once, with Loom's answer authority, from the link — the link's four outcomes, the crossing's own `attempt` and a session's `epoch`, `settle`, the `loom.link.Crossed` records the history keeps) and what the supplied host REMEMBERS AND KEEPS (the `history` section, `--log`, the `history`/`log` console words) | guides/running-loom.md#10-link-to-another-host, guides/running-loom.md#11-what-this-host-remembers-and-what-it-keeps, ../src/host/link.hpp | MSG-02, ANS-02, ANS-05, MSG-10 | tests/test_bridge.cpp (the `link:` cases), tests/test_host_policy.cpp (the plan's links and history) | — |
| dynamic loading, artifacts | reference/kernel.md, reference/dynamic-abi.md | KERN-01..05 | tests/test_kernel.cpp | evidence/night-lab.md (answer-seam) |
| opening a weave on Windows (where its libraries are looked for, what a refusal says) and the code page Loom's programs run in | reference/kernel.md#opening-a-weave-on-windows, guides/tools.md#platform-differences-that-will-actually-bite-you | — | tests/test_windows_loader.cpp, tests/host_process/run.cmake, tests/session/journey.py, tests/package/run.cmake, tests/doctest_main.cpp | a weave whose library sat beside it refused while one on `PATH` loaded, and a host that could not read a boot row from a folder named outside the system's code page |
| WHO DECIDES what a loaded artifact may do (the admission policy; the two stages and why they are two; why a reload cannot re-grant; why a declaration is never authority; the explicit permissive policy and why it has to be named; which build it is, read from its bytes only when a policy asks, once per operation) | reference/capabilities.md#admitting-a-loaded-artifact, ../include/zen/kernel/admission.hpp | GATE-05, KERN-01 | tests/test_admission.cpp | a kernel that granted every loaded library everything, through three doors |
| RUNNING Loom without writing a host (the supplied host, the boot plan and the authority store, what the console can do, the two shutdowns, the exit codes, the recovery route over a plan that will not parse) | guides/running-loom.md, guides/tools.md | GATE-05 | tests/test_admission.cpp, tests/test_host_policy.cpp, tests/host_process/run.cmake | — |
| WHEN a host's standing decisions become effective for a freshly loaded artifact (the one window between registration and `zen.Activated`, why a host that adopts on the operation's answer is necessarily one turn late, and why putting it in the DOOR makes every load route produce the same governed participant) | reference/capabilities.md#when-delegated-authority-can-first-be-installed, ../include/zen/kernel/control.hpp | GATE-05, LIFE-04 | tests/host_process/run.cmake | a boot-started weave whose approved startup send was refused CapabilityDenied |
| a HOST that must serve a bus and a person at once (the bounded turn, why an unbounded drain loses the operator to a cooperative weave that re-arms itself, why a conversation that has not settled is reported as pending rather than completed, why the wait for a line must hold while one is half typed — and a Windows console is therefore read on its own thread — and the input limits that hold the same on every platform: the longest command, what the reader may hold, and a longer line refused whole) | guides/running-loom.md#9-work-that-starts-by-itself-and-work-that-never-stops, ../src/host/line_input.hpp, reference/bounds.md#supplied-host-input-what-loom-host-reads-before-a-command-exists | MSG-09 | tests/host_process/run.cmake, tests/host_terminal/witness.cpp, suite `host_input` | a host loop that never returned, one approved self-addressed message, a half-typed command on a Windows console, and a Windows reader that read a whole file into memory |
| ONE OWNER PER DECISION FILE (why whole-file replacement makes a second writer a revocation hazard rather than a lost-update one, what the OS does about a host that dies, and what a refused second host is told) | ../src/host/store_lock.hpp, guides/running-loom.md | — | tests/test_host_policy.cpp, tests/host_process/run.cmake | two terminals, one person |
| a callback slot's status is a fact (what the bus records for every status of a new descriptor or host-table slot, including the one that is neither OK nor an error; why a discarded status is a silent lie) | reference/dynamic-abi.md#when-fields-or-signatures-evolve, reference/joint-publication.md#what-crosses-the-abi | KERN-04, SENSE-06 | tests/test_joint.cpp | — |
| constructing the C ABI tables (which slots share a type, what catches a miswire vs. what only looks like it does, the checklist for adding a field) | reference/dynamic-abi.md#constructing-the-tables | KERN-04 | tests/test_kernel.cpp | — |
| what running a `dlopen`ed weave in-process costs (address space, and what the grant does *not* bound) | guides/dynamic-weaves.md#what-loading-it-in-process-means, reference/capabilities.md | — | tests/test_kernel.cpp | — |
| building a loadable weave | guides/dynamic-weaves.md, reference/kernel.md | KERN-05, POP-05 | tests/check_weave_contract.cmake, tests/weave_population.cmake, tests/check_weave_population.cmake | twenty-three loadable weaves that left the build contract while every lane stayed green |
| prepared replacement | guides/replacing-a-service.md, reference/prepared-replacement.md | PR-01..09 | tests/test_kernel.cpp | decisions/admission-and-activation-share-one-boundary.md, decisions/no-rollback-after-committed-production.md, evidence/night-lab.md |
| senses (latest claims) | guides/observing.md, reference/senses.md | SENSE-01..05 | tests/test_sense.cpp, test_kernel.cpp | decisions/a-claim-is-not-a-message.md |
| OBSERVATION — one office's publications told live to a subscriber a host admits (the relay `loom.observe` and its host policy, readiness, holder-only telling, `cause` from a fence the subscriber opened, the window, `Gap` and `latest` coalescing, the four endings, a link's custody -- controls crossing only for the subscriber, a gone subscriber released on the host's own turn -- and `lost`, the Python `Subscription`), and a tool package that BUILDS ON another (`uses`) | reference/observation.md, guides/observing.md#following-another-participants-publications, reference/bounds.md#observation-relay, guides/sessions.md#a-package-that-builds-on-another, ../include/zen/observe/relay.hpp | MSG-02, ANS-05, LIFE-08 | tests/test_observe.cpp, tests/test_bridge.cpp (the `link:` cases on far observations), tests/test_runs.cpp (the `uses` cases), tests/session/test_client.py, tests/session/journey.py, tests/session/observe_journey.py | an agent that polled for what it could have been told |
| joint publication of latest claims (several claims change together in one exchange; the showing and its three answers — Applied, Declined, Failed; the hold and diagnostic access; the record kept until released; the operator's notices; what crosses ABI v8; the operator's authority — the exact life and incarnation it names, why a successor is minted for again, and why it reaches only its holder's own records) | reference/joint-publication.md, reference/joint-publication.md#authority, reference/senses.md#lifetime | SENSE-06, SENSE-07 | tests/test_joint.cpp, tests/hook_return/CMakeLists.txt, test_kernel.cpp (the v8 gate at reload) | publication is not application; a queued notice is not consumption; a repaired owner is not proof its old operation applied; a refusal is judged by what it changed, not by its name |
| authored handoff (continuity across an incompatible schema) | reference/handoff.md | HANDOFF-01..03, PR-09 | tests/test_handoff.cpp | decisions/migration-is-authored-not-inferred.md |
| event-loop composition | reference/messaging.md#two-dispatch-turns-and-the-call-site-says-which | MSG-09 | tests/test_switchboard.cpp, test_bridge.cpp | — |
| Timer continuity — **not in this repository**; Zengine's Timer package owns and documents it | [Zengine](https://github.com/Krealsion/Zengine) | — | — | — |
| what a green run means (suite/case/enforcement populations, declared absence, the opt-out, the required build-artifact population) | laws/population-laws.md, ../AGENTS.md | POP-01..05 | tests/suite_population.txt, tests/entry_population.txt, tests/check_population.cmake, tests/check_entry_population.cmake, tests/verify.cmake, tests/enforcement_gate.hpp, tests/weave_population.cmake | — |
| what a comment or a page may carry (the standard, and the three checks that hold it: `source_comments`, `doc_standard`, `doc_links`) | ../CONTRIBUTING.md#comments-and-documents | — | tests/check_source_comments.cmake, tests/check_doc_standard.cmake, tests/private_ids.cmake, tests/check_doc_links.cmake | — |
| how the `isolation` granted-network positive control proves the sandbox is a sandbox and not a muzzle — the endpoint the test binds and owns, why the positive witness is a successful connect plus a token byte rather than a particular errno, and why no host's closed-port behaviour is consulted (a failure there is a real failure) | reference/capabilities.md#the-granted-network-positive-control-and-the-endpoint-it-uses | POP-04 | tests/test_isolation.cpp, tests/weavelib/test_weave.cpp | a positive control that depended on how the host treated a closed port |
| known limitations | reference/known-seams.md | MSG-04, ANS-02, PR-09 | — | evidence/night-lab.md |
| bounds/capacities | reference/bounds.md | — | grep the constant name | — |

**Do-not-assume answers** (each resolvable from the row above): a connection
to the bridge holds **no authority until the host's admission policy answers
its Hello** — under `operator_admission()` reachability of the socket *is* the
authority, and no policy adds transport security, so a bridge listener must
not be exposed to an untrusted network
([bridge](reference/bridge.md#admission-who-decides-and-when)) · an
in-process weave, native or `dlopen`ed, shares the host address space: the
grant bounds what it may **say**, never what it may **touch**
([capabilities](reference/capabilities.md#the-grant-in-process)) · **grants are
not mutable** — a subject's *delegated* message authority can be replaced live
by a host-minted `GrantAuthority`, but its admission baseline never changes and
its OS/filesystem/resource containment was consumed into a namespace, a mount
view and a cgroup leaf before the child ran, where no later write reaches it
(GATE-05) · **a Weaver is not a broker** — approval changes authority and
performs nothing, so the governed session retries its own action and the target
sees the *session* as sender; and a Weaver's death revokes nothing it installed,
because a grant is not a lease
([weaver](reference/weaver.md)) · **a terminal is not root** — a `TerminalSession` is an
ordinary weave with no `Switchboard&`, no tap, no registry read and no
`allow_any`; its transcript says SUBMITTED and never "delivered", because a
sender gets no delivery guarantee; its explicit refusal door can report a
pre-handler failure. `ConsoleEngine` stays a separate trusted
host/debug lens that *can* say delivered
([terminal](reference/terminal.md)) · a transaction id is never readiness authority (PR-04, ANS-05) · `Committed`
becomes true only inside the admission dispatch (PR-07) · a candidate receives
preparation conversation before activation, production never (PR-01, PR-08) ·
activation is not answerable (LIFE-05) · a Sense is a **latest claim**, visible
at the claim call, predicting nothing about queued work (SENSE-02) — and holding
a role attaches no claim, exactly as it attaches no speech (SENSE-04) · role
movement never relabels a predecessor's office claim; it stamps it stale
(SENSE-03) · reading a Sense needs its own **observe rule**, absent by default —
a send rule is never consulted (SENSE-05) · authored handoff added **no Loom
API**: migration is an authored transformation before admission, never coercion
inside the gate (HANDOFF-01) · `pump_pending()` is the bounded
host-loop turn and leaves newly enqueued work for the next one, while
`drain_until_idle()` keeps going until the queue is empty and says so in its name; there is
no `pump()` or `run()` (MSG-09) ·
a native callback that throws propagates to the host and poisons nothing — the
bus restores its own state, the failed envelope is consumed with no outcome
recorded, and a deferred answer already minted survives (MSG-10) · an observer
may add or remove observers mid-notification: additions join the next event,
removals take effect at once (MSG-11) · `unregister_weave` returns `nullptr`
without mutating anything when the id names the weave whose callback is running,
and the host may simply retry after it exits — while a **different** weave is
still removable during that same callback (LIFE-06) · a registered schema is **not** kept forever — a
Registry retains one while a live claim requires it and drops it from lookup
after the last release, so a shape whose last acceptor left stops resolving
(LIFE-08); "reclaimed" means undiscoverable, never destroyed, and a
`RemoteConsole`'s learned mirror is deliberately outside that guarantee
([known-seams](reference/known-seams.md#a-remote-consoles-learned-schema-mirror-is-still-append-only)) ·
a value's serialized size does not bound
what decoding it costs — a compact encoding may legitimately stand for many
values, so decode carries its own shared, host-owned materialization budget
(GATE-02, [reference/bounds.md](reference/bounds.md#the-decode-materialization-bound)) ·
replacement
preserves nothing and
produces **no atomic incumbent snapshot** — a captured description can go
stale before admission unless a stronger domain/package boundary prevents
mutation (PR-09, known-seams § continuity) · current role *membership* is a
live lookup (`role_holder`); office **authorship** is a stamped delivery fact
(`mail.authored_from_role`, MSG-07) that only an explicit `as_role(...)` act
produces — a role-holder's ordinary message still proves nothing about
speaking *as* the office, since holding is necessary and not sufficient
(MSG-04, MSG-07) · the candidate supplies the domain answer, the
**coordinator** maps it to Ready/Refused, and the Switchboard authenticates
only the conversation (PR-04) · role-authorship evidence lives in
[evidence/night-lab.md](evidence/night-lab.md#role-authored-provenance--five-sightings-workarounds-priced)
· admission-at-dispatch rationale lives in
[decisions/admission-and-activation-share-one-boundary.md](decisions/admission-and-activation-share-one-boundary.md).

**Build and test:** [`AGENTS.md`](../AGENTS.md) at the repository root. **Taking an issue**
labelled `ready` to a pull request: [contributing/taking-an-issue.md](contributing/taking-an-issue.md);
**best practices**, by subsystem: [contributing/best-practices.md](contributing/best-practices.md).
