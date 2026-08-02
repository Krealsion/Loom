# Known seams — reference

Current limitations and open design candidates, stated precisely so shorthand
cannot harden into false guarantees. Statuses used here: **KNOWN SEAM**
(a real, current limitation), **EVIDENCE-BACKED CANDIDATE** (core work an
application portfolio motivates), **GUIDELINE** (a design rule for
applications), **KNOWN AUTHORING FRICTION** (ergonomics, not missing truth),
**SPECULATIVE** (a sketch, not an API).

## Role-authored provenance

**Status: OPEN / EVIDENCE-BACKED CORE CANDIDATE.**

Four distinct facts, and only the first three exist today:

```text
sender identity          who the exact weave was            EXISTS (the bus stamp)
role-addressed delivery  where the message was routed       EXISTS (send_to_role)
role membership          which office the weave holds now   EXISTS (role_holder lookup)
role-authored provenance which office the weave
                         DELIBERATELY SPOKE AS              OPEN — does not exist
```

Loom currently cannot prove that a delivery was **intentionally authored in
the capacity of role R, with Loom verifying at authorship time that the sender
actually held R**. The missing fact has two parts, and both are load-bearing:
*authorization* (the sender actually held R) and *authorship intent* (this
particular message was deliberately spoken as R). Holding the role is
necessary; **holding the role is not sufficient** — the same exact weave,
holding the same exact office, may speak personally or speak as the office,
and those are different statements (Night Lab's lobby distinguishes them
directly). A fact reduced to "the sender held role R" would authenticate
personal speech merely because its author happened to occupy an office. And
`send_to_role(R, m)` is none of this: it means "deliver to whoever holds R" —
never "I speak as R" ([MSG-04](../laws/messaging-laws.md)).

Consequence: an *office's* announcements are unauthenticatable — including
**publications**, which any authorship design must cover (a direct-send-only
answer misses the forged-`WorkerOpen` case). Identity works when you talk to
somebody; it fails when you talk to whoever holds an office. Five independent
Night Lab sightings, escalating to a forged announcement destroying healthy
work and a player joining an attacker's server — and both application-level
workarounds are built and **priced**: a registry's belief rests on an
unauthenticated announcement; inverting push to pull works but spends the
Loom-wide deferral bound, strands strict receivers across honest replacement,
and cannot cover observers of publications
([evidence](../evidence/night-lab.md#role-authored-provenance--five-sightings-workarounds-priced)).

If built, the shape is a delivery fact of the same family as `answers_ask()`,
carrying **both** halves: *this delivery was intentionally authored as role R,
and Loom verified that the author held R at the moment of authorship*. Any
syntax (e.g. `send_from_role`) is **SPECULATIVE**; do not document it as
current.

## Sender cannot observe send fate

**Status: KNOWN SEAM.**

A weave-originated send may be refused later at delivery; the sending weave
receives no eventual delivery result (native tickets are journal handles the
*host* can read; dynamic sends return no ticket at all). Applications needing
absence detection author it per domain: watchdogs, timeouts, reconciliation,
application-level acknowledgment. Do not invent ticket-outcome semantics in
docs or designs; the refusals are observable on the tap/journal — by an
observer, not the sender.

## Continuity is authored

**Status: GUIDELINE (the negative half is law: [PR-09](../laws/replacement-laws.md)).**

Prepared replacement verifies the successor and **does not create an atomic
continuity handoff from the incumbent**; graceful swap preserves authored work
and verifies nothing. The two ceremonies are disjoint. What applications do
about it is a **domain pattern, not a substrate ceremony**: ask the incumbent
to *describe* itself with an ordinary, non-mutating question, and supply that
description to the candidate's preparation. The staging varies — several Night
Lab projects captured the description *before* the successor was even loaded,
others during preparation — and either way the description is a **snapshot
taken while the incumbent remains live**: the incumbent may change after it,
unless the application or package establishes a stronger boundary. The
application decides whether stale, replayed, restarted, degraded, or exactly
transferred state is acceptable. **What crosses is a domain decision** — six
Night Lab applications carried six different things (work / obligation /
intent / a reopened question / a waiting-fact / a fleet tally) — so the
repeated thing is a *hole the domain fills*, not a missing Loom primitive. The
Timer package is the counterexample that proves why generic snapshot
continuity is insufficient: it could not tolerate a stale moving snapshot, so
it built an exact final boundary on the substrate (its letter is written
*after* admission freezes the incumbent).

## Minted identity needs a surviving namespace

**Status: GUIDELINE.**

An identity may be minted by one incarnation, but its **namespace must live at
least as long as references to it may live** — in practice, cross a
replacement (carry a high-water mark, or derive names from durable facts). The
paired failure: multiple independent authors of one namespace require explicit
coordination (two authors of a build-attempt number collided in Night Lab).
Three sightings, two of them defects. No Loom identity allocator exists, and
none is planned by this note.

## PreparedReplacement host/coordinator friction

**Status: KNOWN AUTHORING FRICTION — not missing truth.**

The handle is host-owned; `offer_current_answer()` must run inside the
coordinator's current delivery. Applications bridge it with a host-provided
handle reference in the coordinator. Ubiquitous in Night Lab, harmless in
practice, recorded so the pattern is recognized rather than re-derived.

## `reply_to` — low observed use

**Status: CURRENT, evidence-noted.** Ordinary replies exist and work; across
six applications every response wanted an *answer* or a *role send* instead.
See [messaging](messaging.md#answers).

## Deferred capacity is Loom-wide

**Status: CURRENT (by design), commonly mis-assumed.** 64 outstanding
deferrals anywhere exhaust the 65th everywhere
([ANS-02](../laws/answer-authority-laws.md), [bounds](bounds.md)).

## Deferred-with-intent (the standing trigger map)

Certain triggers (hooks left deliberately): the **migration layer** (first
persisted value that must evolve); **weaver identity** (first cross-restart
persistence / author-trust decision); the **role→protocol registry** (the
third broker). Maybe/never: native Windows containment (WSL-hosting
dominates), seccomp (escape-tier threat model), multi-threaded dispatch,
production broker hardening. History holds the reasoning
([history](../history/README.md)).
