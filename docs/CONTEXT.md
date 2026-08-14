# CONTEXT — the machine router

Routing, not prose. Retrieve the files for your topic; do not preload
everything. Vocabulary is canonical in [terminology.md](terminology.md);
**normative** = `reference/` + `laws/` (current truth); `decisions/` = why;
`history/` = frozen record (never normative unless a law cites it);
`evidence/` = what applications found (never the API contract).

**Ownership:** Loom (this repo) owns substrate truth. Zengine owns
package truth (Timer/Input/Surface) — `Zengine/docs/`. Night Lab
(github.com/Krealsion/zen-night-lab: marathon pinned `bf09f79`, the
role-authorship follow-up beside it) is read-only evidence.

**Recommended order for a cold start:** this file → the topic row below →
the named laws → the reference page → tests when exactness matters.

| Topic | Read | Laws | Tests | Why / evidence |
|---|---|---|---|---|
| values, schemas, the gate | reference/values-and-admission.md | GATE-01..04 | tests/test_gate.cpp, test_schema.cpp, test_registry.cpp, test_serialize.cpp | decisions/one-gate-at-every-boundary.md |
| how long a schema stays resolvable (what a claim is, who holds one, what a rejected candidate leaves, why a remote console's mirror is different) | reference/values-and-admission.md#registry, reference/bounds.md#schema-registry-bounded-by-claims-not-by-a-number | LIFE-08 | tests/test_registry.cpp (BL-0 cases), test_switchboard.cpp, test_kernel.cpp | COLD-1/COLD-2 finding C-10 / F-9 |
| message authority, dispatch, roles | reference/messaging.md | MSG-01..07 | tests/test_switchboard.cpp | history/pre-r2c/DESIGN.md §Switchboard |
| native callback boundaries (a handler or observer that throws; mutating the tap list mid-notification) | reference/messaging.md#dispatch-model, reference/messaging.md#observation | MSG-10, MSG-11 | tests/test_switchboard.cpp (STF-1 sections) | — |
| answers, deferral, provenance | reference/messaging.md#answers | ANS-01..07 | tests/test_provenance.cpp | decisions/readiness-is-authenticated-conversation.md |
| office authorship (speaking as a role) | reference/messaging.md#office-authorship-role-authored-provenance | MSG-07, MSG-04 | tests/test_role_authorship.cpp, test_kernel.cpp (the ABI-v5 office cases), test_isolation.cpp (pipe) | decisions/office-authorship-is-deliberate.md, evidence/night-lab.md |
| lifecycle, zen.Activated | reference/lifecycle.md | LIFE-01..05 | tests/test_provenance.cpp, test_manager.cpp | decisions/lifecycle-authority-is-loom-owned.md, decisions/committed-activation-is-not-answerable.md |
| permanent removal during a callback (a weave, or its host, unregistering the weave whose handler is running) | reference/lifecycle.md#permanent-removal-and-the-active-callback | LIFE-06 | tests/test_switchboard.cpp, test_kernel.cpp (R2F-B cases) | — |
| long-lived transport channels (a framed channel's retained send/receive buffers; a peer that keeps up but never lets the socket run dry) | reference/bounds.md#transport-channels-framed-byte-channels | LIFE-07 | tests/test_bridge.cpp, test_isolation.cpp (R2F-C cases) | — |
| what a console retains (the tap window, the m1/m2/... reply buffer, why an `mN` label can refuse, how eviction is surfaced) | reference/bounds.md#console-operator-history | — | tests/test_console.cpp, test_bridge.cpp (C-1 cases) | COLD-2 finding C-1 |
| what the HOST knows about the bus RIGHT NOW (the recorder: what one record holds, how delivery truth and retention truth are kept apart, the four honest answers to "what became of seq N", the last-call store vs. the recent FIFO vs. the protected window, per-shape retention policy, the two budgets, the structural blacklist) | reference/history.md, reference/bounds.md#recorder-host-working-memory | MSG-10 | tests/test_history_recorder.cpp | RTH-0 (what Zen remembered before), RTH-1, RTH-1a |
| what the host CHOSE NOT TO FORGET (the logger: whitelist selection, the conservative default, per-shape caps and why there is no global one, the three durable origins and why a diagnostic may never pretend to be a message) | reference/history.md#logger--the-durable-selected-record, reference/bounds.md#logger-durable-record | MSG-10 | tests/test_history_logger.cpp | RTH-1a |
| grants, isolation, powerbox | reference/capabilities.md | GATE-03, MSG-02 | tests/test_capabilities.cpp, test_isolation.cpp, test_policy.cpp | history (B1–B5, P1–P2) |
| changing a LIVE subject's message authority — what baseline/delegated/effective mean, who may administer one (a host-minted `GrantAuthority`), the ceiling and how attenuation is decided, and why OS/filesystem/resource reach is not in it | reference/capabilities.md#live-delegation-grant-0 | GATE-05, GATE-03 | tests/test_grant.cpp | GRANT-0 |
| putting a HUMAN in that loop — the Weaver (the first policy delegate), the operator seat, the authority-request vocabulary, what approval does and deliberately does not do, and why the Weaver keeps no permission store | reference/weaver.md | GATE-05, MSG-02, ANS-01..07 | tests/test_weaver.cpp | WEAVER-1 |
| giving that human HANDS — a terminal session (an ordinary participant with one identity, a supplied vocabulary and its own transcript), what "submitted" does and does not mean, how several outstanding asks are told apart by Loom's own correlation, why the transcript is not fed by the tap, and how one presentation shows two identities without merging them | reference/terminal.md | MSG-01..07, ANS-01..07, GATE-05 | tests/test_terminal.cpp | TERM-0 |
| the sandbox exec boundary — what a child inherits vs. what Zen authors: ambient descriptors (a host socket, file, pipe or terminal already open at mount) and the child's environment (`LD_*`, tokens, session addresses) | reference/capabilities.md#the-exec-boundary-three-independent-facts | — | tests/test_isolation.cpp (C-2, C-2a cases) | COLD-2 finding C-2 |
| the remote-operator bridge — what it trusts, whether a socket may be exposed, where connection identity comes from, what it validates and what it deliberately does not | reference/bridge.md | MSG-02, GATE-01, MSG-09, LIFE-07 | tests/test_bridge.cpp | COLD-1/COLD-2 finding F-4 |
| dynamic loading, artifacts | reference/kernel.md, reference/dynamic-abi.md | KERN-01..05 | tests/test_kernel.cpp | evidence/night-lab.md (answer-seam) |
| constructing the C ABI tables (which slots share a type, what catches a miswire vs. what only looks like it does, the checklist for adding a field) | reference/dynamic-abi.md#constructing-the-tables-bl-4 | KERN-04 | tests/test_kernel.cpp (BL-4 case) | BL-4 |
| what running a `dlopen`ed weave in-process costs (address space, and what the grant does *not* bound) | guides/dynamic-weaves.md#what-loading-it-in-process-means, reference/capabilities.md | — | tests/test_kernel.cpp | COLD-1/COLD-2 finding F-1 |
| building a loadable weave | guides/dynamic-weaves.md, reference/kernel.md | KERN-05, POP-05 | tests/check_weave_contract.cmake, tests/weave_population.cmake, tests/check_weave_population.cmake | COLD-2 finding C-3 |
| prepared replacement | guides/replacing-a-service.md, reference/prepared-replacement.md | PR-01..09 | tests/test_kernel.cpp (R2B-3*/4a sections) | decisions/admission-and-activation-share-one-boundary.md, decisions/no-rollback-after-committed-production.md, evidence/night-lab.md |
| senses (latest claims) | guides/observing.md, reference/senses.md | SENSE-01..05 | tests/test_sense.cpp, test_kernel.cpp (R2E-0 sections) | decisions/a-claim-is-not-a-message.md |
| authored handoff (continuity across an incompatible schema) | reference/handoff.md | HANDOFF-01..03, PR-09 | tests/test_handoff.cpp | decisions/migration-is-authored-not-inferred.md |
| event-loop composition | reference/messaging.md#bounded-dispatch | MSG-09 | tests/test_switchboard.cpp, test_bridge.cpp (R2E-0 sections) | Codex Rule Garden finding 1 |
| Timer continuity — **not in this repo**; owned by the separate Zengine repository, and unreachable from a Loom-only checkout | Zengine repo: `docs/reference/timer-continuity.md` | TIMER-01..05 | Zengine repo: `tests/test_timer.cpp` | Zengine repo: `docs/decisions/timer-continuity-carries-remaining-duration.md` |
| what a green run means (suite/case/enforcement populations, declared absence, the opt-out, the required build-artifact population) | laws/population-laws.md, ../AGENTS.md | POP-01..05 | tests/suite_population.txt, tests/entry_population.txt, tests/check_population.cmake, tests/check_entry_population.cmake, tests/verify.cmake, tests/enforcement_gate.hpp, tests/weave_population.cmake | — |
| how the `isolation` granted-network positive control proves the sandbox is a sandbox and not a muzzle — the endpoint the test binds and owns, why the positive witness is a successful connect plus a token byte rather than a particular errno, and why BL-VER-07's WSL2 mirrored-networking exception is RETIRED (a failure there is now a NEW failure) | reference/capabilities.md#the-granted-network-positive-control-and-the-endpoint-it-uses, ../AGENTS.md | POP-04 | tests/test_isolation.cpp, tests/weavelib/test_weave.cpp | BL-VER-08 (the replacement witness), BL-VER-07 + TERM-0-RB §66 (the retired exception) |
| known limitations | reference/known-seams.md | MSG-04, ANS-02, PR-09 | — | evidence/night-lab.md |
| bounds/capacities | reference/bounds.md | — | grep the constant name | — |

**Do-not-assume answers** (each resolvable from the row above): the Bridge
**does not authenticate** — `authorize_connection` grants full operator
authority to every reachable connection, so reachability of the socket *is*
the authority, and a bridge listener must not be exposed to an untrusted
network ([bridge](reference/bridge.md#authentication-posture)) · an
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
sender is not told its send's fate; and `ConsoleEngine` stays a separate trusted
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
inside the gate (HANDOFF-01) · `pump()` still drains to empty; `pump_pending()`
is the bounded turn and leaves newly enqueued work for the next one (MSG-09) ·
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
(MSG-04/MSG-07) · the candidate supplies the domain answer, the
**coordinator** maps it to Ready/Refused, and the Switchboard authenticates
only the conversation (PR-04) · Timer transfers remaining duration because a
due timestamp cannot cross clocks/downtime (TIMER-03) · `TimedWeave` bindings
are authored, reconciled at activation/TimerReady only (TIMER-05) · role-authorship evidence lives in
[evidence/night-lab.md](evidence/night-lab.md#role-authored-provenance--five-sightings-workarounds-priced)
· admission-at-dispatch rationale lives in
[decisions/admission-and-activation-share-one-boundary.md](decisions/admission-and-activation-share-one-boundary.md).

**Build/test:** see [`AGENTS.md`](../AGENTS.md) at the repo root.
