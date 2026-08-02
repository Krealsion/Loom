# CONTEXT — the machine router

Routing, not prose. Retrieve the files for your topic; do not preload
everything. Vocabulary is canonical in [terminology.md](terminology.md);
**normative** = `reference/` + `laws/` (current truth); `decisions/` = why;
`history/` = frozen record (never normative unless a law cites it);
`evidence/` = what applications found (never the API contract).

**Ownership:** Loom (this repo) owns substrate truth. Zengine owns
package truth (Timer/Input/Surface) — `Zengine/docs/`. Night Lab
(github.com/Krealsion/zen-night-lab, pinned `bf09f79`) is read-only evidence.

**Recommended order for a cold start:** this file → the topic row below →
the named laws → the reference page → tests when exactness matters.

| Topic | Read | Laws | Tests | Why / evidence |
|---|---|---|---|---|
| values, schemas, the gate | reference/values-and-admission.md | GATE-01..04 | tests/test_gate.cpp, test_schema.cpp, test_registry.cpp, test_serialize.cpp | decisions/one-gate-at-every-boundary.md |
| message authority, dispatch, roles | reference/messaging.md | MSG-01..06 | tests/test_switchboard.cpp | history/pre-r2c/DESIGN.md §Switchboard |
| answers, deferral, provenance | reference/messaging.md#answers | ANS-01..07 | tests/test_provenance.cpp | decisions/readiness-is-authenticated-conversation.md |
| lifecycle, zen.Activated | reference/lifecycle.md | LIFE-01..05 | tests/test_provenance.cpp, test_manager.cpp | decisions/lifecycle-authority-is-loom-owned.md, decisions/committed-activation-is-not-answerable.md |
| grants, isolation, powerbox | reference/capabilities.md | GATE-03, MSG-02 | tests/test_capabilities.cpp, test_isolation.cpp, test_policy.cpp | history (B1–B5, P1–P2) |
| dynamic loading, artifacts | reference/kernel.md, reference/dynamic-abi.md | KERN-01..04 | tests/test_kernel.cpp | evidence/night-lab.md (answer-seam) |
| prepared replacement | guides/replacing-a-service.md, reference/prepared-replacement.md | PR-01..09 | tests/test_kernel.cpp (R2B-3*/4a sections) | decisions/admission-and-activation-share-one-boundary.md, decisions/no-rollback-after-committed-production.md, evidence/night-lab.md |
| Timer continuity | Zengine/docs/reference/timer-continuity.md | TIMER-01..05 | Zengine/tests/test_timer.cpp | Zengine/docs/decisions/timer-continuity-carries-remaining-duration.md |
| known limitations | reference/known-seams.md | MSG-04, ANS-02, PR-09 | — | evidence/night-lab.md |
| bounds/capacities | reference/bounds.md | — | grep the constant name | — |

**Do-not-assume answers** (each resolvable from the row above): a
transaction id is never readiness authority (PR-04, ANS-05) · `Committed`
becomes true only inside the admission dispatch (PR-07) · a candidate receives
preparation conversation before activation, production never (PR-01, PR-08) ·
activation is not answerable (LIFE-05) · replacement preserves nothing and
produces **no atomic incumbent snapshot** — a captured description can go
stale before admission unless a stronger domain/package boundary prevents
mutation (PR-09, known-seams § continuity) · current role *membership* is a
live lookup (`role_holder`), but no delivery fact proves office **authorship**
— a role-holder's ordinary message proves nothing about speaking *as* the
office, since holding is necessary and not sufficient (MSG-04, known-seams §
role-authored-provenance) · the candidate supplies the domain answer, the
**coordinator** maps it to Ready/Refused, and the Switchboard authenticates
only the conversation (PR-04) · Timer transfers remaining duration because a
due timestamp cannot cross clocks/downtime (TIMER-03) · `TimedWeave` bindings
are authored, reconciled at activation/TimerReady only (TIMER-05) · role-authorship evidence lives in
[evidence/night-lab.md](evidence/night-lab.md#role-authored-provenance--five-sightings-workarounds-priced)
· admission-at-dispatch rationale lives in
[decisions/admission-and-activation-share-one-boundary.md](decisions/admission-and-activation-share-one-boundary.md).

**Build/test:** see [`AGENTS.md`](../AGENTS.md) at the repo root.
