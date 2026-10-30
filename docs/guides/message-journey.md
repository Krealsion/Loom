# A message's journey

What happens to one message between the line that sends it and the handler that runs because of
it, and why each step is there. Read it once and the rest of Loom's messaging pages are details
of a path you have already walked.

The words you need first: a **weave** is a participant — a piece of code with its own state —
living on a **bus**, the `Switchboard`, which carries every message between weaves. The
program that owns the bus is the **host**. A message is a value of a declared **shape** (a
named, versioned record such as `Job v1`). Each weave says which shapes it hears (its
**doors**) and the host decides what it may say (its **grant**).

## The record you will read

Every journey below is run by [`examples/message_journey.cpp`](../../examples/message_journey.cpp)
on a bus of its own, and the blocks on this page are that program's output: the bus's own
record of the journey, as its tap reports it. A test (`message_journey`) runs the program and
checks every block here against what it prints, so if Loom's behaviour changes, the test goes
red instead of this page quietly becoming untrue.

A line of the record reads like this:

```text
#2 Delivered sender -> worker  Job v1, queued in #1
     worker reads: from sender, answers my ask no
```

- `#2` is the number the bus gave the message when it was sent. Numbers only grow.
- `Delivered` or `Refused` is what became of it, and a refusal ends with the bus's reason.
- `sender -> worker` is who sent it, as the bus stamped it, and who it reached. `host` is the
  program that owns the bus; `loom` is the bus speaking for itself.
- `queued in #1` names the delivery whose handler sent it. A message the host sent between
  turns has none.
- The indented `reads:` line is what the receiving handler read off its delivery: who the bus
  says sent it, and whether Loom vouches for it as the answer to something the receiver asked.

## The spine: one message, from the send to the handler

The host nudges a weave called `sender`, and `sender`'s handler sends one `Job` to a weave
called `worker`:

```cpp
void on(const Start&, loom::Mail& mail) { mail.send(worker, Job{1}); }
```

<!-- journey: spine -->
```text
#1 Delivered host -> sender  Start v1
#2 Delivered sender -> worker  Job v1, queued in #1
     worker reads: from sender, answers my ask no
```

Between `mail.send` and `worker`'s handler, the message passes these steps, in this order.

