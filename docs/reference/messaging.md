# Messaging — reference

The Switchboard: the in-process bus, first live boundary. Laws:
[MSG-01..11](../laws/messaging-laws.md), [ANS-01..07](../laws/answer-authority-laws.md).
Guide: [messaging](../guides/messaging.md).

## Dispatch model

Single-threaded FIFO. `send`/`publish` enqueue; a dispatch turn
(`pump_pending()` or `drain_until_idle()`) delivers, one envelope at a time,
non-reentrant — a handler's sends become *later* deliveries
([MSG-01](../laws/messaging-laws.md)). "Between two deliveries" is therefore a
real atomic boundary; the admission dispatch rides it.

**If a native handler throws**, the exception propagates out of whichever
dispatch turn was running to whoever called it — Loom does not swallow it, does not turn
it into a refusal, and does not terminate. What Loom guarantees is that it has
put its own temporary state back first: dispatch is not left looking reentrant,
and the finished delivery's answer authority is not left standing. The failed
envelope is consumed (never silently retried, and its ticket keeps reading
`Pending` — no outcome was recorded); anything queued behind it is delivered by
the next turn
([MSG-10](../laws/messaging-laws.md#msg-10--a-callback-that-throws-costs-the-delivery-not-the-bus)).
A `.so` weave is a different boundary: its exceptions are caught at the ABI seam
and never reach the Switchboard ([dynamic-abi](dynamic-abi.md)).

**The weave being dispatched cannot be removed under itself.**
`unregister_weave` refuses — `nullptr`, nothing mutated — for the exact weave
whose callback is running, on both the ordinary and the committed-activation
call, and works again the moment that callback exits
([LIFE-06](../laws/lifecycle-laws.md), [lifecycle](lifecycle.md#permanent-removal-and-the-active-callback)).
Removing a *different* weave mid-callback is untouched.

## The envelope and the delivery order

`Message{payload, sender, reply_to, correlation}` is what crosses; everything
that carries *standing* lives on the bus-private envelope (sender-life stamp,
answer-target expectation, preparation identity, admission payload,
provenance) — no wire form, no reachable constructor, overwritten by every
ordinary enqueue ([ANS-07](../laws/answer-authority-laws.md)).

A gated (weave-originated) delivery is judged in this order, each refusal a
distinct observable reason:

```text
1  sender life current?          SenderLifeEnded
2  seal boundaries               SealedSpeech (outbound) / NoSuchTarget (inbound disguise)
3  effective authority permits
   shape→target?                CapabilityDenied   (baseline union delegated,
                                                    read NOW — GATE-05)
4  role resolves?                NoSuchTarget
5  target alive?                 TargetUnavailable
6  answer-target still exact?    AnswerTargetChanged
7  accept-set door?              NotAccepted
8  the gate                      GateRefused (with the gate's error)
```

Host/root sends (`Switchboard::send`, ungated) skip 1–3. Further reasons
exist: `ForeignAuthority` (an authority this Loom did not issue), `Exhausted`
(a published bound), `AdmissionRevoked` (a scheduled admission whose world
drifted — an admission refusal, not a message failure),
`RoleAuthorshipDenied` (an office-authorship request from a sender that does
not hold the office — refused at the *authorship* moment, before anything is
queued; see below), and `SeamUnresolved` (a loaded weave *addressed* something
with a shape this Loom has never heard of — rejected at the library/host seam
before routing, so no target was ever consulted; a publication addresses nothing
and is not refused for this, see [diagnostics](#diagnostics-and-the-seam)).

## Addressing

- **Directed** — `send(WeaveId, msg)`.
- **Role** — `send_to_role(role, msg)`: resolved to the singleton holder *at
  delivery* ([MSG-04](../laws/messaging-laws.md) — destination, not office).
  Unheld role → `NoSuchTarget`, indistinguishable from an unknown id.
- **Publish** — every alive, unsealed, accept-matching weave at *enqueue*
  time, each delivery independently gated ([MSG-06](../laws/messaging-laws.md)).

Grants authorize by shape→target or shape→role
([reference/capabilities](capabilities.md)); authorization-by-role happens
*before* resolution, so an unauthorized sender cannot learn whether a role is
held.

## Sender-visible dispatch refusal

An ordinary native or in-process loaded weave opts in by **explicitly accepting
`zen.DispatchRefused` v1** (`loom::DispatchRefused`, in
`zen/weave/dispatch_refusal.hpp`). `AcceptMode::AnyRegistered` alone does not opt in.
The declaration is captured at authorship. Ordinary directed and role-addressed
sends participate, including verified office-authored sends; publication, answers,
preparation and lifecycle operations do not acquire this contract.

```cpp
// Add DispatchRefused to this weave's Accept<...> list.
attempt = mail.send_to_role("builder", Build{...}, correlation);
// Later, in on(const loom::DispatchRefused& refused, loom::Mail& mail):
if (!mail.dispatch_refused()) { return; } // shape alone is ordinary speech
if (refused.refused_attempt().seq == attempt.seq && attempt.valid()) {
    // This exact send was refused before the target handler was entered.
}
```

`Ticket::seq` from the four ordinary addressed send doors identifies the queued
attempt in this Loom, including across ABI v7. It grants no authority, needs no
journal lookup, and proves only enqueue. Zero means no queued identity. Sequences
are not reused: after the last unsigned 64-bit sequence, native enqueue throws
`std::overflow_error`; the loaded addressed seam returns a refusal. A callback
that could not queue has no later dispatch notice. Existing dynamic answer
success sentinels and publication return values are separate contracts.

A notice is **Loom's later attestation**, not an application answer:
`mail.dispatch_refused()` is true, `answers_ask()` is false, and neither an
immediate nor a deferred answer right exists. Ordinary sends strip provenance;
copying the same Message, shape, sequence, sender or correlation cannot mint this
fact. The in-process image shares native memory; this is supported-API authority,
not a memory sandbox. See [dynamic ABI](dynamic-abi.md).

The safe reasons are `CapabilityDenied`, `NoSuchTarget`, `TargetUnavailable`,
`NotAccepted`, and `GateRefused`. The existing dispatch owner decides; no handler
has run. Sealed candidates remain concealed. A denied send reports
`CapabilityDenied` regardless of the addressed target's state, even when the host
diagnostic's earlier seal check reports `NoSuchTarget`. Original host diagnostics
are retained. A notice includes no gate detail string or resolved role holder.

The notice carries the original shape/version, target **or** role, attempt, and
safe reason. The original unsigned correlation is on the delivery envelope.
`attempt` and direct `target` use canonical unsigned decimal text because the
value grammar's integer is signed; `refused_attempt()` and `addressed_weave()`
decode without truncation. Role addresses and shape names are carried whole.
The original payload and `reply_to` are not copied. The notice returns to the
actual author, never its reply address or a subsequent holder of its office.

Loom captures the author's life **and incarnation at authorship**, checks both
before generating a notice, and checks both again before delivering it. Death,
removal, revival or live replacement disposes the obsolete notice. This does not
change MSG-03: ordinary queued speech still survives live code replacement.

Caller correlations may be zero or reused. Match the returned queued attempt,
then the expected authored context, after authenticating the notice. A small
`AskBook::bind_attempt(id, seq)` / `match_attempt(seq)` helper keeps this scalar
on the existing local record; it sends nothing and authenticates nothing.
Binding rejects zero, rebinding and duplicate attempts. A consumer forgets only
the matching local operation; forgotten or ambiguous records must stay untouched.
A refused non-ask needs no AskBook to be reported.

At most one notice is appended to the ordinary FIFO after an eligible refusal.
`pump_pending()` leaves it for a later turn; no handler is re-entered. There is
no receipt persistence or eventual-delivery promise. A missing, dead, replaced or
closed recipient disposes the notice; rejecting it or failing in its handler
never generates refusal-of-refusal traffic. Allocation failure or exhausted
sequence space may drop a notice while preserving the original host refusal.
See [bounds](bounds.md#dispatch-refusal-notices).

**Absence proves nothing about delivery or success.** Handler failure, a
delivered-but-unanswered request, or a later released/invalidated deferred answer
right produces no dispatch-refusal claim. Malformed dynamic bytes, unresolved
dynamic shapes, refused office authorship and invalid deferred rights can fail
synchronously before ordinary dispatch begins. Timeout, cancellation, abandonment,
retry, authority requests and publication aggregation are separate capabilities.
The isolated pipe refuses a manifest requesting the notice door; its attestation
protocol is not extended. Wire-originated payloads cannot attest this fact.

## Office authorship (role-authored provenance)

A weave may **deliberately** author one statement in the capacity of a role it
currently holds ([MSG-07](../laws/messaging-laws.md)). The maker surface is a
per-statement view:

```cpp
mail.as_role("matchmaker").send(player, MatchCreated{server});
mail.as_role("worker.a").send_to_role("dispatcher", JobDone{...});   // AS worker.a, TO dispatcher
mail.as_role("worker.a").publish(WorkerOpen{...});
```

Loom verifies membership at the **authorship moment** (`role_holder(R) ==
sender`, at enqueue) and stamps the fact as delivery provenance. The view
carries a role name and no authority — every emission re-verifies; it is
non-copyable with rvalue-qualified verbs, so the one-expression spelling is
the intended one. Raw `loom::Weave` authors use the underlying Bus verbs
(`office_send` / `office_send_to_role` / `office_publish`, office first,
ordinary parameters after).

The recipient reads the stamped fact off the delivery — no Switchboard
access, no role lookup, no payload field:

```cpp
if (!mail.authored_from_role("matchmaker")) { reject(); return; }
// mail.authored_role() — the exact office, empty for personal speech
```

Semantics, exactly:

- **Holding attaches nothing.** The same holder's `mail.send(...)` arrives
  with `authored_from_role(R) == false`; only the explicit act stamps.
- **Historical, immutable.** The fact means "the author held R and
  deliberately spoke as R *when the statement was made*" — later role
  movement never rewrites or clears it, and delivery never recomputes it.
  Current membership is `role_holder`'s question, a different one.
- **Refusal is loud and precise.** A sender that does not hold the office gets
  the invalid Ticket / `OfficePublication{authored=false}`, nothing is queued,
  and the tap shows `RoleAuthorshipDenied` — never a downgrade to personal
  speech. `OfficePublication{authored=true, recipients=0}` is the different,
  honest fact of an office that spoke to an empty room.
- **Orthogonal to destination** (MSG-04): an office-authored send to another
  office preserves both facts — authored as `worker.a`, delivered to
  `dispatcher` — in separate representations end to end.
- **Not a super-grant.** Every office-authored delivery is still authorized
  against the sender's ordinary grant, and sender-life/seal/routing laws
  refuse independently — office speech from a dead life is `SenderLifeEnded`.
- **Unlaunderable.** Provenance has no wire form; every ordinary enqueue
  clears it, so a stored-and-resent Message is personal speech.
- **Orthogonal to answers/activations in representation**: the authored office
  is a second axis beside `answers_ask()`/`lifecycle_attested()`, so the type
  admits combined facts (no public V1 door produces them; an answer's
  provenance never inherits the ask's office).
- **Dynamic parity** ([dynamic-abi](dynamic-abi.md); the office doors arrived
  at ABI v5): the same authoring
  and reading surface works in a loaded weave, with the host verifying every
  request. Out-of-process weaves fail closed in both directions — the
  isolation pipe carries no attestation in V1.

## Answers

A delivered request grants its handler **one** answer opportunity
([ANS-01](../laws/answer-authority-laws.md)):

- `mail.answer(msg)` — immediately, from inside the handler;
- `mail.defer_answer()` → move-only `DeferredAnswer`, spent later via
  `spend_deferred` by the exact incarnation that earned it — a *conversion* of
  the same one right ([ANS-02](../laws/answer-authority-laws.md));
- the recipient checks `mail.answers_ask()` — Loom's word that this delivery
  is THE authorized answer to a request it sent.

Deferred capacity is **bounded at the Loom level, not per weave**
(`kMaxDeferredAnswers = 64`); overflow refuses visibly as `Exhausted` and the
immediate opportunity survives. A long-running operation that would hold a
slot for ages may prefer an immediate authenticated acknowledgment followed by
ordinary later speech — the ack is provable, the follow-up is ordinary, and no
bounded slot is parked.

An answer is delivered only to the exact life+incarnation that asked
([ANS-03](../laws/answer-authority-laws.md)); correlations identify but never
authenticate ([ANS-05](../laws/answer-authority-laws.md)).

**`reply_to` / ordinary replies** — a `Message` carries a `reply_to` address a
responder may target with an ordinary send. That reply is *ordinary speech*:
no provenance, no authority, delivered to whatever occupies the address.
Evidence note: across six Night Lab applications, no natural use for ordinary
reply survived — every response wanted either an *answer* (provable) or a
*role send* (replacement-surviving). Kept, documented, low-observed-use.

### The asker's own book

An asker owns the record of what it is still asking. `loom::AskBook`
([`weave/ask_book.hpp`](../../include/zen/weave/ask_book.hpp)) is that record,
written once instead of per consumer: open a conversation and it hands back the
correlation to send and a small local id to hold it by; hand it an arrival's
correlation and bus-stamped sender and it says which — if any — of *your*
conversations that arrival settles.

```cpp
loom::AskBook asks{4};                       // the bound is the owner's; there is no default
const loom::AskOpened mine = asks.open(manager, loom::LoadWeave::zen_name, 1);
bus.send_as(self, manager, loom::Message(payload, self, self, mine.correlation));
...
if (const std::optional<loom::PendingAsk> closed = asks.settle(mail.correlation(),
                                                               mail.sender())) {
    // this arrival closed closed->id, and the record is still readable here
}
```

- **Both halves, and neither is sufficient.** The correlation says *which*
  conversation; the bus-stamped sender says the answer came from the weave that
  was asked. A correlation identifies and never authenticates
  ([ANS-05](../laws/answer-authority-laws.md)) — it is guessable by design — and
  the respondent you *are* waiting on is perfectly able to say something
  admissible about a different conversation.
- **An ask to an office cannot name its respondent**, because whoever holds the
  office at delivery is not knowable when you ask. `open_to_role` is that case
  said out loud; its record constrains the correlation and nothing narrower.
- **Recognizing and closing are two acts.** `match(...)` looks without removing;
  `settle(...)` closes and hands the record back, so a consumer never has to
  choose between reading its own bookkeeping and destroying it.
- **It interprets nothing.** No shape appears in that header. What an answer
  *means* — succeeded, refused, here is the value — stays with the consumer, and
  a conversation may be answered by any shape at all.
- **Outstanding means only "I asked, and no answer of mine has settled it."**
  Not that the respondent received it, owes an answer, or will ever reply. There
  is no timeout, no expiry and no unanswerability notice in Loom today, so an ask
  stays outstanding until the asker locally `forget`s it — which cancels nothing
  at the far end, because there is no cancellation vocabulary to cancel it with.

`loom::relay` ([`weave/relay.hpp`](../../include/zen/weave/relay.hpp)) is the same
wall for the opposite role: a **middleman** forwarding somebody else's request and
relaying the answer back. The two are deliberately separate — a relay's record is
about the asker it answers *for* and sheds its oldest entry when full, while an
asker's book refuses a new conversation rather than drop one of its own.

## Self-description — what may be said to this weave

**Ask the target.** Every woven weave (`WeaveBase`) carries a fifth substrate
door beside the four `zen.Poke*` ones:

```text
zen.DescribeAccepted {}          ->   zen.AcceptedShapes { referenced?, accepted }
```

The request is **fieldless and addressed to the target**, because the envelope
already names who is being asked and the target owns the answer:
`send_to_role("zengine.timer", DescribeAccepted{})`. There is no directory
service, no `role` field, no `WeaveId` lookup, and no registry enumeration
anywhere in the path — Loom has never had a way to enumerate schemas, and this
does not add one.

The answer is built from `Weave::accepted_schemas()` — **the same vector the
Switchboard captured as this weave's doors at registration**, which is what
every delivery is matched against. One store, read twice; there is no second
acceptance list that could drift from the enforced one.

**Two lists, and the difference is load-bearing:**

| Section | Is | Is not |
|---|---|---|
| `accepted` | the ROOTS — the `(name, version)` shapes that may actually be SENT to this target, in the target's own declaration order | filtered, categorised, or ranked. It is the raw accept-set, so it mixes commands, replies a relay accepts, notifications and the substrate doors; Loom encodes no direction metadata and this invents none |
| `referenced` | the transitive structural CLOSURE the roots nest, in post-order (a schema's own references first), deduplicated by (name, version). Optional; absent means nothing nested | sendable. A dependency is present only so a root can be understood |

The closure is not decoration. `zen.SchemaDesc v1` names a nested message by
`(name, version)` and `decode_schema` resolves that against a dependency
`Registry`, so a consumer handed only the roots **cannot decode a root that
nests anything** — measured, not assumed. Shipping the closure in post-order is
what makes one round trip self-sufficient for a stranger that never compiled
against any of these shapes. A shape that is genuinely both a root and a
dependency appears in both lists; re-registering it is identical and therefore a
no-op. Consumer side: `decode_accepted_referenced` then `decode_accepted_roots`
(`zen/weave/describe.hpp`), over `zen.SchemaDesc v1` unchanged — no second
descriptor format, and `zen.Manifest` is untouched.

**The answer includes the question.** A target genuinely accepts
`zen.DescribeAccepted`, so it says so. That terminates: the request is
fieldless, so it names no dependency and describes nothing that describes it
back.

**Three properties worth stating out loud:**

- **Discovery is an ordinary gated message.** Permission to ask target X about
  its vocabulary *is* an ordinary `SendRule` for `(zen.DescribeAccepted, 1)` to
  X or to X's role. Nothing is auto-granted globally, there is no new authority
  type, and the answer is an ordinary gated send from the target — a weave
  mounted without `allow_describe_answers` is `CapabilityDenied` at delivery,
  visible on the tap, exactly as an ungranted poke answer is.
- **Knowing a door exists is not permission to walk through it.** Receiving
  `StartTimer v1` in the answer confers nothing toward sending it; the composed
  message is refused by the ordinary gate like any other. Asking one target
  confers nothing toward another.
- **The answer is a SNAPSHOT** — what this target accepted when it answered, and
  specifically the holder that answered, since `send_to_role` resolves at
  delivery. It is not live: there is no subscription, no invalidation, no
  arrival/departure notification and no polling. If the role is swapped
  afterwards the snapshot is stale, and that is honest rather than a defect.

**A self-describing weave cannot be `AcceptMode::AnyRegistered`.** The wildcard
widens the door set on the Switchboard's side, where the weave cannot see it and
so cannot describe it; the two are refused together at `register_weave` rather
than left to produce a truthful-looking understatement. So self-description is
available exactly for finitely-declared accept-sets. Every wildcard weave in
Loom (the console, the bridge's operator proxy) is a raw `loom::Weave` that
carries no substrate doors at all, so nothing is narrowed by this.

## Observation

`add_observer` taps every delivery/refusal/lifecycle event (`BusEvent`, with
diagnostic life/incarnation fields on the relevant refusals). The journal
retains the last `kJournalCapacity = 1024` delivery outcomes by ticket
(`outcome(Ticket)`); older entries read as `Pending`, exactly like unknown
seqs. The Poke doors (`ZEN_EXPOSE`/`ZEN_HIDE`) allow live field
inspect/manipulate *by message* where a weave opts in.

**What a `BusEvent` carries, and what each field is NOT** (RTH-1 added the last
four; every one of them was already on the private `Envelope` and was simply not
carried out to an observer):

| Field | Is | Is not |
|---|---|---|
| `seq` | the delivery's identity on this bus | 0 on a lifecycle event, which is not a delivery |
| `sender` / `target` | the bus's own stamp, and the RESOLVED recipient | anything the sender chose |
| `addressed_role` | the office the sender NAMED, resolved to `target` at dispatch | the office the sender spoke as — that is `authored_role` |
| `correlation` | the number the sender put on the conversation | authority of any kind ([ANS-05](../laws/answer-authority-laws.md)); 0 is both "none stated" and a legal choice |
| `dispatch_parent` | which delivery was being dispatched when this one was ENQUEUED | causality. An async observation names the delivery that *drained* it — a timer beat, say — not the request its operation belongs to |
| `handler_elapsed_ns` | how long the handler held the one mind, on the steady clock | a time. Loom stamps no message with a clock reading; this is one call's elapsed measure and is 0 wherever no handler ran |
| `payload` | the complete admitted `Value`, **valid only during the callback** | durable. It points into a `Message` that dies when the delivery returns; a retainer must copy or serialize inside the callback |

`EventKind::HandlerFailed` is a fourth delivery kind beside `Delivered` and
`Refused`: the handler was entered and did not complete
([MSG-10](../laws/messaging-laws.md#msg-10--a-callback-that-throws-costs-the-delivery-not-the-bus)).
It carries no reason, and the journal records no outcome for it.

A bounded, structured, host-side consumer of all of this ships with the Loom —
see [history](history.md).

An observer may **subscribe or unsubscribe from inside a notification**
([MSG-11](../laws/messaging-laws.md#msg-11--every-event-has-its-own-view-of-the-tap-list)).
Each event fixes its recipients at entry, so one added during an event joins at
the *next* one; one removed during an event does not hear it, which is what makes
the `remove_observer`-in-a-destructor idiom safe. An observer that throws
propagates to whoever was pumping and stops that event's remaining
notifications; it is never silently swallowed.

Senders do not otherwise observe delivery fate
([known-seams](known-seams.md#sender-cannot-observe-send-fate)).

### Diagnostics and the seam

A rejection Loom performs is observable somewhere Loom owns
([MSG-08](../laws/messaging-laws.md)). The dynamic seam admits a loaded weave's
bytes host-side *before* routing; when that fails nothing is queued, so no
delivery-time refusal can report it. Those rejections now get a seq, a journal
slot and a tap event — `SeamUnresolved` for an unresolvable claimed shape,
`GateRefused` (with the gate's error) for bytes that fail the gate — carrying the
**claimed** (name, version), the sending artifact, and a target *only where one
was actually named*. A publication names none; a role is a slot resolved at a
delivery that never happened. Nothing is manufactured. A role door does report the
**role**, which the library stated and the seam was handed: an address, beside an
invalid target, and never a resolution.

**An unresolvable PUBLICATION is not one of these.** Being unheard is what a
publication into a world with no listeners *is*, on either tier: an accept-set is
claimed in the registry for its weave's whole life and fanout selects by that same
(name, version), so a shape that does not resolve has no accepter and the
publication reached zero recipients — exactly what a native `publish` does
silently. What still refuses is a publication whose shape resolves and whose bytes
fail the gate: there a real accepter was denied real bytes.

These are immediate seam rejections: no queued-attempt ticket is returned, and
no later dispatch-refusal notice follows. ABI v7's ordinary addressed callbacks
return real attempts when they enqueue; see
[sender-visible dispatch refusal](#sender-visible-dispatch-refusal) and
[dynamic ABI](dynamic-abi.md) for that contract. The host diagnostics add no
future, retry or dead letter. This diagnostic gap was found by Night Lab III
(P-011), where a loaded weave's emission vanished entirely while the identical
native reach refused loudly; the publication half was corrected by FRIC-0, where
the same uniformity had made an ordinary quiet startup look like a failure.

## Two dispatch turns, and the call site says which

Nothing runs until a host asks for a dispatch turn, and there are exactly two to
ask for ([MSG-09](../laws/messaging-laws.md)):

| | bound | newly enqueued work | you must pick |
|---|---|---|---|
| `pump_pending()` | `pending()` at entry | waits for the next turn | nothing |
| `drain_until_idle()` | until the queue is empty | dispatched in the same call | nothing |

**`pump_pending()` is the ordinary host-loop operation.** It dispatches exactly
the backlog present at entry, returns how many it made, and hands control back:

```cpp
while (serving) {
    poll_sockets();
    bus.pump_pending();     // exactly the backlog that was waiting; control returns
}
```

The snapshot is a **work boundary, not a quota**: the backlog you walked in with
is processed in FIFO order, and a handler's own continuation lands behind it. So
a busy bus clears its whole backlog in one turn while a self-re-arming producer
still cannot hold the turn open.

**`drain_until_idle()` keeps going until nothing is queued** — and work a
handler enqueues during the call belongs to the call, so the queue empties only
when the participants themselves stop producing. It is the right call for a test
or script that wants everything settled before it asserts, for a one-shot
bootstrap, and for a host whose *entire program* is the bus and whose exit is
`stop()`.

It is also **unbounded by contract**, and the name is where that is said. A
perpetual in-process service — a repeating Timer re-arms itself inside its own
handler — means the queue never empties and the drain never returns. That is the
correct answer to the question asked, not a defect: quiescence in a world that
will not become quiescent does not exist, and Loom will not invent a turn budget
or a deadline to pretend otherwise.

**The names carry that difference deliberately** (FRIC-1). The drain used to be
called `pump()`, with `run()` beside it as a synonym, and the bounded turn wore
the qualifier — so the short, obvious-looking name made the expensive promise
and a first-contact host that reached for it never got control back. Neither
spelling survives; the behaviour behind the drain is unchanged.

A numeric `pump_bounded(n)` existed briefly and was withdrawn in R2E-0a: sizing
`n` means knowing the producer's rate, and with a real Zengine Timer 64 throttled
the Codex Rule Garden 17× while a value large enough not to throttle was
drain-to-empty again. If you want a hard cap on work per turn, bound it in your
own loop — Loom will not pretend it can pick the number for you.

Both are counts, never deadlines; FIFO is untouched; `stop()` ends either turn
early and the return value reports what actually happened.
`BridgeServer::set_bounded_dispatch()` exposes the policy, defaulting to
drain-to-empty.

## Tests

Suites `switchboard`, `provenance`, `capabilities`, `poke`, `describe`; the
bridge suite for the operator protocol.
