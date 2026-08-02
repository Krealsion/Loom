# Night Lab — the application evidence

**Repository:** <https://github.com/Krealsion/zen-night-lab> · pinned commit
**`bf09f79`** · built against Loom `78d64ea` / Zengine `f6a4c69` (ABI v4).
Two experiments live side by side: `original/` (Night One, preserved against
its own older substrate) and `marathon/` (Night Two — six applications, all
green: 159 cases / 848 assertions, 86 mutations + 6 canaries). Reports:
`marathon/FINAL-REPORT.md`, `marathon/EVIDENCE.md`, `marathon/FRICTION.md`.

The six: `kitchen-replay` · `download-manager` · `build-farm` ·
`import-pipeline` · `lobby` · `scheduler` (which replaces the **Zengine
Timer** itself through the Timer package's own vocabulary).

## The facade held — 178 : 0

178 semantic `loom::PreparedReplacement` operations across six applications;
**zero** raw prepared-replacement calls in application code. Meaning: the
authoring handle is done; proposals to extend it start from this number.
→ concept: [prepared-replacement](../reference/prepared-replacement.md).

## Role-authored provenance — five sightings, workarounds priced

Escalating consequence chain: a dish nobody cooked → an operation ended by a
guessed correlation → a fabricated "build succeeded" → a healthy build
destroyed by a forged `WorkerOpen` (in the farm, an announcement is evidence)
→ a lobby player sent to an attacker's server. Both application-level
workarounds were built and measured: a registry (its belief rests on an
unauthenticated announcement) and push-to-pull inversion (works; spends the
Loom-wide deferral bound, strands strict receivers across honest replacement,
covers no observers). The import pipeline is the control: identity works when
you talk to *somebody*; it fails when you talk to *whoever holds an office*.
Meaning: independent applications need trusted office authorship.
→ seam: [known-seams § role-authored provenance](../reference/known-seams.md#role-authored-provenance)
(status: evidence-backed core candidate). Sightings: lobby `MatchCreated`,
build-farm `JobDone` and `WorkerOpen`, kitchen and download cases —
`marathon/EVIDENCE.md`.

## Describe-then-hand-over — six of six, and it is a hole

Every project that needed continuity across a replacement independently
invented the same bridge (ask the incumbent to describe itself during the
preparation window; hand the description to the candidate) — and every project
carried something *different* (work / obligation / intent / a reopened
question / a waiting-fact / a fleet tally). Meaning: the repeated thing is a
**domain hole, not a missing substrate helper**; whether continuity is
possible at all is a property of the domain.
→ law: [PR-09](../laws/replacement-laws.md) · guideline:
[known-seams § continuity](../reference/known-seams.md#continuity-is-authored).

## Rejected proposals — zero sightings from six

- an **activation-sequence owner** (one operator, one counter, nothing
  contended — even in the program replacing two services);
- **outcome-observation ergonomics** (`state()`/`take_outcome()` were pleasant
  every time; not one line of glue).

Meaning: do not build these on the strength of old hypotheses.

## Also carried forward

- **One attestation per operation** — you choose which half of a two-sided
  exchange to defend (asserted in both directions as a case).
- **`reply_to` found no natural use** across six applications — every response
  wanted an answer (provable) or a role send (replacement-surviving). Kept and
  documented; see [messaging](../reference/messaging.md#answers).
- **The seal is a security property**: of five possible speakers of a stale
  claim, the retired predecessor is the one the substrate silences completely.
- **`TimedWeave` bindings are authored** — the scheduler priced the dynamic
  case and the raw protocol carried it
  ([TIMER-05](../../../Zengine/docs/laws/timer-laws.md)).
- **Minted identities need surviving namespaces** — three sightings, two
  defects → the [guideline](../reference/known-seams.md#minted-identity-needs-a-surviving-namespace).
- `Mail::answer()` across the seam and `TimedWeave`-vs-activation, found by
  Night One, are **closed** (ABI v4; `on_timed_activation`) — re-tested in the
  marathon, not assumed (`marathon/repro_answer_seam.cpp`).
