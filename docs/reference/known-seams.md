# Known seams — reference

Current limitations and open design candidates, stated precisely so shorthand
cannot harden into false guarantees. Statuses used here: **KNOWN SEAM**
(a real, current limitation), **EVIDENCE-BACKED CANDIDATE** (core work an
application portfolio motivates), **GUIDELINE** (a design rule for
applications), **KNOWN AUTHORING FRICTION** (ergonomics, not missing truth),
**SPECULATIVE** (a sketch, not an API).

## Role-authored provenance

**Status: CLOSED (R2D-0) — current, law-backed.**

The fourth fact exists. All four now:

```text
sender identity          who the exact weave was            EXISTS (the bus stamp)
role-addressed delivery  where the message was routed       EXISTS (send_to_role)
role membership          which office the weave holds now   EXISTS (role_holder lookup)
role-authored provenance which office the weave
                         DELIBERATELY SPOKE AS              EXISTS (mail.as_role /
                                                            authored_from_role, MSG-07)
```

Both load-bearing halves are carried: *authorization* (Loom verified the
sender held R at the authorship moment) and *intent* (this statement was
deliberately spoken as R). Holding remains insufficient by law — the same
holder's personal speech arrives unauthored — and **publications are
first-class** (the forged-`WorkerOpen` case is exactly what
`as_role(R).publish` closes). Current semantics:
[messaging](messaging.md#office-authorship-role-authored-provenance); law:
[MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit);
why explicit-not-inferred won:
[decision](../decisions/office-authorship-is-deliberate.md); the discovery and
pricing of the seam:
[evidence](../evidence/night-lab.md#role-authored-provenance--five-sightings-workarounds-priced)
and the focused replay `playground/night-lab/followups/role-authorship/`.

What remains deliberately out of scope (seams, not gaps): office authorship
across the **out-of-process pipe** fails closed in both directions (the pipe
carries no attestation in V1; the first out-of-process office pulls the
verified control-protocol frame), and no public door yet produces a combined
answer+office fact (the representation admits it; `answer_as_role` waits for a
consumer).

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
