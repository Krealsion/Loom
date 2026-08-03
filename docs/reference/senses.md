# Senses — reference

The second thing a participant can say. Laws:
[SENSE-01..05](../laws/sense-laws.md).

```text
MESSAGES   what happened / what I want done      causal, FIFO, queued
SENSES     what I currently claim is so          acausal, latest-only, pulled
```

A **Sense** is *a deliberate immutable claim of the latest observation a
participant has made available*. The canonical term is **latest claim** — never
"current state", never "same-frame truth".

## Why it is not a message

A renderer, inspector, status panel or editor warning asks for already-known
state many times. Expressed as traffic that is
`ask → FIFO → handler → answer → FIFO → reader`: six steps, two queue turns, and
no causality added. A Sense is a synchronous read of a value somebody already
decided to expose.

It is deliberately **not** a second state system: it carries no causality,
participates in none, reorders nothing, never applies queued work speculatively,
and never grows a journal.

## Claiming

```cpp
class Lamp : public loom::WeaveBase<Lamp, LampState,
                                    loom::Accept<Dim>,
                                    loom::Emit<>,
                                    loom::Claims<Brightness>> {
    void on(const Dim& d, loom::Mail& mail) {
        state_.level -= d.by;
        mail.claim(Brightness{state_.level});             // personal
        mail.as_role("room.light").claim(Brightness{...}); // as the office
    }
};
```

`Claims<T...>` is a third declaration list beside `Accept<...>` and `Emit<...>`,
because a claim is neither a delivered message nor an emitted one. Unlike
`Emit<...>` it is **enforced** (an undeclared shape refuses) and it **registers**
at mount — which is what makes a participant's Sense capability discoverable
before it has claimed anything.

`SenseClaimResult{accepted, why, revision}` is the verdict. `why` is one of
`Undeclared`, `OfficeNotHeld`, `GateRefused`.

## Observing

```cpp
loom::SenseReading r = mail.latest<Brightness>(lamp_id);          // personal
loom::SenseReading r = mail.latest_from_office<Brightness>("room.light");
if (r) { Brightness b = loom::from_value<Brightness>(*r.value); }
```

The reading owns its value — `std::optional<Value>`, by value. There is no
pointer or reference into the claimant anywhere in the type, so
`other.sense.level = 9000;` has no spelling ([SENSE-01](../laws/sense-laws.md)).

`SenseRefusal` keeps four different problems apart, because they send a maker to
four different places:

```text
NoClaim         nothing has been claimed under this key
NotAuthorized   the reader's grant does not permit observing this shape
Undeclared      (claim side) the shape is not in Claims<...>
OfficeNotHeld   (claim side) the claimant does not hold that office
GateRefused     (claim side) the value did not pass the gate
```

## When a claim becomes visible

**At the successful claim call.** Nothing defers to handler completion; there is
no settlement step.

That rule is the smallest one available *and* indistinguishable from the
alternative: dispatch is single-threaded and non-reentrant
([MSG-01](../laws/messaging-laws.md)), so no other participant can run between a
claim call and the end of the handler that made it. Only the claimant, observing
itself, can tell them apart.

The consequence is the ordering guarantee, and it comes free from FIFO rather
than from anything this repository does:

```text
queue:  Damage{18}   ReaderTick   Damage{30}   ReaderTick
              ↓           ↓            ↓            ↓
         claim 82    reads 82     claim 52     reads 52
```

A reader queued **ahead** of a change reads the previous claim. Loom will not
apply queued work speculatively to make a claim look current
([SENSE-02](../laws/sense-laws.md)).

## Provenance

Every reading carries `SenseAuthorship`:

```text
author                    the exact weave that claimed
author_life               + author_incarnation, as of the claim
author_life_is_current    is that still the life at that address?
office                    the office it was authored as; empty = personal
office_holder_is_current  meaningful only when office is non-empty
revision                  monotonic per key; orders replacement of THIS claim
schema_name + version
```

`author_life_is_current` mirrors `BusEvent::sender_life` / `sender_life_now` —
the same question, the same answer shape. Both "is that still true?" fields are
asked **at read time and never stored**, so they cannot go stale inside the
repository.

A Sense value is therefore never naked globally-trusted data: a reader can always
answer *who claimed this?*

## Personal is not office

Holding a role attaches nothing. The same holder's `mail.claim(x)` and
`mail.as_role(R).claim(x)` land under **different keys** — separate maps, so a
personal claim is not reachable through an office key and cannot be promoted into
one. This is [MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit)'s
law in this category, in the same grammar, because it is the same law.

## Role movement

After a replacement moves the role, a role-bound reading still returns the
**predecessor's** claim, stamped `author = predecessor`,
`office_holder_is_current = false`. It is never relabelled, and the successor is
considered to have claimed nothing until it deliberately claims.

Returning nothing was deliberately rejected: it would collapse "this office has
never claimed" and "this office's claim is the previous holder's" into one empty
answer. The strict reading is one visible line:

```cpp
if (r && r.by.office_holder_is_current) { ... }
```

## Authority

Reading requires an **observe rule**, absent by default:

```cpp
Grant{}.allow_observe("Brightness", 1)   // this shape
Grant{}.allow_observe_any()              // an inspector, a renderer, a console
```

A **send rule is never consulted for a read**. They answer different questions
("may you emit this shape *there*" vs "may you pull it"), and reporting one as
the other sends an operator to edit the wrong thing. Because the floor is empty,
no existing weave gained reach from Senses existing — a Sense repository is not a
universal exfiltration rail. Authorization happens *before* the lookup, so an
unauthorized reader cannot learn whether a claim exists.

Loaded artifacts get their grant from `Kernel::load`'s explicit-grant overload:
reading another participant's claims is a host decision, not a consequence of
being loadable.

## Lifetime

| Key | Replaced by | Erased when |
|---|---|---|
| personal `(WeaveId, shape)` | a newer claim from that weave | the weave is unregistered |
| office `(role, shape)` | a newer claim as that office | the role becomes unheld |

Bounded by `registered weaves × declared shapes` + `held roles × shapes` — never
by claims ever made, never one entry per historical incarnation. A reload, a
revival or a thousand re-claims replace the value under one key.

An admission overwrites the role holder **in place** and never passes through
unheld, which is exactly why a predecessor's office claim survives a replacement
rather than being deleted.

## Native/dynamic parity

Senses cross the seam at [ABI v6](dynamic-abi.md): claim/observe doors both ways,
and the declared claim-set in the manifest. A loaded reader receives **bytes**
and re-admits them against its own definition of the shape, so no host pointer
into the repository ever reaches a library. The out-of-process pipe fails closed
in both directions, joining the standing V1 law.

## Tests

Suite `sense` (S1–S6, discovery, the reading-owns-its-value case, the
no-bus-traffic case); suite `kernel` (office claim across a real committed
admission, the sealed candidate refused, dynamic parity); suite `handoff`
(Senses meeting an authored handoff).
