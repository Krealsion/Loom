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

## Live authority administration — what GRANT-0 deliberately did not build

**Status: KNOWN SEAM** (three of them, kept apart on purpose). The primitive
itself is current and law-backed
([GATE-05](../laws/admission-laws.md#gate-05--baseline-authority-is-admission-time-delegated-authority-is-live-effective-authority-decides),
[capabilities](capabilities.md#live-delegation-grant-0)); these are the edges
around it, stated so shorthand cannot harden into a guarantee.

```text
CONTAINMENT IS STILL ADMISSION-TIME.  os_cap / FsAccess / ResourceLimits are
    consumed once, at IsolationHost::mount, into a network namespace, a
    pivot_root'ed mount view and a cgroup leaf. Nothing in this process can
    revisit them, in EITHER direction: a live grant would not open a namespace,
    and a live revocation would not claw back an already-open socket, an
    inherited descriptor or a spawned child. `LiveAuthority` therefore has no
    vocabulary for them, and that absence is the guarantee. Narrowing them live
    would need the isolation backend to prove it, and it does not today.

AN ADMINISTRATION ACT IS NOT ON THE TAP.  Delegation queues no message and emits
    no BusEvent, so an operator watching traffic sees the CONSEQUENCES of an
    authority change (a send that now lands, or now refuses `CapabilityDenied`)
    and never the change itself. A `GrantChange` result is returned to the
    caller and nowhere else. A future operator surface that must show "who
    granted what, when" needs its own answer; this is deliberately not an audit
    log, and it meets the same wall the delivery tap does — whole-bus
    observation is host authority, not a grant.

    STILL TRUE AFTER WEAVER-1, and worth stating precisely, because the Weaver
    looks like it closed this and did not. An operator sees the workflow it is
    ITSELF PART OF — its own prompts, its own acks, its own refusals — because
    the Weaver deliberately sends it those messages. It does not see an
    authority change made by any other holder of a capability over the same
    subject, and there is no event it could subscribe to that would show one.
    A Weaver reading `describe_authority` afterwards reports the truth (it keeps
    no picture of its own to be stale), so the gap is NOTIFICATION, not
    correctness.

THE CAPABILITY IS NOT ATTENUABLE BY ITS HOLDER.  A `GrantAuthority` governs one
    subject with one ceiling, and there is no verb by which its holder mints a
    narrower one for somebody else. One administrator per governed subject, the
    host minting each. Multi-administrator delegation is a real future rule; it
    waits for a consumer, exactly as a per-claimant observe rule does.
```

There is also no time-based expiry and no one-shot authority: a delegated rule
is reusable until it is explicitly replaced or the subject dies. "Allow while
this session lives" and "allow until revoked" are both real; **"allow once" is
not claimed**, and a policy that needs it must broker the action rather than
pretend a reusable grant is consumable. [WEAVER-1](weaver.md) says exactly that
to the human, in the prompt's own words, rather than leaving them to assume.

## A policy delegate's death does not revoke what it granted

**Status: KNOWN SEAM** (WEAVER-1), and it follows from the line above rather
than from anything the Weaver does: an installed grant is not a lease. If the
[Weaver](weaver.md) dies after an approval, the governed session **keeps** the
delegated authority, and there is now no message by which anyone can take it
back — the seat that could revoke it is the seat that is gone. Only the host,
holding a capability of its own, can.

That is stated rather than solved. Session-death-on-Weaver-failure, leases,
supervisor restart and a host emergency revoke are all real answers and all
speculative today; adding RAII revocation would silently change GRANT-0's
"a capability is not a lease" into its opposite for one caller.

## The operator seat is a WeaveId, not a person

**Status: KNOWN SEAM** (WEAVER-1). A Weaver treats one exact WeaveId's
decisions as the user's. That check is real and enforced against the bus stamp
— reachability is emphatically not identity — but it authenticates a *weave*,
not a human. The current console is a **bootstrap** operator: it holds
`allow_any()` and host-wired discovery and the tap, which a properly delegated
user terminal would not. Remote/external authentication does not exist at all
(see [bridge](bridge.md#authentication-posture)).

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

**Status: KNOWN SEAM — current and deliberate. For authentication and access
control, reachability is the boundary.**

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
and any forward re-exposes the port). `bridge_listen_unix` sets no explicit
socket-file permissions, so the ambient `umask` decides who can open the node —
which under this seam is an access-control decision rather than a hygiene one,
since whoever reaches it holds operator authority. Nothing here adds permission
handling or authentication; the point is that the umask is currently part of the
boundary. Full model: [bridge](bridge.md).

## A remote console's learned schema mirror is still append-only

**Status: KNOWN SEAM — narrowed by BL-0, deliberately not closed. The
authoritative host registries are fixed; this one client-side mirror is not.**

C-10/F-9 was one shape in four places, and BL-0 closed three of them. What a
host retains is now bounded by live claims
([LIFE-08](../laws/lifecycle-laws.md#life-08--a-schema-is-retained-by-a-live-claim-never-by-having-been-registered)):
the Switchboard's registry (the vocabulary every raw emission is gated against),
the Kernel's manifest dependency registry, and an isolation host's all shrink
again when the weave, artifact or mount that needed a shape goes away.

`RemoteConsole::registry_` does not, and the reason is structural rather than an
oversight: it is a **learned mirror in the operator's process**, filled from
`Schema` frames the host sends in reply to `Describe`. Nothing in that process
has a lifetime that means "this shape is still needed" — the console's other
windows (`kConsoleTapCapacity`, `kConsoleBufferCapacity`, `kMaxPendingDelivered`,
`kMaxAbsentSchemas`) are each bounded on their own terms, and a schema outlives
all of them because its whole point is to be reusable. So a console session
attached to a host with genuinely churning shape diversity still learns one
entry per distinct shape it ever sees, for as long as that session lasts.

```text
FIXED       host: Switchboard / Kernel / IsolationHost registries
            bounded by live claims

STILL GROWS RemoteConsole::registry_ (client process, one console session)
            one entry per distinct shape observed

BOUNDED TODAY BY  the session's own lifetime — a fresh console starts empty —
                  and by how many distinct shapes a host actually publishes
```

Deliberately not solved here: the natural fixes are a bounded memo with
re-`Describe` on a miss (the shape `kMaxAbsentSchemas` already has) or a
snapshot-refresh opcode, and both are console/wire decisions rather than
Registry lifetime ones. **Trigger:** an operator console held open across a host
that turns over schema identities, or any claim that the *client* is bounded.
Until then, do not read LIFE-08 as covering it.

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
