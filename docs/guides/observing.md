# Observing — the guide

You have a participant that knows something, and other participants that want to
know it, repeatedly, cheaply. This is that.

Reference: [senses](../reference/senses.md). Laws:
[SENSE-01..05](../laws/sense-laws.md).

## The choice, in one table

| You want to say | Use |
|---|---|
| something happened | a message (`publish`) |
| please do something | a message (`send`) |
| answer this question | an ask + an answer |
| **this is what I currently claim is so** | **a Sense** |
| (as a reader elsewhere) tell me each thing that office publishes, as it happens | a subscription at the host's [observation relay](#following-another-participants-publications) |

If a consumer would otherwise ask the same question over and over and act on the
reply, that is a Sense. If the *asking* is the point — because it changes
something, or because the answer must be authenticated to a conversation — that
is still an ask.

## Claiming

Declare what you can claim, then claim it:

```cpp
class Lamp : public loom::WeaveBase<Lamp, LampState,
                                    loom::Accept<Dim>,
                                    loom::Emit<>,
                                    loom::Claims<Brightness>> {
public:
    void on(const Dim& d, loom::Mail& mail) {
        state_.level = std::max(0, state_.level - d.by);
        mail.claim(Brightness{state_.level});
    }
};
```

One line at the end of an ordinary handler. It enqueues nothing, allocates no
seq, and produces no bus event.

`Claims<...>` is not decoration: an undeclared shape is refused, and the declared
set registers at mount so consumers can discover what you provide before you have
claimed anything.

## Claiming as an office

Same law as office speech, same grammar:

```cpp
mail.as_role("room.light").claim(Brightness{state_.level});
```

Holding the role attaches nothing — a personal claim stays personal. This is the
deliberate act, verified at the claim moment, and a claimant that does not hold
the office is refused (`OfficeNotHeld`) with nothing stored.

Claim as the office when the thing claiming is **the service**, and personally
when it is **this particular weave**. A successor should claim as the office from
its `zen.Activated` handler; before that, the office's claim is its
predecessor's and says so.

## Observing

```cpp
loom::SenseReading r = mail.latest<Brightness>(lamp_id);
if (r) {
    Brightness b = loom::from_value<Brightness>(*r.value);
}
```

You get a **copy**. Nothing you do to it reaches the claimant.

Ask the host for a read rule when you mount the reader — it is absent by default:

```cpp
bus.register_weave(std::move(panel), loom::Grant{}.allow_observe("Brightness", 1));
kernel.load("panel", path, "", loom::Grant{}.allow_any().allow_observe_any());
```

## The four things to get right

**1. A claim is not the truth about the world.** It is the latest claim Loom
accepted from that author. Queued work may already make it stale. If you need
"this is exactly true right now", you need a boundary the domain constructs —
see [handoff](../reference/handoff.md).

**2. Check the refusal, not just emptiness.**

```cpp
if (!r) {
    switch (r.refusal) {
    case loom::SenseRefusal::NoClaim:       /* nobody has claimed it */ break;
    case loom::SenseRefusal::NotAuthorized: /* your grant, not their silence */ break;
    default: break;
    }
}
```

`NoClaim` and `NotAuthorized` send you to opposite places. Treating them as one
empty answer is how a misconfigured grant becomes an afternoon.

**3. Decide what a stale office claim means to you.** After a role moves, a
role-bound reading is the *predecessor's*, honestly stamped:

```cpp
if (r && r.by.office_holder_is_current) { /* the current holder's claim */ }
if (r && r.by.office_claim_is_stale())  { /* the previous holder's */ }
```

Loom will not hide the distinction, and will not choose for you.

**4. Do not build a journal out of it.** A Sense keeps one value per key. If you
want history, tap the bus (`add_observer`) — that is what the tap is for.

## Following another participant's publications

A Sense is the latest claim; it cannot say *every* time something happened. A reader that must
see each occurrence — an agent following one build through to its end on another host, a monitor
counting enemies past a checkpoint — subscribes at the producer host's **observation relay**
([reference](../reference/observation.md)):

```python
sub = ctx.observe("td.game", [("TdSeen", 1), ("TdOccurred", 1)], via="workshop",
                  latest=["TdSeen"])          # TdSeen is where the game stands: the newest is enough
press = ctx.ask("zengine.input", "InjectInput", {...}, via="workshop", settle=True)
began = [o for o in sub.drain() if o.kind == "observed" and o.cause == press.correlation]
while True:
    o = sub.next(30.0)                        # Observation, Gap, Ended -- or None: your wait ran out
    ...
```

Get these right, in this order:

1. **The producer's host decides.** Nothing is observable until that host mounts a relay with a
   policy that admits you, and its default admits nobody. Being able to type into an application
   is not permission to observe it, and observing it is no permission to act on it.
2. **Subscribe before you act.** A subscription tells what is published after it began and
   nothing before; the answer to `Subscribe` is the moment. A reader that joins late learns the
   past only from the producer — its whole-state shape (name it `latest`) and its own numbering.
3. **Attribute by `cause`, not by arrival.** A settle-requested ask's synchronous consequences
   carry your correlation; the first thing to arrive after a key press is only the first thing to
   arrive. Work the producer defers (a build, a timer) carries none: join it by the producer's own
   words (an ask number, an operation number).
4. **A `Gap` means "cannot tell".** Occurrences can be lost — a shut window, a slow reader — and
   each loss is said. A count that needed them is not a smaller count; it is unknown.
5. **Ending stops only the words.** Releasing, or losing the link, says nothing about what the
   producer did next.

## What it is not

- **not shared memory** — readings are by value, and there is no lvalue path into
  a claimant;
- **not a message** — no causality, no ordering guarantees of its own beyond
  "later than the claim call";
- **not free** — reading is authorized, deliberately, so a Sense repository does
  not become a universal read rail;
- **not a snapshot ceremony** — see
  [handoff](../reference/handoff.md#exact-vs-stale--keep-them-visibly-apart).
