# Sense laws (SENSE)

Reference: [senses](../reference/senses.md). Guide:
[observing](../guides/observing.md).

A **Sense** is a deliberate immutable claim of the latest observation a
participant has made available. It is the second thing a participant can say:

```text
MESSAGES   what happened / what I want done      causal, FIFO, queued
SENSES     what I currently claim is so          acausal, latest-only, pulled
```

The canonical term is **latest claim**, used everywhere. Never "current state",
never "same-frame truth", never "latest real state".

## SENSE-01 — A claim is authored observation, not shared state

LAW — A Sense is a value a participant **deliberately claimed**, materialized by
Loom and handed to readers **by value**. No reader holds a pointer, reference or
alias into a claimant.

MEANS
- `other.sense.health = 9000;` has no spelling — a reading owns its copy, and
  scribbling on it reaches nobody;
- mutation of another participant remains what it always was: intentional Loom
  traffic (a domain message, a Poke, an authorized operation);
- a claim is admitted through the one gate before it is stored, so the
  repository never holds a value that did not pass the gate;
- a claim is an act: nothing is claimed on a weave's behalf, ever.

DOES NOT MEAN
- that reading is free of authority — see SENSE-05;
- that a Sense is a message: claiming and observing enqueue nothing, allocate no
  seq, and produce no `BusEvent`;
- that a weave may claim any shape — only what it declared (SENSE-04).

PROVEN BY — `Switchboard::claim_as` / `observe`, `SenseReading::value`
(`std::optional<Value>`, by value; the repository is private and reached only by
container operations); suite `sense` (the reading-owns-its-value case, the
no-bus-traffic case), suite `kernel` (dynamic parity: a loaded reader receives
bytes, never a host pointer).

## SENSE-02 — A claim becomes visible at the claim call, and predicts nothing

LAW — A successful claim is visible to any observation that happens after it.
Loom never applies queued work speculatively to make a claim look current.

MEANS
- a reader delivered **after** a state-changing message observes the new claim;
  one delivered **before** it observes the previous claim;
- the repository reorders nothing, because it participates in causality at all;
- pending FIFO work may already make a claim stale with respect to what happens
  next, and Loom says so rather than hiding it;
- the settlement rule is the smallest one available: because dispatch is
  single-threaded and non-reentrant ([MSG-01](messaging-laws.md)), no other
  participant can run between a claim call and the end of the handler that made
  it, so "visible at the call" and "visible at handler completion" are
  indistinguishable to everyone except the claimant observing itself. Loom takes
  the simpler rule and has no settlement step to forget.

DOES NOT MEAN
- that a claim is the truth about the world — it is the latest claim Loom
  accepted from that author, and nothing more;
- that a handler's claims are transactional: there is no rollback, and a claim
  made mid-handler stands even if the handler later throws;
- that revisions are a clock — a revision orders replacement **of one key**, and
  is not comparable across keys.

PROVEN BY — `Switchboard::claim_as` (writes on the call); suite `sense` (S1 the
ordering witness, S2 the no-future-knowledge witness, the newer-claim-always-wins
case).

## SENSE-03 — Role movement never relabels a predecessor's claim

LAW — An office claim records the exact weave that made it. When the role moves,
the claim is still that weave's, stamped `office_holder_is_current == false`. A
successor is considered to have claimed nothing until it deliberately claims.

MEANS
- a role-bound reading after a replacement returns the predecessor's claim, with
  `author`, `office` and the staleness stamp all truthful;
- the alternative — returning nothing — was **deliberately rejected**: it would
  collapse "this office has never claimed" and "this office's claim is the
  previous holder's" into one empty answer, and those are different facts. A
  reader wanting the strict view writes
  `if (r && r.by.office_holder_is_current)`, which is one visible line;
- once the successor claims as the office, the role-bound view follows it, at
  the next revision of the same key;
- `author_life_is_current` answers the same question about the claiming *life*,
  mirroring `BusEvent::sender_life` / `sender_life_now`.

DOES NOT MEAN
- that a stale claim is withheld — staleness is a stamp, never an absence;
- that a sealed candidate can pre-claim an office: it does not hold the role, so
  it is refused (SENSE-04);
- that the reading is recomputed history — the author, life and incarnation are
  stored as of the claim; only the two "is that still true?" questions are asked
  at read time, so they can never go stale inside the repository.

