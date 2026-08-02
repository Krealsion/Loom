# Messaging

How weaves talk: plain sends, roles, publications — and answers, which are the
one kind of speech Loom itself vouches for.

## Three ways to address

```cpp
mail.send(target_id, Job{...});          // to a weave you know
mail.send_to_role("storage", Put{...});  // to whoever holds the office right now
mail.publish(Tick{...});                 // to everyone who declared they hear it
```

Role sends resolve **at delivery**, so they keep working across a service
replacement — that is why production traffic addresses roles, not ids. One
honest limit to keep in mind: a role send says where the message *goes*; it
proves nothing about the sender having spoken *as* that role
([the seam](../reference/known-seams.md#role-authored-provenance)).

## Asks and answers

When your handler runs, that delivery may carry the right to answer — one
answer, for that delivery, while you are handling it:

```cpp
void on(const QueryVersion&, loom::Mail& mail) {
    mail.answer(VersionReply{"v2"});     // THE authorized answer to this ask
}
```

The requester checks `mail.answers_ask()` on what comes back — Loom's own word
that this is the answer to *its* ask, not a lookalike. Correlations name
conversations; they never authenticate ([ANS-05](../laws/answer-authority-laws.md)).

If the answer isn't ready yet, convert the right and keep it:

```cpp
void on(const PrepareStation& ask, loom::Mail& mail) {
    pending_ = mail.defer_answer();          // the same one right, retained
}
void on(const OvenHot&, loom::Mail& mail) {
    loom::answer_deferred(pending_, mail, StationReady{});
}
```

Two truths worth designing around:

- **Deferred capacity is bounded at the Loom level, not per weave** (64
  outstanding conversations, everywhere — [bounds](../reference/bounds.md)).
  A long-running operation often does better with an immediate authenticated
  acknowledgment (`mail.answer(Ack{})`) followed by ordinary later speech —
  the ack is provable, and no shared slot sits parked for minutes.
- An answer belongs to the exact life that asked; if the asker died or was
  replaced meanwhile, the bus refuses the delivery rather than misdelivering
  it ([ANS-03](../laws/answer-authority-laws.md)).

`mail.reply(...)` also exists — an *ordinary* send back at the sender's
address, with no authority attached. Evidence note: across six Night Lab
applications, nothing ended up wanting it; every response wanted either an
answer (provable) or a role send (replacement-surviving). It remains
supported.

## When something refuses

A refusal is a named observable event, never silence — but note it is the
*observer's* to see, not the sender's (a weave cannot watch its own send's
fate; see [diagnostics](diagnostics.md) and
[the seam](../reference/known-seams.md#sender-cannot-observe-send-fate)).

## Deeper

The compiled twin of this page is
[`examples/answering.cpp`](../../examples/answering.cpp) — both the immediate
and the deferred path, runnable. Reference:
[messaging](../reference/messaging.md) (the full delivery order and refusal
ladder) · laws: [MSG](../laws/messaging-laws.md),
[ANS](../laws/answer-authority-laws.md).
