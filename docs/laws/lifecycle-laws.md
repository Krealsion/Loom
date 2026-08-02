# Lifecycle laws (LIFE)

Reference: [lifecycle](../reference/lifecycle.md).

## LIFE-01 — `zen.Activated` means exactly one thing

LAW — `zen.Activated{sequence}` means *a new code incarnation has successfully
committed at this address*, and nothing else.

MEANS
- ten non-meanings are pinned in the header: not healthy, not ready, not a
  role-holder, state not preserved, no predecessor implied, not graceful,
  resources not promised, no loop commanded, no work re-commanded, system not
  ready;
- the one-field minimality is itself pinned — widening the shape means
  deleting a test that says why not.

DOES NOT MEAN
- "start your main loop" — what a weave does with its first breath is the
  weave's own business.

PROVEN BY — `include/zen/weave/lifecycle.hpp`; suite `weave` (minimality),
`provenance`.

## LIFE-02 — Participation is declared, never attempted

LAW — A weave receives lifecycle facts iff it lists the shape in its accepted
schemas. Nobody sends blindly and calls the refusal "optional".

MEANS
- a non-participant costs nothing: no message, no manufactured refusal, no
  sequence spent.

DOES NOT MEAN
- that declaring the shape grants anything beyond hearing it.

PROVEN BY — the control door asks `Kernel::accepts` first; suite `manager`.

## LIFE-03 — The sequence is finite and honest

LAW — Activation sequences are monotonic within an operator lineage, never
reused after an aborted attempt, and the allocator refuses at the boundary
rather than wrapping.

MEANS
- consumer identity is the pair (attested sender, sequence) — per attesting
  operator, checked by the consumer's cursor;
- gaps are legal (a scheduled-then-refused admission spends its number);
- at `INT64_MAX` or a negative revived lineage the door refuses, naming the
  boundary — it does not brick.

DOES NOT MEAN
- that sequences are globally unique across operators, or that Loom owns
  allocation (the operator supplies the number).

PROVEN BY — `activation_block()` preflight; suites `manager`, `kernel`
(sequence passthrough and gap cases).

## LIFE-04 — Lifecycle authority is Loom-owned and board-relative

LAW — Only trusted host infrastructure holding this Loom's `LifecycleAuthority`
can attest lifecycle facts. A grant to *emit* `zen.Activated` is not authority
to *attest* it.

MEANS
- the mint is private and non-static on the Switchboard, reachable through one
  friend in a host-wiring header no weave-authoring header includes;
- an authority names its issuing board and is refused elsewhere
  (`ForeignAuthority`) — anyone may own a Switchboard; nobody thereby owns
  yours;
- an ordinary `zen.Activated`, however well-shaped and well-granted, carries no
  attestation and consumers ignore it as a lifecycle fact.

DOES NOT MEAN
- that the shape is secret, or that sending it is forbidden — provenance, not
  payload type, is the distinction.

PROVEN BY — `LifecycleAuthority` + `issued_here`; suites `provenance` (decoy
board, impostor), `kernel`.

## LIFE-05 — A committed activation is not answerable

LAW — The activation delivered as part of an admission is Loom's own act, not a
send: no ordinary grant is consulted, and its handler holds **no** answer
authority — nobody asked anything.

MEANS
- `mail.answer()` inside that handler refuses visibly; `mail.defer_answer()`
  returns an invalid capability and consumes no bounded capacity;
- ordinary sends from inside the handler work normally — not-answerable is not
  mute (a prepared Timer's first act is an ordinary claim send);
- a later real ask is answerable normally: authority is scoped per delivery,
  never removed from the weave.

DOES NOT MEAN
- that `announce_lifecycle` changed — that is still an ordinary gated send with
  ordinary answer semantics;
- that `zen.Activated`-shaped ordinary messages became unanswerable — the
  distinction is provenance, not shape.

PROVEN BY — `deliver_admission` sets no reply authority; suite `kernel`
("first breath is not a question" cases, held-full capacity proof).
