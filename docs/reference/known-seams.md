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

**Status: KNOWN SEAM.** (Narrowed at R2E-0, not closed — see below.)

A weave-originated send may be refused later at delivery; the sending weave
receives no eventual delivery result (native tickets are journal handles the
*host* can read; dynamic sends return no ticket at all). Applications needing
absence detection author it per domain: watchdogs, timeouts, reconciliation,
application-level acknowledgment. Do not invent ticket-outcome semantics in
docs or designs; the refusals are observable on the tap/journal — by an
observer, not the sender.

What R2E-0 changed is only the **observer's** side, and only where a refusal was
observable *nowhere*: see the next entry. Nothing about the sender's view moved.

## The silent dynamic seam

**Status: CLOSED (R2E-0) — current, law-backed
([MSG-08](../laws/messaging-laws.md)).**

Night Lab III (P-011) found the one place a Loom-owned rejection was silence: a
loaded weave emits a shape whose registrar was never loaded, the host seam cannot
resolve it, and the emission disappeared — no recipient, no `BusEvent`, no
journal entry, and the shim is fire-and-forget by design. The identical intent
from a native weave refused loudly (`NoSuchTarget`), so the observability floor
differed **by tier**.

Seam rejections now leave a host-side fact at the same altitude a capability
refusal already had: `SeamUnresolved` for an unresolvable claimed shape,
`GateRefused` for bytes that fail the gate, carrying the claimed (name, version),
the sending artifact, and a target only where one was actually named. Reproducer:
`playground/night-lab/workshop-marathon/repros/core/silent-seam-emission/`, and a
real-artifact regression fixture in suite `kernel`.

Deliberately **not** closed by this: send fate (above). No ticket crosses the
seam, no future exists, and nothing is returned to a sender that was not returned
before.

## Event-loop composition

**Status: CLOSED (R2E-0, corrected R2E-0a) — current, law-backed
([MSG-09](../laws/messaging-laws.md)).**

`pump()`'s drain-to-empty contract does not compose with a perpetual in-process
service: a repeating Timer re-arms itself inside its own handler, so the queue
never empties and a single-threaded host never returns to poll its sockets. Found
by the Codex Rule Garden, whose workaround was a fake application message whose
handler called `Switchboard::stop()`.

`pump_pending()` is the bounded turn, and the only one: it dispatches the backlog
present at entry and leaves work enqueued during the turn for the next one.
`pump()` is unchanged for every existing caller, and
`BridgeServer::set_bounded_dispatch()` is off by default (drain to empty). The
Rule Garden's fake yield message is deleted and replaced by that surface in suite
`bridge`.

**The first attempt is kept as evidence, not as API.** R2E-0 shipped a *numeric*
bound first — `pump_bounded(n)` and `BridgeServer::set_dispatch_budget(n)` — and
the same consumer that found the seam disproved it: `pump_bounded(64)` made the
Rule Garden's live round-trip 17× slower, and a budget large enough not to
throttle was drain-to-empty again. Asking a host to size its turn against a
producer's rate is a number nobody can pick. R2E-0a removed both surfaces; what
survives is the work boundary, not the quota.

## Continuity is authored

**Status: NARROWED (R2E-0) — the pattern is now law-backed
([HANDOFF-01..03](../laws/handoff-laws.md)) and worked end to end in
[reference/handoff](handoff.md); the negative half remains law
([PR-09](../laws/replacement-laws.md)). No Loom API was added.**

R2E-0 built the case this note said the substrate did not have: two real
artifacts with genuinely incompatible state schemas, a temporary migrator, an
exact FIFO boundary, and a full prepared replacement across them. The conclusion
is the one below, sharpened rather than replaced — **what crosses is still a
domain decision**, and the migration that carries it is authored, refusable and
attributable. What is new is that the *shape* of the authoring is now written
down and pinned, and that the migration layer trigger in the standing map below
has fired and been answered without a migration registry.

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

**Status: GUIDELINE — reinforced (R2E-0), still no allocator.**

R2E-0 added a fourth sighting and, for the first time, a **control case**: the
handoff witness carries a high-water mark across an incompatible state schema (47
issued → 48 next), and its twin drops it deliberately and mints a duplicate id.
The difference is the migration's. No Loom identity allocator exists, and none is
planned by this note.

