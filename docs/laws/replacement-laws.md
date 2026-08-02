# Prepared-replacement laws (PR)

Reference: [prepared-replacement](../reference/prepared-replacement.md) ·
guide: [replacing a service](../guides/replacing-a-service.md).

## PR-01 — Candidate isolation

LAW — Before admission, a prepared candidate has no public standing.

MEANS
- the candidate is registered, loaded and real — and sealed to its exact
  coordinator;
- it holds no role, receives no publications, and an ordinary send to it is
  refused as `NoSuchTarget` — deliberately indistinguishable from an
  unregistered id, so the world cannot learn it exists;
- its own speech reaches only its coordinator; anything else is a visible
  `SealedSpeech` refusal.

DOES NOT MEAN
- that the candidate receives no messages — preparation is a private
  conversation across the seal;
- that the preparation conversation is public admission.

PROVEN BY — `WeaveRecord::sealed_by` + the routing paths themselves; suite
`kernel` (seal cases, hostile-witness candidate that *tries* to escape).

## PR-02 — One bounded transaction, exactly owned

LAW — A replacement transaction belongs to exact participant lives, is bounded,
names one incumbent and one candidate exclusively, and produces exactly one
terminal outcome for the exact operator that began it.

MEANS
- bounds are published: 8 concurrent transactions, budget ≤ 1024 steps, 16
  retained outcomes; capacity refuses before anything is touched;
- one transaction per incumbent AND per candidate;
- outcomes are evidence, not authority — consumed once, by the operator life
  that began the transaction; a successor inherits nothing.

DOES NOT MEAN
- that the budget is a clock — it is an explicit authored step
  (`tick_preparation`), and nothing else spends it.

PROVEN BY — the transaction registry in `switchboard.cpp`; suite `kernel`
(bounds, exclusivity, exact-once outcome, budget-of-one ceremony).

## PR-03 — Admission recognizes its exact owner

LAW — A candidate may be admitted only while the exact coordinator life and
incarnation that sealed it still stands — checked when admission is scheduled
and again when it dispatches.

MEANS
- a coordinator that died, revived, or was reloaded is a different participant
  at the same address, and its successor inherits neither the candidate nor
  the admission;
- a stale queued admission cannot land on a namesake loaded in the candidate's
  place — every participant travels as an exact life + incarnation.

DOES NOT MEAN
- that possessing a transaction id or a lifecycle authority alone can admit —
  every named precondition must hold.

PROVEN BY — `admission_blocked` (one function, asked at both moments); suite
`kernel` (drift-before-dispatch cases).

## PR-04 — Readiness is the candidate's authenticated answer

LAW — A transaction becomes `Ready` only by consuming the exact sealed
candidate's authenticated answer to that transaction's one preparation ask.

MEANS
- the deciding fact is the ask's own bus-private envelope identity, carried
  into the answer — never the correlation (a number a sender chooses), never a
  host assertion (`mark_candidate_ready` was withdrawn);
- one ask, one answer: a second offer meets `WrongState` (the state already
  moved), a forged or mis-addressed offer meets `InvalidReadiness` and
  consumes nothing;
- the bus authenticates **that** the candidate answered **that** ask; the
  coordinator is trusted to map the domain answer (`StationReady` etc.) onto
  Ready/Refused — the bus never reads the payload's meaning.

DOES NOT MEAN
- that a hostile offer can end a legitimate transaction — command refusals
  never terminalize;
- that immediate and deferred readiness differ — one definition, riding the
  same rails.

PROVEN BY — `accept_preparation_answer` (every term from the live delivery);
suite `kernel` (forged/wrong-handle/replayed readiness).

## PR-05 — Activation precedes candidate-reachable production

LAW — The admission envelope is placed immediately ahead of the first queued
envelope that could reach the candidate; nothing is dropped and unrelated FIFO
is untouched.

MEANS
- role traffic queued before commit still lands correctly on whichever side of
  the boundary it belongs to (resolution is delivery-time);
- ordering is never bought with silence.