**1. Who may send it.** A weave speaks only through the `Mail` its handler was given, and the
bus stamps the weave's own id on everything sent through it. Nothing in the message can name a
different sender. *Why:* a payload can lie about anything it carries; the stamp is the one fact
about a message its author cannot write. The host, which owns the bus, sends with no stamp and
no grant check at all (`#1` above): it is the authority everything else is granted from.
*Where:* `Mail::send`, `Switchboard::send_as` (a weave) and `Switchboard::send` (the host);
[MSG-02](../laws/messaging-laws.md#msg-02--the-bus-stamps-the-sender).

**2. Where it waits.** The send puts the message at the back of the bus's one queue and
returns at once with a `Ticket`, its number. Nothing runs yet. The bus also notes which
delivery was being handled when it was sent (`queued in #1`). *Why:* if a send ran its
receiver straight away, a handler could find itself called again from inside its own work.
One queue, first in first out, means each handler runs to its end before the next begins. A
ticket says *queued*, never *delivered*. *Where:* `Switchboard::enqueue_directed`;
[MSG-01](../laws/messaging-laws.md#msg-01--single-threaded-fifo-non-reentrant).

**3. The host's dispatch turn.** Messages move only when the host asks the bus for a turn:
`pump_pending()` delivers what was waiting when the turn began and hands control back, and
`drain_until_idle()` keeps going until nothing is left. *Why:* the host decides when work
happens, so a weave that keeps sending to itself cannot take the host's loop away from it.
*Where:* `Switchboard::pump_pending`, `Switchboard::drain_until_idle`;
[MSG-09](../laws/messaging-laws.md#msg-09--a-dispatch-turn-can-be-bounded-without-bending-fifo),
[two dispatch turns](../reference/messaging.md#two-dispatch-turns-and-the-call-site-says-which).

**4. Is its author still the one that sent it?** When the message reaches the front of the
queue, the bus checks the sender is still the same life that sent it — not dead, not removed,
not revived since. *Why:* a message is the word of the participant that said it; if that
participant has died, whatever now answers to its id never said it. *Where:* the first check in
`Switchboard::deliver_one`, refused as `SenderLifeEnded`;
[MSG-03](../laws/messaging-laws.md#msg-03--a-message-belongs-to-a-life).

**5. May it say this, here?** The bus reads the sender's grant *now*: does it allow this shape
to this target? *Why:* authority can change while a message waits, and the grant that counts is
the one at delivery. It is asked before the target is looked up, so a sender with no right to
speak cannot even learn whether its target exists. *Where:* `deliver_one`, refused as
`CapabilityDenied`; [GATE-03](../laws/admission-laws.md#gate-03--authorization-is-not-conformance),
[GATE-05](../laws/admission-laws.md#gate-05--baseline-authority-is-admission-time-delegated-authority-is-live-effective-authority-decides).

**6. How its target is found.** A message sent to an id goes to that id; one sent to an
office (below) goes to whoever holds the office at this moment. The target must exist and be
alive. *Why:* a target that is gone is a different fix from a target that is down, so the two
are told apart. *Where:* `deliver_one`, refused as `NoSuchTarget` or `TargetUnavailable`;
[addressing](../reference/messaging.md#addressing).

**7. Does the receiver take it?** The target must have a door for this shape and version, and
the message must then pass **the gate**: the one check that a value really is a well-formed
instance of the shape it claims. *Why:* a handler is written for one shape and must never see
anything else; one gate at every boundary means no road around it. *Where:* `deliver_one`,
refused as `NotAccepted` or `GateRefused` (with the gate's own error);
[GATE-01](../laws/admission-laws.md#gate-01--one-gate). A target held because it could not apply
a value published under its name is refused `ApplicationFailed` here too
([joint publication](../reference/joint-publication.md#the-hold-and-diagnostic-access)).

**8. What Loom stamps on the delivery.** The handler is handed the admitted value and a `Mail`
that carries the delivery's facts: the sender the bus stamped (`mail.sender()`), the
conversation number the sender chose (`mail.correlation()`), and **provenance** — what Loom
itself vouches for: whether this is the answer to an ask of the receiver's
(`mail.answers_ask()`), whether it was spoken as an office (`mail.authored_role()`), whether it
is Loom's refusal notice (`mail.dispatch_refused()`). A delivery a weave sent also carries the
right to answer it, once. *Why:* three questions decide whether a message means anything — is it
well-formed (the gate), who said it (the stamp), and does Loom vouch for its standing
(provenance) — and none of them can be answered from the payload.
*Where:* `loom::Provenance` (`zen/switchboard/message.hpp`), `loom::Mail` (`zen/weave/weave.hpp`);
[the mental model](mental-model.md),
[ANS-01](../laws/answer-authority-laws.md#ans-01--answer-authority-is-the-delivery).

**9. The handler runs alone.** One handler runs at a time, to its end. What it sends joins the
back of the queue as new messages, each with its own number, each to walk these same steps.
*Why:* nothing to lock, and nothing it sends can run in the middle of it. A handler that throws
costs that one delivery: the exception goes to the host, and the bus is left in order.
*Where:* `deliver_one`;
[MSG-10](../laws/messaging-laws.md#msg-10--a-callback-that-throws-costs-the-delivery-not-the-bus).

**10. What comes back when a step says no.** Every refusal is a named reason, recorded where
the host can see it: on the tap, and in the outcome of the message's ticket
(`Switchboard::outcome`). The sender is told only if it asked to be, by accepting
`zen.DispatchRefused` (see [refusals](#a-refusal), below), and silence proves nothing either way.
*Why:* a refusal is something an operator has to be able to see, and each reason points at a
different fix. *Where:* `RefusalReason`;
[MSG-05](../laws/messaging-laws.md#msg-05--refusals-are-structured-and-observable), the full
order in [the envelope and the delivery order](../reference/messaging.md#the-envelope-and-the-delivery-order).

## The variations

Each is the spine with one difference.

### To an office

A weave can hold an **office** — a named role such as `storage` — and a message can be sent to
the office instead of to an id:

```cpp
mail.send_to_role("storage", Put{1});
```

<!-- journey: role -->
```text
#1 Delivered host -> sender  Start v1
#2 Delivered sender -> store  Put v1, to office storage, queued in #1
     store reads: from sender, answers my ask no
```

**The difference:** at step 6 the bus looks up who holds `storage` *now*, at delivery, not
when the message was sent. *Why:* the weave behind an office can be replaced while messages
wait for it, and they reach the new holder. An office nobody holds refuses exactly like an
unknown id. Sending to an office says where the message goes and nothing about who sent it.
*Where:* `Mail::send_to_role`;
[MSG-04](../laws/messaging-laws.md#msg-04--role-addressing-is-destination-not-office).

### Speaking as the office

Holding an office is not speaking for it. The same weave sends twice, once in its own name and
once deliberately as the office it holds:

```cpp
mail.send(worker, Match{7});                       // personal
mail.as_role("matchmaker").send(worker, Match{7}); // as the office
```

<!-- journey: office -->
```text
#1 Delivered host -> sender  Start v1
#2 Delivered sender -> worker  Match v1, queued in #1
     worker reads: from sender, spoken as the matchmaker office no
#3 Delivered sender -> worker  Match v1, as office matchmaker, queued in #1
     worker reads: from sender, spoken as the matchmaker office yes
```

**The difference:** before step 2, at the moment it is said, the bus checks the sender holds
`matchmaker`, and stamps that fact on the message; `mail.authored_from_role("matchmaker")` reads
it at the receiver. *Why:* when trust follows a replaceable office rather than one exact weave,
the receiver needs Loom's word that the office spoke. A weave that does not hold the office is
refused there and then (`RoleAuthorshipDenied`), and nothing is sent in its place. The office
adds no authority: the grant still decides at step 5. *Where:* `Mail::as_role`;
[MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit),
[office authorship](../reference/messaging.md#office-authorship-role-authored-provenance).

### A publication

```cpp
mail.publish(Tick{1});
```

<!-- journey: publication -->
```text
#1 Delivered host -> sender  Start v1
#2 Delivered sender -> worker  Tick v1, queued in #1
     worker reads: from sender, answers my ask no
#3 Delivered sender -> second worker  Tick v1, queued in #1
     second worker reads: from sender, answers my ask no
```

**The difference:** at step 2 the bus makes one message for every weave that has a door for
`Tick` at that moment, and each then walks steps 3 to 9 on its own. *Why:* the listeners are
picked when the news is said; a weave that starts listening later does not hear news from
before it did. *Where:* `Mail::publish`;
[MSG-06](../laws/messaging-laws.md#msg-06--publication-picks-recipients-at-enqueue).

### An ask and its answer

`asker` sends a `Query` under a conversation number of its own (41); `responder` answers, then
tries to answer a second time; `asker` answers the answer:

```cpp
void on(const Query& q, loom::Mail& mail) {   // responder
    mail.answer(Reply{q.n});
    mail.answer(Reply{q.n});                    // refused: one ask, one answer
}
void on(const Reply&, loom::Mail& mail) {     // asker
    if (mail.answers_ask()) { mail.answer(Thanks{1}); }
}
```

<!-- journey: ask -->
```text
#1 Delivered host -> asker  Start v1
#2 Delivered asker -> responder  Query v1, correlation 41, queued in #1
     responder reads: a second answer queued no
#3 Delivered responder -> asker  Reply v1, correlation 41, queued in #2
     asker reads: from responder, answers my ask yes, correlation 41
#4 Refused   responder -> asker  Reply v1, queued in #2: CapabilityDenied
#5 Delivered asker -> responder  Thanks v1, correlation 41, queued in #3
     responder reads: from asker, answers my ask yes
```

**The difference:** at step 8 the delivery of `Query` carried the right to answer it once.
`mail.answer` spends it, and Loom — not the responder — picks who the answer goes to and the
conversation number it carries. The asker's `mail.answers_ask()` is Loom's word that this is the
answer to *its* ask; a correlation is only a label anyone can copy. The second answer finds the
right spent and is refused on the spot (`#4`), and nothing was queued. An answer is a delivery
like any other, so it carries a right of its own: the `Thanks` is the answer to the `Reply`, a
new conversation, which proves nothing about the ask before it. *Where:* `Mail::answer`,
`Mail::answers_ask`; [ANS-01](../laws/answer-authority-laws.md#ans-01--answer-authority-is-the-delivery),
[ANS-05](../laws/answer-authority-laws.md#ans-05--correlation-identifies-it-never-authenticates),
[answers](../reference/messaging.md#answers).

### A deferred answer

The responder cannot answer yet, so it keeps the right and spends it when the host tells it the
oven is hot:

```cpp
void on(const Query&, loom::Mail& mail) { pending_ = mail.defer_answer(); }
void on(const Hot&, loom::Mail& mail) { loom::answer_deferred(pending_, mail, Reply{1}); }
```

<!-- journey: deferred -->
```text
#1 Delivered host -> asker  Start v1
#2 Delivered asker -> responder  Query v1, correlation 41, queued in #1
     responder reads: kept the answer for later yes
#3 Delivered host -> responder  Hot v1
#4 Delivered responder -> asker  Reply v1, correlation 41, queued in #3
     asker reads: from responder, answers my ask yes, correlation 41
```

**The difference:** the right to answer outlives the handler. It is the same one right, not a
second: deferring spends the right to answer at once. The answer still goes to the exact asker
that asked, and is refused if that asker has died or been replaced in the meantime. It is sent
from a later delivery (`queued in #3`). *Why:* an answer that depends on something not yet
known should not need a second, unprovable message. The bus holds a bounded number of kept
answers for everyone together (`kMaxDeferredAnswers`). *Where:* `Mail::defer_answer`,
`loom::answer_deferred`;
[ANS-02](../laws/answer-authority-laws.md#ans-02--deferring-converts-it-never-adds),
[ANS-03](../laws/answer-authority-laws.md#ans-03--an-answer-belongs-to-the-life-that-asked).

### A refusal

`sender` accepts `zen.DispatchRefused`, which asks Loom to tell it when a send of its is
refused. Its grant allows `Job` and nothing else. It sends three messages that fail at three
different steps:

```cpp
mail.send_to_role("nobody", Job{1}); // step 6: nobody holds that office
mail.send(put_only, Put{1});         // step 5: the grant does not allow Put
mail.send(put_only, Job{1});         // step 7: put-only has no door for Job
```

<!-- journey: refusals -->
```text
#1 Delivered host -> sender  Start v1
#2 Refused   sender -> nobody  Job v1, to office nobody, queued in #1: NoSuchTarget
#3 Refused   sender -> put-only  Put v1, queued in #1: CapabilityDenied
#4 Refused   sender -> put-only  Job v1, queued in #1: NotAccepted
#5 Delivered loom -> sender  zen.DispatchRefused v1, queued in #2
     sender reads: Loom's refusal notice yes, for my send #2: NoSuchTarget
#6 Delivered loom -> sender  zen.DispatchRefused v1, queued in #3
     sender reads: Loom's refusal notice yes, for my send #3: CapabilityDenied
#7 Delivered loom -> sender  zen.DispatchRefused v1, queued in #4
     sender reads: Loom's refusal notice yes, for my send #4: NotAccepted
```

**The difference:** each message stops at its step, no handler runs, and the reason is recorded.
Then, because the sender asked, Loom queues one notice per refusal back to the sender —
`mail.dispatch_refused()` is Loom's word that it is genuine, and `refused_attempt()` names the
exact send by its ticket number. *Why:* a sender that cares can learn of a refusal without being
able to forge one or mistake another's for its own. A notice is not an answer, the reasons it
can carry are listed in the reference, and a sender that hears nothing has learned nothing.
*Where:* `loom::DispatchRefused` (`zen/weave/dispatch_refusal.hpp`);
[MSG-12](../laws/messaging-laws.md#msg-12--a-sender-may-learn-an-authenticated-refusal-of-its-exact-send),
[sender-visible dispatch refusal](../reference/messaging.md#sender-visible-dispatch-refusal).

### A reply

`mail.reply(...)` sends an ordinary message to the **reply address** the incoming message
carried. The host sends `responder` two `Ping`s, the first naming `listener` as its reply
address and the second naming none:

```cpp
bus.send(responder, Message(to_value(Ping{7}), {}, listener));
bus.send(responder, Message(to_value(Ping{8})));
// responder: void on(const Ping& p, loom::Mail& mail) { mail.reply(Pong{p.seq}); }
```

<!-- journey: reply -->
```text
#1 Delivered host -> responder  Ping v1
#2 Delivered host -> responder  Ping v1
#3 Delivered responder -> listener  Pong v1, queued in #1
     listener reads: from responder, answers my ask no
#4 Refused   responder -> nobody  Pong v1, queued in #2: NoSuchTarget
```

**The difference:** a reply goes where the reply address points, and nowhere else: with no
reply address it is sent to no one and refused `NoSuchTarget`. It is ordinary speech, so Loom
vouches for nothing (`answers my ask no`). *Why:* the reply address is a field the sender fills
in, so it can point anywhere; when the response has to be provable, use an answer. *Where:*
`Mail::reply`; [`reply_to` and ordinary replies](../reference/messaging.md#answers).

### Across a link to another host

A host can link to another running host and put an office in front of it, `loom.link.<name>`.
A weave asks across by sending that office one `loom.link.Ask`, which carries the far office it
means and the message as bytes:

```cpp
mail.send_to_role(loom::link::role_of("far"), loom::link::ask_role("echo", Echo{"across"}), 1);
```

<!-- journey: link -->
```text
near host:
#1 Delivered host -> asker  Start v1
#2 Delivered asker -> link  loom.link.Ask v1, to office loom.link.far, correlation 1, queued in #1
#3 Delivered link -> link  loom.link.Crossed v1
#4 Delivered link -> asker  Echoed v1, correlation 1, queued in #3
     asker reads: from link, answers my ask yes, correlation 1, "echo: across"
far host:
#1 Delivered session -> echo  Echo v1, to office echo, correlation 1
#2 Delivered echo -> session  Echoed v1, correlation 1, queued in #1
```

**The difference:** the journey runs twice, once on each bus. On the near host the ask walks the
spine to the link (`#2`). The link puts it on the wire under a number of its own; on the far host
the link's session is an ordinary sender with a grant the far host chose, and its message walks
the far spine to `echo`, whose answer goes back to the session. The link reads what crossed on
the near host's turn and says it to itself first (`#3`, `loom.link.Crossed`), so the near history
keeps it; then it answers the near ask through Loom's own answer door, so `answers my ask yes`
is the near bus's word. *Why:* each host judges what its own bus delivers; the far host's grant
decides what the session may say there, and the near asker settles on its own conversation
number, the link's stamp and `answers_ask()` together. When no far answer comes back, the link
answers with `loom.link.Outcome` (refused, dispatch-refused, unlinked or lost). *Where:* the
supplied host's link (`src/host/link.hpp`), the ask in `zen/bridge/link.hpp`;
[link to another host](running-loom.md#10-link-to-another-host),
[bridge](../reference/bridge.md).

## Where the exact rules live

This page explains; the rules, bounds and edge cases are in the reference and the laws, which
are the only pages that bind:

- [messaging reference](../reference/messaging.md): the delivery order, every refusal reason,
  answers, office authorship, fences;
- laws: [MSG](../laws/messaging-laws.md), [ANS](../laws/answer-authority-laws.md),
  [GATE](../laws/admission-laws.md);
- [messaging guide](messaging.md): what to write, shape by shape, with
  [`examples/answering.cpp`](../../examples/answering.cpp) as its compiled twin.
