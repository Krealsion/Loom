# Known seams — reference

Current limitations and open design candidates, stated precisely so shorthand
cannot harden into false guarantees. Statuses used here: **KNOWN SEAM**
(a real, current limitation), **EVIDENCE-BACKED CANDIDATE** (core work an
application portfolio motivates), **GUIDELINE** (a design rule for
applications), **KNOWN AUTHORING FRICTION** (ergonomics, not missing truth),
**SPECULATIVE** (a sketch, not an API).

## Role-authored provenance

**Status: OPEN / EVIDENCE-BACKED CORE CANDIDATE.**

Current Loom can prove: *which weave* sent a delivery; *whether it answers my
ask*; *whether it is lifecycle-attested*. It cannot prove: *the sender
authored this message in the capacity of role R*. And `send_to_role(R, m)`
means "deliver to whoever holds R" — never "I speak as R"
([MSG-04](../laws/messaging-laws.md)).

Consequence: an *office's* announcements are unauthenticatable. Identity works
when you talk to somebody; it fails when you talk to whoever holds an office.
Five independent Night Lab sightings, escalating to a forged announcement
destroying healthy work and a player joining an attacker's server — and both
application-level workarounds are built and **priced**: a registry's belief
rests on an unauthenticated announcement; inverting push to pull works but
spends the Loom-wide deferral bound, strands strict receivers across honest
replacement, and cannot cover observers of publications
([evidence](../evidence/night-lab.md#role-authored-provenance--five-sightings-workarounds-priced)).

The narrowest shape, if built, is a delivery fact of the same family as
`answers_ask()` — *"the sender held role R when this was sent."* Any syntax
(e.g. `send_from_role`) is **SPECULATIVE**; do not document it as current.

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

Prepared replacement verifies the successor and preserves nothing; graceful
swap preserves authored work and verifies nothing. The two ceremonies are
disjoint, and the preparation window — incumbent alive, successor reachable —
is where applications bridge them: ask the incumbent to *describe* itself (an
ordinary question that changes nothing) and hand that description to the
candidate in the preparation ask. **What crosses is a domain decision** — six
Night Lab applications carried six different things (work / obligation /
intent / a reopened question / a waiting-fact / a fleet tally) — so the
repeated thing is a *hole the domain fills*, not a substrate helper. A
description taken while the incumbent remains live is a **snapshot**, not an
atomic final handoff; the Timer package is the one domain that required an
exact boundary and built it on the substrate (the letter written *after*
admission freezes the incumbent).

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