DOES NOT MEAN
- head insertion (which would break unrelated FIFO), or a
  committed-but-activating parking state.

PROVEN BY — the placement scan in `schedule_admission`; suite `kernel`
(ordering cases with mixed queued traffic).

## PR-06 — Terminalization is exact-once, by ordering

LAW — Ending a transaction removes it from the active registry *first*, then
records exactly one outcome, then performs cleanup that may re-enter.

MEANS
- the invalidation hook re-entering during cleanup finds nothing to end twice;
- ending discards a still-sealed candidate (substrate law — whoever loaded it),
  which releases its artifact.

DOES NOT MEAN
- that a "currently finishing" flag exists — the non-reentrancy is structural,
  true at one place instead of honored at every call site.

PROVEN BY — `finish_txn`; suite `kernel` (every abort route → one outcome).

## PR-07 — Commit schedules; only the dispatch commits

LAW — A successful commit request means the admission is *scheduled*. The
transaction is `AdmissionPending` — a real, observable state — until the
admission envelope dispatches, and `Committed` becomes true only inside that
dispatch, after topology actually moved.

MEANS
- between commit and dispatch: the incumbent is still the service, the
  candidate still sealed, the slot still held, exclusivity still active, abort
  still possible (after which the queued envelope finds nothing and revives
  nothing);
- a caller distinguishes scheduled / committed / refused — never `ok` for
  "perhaps later";
- Kernel queries tell the truth in both windows.

DOES NOT MEAN
- that `AdmissionPending` may be hidden or renamed by any layer above (the
  facade's `state()` delegates every time).

PROVEN BY — `TxnState::AdmissionPending` + `deliver_admission`; suite `kernel`
(pending-window cases, abort-while-pending).

## PR-08 — No production before committed activation

LAW — No production delivery reaches the candidate before its committed
activation. Activation is the candidate's first delivery **as a live
participant**.

MEANS
- the admission envelope IS the activation: one dispatch revalidates, admits
  the activation through the candidate's own gate, *then* moves topology,
  terminalizes, and delivers — nothing runs between;
- a candidate that cannot receive its activation (no contract, gate refusal)
  is not admissible, refused before anything moves;
- after `Committed` is reported, no delivery-time check (grant, sender life,
  target, gate) can refuse the activation — "publicly admitted but never told"
  is unrepresentable.

DOES NOT MEAN
- that activation is the first delivery *ever* — the preparation conversation
  necessarily reaches the sealed candidate first, outside public standing;
- that a committed activation is coordinator speech or an ask (see
  [LIFE-05](lifecycle-laws.md#life-05--a-committed-activation-is-not-answerable)).

PROVEN BY — `PendingAdmission` on the envelope; suite `kernel` (first-breath
vertical, original missing-grant reproducer, no-post-commit-refusal
assertions).

## PR-09 — Replacement verifies the successor, not continuity

LAW — Prepared replacement verifies the incoming candidate. It does not
automatically preserve incumbent work or state — and it tells the incumbent
nothing.

MEANS
- the incumbent remains live and serving throughout preparation; a description
  taken during that window is a **snapshot**, not an atomic final handoff;
- what (if anything) crosses a replacement is an authored, domain-owned
  decision — work, obligation, intent, a reopened question, or nothing
  (six independent Night Lab applications each answered differently);
- the Timer package is the one domain that required an exact final boundary,
  and built it *on top* of this substrate (its letter is written after
  admission freezes the incumbent).

DOES NOT MEAN
- that `Describe → hand-over` is a substrate feature — it is a domain pattern
  filling a real hole, and the hole is the honest shape (see
  [known-seams](../reference/known-seams.md#continuity-is-authored));
- that graceful swap (the letter ceremony) and prepared replacement are one
  mechanism — they are disjoint ceremonies talking to opposite parties.

PROVEN BY — the substrate carries no incumbent snapshot anywhere in the
transaction; evidence: [Night Lab](../evidence/night-lab.md)
(describe-then-hand-over, six sightings); Zengine suite `timer` (the authored
exact-boundary case).
