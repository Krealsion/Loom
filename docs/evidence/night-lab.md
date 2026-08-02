# Night Lab — the application evidence

**Repository:** <https://github.com/Krealsion/zen-night-lab>. Three
experiments live side by side, each pinned to the substrate it ran on:
`original/` (Night One, its own older Loom), `marathon/` (Night Two at
`bf09f79` — six applications against Loom `78d64ea` / Zengine `f6a4c69`,
ABI v4, all green: 159 cases / 848 assertions, 86 mutations + 6 canaries),
and `followups/role-authorship/` (the R2D-0 replay against Loom `30eab0a`,
ABI v5 — 4 cases / 59 assertions, 3 mutations RED, verdict CLOSED). Reports:
`marathon/FINAL-REPORT.md`, `marathon/EVIDENCE.md`, `marathon/FRICTION.md`,
`followups/role-authorship/REPORT.md`.

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
destroyed by a forged `WorkerOpen` **publication** (in the farm, an
announcement is evidence — and a publication is the case any direct-send-only
authorship design would miss) → a lobby player sent to an attacker's server.
The lobby also supplies the sharpest half of the finding: **the same exact
weave, holding the same exact role, speaking personally versus speaking as
the office** — holding is necessary and not sufficient, so the missing fact
is two-part (the sender held R, *and* deliberately authored this message as
R). Both application-level workarounds were built and measured: a registry
(its belief rests on an unauthenticated announcement) and push-to-pull
inversion (works; spends the Loom-wide deferral bound, strands strict
receivers across honest replacement, covers no observers). The import
pipeline is the control: identity works when you talk to *somebody*; it fails
when you talk to *whoever holds an office*. Meaning: independent applications
need trusted office authorship.

**This evidence forced R2D-0, and the seam is CLOSED.** The follow-up replay
(`followups/role-authorship/` in the same repository, pinned to its own
vendored Loom at ABI v5) re-ran the three sharpest verticals on
`mail.as_role(...)` / `mail.authored_from_role(...)`: the lobby's strict
player joins the office push, rejects the same holder's chatter and every
rogue, and follows the office through a real replacement — with the entire
pull apparatus deleted; the farm's `WorkerOpen` **publication** verifies at
every listener while the rogue's same-shaped one verifies as nothing; and the
download manager attests BOTH halves of its long operation (the acceptance as
the answer, the terminal truth as office speech) with zero deferred slots
parked. → current law:
[MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit);
seam entry (closed):
[known-seams § role-authored provenance](../reference/known-seams.md#role-authored-provenance).
Original sightings: lobby `MatchCreated`, build-farm `JobDone` and
`WorkerOpen`, kitchen and download cases — `marathon/EVIDENCE.md`.

## Describe-then-hand-over — a domain pattern, not one ceremony

Applications that needed to decide what remains true across a replacement
repeatedly expressed that decision by asking the incumbent for an ordinary,
**non-mutating description** and supplying it to the candidate's preparation.
The staging **varied**: several projects captured the description and *then*
loaded and prepared the successor; others asked during preparation. In every
case the description was a **snapshot taken while the incumbent remained
live** — never a transaction-authenticated final handoff — and every project
carried something *different* (work / obligation / intent / a reopened
question / a waiting-fact / a fleet tally): the kitchen tolerated staleness
through idempotency; the download manager's inherited `bytes_done` was an
obligation it could degrade honestly, not the exact incumbent state at
admission; the import pipeline's snapshot could race later requester
activity; the farm carried intent and restarted work. The Timer is the
counterexample: it could not tolerate a stale moving snapshot, so it built an
exact final boundary on the substrate. Meaning: the repeated thing is a
**domain hole, not a missing substrate ceremony**; whether continuity is
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