An identity may be minted by one incarnation, but its **namespace must live at
least as long as references to it may live** — in practice, cross a
replacement (carry a high-water mark, or derive names from durable facts). The
paired failure: multiple independent authors of one namespace require explicit
coordination (two authors of a build-attempt number collided in Night Lab).
Three sightings, two of them defects. No Loom identity allocator exists, and
none is planned by this note.

## PreparedReplacement host/coordinator friction

**Status: KNOWN AUTHORING FRICTION — not missing truth. Sighting count rose at
R2E-0; still not extracted.**

The handle is host-owned; `offer_current_answer()` must run inside the
coordinator's current delivery. Applications bridge it with a host-provided
handle reference in the coordinator. Ubiquitous in Night Lab, harmless in
practice, recorded so the pattern is recognized rather than re-derived.

R2E-0's Handoff Garden hit it once more, and the phase deliberately did **not**
fix it: the friction was the same single `PreparedReplacement*` member the
existing evidence describes, and one more instance of a known pattern is not
repeated *independent* friction. What would earn an extraction is a coordinator
needing to pass something the handle cannot reach — and the authored handoff did
not: the migration result travelled as an ordinary domain message, and the
transaction handle carried only what it always carried.

## `reply_to` — low observed use

**Status: CURRENT, evidence-noted.** Ordinary replies exist and work; across
six applications every response wanted an *answer* or a *role send* instead.
See [messaging](messaging.md#answers).

## The bridge does not authenticate

**Status: KNOWN SEAM — current, deliberate, and the only safety property this
component has is deployment.**

`authorize_connection()` is a real single chokepoint, consulted before a proxy
is registered or a frame is read — and today it returns a full operator grant
for **every** connection (model A: *reachability is authority*). There is no
token, no key, no challenge, no peer-credential check and no transport
security anywhere in the bridge. A party that can reach the socket can
enumerate the bus, read every event through the tap, and send any admissible
message to any target. **Do not bind a bridge listener where an untrusted
party can reach it.**

What that buys is that the *whole* future lives in two lines — the function
and the one `register_weave` call consuming its result. Model B (a bearer
token) and model C (per-connection graduated grants, where the *weaver*
concept is born because differential authority is the first place
authorization needs authentication) change those and nothing downstream. The
chokepoint is built; the token is not, and no trigger has fired for it.

Two smaller current facts at the same boundary. `bridge_listen_tcp` binds
`INADDR_LOOPBACK` unconditionally, so the shipped helper cannot be aimed
off-host — a real mitigation, and still a *reachability* property rather than an
authentication one (`BridgeServer` accepts any socket an embedder hands it,
and any forward re-exposes the port). `bridge_listen_unix` sets no socket-file
permissions; the ambient umask decides. Full model: [bridge](bridge.md).

## Deferred capacity is Loom-wide

**Status: CURRENT (by design), commonly mis-assumed.** 64 outstanding
deferrals anywhere exhaust the 65th everywhere
([ANS-02](../laws/answer-authority-laws.md), [bounds](bounds.md)).

## Activation-sequence ownership

**Status: WATCHING — two sightings, deliberately unsolved.**

`commit(sequence)` takes a number the operator supplies, and no host sequence
owner exists to consume. The Codex Rule Garden invented `++activation_sequence`
solely to satisfy the call; R2E-0's Handoff Garden passed a literal `1` for the
same reason. Two sightings whose only meaning is *"the API needs a number"*.

Not solved here, on purpose: the number is real authority (it is what Loom
attests, and what consumers order their lineage by), so an allocator would have
to decide *whose* lineage it belongs to — and neither sighting has an opinion
about that. What would earn a fix is a consumer for which the sequence carries
domain meaning, rather than one that needs any monotonic integer.

Notably, Senses did **not** add a third: a claim's `revision` is minted by Loom
per key and never passed in by a caller, so it created no synthetic counter.

## Deferred-with-intent (the standing trigger map)

Certain triggers (hooks left deliberately): **weaver identity** (first
cross-restart persistence / author-trust decision); the **role→protocol
registry** (the third broker). Maybe/never: native Windows containment
(WSL-hosting dominates), seccomp (escape-tier threat model), multi-threaded
dispatch, production broker hardening. History holds the reasoning
([history](../history/README.md)).

**The migration layer trigger has fired and been answered (R2E-0)** — by an
authored pattern and two laws, not by a migration registry. See
[handoff](handoff.md) and the
[ADR](../decisions/migration-is-authored-not-inferred.md).
