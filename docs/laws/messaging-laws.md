# Messaging laws (MSG)

Reference: [messaging](../reference/messaging.md).

## MSG-01 — Single-threaded, FIFO, non-reentrant

LAW — Dispatch is single-threaded FIFO. A dispatch turn is non-reentrant; a
handler's sends enqueue *later* deliveries, never nested ones. What one sender
may have waiting is at most `kMaxPendingPerSender`. A send past that is refused
`PendingPerSender`, queues nothing, and leaves every other sender's waiting
messages untouched.

MEANS
- ordering is deterministic;
- "between two deliveries" is a real atomic boundary the substrate itself uses
  (admission executes whole inside one queue turn);
- there are no locks because there is nothing to lock;
- a weave that sends more messages than it is delivered cannot grow the queue
  without limit: its own next send is refused, by that name, on the tap and in
  the journal ([MSG-05](#msg-05--refusals-are-structured-and-observable));
- messages already waiting stay waiting, in the order they were queued.

DOES NOT MEAN
- that concurrency is supported and merely untested — multi-threaded dispatch
  is an explicit non-feature until a consumer forces it;
- that the bound is a turn quota, or a cap on `drain_until_idle()`
  ([MSG-09](#msg-09--a-dispatch-turn-can-be-bounded-without-bending-fifo)). A
  sender under the bound still enqueues behind what is already waiting, and a
  producer that keeps replacing exactly what was delivered still never lets the
  drain return;
- that one sender at the bound refuses anyone else's send;
- that a dispatch-refusal notice, a joint notice, or a scheduled admission is
  that sender's speech. Those are the bus's own acts and are not counted;
- that an answer, immediate or deferred, is this speech. Each spends a right one
  delivery created, so answers cannot outgrow what was delivered. The bound
  neither counts an answer nor refuses one;
- that a call nests a delivery: it runs no handler and delivers nothing
  ([MSG-14](#msg-14--a-call-is-a-function-of-its-argument-alone)).

PROVEN BY — `Switchboard::drain_until_idle` / `pump_pending` (the shared
`in_dispatch_` guard); `kMaxPendingPerSender`; suites `switchboard`
(reentrancy cases, and a weave that sends two messages for every delivery:
the bound refuses its next send by name while another weave's send is still
delivered), `kernel` (admission atomicity riding the queue turn).

## MSG-02 — The bus stamps the sender

LAW — Sender identity on any weave-originated delivery is stamped by the bus
(the `WeaveBus`/connection), never read from the payload or the wire.

MEANS
- a weave can only speak as itself; `send_as` is host root authority;
- forged `reply_to`/sender fields in payloads are inert;
- out-of-process and bridge frames get their sender from the connection.

DOES NOT MEAN
- that the stamp says anything about *role*: see MSG-04.

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

## MSG-04 — Role addressing is destination, not authorship

LAW — `send_to_role(R, m)` means *deliver to whoever holds R at delivery time*.
It proves nothing about the sender having spoken *as* R.

MEANS
- resolution happens at delivery, so queued role traffic follows the topology;
- authorization for role sends is by role name (stable across replacement);
- each role has one holder, and a holder may hold other roles too
  ([MSG-13](#msg-13--a-weave-answers-to-the-set-of-roles-it-was-bound-to)):
  whoever holds R is reached, whatever else it holds;
- the recipient reads where it was reached: `mail.addressed_role()` is R for a delivery
  addressed to R, resolved to it at delivery, and empty for a directed send and a publication,
  alike for a native, a loaded, a boxed and an isolated weave. It says where the delivery went,
  so a weave holding several roles tells their traffic apart by it.

DOES NOT MEAN
- that a role may have several holders: the role table maps each name to one;
- that a message *from* a role-holder carries any attestation of that role —
  merely *holding* R could never supply it (the same weave, holding the same
  role, may speak personally or as the role; holding is necessary, not
  sufficient). Deliberately speaking **as** R is its own explicit act with its
  own law: [MSG-07](#msg-07--role-authorship-is-explicit);
- that the addressed role is authority, or says who spoke: a handler that branches on it still
  checks authorship and its grants.

PROVEN BY — delivery-time resolution: suite `switchboard`; the
destination/authorship orthogonality: suite `role_authorship` (all four
combinations, and the two roles of a role-to-role send never conflating); the addressed role
read by each kind of weave: suites `seam` (native, loaded), `box` and `isolation`.

## MSG-05 — Refusals are structured and observable

LAW — Every refused delivery is recorded in the journal and emitted to
observers with a named `RefusalReason`; a refused delivery reaches nobody.

MEANS
- silence is never an outcome the bus produces on its own initiative;
- reasons are distinct because they send a weaver to distinct fixes.

DOES NOT MEAN
- that every refusal reaches its sender. An explicit notice acceptor may learn
  an ordinary addressed send's pre-handler refusal under MSG-12; absence proves
  nothing. Disposal of an internal notice is not an ordinary refused send.

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
- that an ordinary publication attests a role — role-authored publication
  is the explicit act ([MSG-07](#msg-07--role-authorship-is-explicit)), whose
  one authorship moment coincides with this law's one recipient-pick moment.

PROVEN BY — `Switchboard::fanout`; suites `switchboard`, `kernel`
(publication-across-admission ordering).

## MSG-07 — Role authorship is explicit

LAW — A delivery carries role-authored provenance only when the sender
deliberately requested authorship as R **and** Loom verified at that moment
that the sender held R. Holding R alone attaches nothing.

MEANS
- personal and role speech by the same WeaveId differ, and a recipient can
  tell (`mail.authored_from_role(R)` / `mail.authored_role()`);
- role identity can survive replacement: a successor legitimately authors as
  the inherited role, a predecessor's *new* attempt refuses
  (`RoleAuthorshipDenied` — never a silent downgrade to personal speech);
- the recipient need not query current role topology — the delivery already
  carries the historical fact, stamped at the authorship moment and never
  recomputed from later topology;
- publication can carry role provenance, to every recipient, with
  "authorship refused" and "authorized, zero recipients" kept distinct
  (`RolePublication`);
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
- a successor inherits predecessor identity — the role survives its
  holder precisely *without* conflating the two.

PROVEN BY — `Switchboard::role_send_as` / `role_send_to_role_as` /
`role_publish_as` (`holds_role_now` at enqueue); suite `role_authorship`
(the hostile matrix, the replacement matrix, the definition-of-done program);
suite `kernel` (dynamic parity, the previous-ABI refusal); suite `isolation` (fail-closed
across the pipe); and the role-authorship replay in [Night Lab](../evidence/night-lab.md).

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
  with a fabricated target would be the worse bug. A role door reports the
  **role it was handed**, which is an address the sender stated and not a
  resolution: it sits beside an invalid target, and the pair reads "where it was
  sent", never "who answered";
- the observability floor is the same on both tiers: a loaded weave's
  unresolvable reach and a native weave's unresolvable reach are both loud, and
  a comparable native failure is not reclassified;
- **an unresolvable PUBLICATION is unheard, not refused**, and that is the same
  floor rather than an exception to it. A weave's accept-set is claimed in the
  registry for its record's whole life and `fanout` selects by the same
  (name, version), so an unresolvable shape has no live accepter *by
  construction*: the publication provably reached nobody, which is what a native
  publication into a world with no listeners does silently every day. Reporting
  the loaded tier and not the native one would make the seam LOUDER than the floor
  this law sets, on the one case where nothing has gone wrong.

DOES NOT MEAN, of that last clause
- that a publication cannot fail. One whose shape RESOLVES and whose bytes fail
  the gate is still refused: somebody accepts that shape and would have been
  handed those bytes, so a delivery that should have happened did not;
- that an addressed reach is ever quiet. A send, a role send, an answer and a
  role send each named somewhere they expected to arrive, so zero arrivals is a
  failure there whatever the reason;
- that the library is told less. The seam's status to the emitting library is the
  same either way; only Loom's own diagnostic is withheld, and `Bus::publish`'s
  recipient count never crosses this seam in either direction.

DOES NOT MEAN
- that a synchronous seam failure becomes a later dispatch notice. MSG-12 begins
  only after an ordinary send has been enqueued; seam diagnostics remain distinct;
- that a diagnostic is an answer — it creates no provenance and no delivery;
- that failures are reported twice. These fire only on the pre-enqueue path;
  anything that passes both checks is queued and reported once by ordinary
  delivery, so no path produces both.

PROVEN BY — `Switchboard::note_seam_refusal` (deliberately `refuse_now`'s body,
not a second mechanism); `kernel.cpp`'s one `seam_reject` helper behind every
addressed seam entry point, and `kUnheardPublication` behind the two publication
doors; suite `kernel` (a real artifact whose unresolvable emission is reported
rather than lost, the native-still-loud control, the no-false-refusal case, and the
three-part publication argument: the native control measured in the same process,
the gate-refused publication that stays loud, and the same shape from the same
seam going quiet only when its address is taken away).
Evidence: [night-lab](../evidence/night-lab.md).

## MSG-09 — A dispatch turn can be bounded without bending FIFO

LAW — Loom offers exactly **two** dispatch turns, and each is spelled for what it
promises. `pump_pending()` dispatches exactly the backlog present at entry and
returns; `drain_until_idle()` keeps dispatching until the queue is empty. Both
honour `stop()`, neither reorders anything, and there is no third.

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
- a bound is a pause between two deliveries, landing exactly where the drain was
  already between two envelopes;
- `drain_until_idle()` is **unbounded by contract**: work enqueued during it
  belongs to it, so it returns only when the participants themselves stop
  producing. Under a perpetual service that is never, and that is the correct
  answer rather than a defect — a caller asking for quiescence in a world that
  will not become quiescent has asked for something that does not exist;
- **the call site says which promise was made.** This is part of the law and
  not presentation: the two names must state the terminating condition each
  turn actually has, because a host chooses between them at the moment it is
  least able to read the substrate's source. There is no `pump()` and no `run()`:
  either would name the drain in words an ordinary C++ reader takes for the
  bounded one, and a first-contact host that reached for the short name would get
  a program that never returns;
- both return how many deliveries they made, and both are non-reentrant; an
  empty queue is a no-op for either, not a drain.

DOES NOT MEAN
- that `BridgeServer` bounds its turns on its own. It drains to idle until a host
  says otherwise;
- that Loom has a thread or a scheduler. It has neither, and neither is planned;
- that the substrate chose a bound. The primitive is the Switchboard's because
  only the queue's owner can bound dispatch without reordering; the **policy** is
  the host's, because only the host knows what it is composing with;
- that `pump_pending()` is unbounded because it takes no number. Its bound is a
  fact about the queue, read before anything runs, and a producer cannot extend
  it from inside the turn;
- that a **numeric** budget is available — see below;
- that `drain_until_idle()` is discouraged. It is the right call for a test or
  script settling a world before it asserts, for a one-shot bootstrap, and for a
  host whose entire program *is* the bus and whose exit is `stop()`.

NO NUMERIC BUDGET. A count of deliveries sized against a *producer's* rate,
chosen from the *consumer's* side, is the same class of fragility as a deadline
approached from the other direction. Measured on a real timer-driven
application, a budget of 64 deliveries a turn made its live round-trip seventeen
times slower (2 s to 34 s), and a budget large enough not to throttle was
drain-to-empty again, with the starvation back. The count exists only as
`pump_pending`'s private implementation.

NOR IS A CAP AN ACCEPTABLE REPAIR OF THE DRAIN, for the same reason: a turn limit
or a wall-clock deadline inside `drain_until_idle()` would convert a semantic
operation into a heuristic and leave the caller with neither promise. The
unbounded operation stays unbounded and says so in its name.

PROVEN BY — `Switchboard::pump_pending`; `Switchboard::drain_until_idle`;
`BridgeServer::set_bounded_dispatch`; suite `switchboard` (the starvation
reproduction, the entry-snapshot bound against a self-re-arming producer, the
finite backlog cleared whole, FIFO and `stop()` across the boundary, the
empty-queue no-op, reentrancy, the drain still draining to empty, one
perpetually productive world left measurably different by each turn, and an
outer host loop keeping control every lap while the service stays healthy),
suite `bridge` (a bridge host staying responsive — accepting and welcoming a
weaver — while a perpetual driver runs, and the default still draining).

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
  (`EventKind::HandlerFailed`). The event carries the delivery's ordinary
  facts and the payload, and NO reason — Loom has none to give. It is emitted
  after the delivery scope is torn down, exactly where a `Delivered` event would
  have been, and the exception is rethrown immediately afterwards. Without it an
  observer's record of the bus would read as though the delivery had never
  happened;
- **the loaded-weave seam says the same word.** A `dlopen`ed weave's exception is
  caught at the ABI boundary and crosses back as a status; the Kernel's
  `HostAdapter` reports a non-OK status through `Switchboard::note_handler_failure()`
  and the bus emits `HandlerFailed` rather than `Delivered`. The exception still
  never reaches the Switchboard — only the fact does;
- the dispatch flag is restored, so the bus does not spend the rest of its life
  believing itself reentrant. A later `pump_pending()` or `drain_until_idle()`
  dispatches normally, and the escaped exception is not a permanent poisoning;
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
  not complete. The queue is what says the message was consumed, and Loom
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
  a dispatch turn over a throwing loaded weave still returns normally;
- that Loom is exception-safe in any wider sense, or thread-safe in any sense.
  Dispatch is still single-threaded
  ([MSG-01](#msg-01--single-threaded-fifo-non-reentrant)).

PROVEN BY — `Switchboard::DispatchGuard` and `Switchboard::DeliveryScope` (held
across `drain_until_idle`, `dispatch_at_most`, `deliver_one`,
`deliver_admission`); suite `switchboard` (a handler throwing through
`drain_until_idle()` and through `pump_pending()`; the stale-readiness witness
in both its handler and its observer form; the deferred answer that survives;
the throwing observer); suite `recorder` (the native throw becomes a
`HandlerFailed` record while the journal stays `Pending`); suite `kernel` (a
loaded weave whose handler throws — the `ZEN_WEAVE_THROW_ON_MAGIC` fixture — is
`HandlerFailed` on the tap and `Pending` in the journal, and the dispatch turn
does not throw).

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

## MSG-12 — A sender may learn an authenticated refusal of its exact send

LAW — Explicit acceptance of zen.DispatchRefused requests a later, Loom-attested
notification when an ordinary directed or role-addressed send is refused before
handler entry. Its return address is the actual author's life and incarnation at
authorship. Every ordinary send path strips caller-supplied attestation.

MEANS

- verified role authorship participates without changing grant or role resolution;
- public reasons preserve authority-before-resolution and candidate concealment;
- the exact queued attempt, never correlation alone, identifies local work;
- notification is FIFO and non-reentrant, at most one per eligible refused send;
- a notice creates no answer right and never generates a notice about itself;
- native and loaded participants share this meaning; unsupported isolated reach refuses.

DOES NOT MEAN

- delivery, success, eventual receipt, cancellation or post-delivery abandonment;
- ordinary queued speech changes its life-only reload rule (MSG-03);
- an in-process image is a native-memory sandbox;
- AskBook becomes a global obligation tracker;
- that an ask is told this notice: an ask through the ask door is told its end,
  `zen.AskEnded`, in the same words for the same refusals (its sibling, ANS-08).

PROVEN BY — Switchboard::capture_refusal_recipient, queue_notice, deliver_one,
and the words' owner `dispatch_refused_value` (zen/switchboard/asks.hpp); suites
dispatch_refusal, dispatch_loaded, terminal, isolation;
installed-package witness. Full representation and disposal contract:
[messaging](../reference/messaging.md#sender-visible-dispatch-refusal).

## MSG-13 — A weave answers to the set of roles it was bound to

LAW — A weave may hold several roles. They are bound at registration, or at a
load, and nowhere else: the whole set, judged before any of it is recorded, and
bound all or none, at most `kMaxRolesPerWeave` of them. A holder may release one
role at any time, and keeps the rest.

MEANS
- an empty name, a name twice, a held name, or a set past the bound refuses the
  registration, and binds none of its names; every incumbent keeps its role;
- an admission policy is asked about the whole set (`AdmissionRequest::roles`),
  so one that allowed a weave one role has not by that allowed it a second;
- a release is the holder leaving that role: traffic addressed to it is refused
  at its turn (`NoSuchTarget`), as to any unheld role; its role claims end
  ([SENSE-05](sense-laws.md#sense-05--reading-is-authorized-and-the-repository-is-bounded));
  a prepared replacement over the holder aborts, `RoleChanged`. The holder's
  other roles, its life and its queued speech are untouched;
- a holder releases through its own bus (`mail.release_role`), a host through
  `Switchboard::release_role`, and a loaded library's role through the control
  door (`ReleaseRole`); a bus with no release says `NotOffered`;
- removal releases every role the weave holds, and each one's claims;
- a replacement moves one role, so an incumbent holding several is refused, in
  words, before anything moves: `TxnReason::SeveralRoles` at `begin`,
  `AdmitRefusal::SeveralRoles` at an admission, `false` from a direct commit,
  and a `zen.Refused` from the Weave Manager's swap before it unloads anything.

DOES NOT MEAN
- that a running weave can take a role: a role is bound only when a weave is
  registered or loaded;
- that a role has several holders ([MSG-04](#msg-04--role-addressing-is-destination-not-authorship));
- that holding two roles lets a weave speak for either without saying so:
  speaking as a role is a statement's own act ([MSG-07](#msg-07--role-authorship-is-explicit));
- that roles split a weave's authority: its grant is one, whatever it holds.
  Whether a second role is right is the application's judgement, not Loom's.

PROVEN BY — `Switchboard::register_weave` (the set overload), `release_role`,
`roles_of`, `admission_blocked`, `begin_prepared_replacement`; suite
`role_authorship` (the set reached, authoring and claiming as each role; a set
refused whole; release and removal letting go of exactly their roles; a release
aborting a replacement; a two-role incumbent's replacement refused), suite
`manager` (a library loaded into a set, the control door's list, release and
the older load; a two-role holder's swap refused), suite `host_policy` (a boot
row's `roles`). [Decision](../decisions/a-weave-answers-to-the-roles-it-is-loaded-with.md).

## MSG-14 — A call is a function of its argument alone

LAW — A weave may call, inside its own handler, the function the holder of a
role provides for an argument's shape, and has the answer or a refusal before the
handler goes on. The function is handed the argument and nothing else: no
instance, no bus, no claim. It runs no handler and delivers nothing. A call needs
a call rule; the argument is admitted at the function's input shape and the
answer at the output shape the caller asked for; every refusal is its own word;
and the tap tells each call and how it ended, inside the caller's delivery and
before that delivery's own event.

MEANS
- the role resolves to its holder at the call, so a provider replaced between two
  calls answers the second with its successor's code;
- no send or observe rule is read for a call, and an unauthorized caller is
  refused before the role is resolved;
- one input shape names one function of a provider; a function's shapes meet the
  agreement wall with the rest of its vocabulary;
- a function that throws, traps or writes no answer is `Failed`, and the caller's
  delivery goes on;
- a loaded image's function runs with every host door refusing, so a table it
  kept from an earlier delivery reaches nothing; a boxed one runs in a store of
  its own, made for the call and dropped after it;
- the isolation pipe carries no call either way and a link carries none, each
  refused by name.

DOES NOT MEAN
- that native code is pure: it is asked to be. A native function shares the
  host's process, and nothing stops it writing its own memory; in a box it is
  imposed;
- that a function's time is bounded in native code: a native function that loops
  holds the thread, as a native handler does. A boxed one has its entry's steps;
- that a call reads the provider's state: a caller that needs it reads the
  provider's claim and passes it in;
- that anything is held between two calls, or that a call is recorded by history.

PROVEN BY — `CallDoor` (`zen/switchboard/calls.hpp`), `Mail::call`,
`Provides<...>`, the kernel's `call` door and `provide` slot, the box's function
store; suites `call` (the answer, the rule, each refusal, the tap, a successor,
delegation, the wall), `call_loaded` (a loaded caller and provider, a reload
between two calls, a forged kept table refused, an image built against ABI 16's
header), `box` (the boxed twins, a provider unable to change its weave),
`isolation` (a provider refused at mount, a call refused both ways), `bridge` (a
link's role). [Decision](../decisions/a-call-is-a-function-of-its-argument-alone.md).
