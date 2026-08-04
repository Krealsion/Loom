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
| message authority, dispatch, roles | reference/messaging.md | MSG-01..07 | tests/test_switchboard.cpp | history/pre-r2c/DESIGN.md §Switchboard |
| native callback boundaries (a handler or observer that throws; mutating the tap list mid-notification) | reference/messaging.md#dispatch-model, reference/messaging.md#observation | MSG-10, MSG-11 | tests/test_switchboard.cpp (STF-1 sections) | — |
| answers, deferral, provenance | reference/messaging.md#answers | ANS-01..07 | tests/test_provenance.cpp | decisions/readiness-is-authenticated-conversation.md |
| office authorship (speaking as a role) | reference/messaging.md#office-authorship-role-authored-provenance | MSG-07, MSG-04 | tests/test_role_authorship.cpp, test_kernel.cpp (v5), test_isolation.cpp (pipe) | decisions/office-authorship-is-deliberate.md, evidence/night-lab.md |
| lifecycle, zen.Activated | reference/lifecycle.md | LIFE-01..05 | tests/test_provenance.cpp, test_manager.cpp | decisions/lifecycle-authority-is-loom-owned.md, decisions/committed-activation-is-not-answerable.md |
| permanent removal during a callback (a weave, or its host, unregistering the weave whose handler is running) | reference/lifecycle.md#permanent-removal-and-the-active-callback | LIFE-06 | tests/test_switchboard.cpp, test_kernel.cpp (R2F-B cases) | — |
| grants, isolation, powerbox | reference/capabilities.md | GATE-03, MSG-02 | tests/test_capabilities.cpp, test_isolation.cpp, test_policy.cpp | history (B1–B5, P1–P2) |
| dynamic loading, artifacts | reference/kernel.md, reference/dynamic-abi.md | KERN-01..04 | tests/test_kernel.cpp | evidence/night-lab.md (answer-seam) |
| prepared replacement | guides/replacing-a-service.md, reference/prepared-replacement.md | PR-01..09 | tests/test_kernel.cpp (R2B-3*/4a sections) | decisions/admission-and-activation-share-one-boundary.md, decisions/no-rollback-after-committed-production.md, evidence/night-lab.md |
| senses (latest claims) | guides/observing.md, reference/senses.md | SENSE-01..05 | tests/test_sense.cpp, test_kernel.cpp (R2E-0 sections) | decisions/a-claim-is-not-a-message.md |
| authored handoff (continuity across an incompatible schema) | reference/handoff.md | HANDOFF-01..03, PR-09 | tests/test_handoff.cpp | decisions/migration-is-authored-not-inferred.md |
| event-loop composition | reference/messaging.md#bounded-dispatch | MSG-09 | tests/test_switchboard.cpp, test_bridge.cpp (R2E-0 sections) | Codex Rule Garden finding 1 |
| Timer continuity | Zengine/docs/reference/timer-continuity.md | TIMER-01..05 | Zengine/tests/test_timer.cpp | Zengine/docs/decisions/timer-continuity-carries-remaining-duration.md |
| known limitations | reference/known-seams.md | MSG-04, ANS-02, PR-09 | — | evidence/night-lab.md |
| bounds/capacities | reference/bounds.md | — | grep the constant name | — |

**Do-not-assume answers** (each resolvable from the row above): a
transaction id is never readiness authority (PR-04, ANS-05) · `Committed`
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
still removable during that same callback (LIFE-06) · a value's serialized size does not bound
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
