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
- the fact crosses the dynamic seam both ways (since ABI v5), with the host
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
suite `kernel` (dynamic parity, the previous-ABI refusal); suite `isolation` (fail-closed
across the pipe); Night Lab follow-up `followups/role-authorship`.

## MSG-08 — A Loom-owned rejection is observable where Loom owns it

LAW — Every rejection Loom performs leaves a fact on a surface Loom owns. A
refusal that no recipient, no sender and no tap can see is not a refusal; it is
silence, and silence is a defect.

MEANS
- the dynamic seam's pre-enqueue rejections — an unresolvable claimed shape, and
  a gate refusal of the bytes — get a seq, a journal slot and a tap event, at
  exactly the altitude a capability refusal already had;
- `SeamUnresolved` is its own reason because every neighbour would mislead:
  `NotAccepted` blames a target's accept-set when routing never ran,
  `GateRefused` blames the payload when no schema existed to judge it against,
  and `NoSuchTarget` blames an address that was never consulted;
- the fact carries the **claimed** (name, version) and the sending artifact, and
  a target **only where one was actually named** — a publication names none, a
  role is a slot resolved at a delivery that never happened. Replacing silence
  with a fabricated target would be the worse bug;
- the observability floor is now the same on both tiers: a loaded weave's
  unresolvable reach and a native weave's unresolvable reach are both loud, and
  a comparable native failure is not reclassified.

