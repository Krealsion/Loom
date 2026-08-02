# Replacing a Service Safely

A live service holds a role. You have a successor you'd like in its place — without
stopping the world, without a moment where nobody answers, and without ever standing
a successor up that was never told it went live. `loom::PreparedReplacement` is the
host-side handle for exactly that.

```cpp
#include <zen/host/prepared_replacement.hpp>

loom::PreparedReplacement upgrade(bus, kernel);

auto started = upgrade.start({
    .operator_id    = operator_id,   // who owns this replacement (and its outcome)
    .coordinator    = coordinator,   // who talks the candidate through preparation
    .role           = "storage",
    .candidate_name = "storage-v2",
    .candidate_path = storage_v2_path,
    .budget         = 16,
});
if (!started) { /* see "When something refuses" below */ }
```

`start` resolves the role's current holder as the incumbent, loads the candidate
**sealed** — outside the world, able to talk only to the coordinator — and opens one
replacement transaction around the two. If any of that refuses, nothing changed:
a candidate the handle loaded is removed again, and the incumbent never noticed.

The incumbent keeps serving through everything below. Nothing here disturbs it.

## Ask, and let the candidate answer for itself

```cpp
upgrade.ask(PrepareStorage{ .schema_version = 3 });
```

The ask is your domain vocabulary — the handle does not care what's in it, and it
does **not** need a transaction id field. Delivery, identity, and which conversation
an answer belongs to are the bus's facts, not the payload's.

The candidate handles the ask like any message, and answers it:

```cpp
void on(const PrepareStorage& request, loom::Mail& mail) {
    if (!can_serve(request.schema_version)) {
        mail.answer(StorageRefused{"unsupported schema"});
        return;
    }
    mail.answer(StorageReady{});
}
```

If preparation takes longer than one delivery, the candidate takes the answer right
with it (`mail.defer_answer()`) and spends it when it finishes. Your coordinator
code does not change either way.

## Offer the answer — the bus judges

In the coordinator's handler for the candidate's answer:

```cpp
void on(const StorageReady&, loom::Mail&) {
    auto offered = upgrade.offer_current_answer(loom::PreparationAnswer::Ready);
    if (!offered) { report(offered.why); }
}
```

`offer_current_answer` does not declare the candidate ready — it offers *the
delivery currently being handled* to the transaction's readiness gate, and the
Switchboard proves the rest: that this is an authenticated answer, from the exact
sealed candidate, to this transaction's exact ask, heard by the exact coordinator.
Offered from the wrong handler, the wrong coordinator, or the wrong handle, it
refuses and nothing moves.

The division of labor is worth keeping straight: **the candidate authentically
supplies the domain answer** (`StorageReady`, `StorageRefused` — the bus never
reads what it means), and **the coordinator maps that answer to the
transaction's Ready or Refused verdict**. A refusing candidate is offered the
same way, as `PreparationAnswer::Refused` — the transaction ends as
`CandidateRefused`, and the incumbent simply continues.

## Commit is a decision — and it means *scheduled*

```cpp
if (upgrade.state() == loom::TxnState::Ready) {
    upgrade.commit(next_activation_sequence());
}
```

`state()` is always the transaction's real state, read from the Switchboard —
`Preparing`, `Ready`, `AdmissionPending`, `Committed`, or `Aborted`. Nothing is
cached, and nothing commits by itself: a candidate reaching `Ready` changes nothing
until *you* decide.

A successful `commit(sequence)` means the admission is **scheduled** — not done.
`state()` now reads `AdmissionPending`, and the incumbent is still the service.
When the queue reaches the admission (pumping is yours; the handle never pumps),
one dispatch does the whole thing: the candidate is told it is live — first, before
any production reaches it — and the role moves, as one event. If the world drifted
in between (a participant died, the role moved), the admission refuses instead, and
the incumbent is still the service. `commit` never over-reports.

The activation sequence stays yours to supply, exactly as with the raw primitive.

## Collect the outcome

```cpp
if (auto outcome = upgrade.take_outcome()) {
    if (outcome->state == loom::TxnState::Committed) {
        // v2 really entered the world, and was told so, in the same breath.
    } else {
        report(outcome->reason);   // the exact reason, e.g. CandidateRefused
    }
}
```

Exactly one outcome exists, it belongs to the operator that started the
replacement, and this handle collects only its own — never a sibling
replacement's. Until something has actually ended, `take_outcome()` is empty.

## When something refuses

Every refusal keeps the substrate's own words. `start` failures name their stage —
`NoRoleHolder`, `CandidateLoad` (with the loader's error), `BeginTransaction` (with
the exact `TxnReason`, e.g. `IncumbentBusy`) — and every other operation returns
the transaction's own result, reasons untranslated: `PreparationAlreadyAsked`,
`PreparationExhausted`, `InvalidReadiness`, `CandidateRefused`, and the rest.

Two habits worth keeping:

- **The handle is not the transaction.** Dropping it aborts nothing, unloads
  nothing, pumps nothing. Abort is a decision: `upgrade.abort()`.
- **The budget is yours.** `upgrade.tick()` spends exactly one preparation unit;
  nothing else spends any.

## What crosses a replacement? You decide — nothing crosses by itself

Prepared replacement **verifies the incoming candidate. It does not
automatically preserve incumbent work or state** — and it tells the incumbent
nothing. The incumbent stays live and serving through the whole preparation, so
anything you ask it about itself during that window is a **snapshot of a moving
service, not an atomic final handoff**.

Whether continuity is even possible is a property of *your domain*, and every
domain answers differently:

```text
build farm         carries stable intent; the successor restarts work
kitchen            tolerates stale work through idempotency
download manager   carries an obligation snapshot; degrades honestly
Timer              cannot tolerate a stale moving snapshot at all —
                   so it authored an exact final boundary on top
```

The common shape (reached independently by every application that needed
one): ask the incumbent to *describe* itself with an ordinary, non-mutating
domain question, and supply that description to the candidate — often captured
*before* the successor is even loaded, sometimes during preparation; either
way it is a snapshot the incumbent may outrun. That is an application pattern
filling a real hole — not a substrate feature — and what belongs in the
description is your call:
[PR-09](../laws/replacement-laws.md) and
[known-seams § continuity](../reference/known-seams.md#continuity-is-authored)
carry the full story, including when the *graceful* ceremony (which preserves
work but verifies nothing) fits better.

## Underneath

The handle is composition, not a second implementation: every operation delegates
to one `Switchboard`/`Kernel` primitive (`begin_prepared_replacement`,
`ask_candidate_to_prepare`, `accept_preparation_answer`,
`commit_prepared_replacement`, `take_outcome`, `Kernel::load_candidate`). Those
primitives remain public and fully supported — reach for them directly when you
need something the handle deliberately doesn't do, such as a nonstandard
activation payload. `zen/switchboard/switchboard.hpp` documents them.