PROVEN BY — `Switchboard::authorship_of` (currency asked at read, never stored);
suite `sense` (S3, the office/personal separation), suite `kernel` (S3 across a
real committed admission, where the role moves in place), suite `handoff` (the
same across a full authored handoff).

## SENSE-04 — Claiming as an office is explicit; the claim-set is a contract

LAW — A weave may claim only shapes it declared in `Claims<...>`, and may claim
**as an office** only if it holds that office at the claim moment. Holding a role
attaches nothing.

MEANS
- the same holder's personal claim and office claim are different keys, and a
  reader can tell — this is [MSG-07](messaging-laws.md#msg-07--role-authorship-is-explicit)'s
  law in the Sense category, reusing the same `as_role(...)` grammar because it
  is the same law;
- an undeclared shape refuses `Undeclared`; an unheld office refuses
  `OfficeNotHeld`, and **nothing is stored** — never a silent downgrade to a
  personal claim;
- the claim-set registers at mount, so a participant's Sense capability is
  discoverable **before** it has claimed anything, rather than after a runtime
  claim accidentally reveals a shape;
- `Claims<...>` is a third list because a claim is neither an accepted message
  nor an emitted one. Reusing `Emit<...>` was rejected twice over: a Sense is
  not an emitted message, and `Emit` is informational and does not register.

DOES NOT MEAN
- that the claim-set widens anything — it is a declaration, not a grant;
- that a role holder's ordinary claim becomes the office's (the exact
  MSG-04/MSG-07 mistake in a new costume);
- that declaring a shape claims it: a declared, never-claimed Sense reads
  `NoClaim`;
- that **no mechanism** can attribute a claim to another weave. The law binds
  *participants*: `Mail` offers no spelling for it, so an ordinary weave claims
  as itself or as an office it holds, and a loaded artifact reaches only the
  gated path where the host verifies membership. The trusted root/host door
  `Switchboard::claim_as` / `office_claim_as` names the claimant explicitly, as
  `Switchboard::send` is the ungated send. Saying otherwise would overstate the
  law and leave an operator auditing provenance unaware that a host-authored
  claim is representable at all.

PROVEN BY — `Switchboard::make_claim` (declaration check), `office_claim_as`
(`holds_role_now` at the claim moment); suite `sense` (S4, S6, the discovery
case), suite `kernel` (the sealed candidate refused, and dynamic parity).

## SENSE-05 — Reading is authorized, and the repository is bounded

LAW — Observing a shape requires an explicit **observe rule** on the reader's
grant, absent by default. The repository retains one claim per meaningful current
key and nothing else.

MEANS
- no existing weave gained any reach from Senses existing, so a Sense repository
  is not a universal data-exfiltration rail: reading takes a deliberate host
  decision, out of band, exactly as sending does;
- a send rule is **never** consulted for a read. The two answer different
  questions ("may you emit this shape *there*" vs "may you pull it"), and
  reporting one as the other would send an operator to edit the wrong thing;
- refusal, absence and staleness stay three different answers: `NotAuthorized`,
  `NoClaim`, and a stamped stale reading;
- authorization happens **before** the lookup, so an unauthorized reader cannot
  learn whether a claim exists;
- keys: personal claims die with the weave; office claims die when the role
  becomes unheld. A replacement's admission overwrites the holder in place and
  never passes through unheld, which is exactly why a predecessor's office claim
  survives it (SENSE-03);
- a reload or a thousand re-claims replace the value under one key — the
  repository is bounded by registered weaves × declared shapes plus held roles ×
  shapes, never by claims ever made and never one entry per historical
  incarnation.

DOES NOT MEAN
- that reading is narrowable by claimant today — the rule selects a **shape**,
  which is the granularity `allow_to_any` already has for sending. A
  per-claimant rule waits for a consumer that needs it;
- that the host is gated: holding a `Switchboard&` is root authority, and its
  `observe` is ungated exactly as its `send` is;
- that a dropped claim is a claim that was wrong — an office with no
  officeholder simply has no current claimant, and keeping the claim would be a
  claim by nobody.

PROVEN BY — `Grant::permits_observe` (its own rule vector),
`Switchboard::observe_as` (authorization before lookup),
`forget_personal_claims` / `forget_office_claims`; suite `sense` (S5 the
lifetime witness, S6 the authorization witnesses), suite `kernel` (a loaded
reader refused without a rule; `Kernel::load`'s explicit-grant overload).
