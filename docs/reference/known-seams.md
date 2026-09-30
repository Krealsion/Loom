# Known seams — reference

Current limitations and open design candidates, stated precisely so shorthand
cannot harden into false guarantees. Statuses used here include **KNOWN SEAM**
(a real, current limitation), **CLOSED** (a former seam, kept here with what
still lies outside it), **GUIDELINE** (a design rule for applications),
**KNOWN AUTHORING FRICTION** (ergonomics, not missing truth), **WATCHING** (a
pattern seen, deliberately not yet answered).

## Role-authored provenance

**Status: CLOSED — current, law-backed.**

Four facts about a message, and each exists:

```text
sender identity          who the exact weave was            the bus stamp
role-addressed delivery  where the message was routed       send_to_role
role membership          which office the weave holds now   role_holder lookup
role-authored provenance which office the weave
                         DELIBERATELY SPOKE AS              mail.as_role /
                                                            authored_from_role (MSG-07)
```

Both load-bearing halves are carried: *authorization* (Loom verified the
sender held R at the authorship moment) and *intent* (this statement was
deliberately spoken as R). Holding is insufficient by law — the same
holder's personal speech arrives unauthored — and **publications are
first-class** (a forged `WorkerOpen` announcement is exactly what
`as_role(R).publish` answers). Current semantics:
[messaging](messaging.md#office-authorship-role-authored-provenance); law:
[MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit);
why explicit-not-inferred:
[decision](../decisions/office-authorship-is-deliberate.md); the applications
that priced the seam:
[evidence](../evidence/night-lab.md#role-authored-provenance--five-sightings-workarounds-priced).

Deliberately outside it: office authorship across the **out-of-process pipe**
fails closed in both directions (the pipe carries no attestation; the first
out-of-process office needs a verified control-protocol frame), and no public
door produces a combined answer+office fact (the representation admits it; an
`answer_as_role` waits for a consumer).

## Live authority administration — what delegation deliberately does not do

**Status: KNOWN SEAM** (three of them, kept apart on purpose). The primitive
itself is current and law-backed
([GATE-05](../laws/admission-laws.md#gate-05--baseline-authority-is-admission-time-delegated-authority-is-live-effective-authority-decides),
[capabilities](capabilities.md#live-delegation)); these are the edges
around it.

```text
CONTAINMENT IS ADMISSION-TIME.  os_cap / FsAccess / ResourceLimits are
    consumed once, at IsolationHost::mount, into a network namespace, a
    pivot_root'ed mount view and a cgroup leaf. Nothing in this process can
    revisit them, in EITHER direction: a live grant would not open a namespace,
    and a live revocation would not claw back an already-open socket, an
    inherited descriptor or a spawned child. `LiveAuthority` therefore has no
    vocabulary for them, and that absence is the guarantee. Narrowing them live
    would need the isolation backend to prove it, and it does not.

AN ADMINISTRATION ACT IS NOT ON THE TAP.  Delegation queues no message and emits
    no BusEvent, so an operator watching traffic sees the CONSEQUENCES of an
    authority change (a send that now lands, or now refuses `CapabilityDenied`)
    and never the change itself. A `GrantChange` result is returned to the
    caller and nowhere else. An operator surface that must show "who granted
    what, when" needs its own answer; this is deliberately not an audit log,
    and it meets the same wall the delivery tap does — whole-bus observation is
    host authority, not a grant.

    The Weaver does not close this, though it can look as if it does. An
    operator sees the workflow it is ITSELF PART OF — its own prompts, its own
    acks, its own refusals — because the Weaver deliberately sends it those
    messages. It does not see an authority change made by any other holder of a
    capability over the same subject, and there is no event it could subscribe
    to that would show one. A Weaver reading `describe_authority` afterwards
    reports the truth (it keeps no picture of its own to be stale), so the gap
    is NOTIFICATION, not correctness.

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
pretend a reusable grant is consumable. The [Weaver](weaver.md) says exactly
that to the human, in the prompt's own words, rather than leaving them to assume.

## A policy delegate's death does not revoke what it granted

**Status: KNOWN SEAM**, and it follows from the line above rather than from
anything the Weaver does: an installed grant is not a lease. If the
[Weaver](weaver.md) dies after an approval, the governed session **keeps** the
delegated authority, and there is then no message by which anyone can take it
back — the seat that could revoke it is the seat that is gone. Only the host,
holding a capability of its own, can.

That is stated rather than solved. Session-death-on-Weaver-failure, leases,
supervisor restart and a host emergency revoke are all real answers and all
speculative; adding RAII revocation would silently turn "a capability is not a
lease" into its opposite for one caller.

## What an ordinary participant may observe

**Status: KNOWN SEAM — narrowed.** Whole-bus observation remains host
authority, not a grant.

What does an ordinary terminal actually need to observe? **Less than a
whole-bus console has, and nothing that needs a new primitive.** Nine
categories, measured against the source:

```text
A own inbound messages              ORDINARY   the weave's own doors
B own authored/submitted messages   ORDINARY   it authored them
C authenticated answers             ORDINARY   provenance + Loom's correlation
D policy notifications to a seat    ORDINARY   an ordinary message to that weave
E authority descriptions asked for  ORDINARY   zen.DescribeAuthority, capability-scoped
F dispatch refusal of its own sends AVAILABLE      explicit, authenticated, best effort
G traffic involving a named role    NOT AVAILABLE  needs a scoped observation law
H whole-bus traffic                 HOST ONLY  Switchboard::add_observer
I authority changes by another
  administrator                     NOT AVAILABLE  delegation queues no message and
                                                emits no BusEvent (see above)
```

So a terminal needs **no global tap** for send, receive, ask, await, the whole
Weaver approval workflow, or authority self-inspection: all six run on A–E, and
`tests/test_terminal.cpp` pins that a participant's transcript contains none of
two other weaves' traffic while the host's tap sees all of it.

What is **not** closed, and is the seam: `watch` — following traffic that
involves some named role or subject — is a separate feature with no ordinary
answer. It is not implemented, and deliberately not faked by polling a registry
or a tap through hidden privileges: an honest terminal says *watching traffic is
not available to an ordinary participant*, because power includes saying no
truthfully. Closing this needs a real scoped observation law (what may be
observed, by whom, authorized how) — the same shape `ObserveRule` gave Senses,
and it has no consumer yet. The relay in [observation](observation.md) is the
narrower thing that does exist: an office's own publications, under a policy.
F and I are separate entries above and below.

## Service discovery is not a participant's power

**Status: KNOWN SEAM in one half; the other half is answered.**

Two different questions:

```text
WHO IS RUNNING?              HOST ONLY   Switchboard::list_weaves
WHAT DOES THAT ONE ACCEPT?   answered    ask the target: zen.DescribeAccepted
```

**Answered.** A participant holding a role name and an ordinary grant can ask one
target what it accepts and get back enough schema truth to reconstruct those
shapes and inspect their fields — see
[self-description](messaging.md#self-description--what-may-be-said-to-this-weave).
It is an ordinary ask under an ordinary grant, answered by an ordinary
participant. It needs no directory: the target is the natural owner of "what I
accept", so no third party has to hold the fact and no new authority type
exists for it.

**Open, deliberately.** `ConsoleEngine::weaves()` enumerates the bus's registry
and needs a `Switchboard&`; a [terminal session](terminal.md) does not have it,
and neither does anything else. Enumerating live weaves is a fact about the
running world rather than about the participant, and handing it to every
terminal would give an ordinary weave a power nothing granted it. `send_to_role`
addresses an office without resolving it, which is why no consumer has needed
the list.

**Absent, and not wanted:** global schema enumeration. Nothing in Loom can
enumerate schemas — `Registry` offers lookup/contains/size and the `Switchboard`
publishes no `registry()` accessor — so even the console's "catalog" is derived
from accept-sets. A `ListSchemas` would have no owner, no natural authority
scope, and no consumer: the product question is *what can I say to THIS thing*,
which is answered directly.

## The operator seat is a WeaveId, not a person

**Status: KNOWN SEAM**, in its human half. A Weaver treats one exact WeaveId's
decisions as the user's. That check is real and enforced against the bus stamp
— reachability is emphatically not identity — but it authenticates a *weave*,
not a human.

The seat needs no special powers: the [terminal](terminal.md)'s operator seat is
an ordinary participant holding four rules — approve / refuse / revoke /
describe, each to one office — with no wildcard, no registry read and no tap,
and it drives the whole Weaver workflow.

The **human** half is the seam. A host designates a WeaveId; there is no account,
no login, no credential and no authenticated person anywhere in this, and
remote/external authentication does not exist at all (see
[bridge](bridge.md#authentication-posture)). A terminal presentation must
therefore label that identity as a seat and never as a person.

## Sender cannot observe send fate

**Status: PARTLY SUPPLIED — dispatch refusal only.**

An explicit notice acceptor can receive an authenticated pre-handler refusal
of an ordinary addressed send, native or in-process loaded; `TerminalSession`
is one such consumer. Contract:
[sender-visible dispatch refusal](messaging.md#sender-visible-dispatch-refusal).

Absence of a notice proves nothing. Delivered-but-unanswered work, later released
or invalidated answer rights, timeout, cancellation, progress/closing answers,
retry and publication aggregation remain separate questions. AskBook stays local
bookkeeping and creates no global obligation. The isolated pipe does not carry
this attestation and refuses a manifest requesting its notice door.

## Loaded coordination of a joint publication

**Status: UNSUPPORTED BY DECISION, refused by name.**

ABI v8 carries a joint publication's *claimant* surface — offer outbound, the
showing inbound — and nothing of the *operator's*. A loaded weave's
`begin_joint`, `commit_joint`, `cancel_joint`, `joint_status` and
`release_joint` inherit `loom::Bus`'s refusing defaults and answer
`NoLiveDelivery`. The authority is a native capability the host mints; carrying
it across the seam means the host keeping the minted authority in a table keyed
by the adapter and checking the operator's exact incarnation at every verb,
with its own witnesses. That is a design of its own, wanted by no consumer yet
([joint publication](joint-publication.md#what-crosses-the-abi)).

## Rejections at the dynamic seam

**Status: CLOSED — current, law-backed ([MSG-08](../laws/messaging-laws.md)).**

A loaded weave's emission is admitted host-side before routing, so a rejection
there happens before any delivery-time refusal could report it. Each such
rejection leaves a host-side fact at the same altitude a capability refusal
has: `SeamUnresolved` for a claimed shape the host cannot resolve (its
registrar was never loaded), `GateRefused` for bytes that fail the gate, each
carrying the claimed (name, version), the sending artifact, a target only where
one was actually named, and the role where the library named one. Without that,
a loaded weave's emission would vanish where the identical native send refuses
loudly.

**A publication is not one of these.** The rule is about an emission that
expected to ARRIVE somewhere. A publication expects nothing of the sort, and an
unresolvable shape has no accepter by construction, so it reached zero recipients
— which native `publish` does in silence. A publication whose shape resolves and
whose bytes fail the gate is still refused. A real-artifact regression fixture is
in suite `kernel`.

This diagnostic is synchronous. The narrower
[dispatch-refusal contract](messaging.md#sender-visible-dispatch-refusal)
returns authenticated refusal to an opted-in native or loaded sender and carries
the real attempt ticket across ABI v7. It does not turn a seam rejection into a
queued attempt or supply general delivery or success knowledge.

## A manifest says what a weave accepts, and since ABI v9 what it declares it says — never what it asks leave to send where

**Status: the descriptive half CLOSED since ABI v9 (
[declared vocabulary is agreed at
admission](../decisions/declared-vocabulary-is-agreed-at-admission.md)); the
authority-request half a KNOWN SEAM.**

`encode_manifest` (`zen/kernel/schema_codec.hpp`) publishes `referenced`,
`accepted`, `state`, `requests`, `claims` and, from v5, `emits`: the shapes the
weave declares it may send, by definition. A host reading a loaded artifact can
discover what it will *receive* and what it *says* it will say — the same two
lists a native weave declares — and `Switchboard::emitted_schemas(id)` answers
the second question for any live participant.

**A shape only its emitter knows has a registrar.** An emitter's declaration
publishes that shape's definition for as long as the emitter lives, so a
directed send of it to an `AcceptMode::AnyRegistered` console is admitted
against the emitter's own definition rather than refused `SeamUnresolved`
(above). Two other ways remain ordinary, and are necessary for a shape the
sender never declared: a standard reply shape (`zen.Result` / `zen.Ack` /
`zen.Refused`), or the receiving side declaring the shape through
`ConsoleEngine`'s `vocabulary` parameter — because `Emit<...>` is not an
exhaustive send list and a runtime-chosen shape still meets the seam.

What stays open, and why the closed half does not close it:

**A schema list is not a request for authority to a destination.** `emits`
says what `Noted v1` means; `requests` is a `zen.CapabilityAsk` — network,
filesystem, roles — with no word for "shape `Noted v1` to role `logbook`". The
admission policy ([admitting a loaded
artifact](capabilities.md#admitting-a-loaded-artifact)) is handed the ask as
advice and can also read the artifact's declared vocabulary from its manifest,
but a person approving speech still has to name the *destinations* themselves.
That is *correct* on the authority question — a declaration never becomes a
grant, and the source of send authority is the person's own policy either way —
and it is worse ergonomics than it needs to be.

**And metadata alone is not an approval interface.** "This weave would like to
say `Noted v1` to role `logbook`, allow it?" needs a destination-bearing ask and
an operator surface that puts the question; the emit-set supplies the shape's
name and definition and nothing more.

Closing the rest is a destination-bearing ask (`zen.CapabilityAsk v2`, or a
sibling section) — another manifest and ABI bump, rebuilding every artifact in
every consumer — plus the surface that asks. **Trigger:** an operator surface
that wants to offer a one-click approval of what an artifact asked for.

## Event-loop composition

**Status: CLOSED — current, law-backed ([MSG-09](../laws/messaging-laws.md)).**

A drain-to-empty turn does not compose with a perpetual in-process service: a
repeating Timer re-arms itself inside its own handler, so the queue never empties
and a single-threaded host never returns to poll its sockets.

`pump_pending()` is the bounded turn, and the only one: it dispatches the backlog
present at entry and leaves work enqueued during the turn for the next one. The
drain is `drain_until_idle()`, named so the call site says which promise it
makes. `BridgeServer::set_bounded_dispatch()` is off by default (drain to idle);
suite `bridge` pins the bounded server loop.

**A numeric bound is not the answer.** A turn budget asks a host to size its turn
against a producer's rate, which is a number nobody can pick: a budget of 64
made a timer-driven application's live round-trip seventeen times slower, and a
budget large enough not to throttle is drain-to-empty again. What Loom offers is
the work boundary, not a quota.

## Continuity is authored

**Status: GUIDELINE — law-backed ([HANDOFF-01..03](../laws/handoff-laws.md)),
worked end to end in [reference/handoff](handoff.md); the negative half is law
([PR-09](../laws/replacement-laws.md)). There is no Loom API for it.**

Prepared replacement verifies the successor and **does not create an atomic
continuity handoff from the incumbent**; graceful swap preserves authored work
and verifies nothing. The two ceremonies are disjoint. What applications do
about it is a **domain pattern, not a substrate ceremony**: ask the incumbent
to *describe* itself with an ordinary, non-mutating question, and supply that
description to the candidate's preparation. The staging varies — some
applications capture the description *before* the successor is even loaded,
others during preparation — and either way the description is a **snapshot
taken while the incumbent remains live**: the incumbent may change after it,
unless the application or package establishes a stronger boundary. The
application decides whether stale, replayed, restarted, degraded, or exactly
transferred state is acceptable. **What crosses is a domain decision** — six
[Night Lab](../evidence/night-lab.md) applications carried six different things
(work / obligation / intent / a reopened question / a waiting-fact / a fleet
tally) — so the repeated thing is a *hole the domain fills*, not a missing Loom
primitive. A timer is the counterexample that shows why generic snapshot
continuity is insufficient: it cannot tolerate a stale moving snapshot, so
Zengine's Timer builds an exact final boundary on the substrate (its letter is
written *after* admission freezes the incumbent).

Where two versions' state schemas genuinely differ, the pattern still holds:
a temporary migrator, an exact FIFO boundary and a full prepared replacement
carry it, and the migration is authored, refusable and attributable
([handoff](handoff.md),
[the decision](../decisions/migration-is-authored-not-inferred.md)).

## Minted identity needs a surviving namespace

**Status: GUIDELINE — no allocator.**

An identity may be minted by one incarnation, but its **namespace must live at
least as long as references to it may live** — in practice, cross a
replacement (carry a high-water mark, or derive names from durable facts). The
paired failure: multiple independent authors of one namespace require explicit
coordination (two authors of a build-attempt number collided in
[Night Lab](../evidence/night-lab.md)). The handoff witness shows both sides: it
carries a high-water mark across an incompatible state schema (47 issued, 48
next), and its twin drops it deliberately and mints a duplicate id — the
difference is the migration's. No Loom identity allocator exists, and none is
planned by this note.

## PreparedReplacement host/coordinator friction

**Status: KNOWN AUTHORING FRICTION — not missing truth, not extracted.**

The handle is host-owned; `offer_current_answer()` must run inside the
coordinator's current delivery. Applications bridge it with a host-provided
handle reference in the coordinator — one `PreparedReplacement*` member. It is
common and harmless in practice, and recorded so the pattern is recognized
rather than re-derived. What would earn an extraction is a coordinator needing
to pass something the handle cannot reach; an authored handoff does not (its
migration result travels as an ordinary domain message, and the transaction
handle carries only what it always carries).

## A construction-layer reply is not an attested answer

**Status: CLOSED for replies to the requester.** Poke and self-description replies use
the answer door when replying to the request's stamped sender. An explicit `reply_to` naming
somebody else keeps ordinary forwarding, without answer provenance. A strict role asker
requires `answers_ask()` before settling its book; correlation alone authenticates
nothing. This is authoring-header behaviour: an artifact built against older headers must be
rebuilt to gain it. See [messaging](messaging.md#substrate-answer-attribution); the `poke`
suite pins direct and redirected replies, including the self-description door.

## `reply_to` — low observed use

**Status: CURRENT, evidence-noted.** Ordinary replies exist and work; across
six applications every response wanted an *answer* or a *role send* instead.
See [messaging](messaging.md#answers).

## The bridge carries no transport security, and a credential is a shared secret

**Status: KNOWN SEAM — current and deliberate. Admission is the host's
policy; the transport is plain.**

A host's `BridgeAdmission` admits a connection under a grant of its choosing,
refuses it in words, or defers it for a later decision, and a connection has no
proxy — acts on nothing — until that answer comes
([bridge](bridge.md#admission-who-decides-and-when)). What a policy judges is
what the peer *presented*: a credential in the Hello is bytes, compared to
something the host holds (a guests file, say). There is no TLS, no challenge and
no peer-credential check in the mechanism, so a credential crosses in the clear,
and `operator_admission()` admits everything that reaches the socket as the
operator. **Do not bind a bridge listener where an untrusted party can reach
it.**

Two smaller facts at the same boundary. `bridge_listen_tcp` binds
`INADDR_LOOPBACK` unconditionally, so the shipped helper cannot be aimed
off-host — a real mitigation, and still a *reachability* property rather than an
authentication one (`BridgeServer` accepts any socket an embedder hands it,
and any forward re-exposes the port). `bridge_listen_unix` sets no explicit
socket-file permissions, so the ambient `umask` decides who can open the node —
which under this seam is an access-control decision rather than a hygiene one,
since whoever reaches it holds operator authority. Nothing here adds permission
handling or authentication; the point is that the umask is part of the
boundary. Full model: [bridge](bridge.md).

## A remote console's learned schema mirror is still append-only

**Status: KNOWN SEAM — deliberately not closed. The authoritative host
registries are bounded; this one client-side mirror is not.**

What a host retains is bounded by live claims
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
attached to a host with genuinely churning shape diversity learns one entry per
distinct shape it ever sees, for as long as that session lasts.

```text
BOUNDED     host: Switchboard / Kernel / IsolationHost registries
            by live claims

GROWS       RemoteConsole::registry_ (client process, one console session)
            one entry per distinct shape observed

BOUNDED BY  the session's own lifetime — a fresh console starts empty —
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
owner exists to consume. Two applications built on Loom have each invented one
solely to satisfy the call — an incrementing counter in one, a literal `1` in the
other: two sightings whose only meaning is *"the API needs a number"*.

Not solved here, on purpose: the number is real authority (it is what Loom
attests, and what consumers order their lineage by), so an allocator would have
to decide *whose* lineage it belongs to — and neither sighting has an opinion
about that. What would earn a fix is a consumer for which the sequence carries
domain meaning, rather than one that needs any monotonic integer.

Senses did **not** add a third: a claim's `revision` is minted by Loom per key
and never passed in by a caller, so it needs no synthetic counter.

## Leak checking stops at the sandbox boundary

**Status: KNOWN SEAM — current, and easy to over-read in the other direction.**

The sanitizer lane (`-DZEN_SANITIZE=ON`) instruments the host process, and its
leak checking covers the host process. It does **not** cover the sandboxed
children: LeakSanitizer cannot introspect a process inside the restricted view,
because the view mounts no `/proc` for it to read. The isolation suite's runs
print `LeakSanitizer has encountered a fatal error` and
`Can't open /proc/<pid>/task for reading` for exactly that reason — LSan failing
to run, not a leak it found; an untouched case prints the same, so it is a
property of the boundary and not of any one change.

The distinction that matters: **child-side leak checking is ABSENT, not clean.**
A run with no leak report says nothing about the children, so "the sanitizer
lane is green" must never be quoted as leak coverage for weave-host code. ASan's
*memory-error* checking is unaffected — it is compiled in and reports from
inside the child — and so is every UBSan check.

Deliberately not solved. Mounting `/proc` into the view to satisfy LSan would
widen the containment the suite exists to prove, which is a worse trade than
the missing coverage ([capabilities](capabilities.md)); the sanitizer lane is
not weakened to hide the messages either. What would earn a fix is a leak
question about child-side code that the host-side lane genuinely cannot reach.

## A cgroup leaf is confirmed by its whole path

**Status: CLOSED — current, pinned by suite `isolation`.**

Before an out-of-process weave is reported resource-contained, the isolation host confirms that
the child sits in its cgroup leaf and that the leaf's limits read back as set (`cgroup_confirm`,
`src/isolation/sandbox.cpp`). The membership half compares the whole path on the cgroup-v2 line
of the child's `/proc/<pid>/cgroup` with the leaf's own path from the v2 root
(`cgroup_v2_path_is`), so a leaf whose name prefixes another's never matches it: a child in
`zen-weave-10` is not confirmed as `zen-weave-1`, and a child in a cgroup beneath a leaf is not
confirmed as that leaf. Suite `isolation` pins the rule twice: as a pure function, without a live
cgroup, and under a delegated scope with a live child in one of two leaves whose names differ only
by a trailing digit, both carrying the same limits so that only membership can tell them apart.

## Deferred-with-intent (the standing trigger map)

Hooks left deliberately, each with its trigger: **weaver identity** (the first
cross-restart persistence or author-trust decision) and the **role→protocol
registry** (a third broker). Maybe or never: native Windows containment (Linux,
including under WSL, is where containment runs), seccomp (the escape-tier threat
model), multi-threaded dispatch, production broker hardening.

Migration between incompatible state schemas is answered by an authored pattern
and two laws, not by a migration registry — see [handoff](handoff.md) and the
[decision](../decisions/migration-is-authored-not-inferred.md).

## A worker does not end with a host that was KILLED — except on Windows, and not because of Loom

**Status: KNOWN SEAM (platform-split, deliberately not closed).**

"Every worker ends with the host" is true of a host that is **asked** to end —
`Shutdown`, `quit`, `loom-session stop` — because the run manager's destructor
ends every execution group it owns, including one whose worker leader has
already exited leaving children behind, and then waits a bounded moment to see
each of them actually end so that the record it leaves reports a code it read
(`kShutdownObserveMs`, [bounds](bounds.md#sessions-and-runs); what it does not
see is recorded `killing`, which promises no code). A host that is **killed
outright** runs no destructor, and what happens then is the operating system's
answer rather than this manager's. The two platforms answer differently, and the
session journey (`tests/session/journey.py`) tests both:

```text
Windows   the execution group is a JOB OBJECT, opened with
          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE. The kernel closes a dying
          process's handles, so the job's last handle closes and the whole
          group goes with the host -- a real guarantee, and the kernel's.
POSIX     there is no equivalent. The workers keep running, reparented, with
          nothing left that owns them. Their records hold the last state their
          manager managed to write, and the `pid` in those records is how a
          person finds them.
```

**What this is not.** It is not containment: a worker is a process of the
session's user and a process that deliberately breaks out of its group is an OS
sandbox's problem, not this one's
([the exec boundary](capabilities.md#the-exec-boundary-three-independent-facts)).
Nor is it a crash-recovery story: nothing resumes a run or a Python stack across
a host restart either way ([sessions](../guides/sessions.md#8-when-something-goes-wrong)).

**What would close it** is a POSIX mechanism that survives the host's death and
owns the group — a subreaper daemon, a cgroup with a release agent, or a
supervisor process that is not the host. Each is a new component with its own
lifetime and its own authority question, which is why none was added for a
limitation that a sentence can state exactly.
