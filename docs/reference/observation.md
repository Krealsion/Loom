# Observation (one participant's publications, told to a subscriber its host admits) — reference

A producer **publishes** what happened, in its own vocabulary, and whoever declares that shape
hears it. A participant somewhere else — another host's agent, a monitor, a tool run — often
needs the same sentences *as they happen*: that a build was taken, that an enemy stepped onto a
cell. It is not a listener the producer's host installed, and it must not get a tap on that
host's whole bus. An **observation relay** is how a host offers exactly that: a subscriber asks
the relay's office for **one office's publications of named shapes**, the host's policy says yes
or no, and from then on the relay tells the subscriber each such publication — in order, numbered,
bounded — until the subscription ends.

Vocabulary: [`zen/observe/vocabulary.hpp`](../../include/zen/observe/vocabulary.hpp). Relay:
[`zen/observe/relay.hpp`](../../include/zen/observe/relay.hpp). Guide:
[observing](../guides/observing.md#following-another-participants-publications).
Limits: [bounds](bounds.md#observation-relay).

| You want | Use |
|---|---|
| the latest claim a participant made available | a [Sense](senses.md) |
| every delivery and refusal on your own bus, host-side | the tap, or [history](history.md) |
| one producer's publications of named shapes, live, as a subscriber some host admitted | **a subscription at `loom.observe`** |
| what happened before you asked | the producer's own record, or history your host keeps — never a subscription |

## Mounting a relay

A relay is host wiring, like a bridge server: it registers listeners, reads who holds an office
and speaks as itself outside its own deliveries.

```cpp
loom::observe::Relay* relay = loom::observe::mount_relay(
    bus, my_policy,                                      // who may observe what
    [&](loom::Fence f) { return server.settle_origin(f); }); // who opened which fence (optional)
```

`mount_relay` registers it in the office `loom.observe` with `relay_grant()` (its own vocabulary
and the standard replies, to whoever asked) and attaches it to the bus. The pointer is for the
host's own calls: `revoke(subscriber, why)`, `end_all(kind, why)`, `forget(subscriber)`,
`rows()`.

**The policy decides, and its default admits nothing.** `ObservePolicy` is a function of the
`ObserveRequest` — the subscriber as the bus stamped it, the producer office, the shapes, the
subscriber's label — returning `allow()` or `refuse(words)`; the words reach the subscriber
verbatim. `observe_nothing()`, the default, refuses everyone and says so. There is no "allow
everything" switch: a host that wants one — a test, a development host — writes that policy
itself, visibly, and nothing in a subscriber's reach can write it for it.

## Authority: observation is its own decision

- **Asking is not being admitted.** A `Subscribe` needs an ordinary send rule to the relay's
  office, like any ask. Being answered `Subscribed` is the host's policy, and only that.
- **No other power implies it.** A guest that may inject input, take pictures or ask what a
  participant accepts has not thereby been allowed to observe anything; a relay is no bus-wide
  feed, and a subscription names one office and at most eight shapes.
- **Observing grants nothing.** A subscriber that sees a producer's words may still say nothing
  to it: the relay's words are ordinary speech, never authority, and never an answer to anything
  the subscriber asked other than `Subscribe` and `Release`.
- **A producer's own grant still governs.** A listener hears a publication the way every
  listener does — at enqueue, gated by the publisher's grant — so a producer that may not publish
  a shape cannot be observed publishing it, and needs no change to be observed.
- **The host decides, not the subscriber.** A subscriber cannot admit itself: the relay's policy
  is the host's code, and a session's credentials never reach it.

## Subscribing

`Subscribe { producer, shapes, latest, encoding, window, label }` is answered `Subscribed` or
`zen.Refused`:

| Refused because | Words |
|---|---|
| the host's policy said no | the policy's own |
| a shape nobody on this bus declares | `no participant here declares <Name> vN` |
| no shapes, or more than `kMaxShapes` | `a subscription names 1 to 8 shapes` |
| `latest` names a shape not subscribed | said so |
| an encoding other than `native` or `compat` | said so |
| `kMaxSubscriptions` held by the relay, or `kMaxPerSubscriber` by this subscriber | said so; release one first |

`Subscribed { subscription, relay, producer, holder, incarnation, window, encoding, latest,
shapes, next }` says where the subscription begins: the relay's **lifetime** (minted when it was
made, never reused — a subscription number is only ever read together with it), the office's
holder and incarnation at that moment (0 when nobody holds it), the window granted, and the
number the first word will carry. `shapes` is `loom.observe.Shapes`: `zen.SchemaDesc` v1 for each
subscribed shape and the closure it nests, in the subscription's encoding — so a subscriber
decodes a shape its own host never declared (the Python client does exactly that).

## Ready means ready

A subscription **begins at the moment the relay handles `Subscribe`** on the producer's bus. Its
listener is registered then and `Subscribed` is enqueued then, so every publication **enqueued
after that moment** is told, and the answer reaches the subscriber before the first of them. A
publication enqueued before it is not told, and nothing earlier is replayed: a relay keeps no
history and invents none.

So a late subscriber joins the **continuation**, and learns the past only from the producer. Two
ways a producer makes that possible, both its own: a **whole-state** shape (the producer states
where it stands after every change — name it in `latest`) is a baseline as soon as the next one
is said; and an **owner sequence** (an occurrence numbered by its producer with no gaps) tells
the subscriber exactly how much it missed before joining and whether it misses anything after.
A late subscriber that needs to count must treat what came before its first number as unknown,
not as zero. No part of this is a sleep: the order above is the bus's own.

## What one observation says

| `Observed` field | Is | Is not |
|---|---|---|
| `seq` | this subscription's number for it, from `next` with no holes (shared with `Gap` and `Ended`) | a bus sequence |
| `shape`, `version`, `payload` | the published value, in the subscription's encoding | re-validated authority: it is what the producer published |
| `producer`, `incarnation` | the bus-stamped publisher and its incarnation **at delivery** — a replaced holder, or a new incarnation after a reload, shows here | a name the producer chose |
| `office` | the office it deliberately spoke as, if it did | proof it holds that office now |
| `coalesced` | for a `latest` shape: how many earlier publications this one stands for | a count of occurrences lost |
| `cause` | the **subscriber's own** correlation of the settle-requested send whose synchronous dispatch published it, or 0 | another participant's number, ever |
| `delivery`, `published_in` | the producer bus's sequence numbers of this publication's delivery to the relay and of the delivery during which it was published — references into that host's history | identities anywhere else |
| `link`, `epoch`, `session` | filled by a link that carried it (below); empty on the relay's own bus | |

**Only the office's holder is told.** A listener hears a subscribed shape from anyone who may
publish it; the relay tells the subscriber only what the bus-stamped holder of the named office
published at that delivery. The same shape from anyone else is counted (`foreign` in `Status`)
and never told — a forged or wrong-source publication cannot reach a subscriber as the
producer's.

**Order** is the producer bus's delivery order to the listener: FIFO, one bus. It says nothing
about any other bus, and nothing about when the producer's *other* consequences land.

## Cause: what the subscriber's own send set in motion

A [fence](messaging.md#fences-when-what-one-send-set-in-motion-has-been-dispatched) is the bus's
record of everything one host-fenced send set in motion synchronously. A bridge server opens one
for each settle-requested send of a session, and `settle_origin(fence)` says which session and
correlation opened it. A relay given that record (`FenceOrigins`) marks an observation published
inside such a fence with the opener's correlation — **only when the opener is the subscriber**.
One session is never told another's numbers.

What `cause` proves: *this publication was set in motion by your send N*, by the bus's own
causality, not by arrival order. What it does not: anything published later from deferred work
(a timer beat, an out-of-process build, a deferred answer spent from another delivery) carries
`cause` 0 even when your send began it — the producer's own words must join those (an operation
number, an ask number). Because a fence settles after its caused deliveries, a settled send's
caused observations are said before its `Settled`: across a bridge they arrive before the
answer that follows it.

## Bounded, and never silent

- **The window.** At most `window` numbered words stand unacknowledged per subscription
  (`kDefaultWindow` when the subscriber says 0, at most `kMaxWindow`). `Acknowledge { through }`
  opens it by that much; a `through` the relay never said is refused.
- **While it is shut, occurrences are dropped and counted**, and the count is said as one
  `Gap { lost, reason }` before anything later — never silently. A shape the subscriber named
  `latest` keeps only its newest publication instead, and that one says how many it stood for
  (`coalesced`). The subscriber decides which of its shapes are state: a count that must be exact
  names none of its occurrences `latest`.
- **The producer never waits.** Nothing a subscriber does — reading slowly, never
  acknowledging, vanishing — slows or blocks the producer; it only costs the subscriber words,
  each loss said.
- **No exactly-once, anywhere.** A relay says each number once; a subscriber finds duplicates and
  holes by the numbers, and treats any `Gap` — the relay's, or one it found itself — as "cannot
  tell". The Python client does both (below). Identity, order and counting rules above are
  sufficient for a count on one subscription of one producer, and for nothing wider.

## Owned lifetime

| Ends because | How it is said | Freed |
|---|---|---|
| its subscriber released it | `Release` is answered `Ended { kind: released }` | listener leaves the bus with its schema claim |
| the host withdrew it (`revoke`) | `Ended { kind: revoked, reason }`, said | the same |
| the relay is ending (`end_all(kGone, …)`) | `Ended { kind: gone }`, said | the same |
| the subscriber is gone (`forget`, or found gone at a delivery) | nothing: there is nobody to tell | the same |
| the session it crossed on ended (a link) | `Ended { kind: lost }`, said by the link | the far relay's own, when that host notices |
| its local subscriber is gone (a link) | nothing; the link asks the far relay to release on the host's own turn | the far relay's, when its answer comes; the link's custody then |

`Ended.last` is the last number said before it and `lost` counts occurrences still held back and
never told. **Ending an observation stops nothing the producer was doing** — a build goes on, a
game goes on — and says nothing about what it did next.

## Across a link

The supplied host's link ([bridge](bridge.md#the-connecting-side)) carries a subscription made through it
exactly as it carries any ask, and holds its **custody**: which far subscription (relay lifetime,
number, the far participant that answered) belongs to which local asker, on which session
(`epoch`). It forwards the far relay's words only for a subscription it holds, from the
participant that answered it, on the session it was made on; everything else is counted
(`stray_observations`) and dropped. It fills `link`, `epoch` and `session`, and translates `cause`
from its own far attempt to **the local asker's own correlation** — or 0. When the session ends,
each subscription it carried is told `Ended { kind: lost }`: what the far relay said after the last
word forwarded is unknown. A new session after a reconnect is a new epoch: nothing of the old one
is forwarded into it, and a subscription number reused by a restarted relay is told apart by the
relay's lifetime.

**Whose subscription it is stays the link's to say.** The far relay judges `Release` and
`Acknowledge` by their subscriber, and every local asker is the link's one session there, so the
far check cannot tell one local asker from another. The link decides before anything crosses: a
control — in either encoding, addressed to the relay's office or to its id — goes out only for the
local participant that holds that subscription, exactly (the same life and incarnation), on the
current session and under the relay lifetime the control names. Anybody else's, and any control
naming a subscription that ended, a session that ended or another relay lifetime, is answered
`Outcome { refused }` with attempt 0: nothing was submitted, and the holder's window and custody
are exactly as they were. Knowing a subscription's numbers is not holding it.

**A local subscriber that is gone is released on the host's own turn.** Removed, dead, or
succeeded by a new life or incarnation — including while its subscription was still being made —
it is noticed by the link's `service()`, not at the next word, because a silent producer sends no
word and a full window can never reopen for a reader that is gone. The link asks the far relay to
release it and tells nobody. Asking is not the far side having let go: the link keeps counting the
subscription (`releasing()`, and the host console's `links` line) until the far relay's answer or
its own `Ended` says it is over; a release the far host refuses to deliver leaves it counted until
the session ends. So abandoned subscribers never use up the far relay's allowance for the session
(`kMaxPerSubscriber`) or the link's own (`kMaxWatches`), and ending one never touches another
asker's subscription or anything the producer is doing.

## From Python

```python
sub = ctx.observe("zengine.builder", [("BuildAsked", 1), ("BuildStatus", 4)],
                  via="workshop", latest=["BuildStatus"])
answer = ctx.ask("zengine.input", "InjectInput", {...}, via="workshop", settle=True)
mine = [o for o in sub.drain() if o.kind == "observed" and o.cause == answer.correlation]
item = sub.next(30.0)   # Observation, Gap or Ended -- or None when this wait ran out
```

`Context.observe` returns a run-scoped `Subscription`, released in the run's cleanup however it
ends. `next(timeout)` hands over items in order; `None` means *this* wait ran out, never that the
producer is silent for good. The client checks the numbers itself: a hole becomes a local `Gap`
with its range, a number already passed is kept out (`repeated`), and past its own bound
(`max_pending`) it drops observations **here** and hands the loss over as a local `Gap`. It
acknowledges half a window at a time, or an eighth once it has read everything, so a burst after
a quiet spell finds nearly the whole window open. `summary()` is its account: words said, lost
(the relay's and its own), repeated, last taken, acknowledged, and how it ended.

## What it is not

- **Not history.** Nothing before the subscription began, and nothing retained: a subscriber
  that needs the past asks the producer's owner, or a host's history, for it.
- **Not a tap.** One office, named shapes, one subscriber, the host's decision each time.
- **Not an answer.** An observation is not the outcome of anything the subscriber asked; a
  dispatch observed is not completion, and a producer's word is only its word.
- **Not a completion proof.** A caused observation proves what the send set in motion *here*;
  work a producer defers, and effects on other hosts, are the producer's to state.

## Tests

Suite `observe` (the relay: policy, readiness, holder-only, cause, window, gap and coalescing,
every ending, retirement); suite `bridge` (the link's custody: forwarding, cause translation,
strays, `lost` at a session's end, controls only from their holder, stale controls, release of a
gone subscriber with a silent producer, a full window or mid-subscribe, abandonment past the far
allowance, an undelivered release still counted); the Python client's O-checks
(`tests/session/test_client.py`: numbering, holes, local bounds, acknowledgement, cause); the
two-process journey `observe_journey` (with real workers stopped outright or exiting unreleased,
more of them than the far allowance).
