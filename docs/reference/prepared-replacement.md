# Prepared replacement — reference

The substrate for replacing a live role-holding service with a verified
successor, with no moment where the world has changed but the successor has not
been told. Laws: [PR-01..09](../laws/replacement-laws.md). Authoring surface:
[`loom::PreparedReplacement`](#the-authoring-handle) — the
[guide](../guides/replacing-a-service.md) leads with it.

## Participants

| Party | Holds | Meaning |
|---|---|---|
| operator | begins/aborts/commits; owns the outcome | the decision-maker |
| coordinator | the preparation conversation; offers the answer | talks the candidate through |
| incumbent | the role, throughout preparation | never told anything |
| candidate | sealed to the exact coordinator | outside the world until admitted |

Every participant is bound as an exact **(id, life, incarnation)** at `begin`;
any drift aborts or refuses — a successor at the same address is a different
participant.

## States

```text
Preparing ──ask/answer──► Ready ──commit──► AdmissionPending ──dispatch──► Committed
    │                        │                    │
    └────────────────────────┴────────────────────┴──────────► Aborted
```

- **Preparing** — the transaction exists; the budget (an authored step counter,
  not a clock) may be spent via `tick_preparation`; the one preparation ask may
  be opened.
- **Ready** — the candidate's authenticated answer was consumed. Nothing
  commits automatically; the operator decides.
- **AdmissionPending** — commit *scheduled* the admission envelope. The
  incumbent is still the service, the candidate still sealed, abort still
  possible. Real and observable; no layer may hide it.
- **Committed / Aborted** — terminal, exact-once, recorded for the exact
  operator life (16 retained, oldest dropped; consumed once).

Bounds: 8 concurrent transactions · budget ≤ 1024 · one active transaction per
incumbent *and* per candidate. Capacity refuses before anything is inspected.

## The preparation conversation

One per transaction. `ask_candidate_to_prepare(id, msg)` sends the ask **as**
the bound coordinator through the seal (ordinary grant applies), with a
correlation Loom mints. The candidate answers through the ordinary answer doors
(immediately, or deferring and spending later — one definition of readiness,
not two). `accept_preparation_answer(id, Ready|Refused)`, called from inside
the coordinator's delivery of that answer, consumes it: the deciding fact is
the ask's own bus-private envelope identity carried into the answer — never the
correlation, never the payload. The bus authenticates **that** the exact
candidate answered **that** ask; mapping the domain payload onto
Ready/Refused is the coordinator's trusted judgment.

`Refused` terminalizes with the candidate's own reason (`CandidateRefused`).
Hostile or mis-addressed offers refuse the command (`InvalidReadiness`) and
terminalize nothing.

## Admission

`admit_candidate(...)` — also the direct host primitive — **schedules**: it
validates everything (this Loom's `LifecycleAuthority`; exact sealed owner
still standing; role held by the exact incumbent; the candidate can receive
this exact activation through its own door and gate) and places **one
envelope that is both the admission and the activation**, immediately ahead of
the first queued envelope that could reach the candidate. It returns
`AdmitResult{scheduled, why, ticket}` — `scheduled`, deliberately not `ok`.

At dispatch, in one queue turn with nothing between any two steps:
revalidate → admit the activation payload through the candidate's gate →
seal the incumbent for retirement, unseal the candidate, move the role →
terminalize `Committed` → deliver the activation. If the world drifted, the
envelope refuses as `AdmissionRevoked` and *nothing* moved — the incumbent is
still the service. After `Committed` is reported, no delivery-time check can
refuse the activation; "publicly admitted but never told" is unrepresentable.

The activation is **Loom's act, not a send**: enqueued ungated, authorized by
the lifecycle authority checked at scheduling; the coordinator's ordinary
`Emit<zen.Activated>` grant is never consulted. The stamped sender names *who
admitted* (for the consumer's per-operator lineage rule), not who spoke. Its
handler holds **no answer authority** ([LIFE-05](../laws/lifecycle-laws.md)).

Failure direction, always: the incumbent remains public, unsealed, serving;
the candidate remains sealed or is discarded (which releases its artifact);
one terminal outcome; Kernel queries agree in both windows.

## What replacement does NOT do

It verifies the successor. It does **not** preserve incumbent work or state,
and it tells the incumbent nothing ([PR-09](../laws/replacement-laws.md)).
Continuity is an authored, domain-owned decision; the preparation window (the
one interval where the incumbent is alive *and* the successor reachable) is
where applications build it. See
[known-seams § continuity](known-seams.md#continuity-is-authored).

## The authoring handle

`zen/host/prepared_replacement.hpp` — `loom::PreparedReplacement`, a move-only
host-side handle bound to one transaction; every operation delegates to one
primitive above. `start()` resolves the incumbent from the role once, loads
the candidate sealed, begins — and unloads a candidate *it* loaded exactly once
if begin refuses (a caller-brought candidate is never destroyed by a failed
start). `commit(seq)` wraps the authority + standard `Activated{seq}` wiring;
ok = scheduled. `state()` asks the Switchboard every time. `take_outcome()`
consumes only this transaction's result. Dropping a live handle aborts
nothing, unloads nothing, pumps nothing.

Evidence of fit: six Night Lab applications drove 178 semantic facade
operations with zero raw calls ([evidence](../evidence/night-lab.md)). Known
authoring friction (not missing truth): the handle is host-owned while
`offer_current_answer` must run inside the coordinator's delivery, so
coordinators hold a host-provided handle reference — see
[known-seams](known-seams.md#preparedreplacement-hostcoordinator-friction).

## Tests

`tests/test_kernel.cpp` — the seal, transaction, readiness, admission,
first-breath, and facade sections; Zengine `tests/test_timer.cpp` — a live
service crossing the boundary.
