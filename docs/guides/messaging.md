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
replacement — that is why production traffic addresses roles, not ids. A role
send says where the message *goes*; it proves nothing about the sender having
spoken *as* that role — speaking for an office is its own deliberate act,
below.

## Speaking as the office

Holding an office is not speaking for it. When your statement should be
trusted because of the *office* — a matchmaker's match, a worker's completion,
a service's announcement — author it deliberately, per statement:

```cpp
void make_match(loom::Mail& mail) {
    mail.as_role("matchmaker").send(player, MatchCreated{server});
}
```

Loom verifies you hold the office at that moment and stamps the fact onto the
delivery. The receiver trusts the office, not the occupant:

```cpp
void on(const MatchCreated& match, loom::Mail& mail) {
    if (!mail.authored_from_role("matchmaker")) {
        return; // personal speech, or a forger — not the office
    }
    join(match.server);
}
```

That check survives replacement for free: an honest successor authors as the
inherited office and passes it; the retired predecessor's *new* attempts
refuse. Publications work identically —
`mail.as_role("worker.a").publish(WorkerOpen{...})` lets every listener verify
the announcement, and the result keeps "not authored" distinct from
"authored, nobody listening". Your ordinary sends stay personal — from the
very same handler, holding the very same office — and everything ordinary
still applies to office speech: your grant, your life, routing.

When do you need it? When trust follows a **replaceable role** rather than one
exact weave. If your counterparty is one specific weave, the sender stamp
already answers everything (`mail.sender()`); if it is "whoever holds the
office", the stamp cannot, and this can. Guide-level law:
[MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit);
exact semantics:
[reference](../reference/messaging.md#office-authorship-role-authored-provenance).

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
