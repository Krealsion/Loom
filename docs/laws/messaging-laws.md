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
  merely *holding* R could never supply it (the same weave, holding the same
  office, may speak personally or as the office; holding is necessary, not
  sufficient). Deliberately speaking **as** R is its own explicit act with its
  own law: [MSG-07](#msg-07--role-authorship-is-explicit).

PROVEN BY — delivery-time resolution: suite `switchboard`; the
destination/authorship orthogonality: suite `role_authorship` (all four
combinations, and the two roles of an office-to-office send never conflating).

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
- that an ordinary publication attests an office — office-authored publication
  is the explicit act ([MSG-07](#msg-07--role-authorship-is-explicit)), whose
  one authorship moment coincides with this law's one recipient-pick moment.

PROVEN BY — `Switchboard::fanout`; suites `switchboard`, `kernel`
(publication-across-admission ordering).

## MSG-07 — Role authorship is explicit

LAW — A delivery carries role-authored provenance only when the sender
deliberately requested authorship as R **and** Loom verified at that moment
that the sender held R. Holding R alone attaches nothing.

MEANS
- personal and office speech by the same WeaveId differ, and a recipient can
  tell (`mail.authored_from_role(R)` / `mail.authored_role()`);
- office identity can survive replacement: a successor legitimately authors as
  the inherited role, a predecessor's *new* attempt refuses
  (`RoleAuthorshipDenied` — never a silent downgrade to personal speech);
- the recipient need not query current role topology — the delivery already
  carries the historical fact, stamped at the authorship moment and never
  recomputed from later topology;
- publication can carry office provenance, to every recipient, with
  "authorship refused" and "authorized, zero recipients" kept distinct
  (`OfficePublication`);
- the fact crosses the dynamic seam both ways (ABI v5), with the host
  verifying membership — a library requests, never attests.

DOES NOT MEAN
- role R widens grants — every delivery is still authorized against the
  sender's ordinary grant, and every independent delivery law (sender life,
  the seal, routing) still applies;
- role destination implies role authorship (MSG-04 is a different fact);
- the current role holder necessarily authored old speech — the fact is
  history, not a claim about now;
- payload role strings have standing — provenance has no wire form, and a
  copied Message re-sent is personal speech;
- a successor inherits predecessor identity — the office survives its
  officeholder precisely *without* conflating the two.

PROVEN BY — `Switchboard::office_send_as` / `office_send_to_role_as` /
`office_publish_as` (`holds_role_now` at enqueue); suite `role_authorship`
(the hostile matrix, the replacement matrix, the definition-of-done program);
suite `kernel` (dynamic parity, v4 refusal); suite `isolation` (fail-closed
across the pipe); Night Lab follow-up `followups/role-authorship`.
