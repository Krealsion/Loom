# Messaging — reference

The Switchboard: the in-process bus, first live boundary. Laws:
[MSG-01..07](../laws/messaging-laws.md), [ANS-01..07](../laws/answer-authority-laws.md).
Guide: [messaging](../guides/messaging.md).

## Dispatch model

Single-threaded FIFO. `send`/`publish` enqueue; `pump()` drains, one envelope
at a time, non-reentrant — a handler's sends become *later* deliveries
([MSG-01](../laws/messaging-laws.md)). "Between two deliveries" is therefore a
real atomic boundary; the admission dispatch rides it.

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
3  grant permits shape→target?   CapabilityDenied
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
queued; see below), and `SeamUnresolved` (a loaded weave claimed a shape this
Loom has never heard of — rejected at the library/host seam before routing, so
no target was ever consulted; see [diagnostics](#diagnostics-and-the-seam)).

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
- **Dynamic parity** ([dynamic-abi](dynamic-abi.md), v5): the same authoring
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

## Observation

`add_observer` taps every delivery/refusal/lifecycle event (`BusEvent`, with
diagnostic life/incarnation fields on the relevant refusals). The journal
retains the last `kJournalCapacity = 1024` delivery outcomes by ticket
(`outcome(Ticket)`); older entries read as `Pending`, exactly like unknown
seqs. The Poke doors (`ZEN_EXPOSE`/`ZEN_HIDE`) allow live field
inspect/manipulate *by message* where a weave opts in.

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
delivery that never happened. Nothing is manufactured.

This is not send fate and does not become it: no ticket crosses the seam, nothing
is returned to the sender that was not already returned, and there is no future,
retry or dead letter. Found by Night Lab III (P-011), where a loaded weave's
emission vanished entirely while the identical native reach refused loudly.

## Bounded dispatch

`pump()` drains to empty — unchanged, and the contract every existing caller
has. `pump_bounded(n)` dispatches at most `n` deliveries and returns how many it
made ([MSG-09](../laws/messaging-laws.md)), for a host composing Loom with an
outer event loop:

```cpp
while (serving) {
    poll_sockets();
    bus.pump_bounded(64);   // control comes back, whatever the bus is doing
}
```

Without it, a perpetual in-process service starves the outer loop: a repeating
Timer re-arms itself inside its own handler, so the queue never empties and a
drain-to-empty pump never returns. Newly enqueued work **counts** toward the
budget — any other rule would leave exactly that producer unbounded. The bound is
a dispatch count, never a deadline; FIFO is untouched; `stop()` still ends the
turn early and the return value keeps "budget exhausted" and "somebody stopped"
apart. `BridgeServer::set_dispatch_budget` exposes the policy, defaulting to 0
(drain to empty).

## Tests

Suites `switchboard`, `provenance`, `capabilities`, `poke`; the bridge suite
for the operator protocol.