DOES NOT MEAN
- that senders observe delivery fate — they still do not
  ([known-seams](../reference/known-seams.md#sender-cannot-observe-send-fate)).
  Nothing is returned to the sender that was not already returned, no ticket
  crosses the seam, and no future, retry or dead letter exists;
- that a diagnostic is an answer — it creates no provenance and no delivery;
- that failures are reported twice. These fire only on the pre-enqueue path;
  anything that passes both checks is queued and reported once by ordinary
  delivery, so no path produces both.

PROVEN BY — `Switchboard::note_seam_refusal` (deliberately `refuse_now`'s body,
not a second mechanism); `kernel.cpp`'s one `seam_reject` helper behind all eight
seam entry points; suite `kernel` (the Night Lab III P-011 reproducer against a
real artifact, the native-still-loud control, the no-false-refusal case).
Evidence: [night-lab](../evidence/night-lab.md), reproducer
`workshop-marathon/repros/core/silent-seam-emission/`.

## MSG-09 — A dispatch turn can be bounded without bending FIFO

LAW — `pump()` drains to empty, unchanged and forever. **One** bounded turn
exists beside it: `pump_pending()` dispatches exactly the backlog present at
entry. Both return how many they dispatched, and neither reorders anything.

MEANS
- a host composing Loom with an outer event loop has a deterministic way to get
  control back, so a perpetual in-process service (a repeating Timer re-arms
  itself inside its own handler, so the queue never empties) cannot starve the
  socket poll;
- `pump_pending()` takes `pending()` **once, at entry**. That snapshot is the
  whole contract:
  - the backlog that existed at entry is dispatched, in FIFO order;
  - work enqueued *during* the turn — including a handler's own continuation —
    lands behind the snapshot and belongs to the **next** turn, which is exactly
    what stops a self-re-arming producer from holding the turn open;
  - a busy bus therefore clears its whole backlog in one turn rather than being
    throttled;
- **the bound is a work boundary, not a quota.** It is a count taken from the
  queue, never a deadline — a core primitive whose result depends on host timing
  jitter cannot be reasoned about;
- a bound is a pause between two deliveries, landing exactly where `pump()` was
  already between two envelopes;
- `stop()` still ends the turn early, and the return value reports what actually
  happened;
- non-reentrant, exactly as `pump()`; an empty queue is a no-op, not a drain.

DOES NOT MEAN
- that `pump()` changed. Every existing caller keeps drain-to-empty, and
  `BridgeServer` defaults to that same contract until a host says otherwise;
- that Loom acquired a thread or a scheduler. It did not, and neither is planned;
- that the substrate chose a bound. The primitive is the Switchboard's because
  only the queue's owner can bound dispatch without reordering; the **policy** is
  the host's, because only the host knows what it is composing with;
- that `pump_pending()` is unbounded because it takes no number. Its bound is a
  fact about the queue, read before anything runs, and a producer cannot extend
  it from inside the turn;
- that a **numeric** budget is available. It was tried and withdrawn — see below.

A NUMERIC BUDGET WAS TRIED AND REJECTED. R2E-0 first shipped `pump_bounded(n)`
and `BridgeServer::set_dispatch_budget(n)`, counting deliveries dispatched. The
consumer disproved it: with a real Zengine Timer, `pump_bounded(64)` throttled
the Codex Rule Garden 17× (2s → 34s), and a budget large enough not to throttle
was drain-to-empty again with the starvation back. A count sized against a
*producer's* rate, chosen from the *consumer's* side, is the same class of
fragility as a deadline approached from the other direction. Both surfaces were
removed in R2E-0a; the count survives only as `pump_pending`'s private
implementation. The experiment is kept as history, not as API — see
[`../decisions/`](../decisions/).

PROVEN BY — `Switchboard::pump_pending`; `BridgeServer::set_bounded_dispatch`;
suite `switchboard` (the starvation reproduction, the entry-snapshot bound
against a self-re-arming producer, the finite backlog cleared whole, FIFO and
`stop()` across the boundary, the empty-queue no-op, reentrancy, and `pump()`
still draining to empty), suite `bridge` (a bridge host staying responsive —
accepting and welcoming an operator — while a perpetual driver runs, and the
default still draining). Evidence: Codex Rule Garden finding 1, and its
follow-up — the fake `GardenYieldPump → bus.stop()` message is deleted, replaced
by `set_bounded_dispatch()`, and the live round-trip runs in the same 2s it did
with the fake.

## MSG-10 — A callback that throws costs the delivery, not the bus

LAW — Loom calls two kinds of code it did not write: a native `Weave::handle`,
and a host observer. An exception escaping either **propagates to the host
caller** — and Loom restores its own temporary dispatch and delivery state
before it leaves. That a handler did not finish is **announced once, as a fact**,
and the exception then continues on its way.

MEANS
- the exception is not swallowed, not translated into a `RefusalReason`, and not
  answered by terminating the process; no weave is quarantined for having thrown;
- **the tap is told, exactly once, that the handler did not complete**
  (`EventKind::HandlerFailed`, RTH-1). The event carries the delivery's ordinary
  facts and the payload, and NO reason — Loom has none to give. It is emitted
  after the delivery scope is torn down, exactly where a `Delivered` event would
  have been, and the exception is rethrown immediately afterwards. Before this
  the throwing path emitted nothing at all, so an observer's record of the bus
  read as though the delivery had never happened;
- **the loaded-weave seam says the same word.** A `dlopen`ed weave's exception is
  caught at the ABI boundary and crosses back as a status; the Kernel's
  `HostAdapter` reports a non-OK status through `Switchboard::note_handler_failure()`
  and the bus emits `HandlerFailed` rather than `Delivered`. The exception still
  never reaches the Switchboard — only the fact does;
- the dispatch flag is restored, so the bus does not spend the rest of its life
  believing itself reentrant. A later `pump()` or `pump_pending()` dispatches
  normally, and the escaped exception is not a permanent poisoning;
- the ambient delivery context — who is being dispatched, its one reply
  authority, and the facts about what was just delivered — is cleared on the
  throwing path exactly as on the returning one. Nothing that runs afterwards
  inherits a finished delivery's right to answer, nor its standing as an
  authenticated readiness answer ([PR-04](replacement-laws.md));
- the throwing envelope is **consumed**: it was taken off the queue before
  dispatch, so it is not silently retried. Everything queued behind it is still
  queued and is delivered by the next turn;
- an observer that throws stops that event's remaining notifications and
  propagates — ordinary C++ behaviour, kept because Loom has no error channel on
  which to report the exception it would otherwise be swallowing.

DOES NOT MEAN
- that Loom says what the failed delivery *did*. Its journal slot stays
  `Pending` — "no outcome was recorded", neither delivered nor refused, **on both
  seams** — and `HandlerFailed` says only that the handler was entered and did
  not complete. The queue is what says the message was consumed, and STF-1
  restores state rather than inventing an outcome;
- that an observer is safe from the exception. The `HandlerFailed` emission runs
  before the rethrow, so an observer that throws from it **replaces** the
  handler's exception with its own — ordinary C++ propagation, and the same trade
  this law already makes for an observer that throws on any other event;
- that a handler's effects are rolled back. There is no transaction: sends it
  already enqueued stay enqueued and are delivered by the next turn. (A claim it
  already made likewise stands — [SENSE-02](sense-laws.md) says so, and says it
  there.);
- that a **deliberately minted** capability is revoked. A `DeferredAnswer` taken
  before the throw is durable state the handler asked Loom for, not ambient
  authority, and stays spendable ([ANS-02](answer-authority-laws.md)). Only the
  ambient right dies with the frame;
- that the dynamic seam propagates. A loaded weave's exceptions are still caught
  at the ABI boundary and never reach the Switchboard
  ([dynamic-abi](../reference/dynamic-abi.md)); what crosses is a status, and what
  the bus does with it is announce `HandlerFailed`. Nothing is rethrown there, so
  a pump over a throwing loaded weave still returns normally;
- that Loom is exception-safe in any wider sense, or thread-safe in any sense.
  Dispatch is still single-threaded
  ([MSG-01](#msg-01--single-threaded-fifo-non-reentrant)).

PROVEN BY — `Switchboard::DispatchGuard` and `Switchboard::DeliveryScope`
(held across `pump`, `dispatch_at_most`, `deliver_one`, `deliver_admission`);
suite `switchboard` (a handler throwing through `pump()` and through
`pump_pending()`; the stale-readiness witness in both its handler and its
observer form; the deferred answer that survives; the throwing observer); suite
`recorder` (the native throw becomes a `HandlerFailed` record while the journal
stays `Pending`); suite `kernel` (a loaded weave whose handler throws — the
`ZEN_WEAVE_THROW_ON_MAGIC` fixture — is `HandlerFailed` on the tap and `Pending`
in the journal, and the pump does not throw).

## MSG-11 — Every event has its own view of the tap list

LAW — `emit()` fixes an event's recipients when the event begins. An observer
**added** during a notification does not receive the event it was added during;
one **removed** during a notification does not receive it either, unless it had
already been called.

MEANS
- an observer may subscribe or unsubscribe from inside its own callback, and
  from another observer's, without undefined behaviour;
- self-removal is safe: the callable outlives the registration that named it,
  for the length of the call;
- removal is honoured **within** the current event, deliberately departing from
  a pure snapshot. `remove_observer` is how the console and the bridge stop a
  callback *before the members it captured die*; a notification that ran the
  removed callback anyway would be a use-after-free. Addition has no such
  pressure, so it waits for the next event;
- growth of the observer list during a notification cannot derail that
  notification — the view is not a pointer into the live container;
- a **nested** event (one an observer causes) takes its own view at its own
  entry, and the observers added before it began are in it.

DOES NOT MEAN
- that notification is transactional. Mutations made before a throw stand;
- that removal is retroactive — an observer already notified for this event
  stays notified;
- that observers gained ordering guarantees beyond registration order, or any
  concurrency guarantee. There are no locks here and none are planned
  ([MSG-01](#msg-01--single-threaded-fifo-non-reentrant)).

PROVEN BY — `Switchboard::emit` (the per-event view; `observers_` held by
`shared_ptr` so a self-removing callback outlives its registration); suite
`switchboard` (add-during-notification, self-removal, removing another,
reallocation pressure, mutation followed by a throw, nested emission).
