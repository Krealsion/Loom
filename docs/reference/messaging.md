# Messaging — reference

The Switchboard: the in-process bus, first live boundary. Laws:
[MSG-01..06](../laws/messaging-laws.md), [ANS-01..07](../laws/answer-authority-laws.md).
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

Host/root sends (`Switchboard::send`, ungated) skip 1–3. Two further reasons
exist: `ForeignAuthority` (an authority this Loom did not issue),
`Exhausted` (a published bound), and `AdmissionRevoked` (a scheduled admission
whose world drifted — an admission refusal, not a message failure).

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

## Tests

Suites `switchboard`, `provenance`, `capabilities`, `poke`; the bridge suite
for the operator protocol.
