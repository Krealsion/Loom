# Terminology — the canonical vocabulary

One definition per term; every other document links here rather than redefining.
Bold marks the normative reference for each.

| Term | Meaning | Normative home |
|---|---|---|
| **Loom** | the substrate: gate, bus, kernel, isolation, console. One `loom::` namespace. The Loom is everyone's | [reference/](reference/) |
| **weave** | the unit of authorship-that-composes: a bus participant, native or loaded, whole and part at once | [guides/writing-a-weave.md](guides/writing-a-weave.md) |
| **weaver** | the human maker. Vocabulary only — deliberately no type in the code (identity is a deferred phase) | [reference/known-seams.md](reference/known-seams.md#deferred-with-intent-the-standing-trigger-map) |
| **schema / shape** | a frozen `(name, version)` + fields; identity = content-id | [reference/values-and-admission.md](reference/values-and-admission.md) |
| **the gate / admission** | `admit()`, the sole conformance authority at every boundary | [laws/admission-laws.md](laws/admission-laws.md) |
| **`Unverified`** | parsed-but-unproven bytes; no accessors | [reference/values-and-admission.md](reference/values-and-admission.md) |
| **grant** | a weave's authorization: which shapes it may send where. Default empty | [reference/capabilities.md](reference/capabilities.md) |
| **`Mail` / `WeaveBus`** | the only bus a handler holds: stamps the weave's identity, routes gated | [reference/messaging.md](reference/messaging.md) |
| **role** | a named singleton capability slot; sends resolve to its holder at delivery. Destination, never office | [laws/messaging-laws.md](laws/messaging-laws.md) (MSG-04) |
| **office authorship** | a delivery fact: the sender deliberately spoke *as* a role it held, verified by Loom at the authorship moment (`as_role` / `authored_from_role`). Holding attaches nothing | [laws/messaging-laws.md](laws/messaging-laws.md) (MSG-07) |
| **publish** | fan-out to alive, unsealed, accept-matching weaves chosen at enqueue | [laws/messaging-laws.md](laws/messaging-laws.md) (MSG-06) |
| **ask** | a delivered request whose handler holds one answer opportunity | [laws/answer-authority-laws.md](laws/answer-authority-laws.md) (ANS-01) |
| **answer** | the one authorized response to an ask; provenance is a delivery fact (`answers_ask()`) | [laws/answer-authority-laws.md](laws/answer-authority-laws.md) |
| **deferred answer** | the same one right, retained by the exact incarnation that earned it; bounded Loom-wide (64) | [laws/answer-authority-laws.md](laws/answer-authority-laws.md) (ANS-02) |
| **correlation** | a conversation-naming number; identifies, never authenticates | [laws/answer-authority-laws.md](laws/answer-authority-laws.md) (ANS-05) |
| **life / incarnation** | two counters: a life ends at death; an incarnation ends at code replacement behind a stable id | [reference/lifecycle.md](reference/lifecycle.md) |
| **lifecycle authority** | the host-minted, board-relative capability to attest lifecycle facts | [laws/lifecycle-laws.md](laws/lifecycle-laws.md) (LIFE-04) |
| **`zen.Activated`** | Loom's authenticated statement that a new incarnation committed at this address — exactly that | [laws/lifecycle-laws.md](laws/lifecycle-laws.md) (LIFE-01) |
| **sealed candidate** | a loaded, real weave with no public standing, conversing only with its coordinator | [laws/replacement-laws.md](laws/replacement-laws.md) (PR-01) |
| **incumbent** | the current role holder a replacement names; never told, live throughout | [reference/prepared-replacement.md](reference/prepared-replacement.md) |
| **candidate** | the sealed would-be successor | [reference/prepared-replacement.md](reference/prepared-replacement.md) |
| **operator** | the participant that begins/commits/aborts a replacement and owns its outcome | [reference/prepared-replacement.md](reference/prepared-replacement.md) |
| **coordinator** | the participant that prepares the candidate and offers its answer | [reference/prepared-replacement.md](reference/prepared-replacement.md) |
| **prepared replacement** | the verified-successor ceremony: seal → transaction → readiness → admission | [reference/prepared-replacement.md](reference/prepared-replacement.md) |
| **Preparing / Ready / AdmissionPending / Committed / Aborted** | the transaction states; `AdmissionPending` = scheduled, world unchanged | [laws/replacement-laws.md](laws/replacement-laws.md) (PR-07) |
| **admission** | the one dispatch that moves topology *and* delivers activation, as one event | [laws/replacement-laws.md](laws/replacement-laws.md) (PR-08) |
| **graceful swap / the letter** | the legacy continuity ceremony: `PrepareShutdown` → `Bequest`/`ClaimBequest`, talking to the outgoing holder | [reference/lifecycle.md](reference/lifecycle.md) |
| **Sense / latest claim** | a deliberate immutable claim of the latest observation a participant made available; read synchronously, by value, carrying truthful authorship. Never "current state" | [reference/senses.md](reference/senses.md) (SENSE-01) |
| **claim-set / `Claims<...>`** | the Senses a weave declares it can claim; registered at mount, enforced at claim time. Not `Accept`, not `Emit` | [laws/sense-laws.md](laws/sense-laws.md) (SENSE-04) |
| **observe rule** | the grant rule authorizing a reader to pull a shape's latest claim; absent by default, never a send rule | [laws/sense-laws.md](laws/sense-laws.md) (SENSE-05) |
| **revision** | monotonic per Sense key; orders replacement of *that* claim. Not a clock, not comparable across keys | [reference/senses.md](reference/senses.md) |
| **authored handoff** | the domain pattern by which one incarnation's meaning deliberately becomes a successor's: boundary → final value → authored migration → prepared replacement. Not a Loom API | [reference/handoff.md](reference/handoff.md) (HANDOFF-01) |
| **migrator** | an ordinary, temporary weave that transforms one schema identity into another; inspectable, testable, versioned, refusable, attributable, and unloaded afterwards | [laws/handoff-laws.md](laws/handoff-laws.md) (HANDOFF-01) |
| **handoff boundary** | an ordinary domain message at an exact FIFO position, after which the domain's declared policy applies. Loom gives it no standing | [laws/handoff-laws.md](laws/handoff-laws.md) (HANDOFF-02) |
| **bounded pump** | `pump_pending()`: exactly the backlog present at entry, then control returns; work enqueued during the turn waits for the next. `pump()` is unchanged and still drains to empty | [laws/messaging-laws.md](laws/messaging-laws.md) (MSG-09) |
| **handoff / remaining duration** | the Timer's letter: schedule progress as durations, never due times | TIMER-03 — Zengine repo, `docs/laws/timer-laws.md` |
| **`TimerReady`** | the Timer package's service announcement, after its continuity decision | TIMER-04 — Zengine repo, `docs/laws/timer-laws.md` |
