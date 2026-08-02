# Messaging laws (MSG)

Reference: [messaging](../reference/messaging.md).

## MSG-01 — Single-threaded, FIFO, non-reentrant

LAW — Dispatch is single-threaded FIFO. `pump()` is non-reentrant; a handler's
sends enqueue *later* deliveries, never nested ones.

MEANS
- ordering is deterministic;
- "between two deliveries" is a real atomic boundary the substrate itself uses
  (admission executes whole inside one queue turn);
- there are no locks because there is nothing to lock.

DOES NOT MEAN
- that concurrency is supported and merely untested — multi-threaded dispatch
  is an explicit non-feature until a consumer forces it.

PROVEN BY — `Switchboard::pump` (`in_dispatch_` guard); suites `switchboard`
(reentrancy cases), `kernel` (admission atomicity riding the queue turn).

## MSG-02 — The bus stamps the sender

LAW — Sender identity on any weave-originated delivery is stamped by the bus
(the `WeaveBus`/connection), never read from the payload or the wire.

MEANS
- a weave can only speak as itself; `send_as` is host root authority;
- forged `reply_to`/sender fields in payloads are inert;
- out-of-process and bridge frames get their sender from the connection.

DOES NOT MEAN
- that the stamp says anything about *office*: see MSG-04.

PROVEN BY — `WeaveBus` (the only bus a handler holds); suites `switchboard`,
`isolation` (stamped-from-connection), `bridge` (forged wire frame loses).

## MSG-03 — A message belongs to a life

LAW — A weave-originated message is stamped with its author's life at enqueue
and refused (`SenderLifeEnded`) if that life has ended by delivery.

MEANS
- a dead, removed, or revived-since author's queued speech dies with the life;
- checked before the grant, so a stale message reaches nothing at all.

DOES NOT MEAN
- that a live code *reload* invalidates queued speech — incarnation and life
  are two counters, deliberately (reload continues a life; revival starts one).

PROVEN BY — `Envelope::sender_life`; suite `provenance` (life/incarnation
cases).

## MSG-04 — Role addressing is destination, not office

LAW — `send_to_role(R, m)` means *deliver to whoever holds R at delivery time*.
It proves nothing about the sender having spoken *as* R.

MEANS
- resolution happens at delivery, so queued role traffic follows the topology;
- authorization for role sends is by role name (stable across replacement).

DOES NOT MEAN
- that a message *from* a role-holder carries any attestation of that office —
  **role-authored provenance does not exist**, and merely *holding* R could
  never supply it anyway (the same weave, holding the same office, may speak
  personally or as the office; holding is necessary, not sufficient). This is
  the system's most evidence-backed open seam (five independent Night Lab
  sightings). See
  [known-seams](../reference/known-seams.md#role-authored-provenance).

PROVEN BY — delivery-time resolution: suite `switchboard`; the *absence* half is
documented, not pinned — see the seam entry and its evidence links.

## MSG-05 — Refusals are structured and observable

LAW — Every refused delivery is recorded in the journal and emitted to
observers with a named `RefusalReason`; a refused delivery reaches nobody.

MEANS
- silence is never an outcome the bus produces on its own initiative;
- reasons are distinct because they send an operator to distinct fixes.

DOES NOT MEAN
- that the *sender* is asynchronously told — a weave cannot observe its send's
  eventual fate without asking; see
  [known-seams](../reference/known-seams.md#sender-cannot-observe-send-fate).

PROVEN BY — `record`/`emit` on every refusal path; suites `switchboard`,
`provenance` (reason-exactness cases).

## MSG-06 — Publication picks recipients at enqueue

LAW — `publish()` fans out to the weaves alive, unsealed and accept-matching at
*enqueue* time, one independently gated delivery each.

MEANS
- a publication from before an admission is not retroactively the successor's;
- sealed candidates are invisible to the world's news;
- zero recipients is legal, not an error.

DOES NOT MEAN
- that publication is attestable — an observer of a publication can never prove
  who held what office (part of the MSG-04 seam).

PROVEN BY — `Switchboard::fanout`; suites `switchboard`, `kernel`
(publication-across-admission ordering).
