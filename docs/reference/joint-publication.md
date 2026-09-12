# Joint publication of latest claims — reference

Several participants change their latest claims **together**, at one observable
boundary, and each is then shown what was published under its own key. Laws:
[SENSE-06, SENSE-07](../laws/sense-laws.md); the Sense it builds on is
[senses](senses.md) ([SENSE-01..05](../laws/sense-laws.md)). Across the ABI:
[dynamic ABI](dynamic-abi.md) (v8). Bounds: [bounds](bounds.md#switchboard).

```text
begin     an OPERATOR binds exact claimants + current revisions of several keys
offer     each CLAIMANT, from inside its own delivery, offers its key's next value
commit    the bus revalidates, then exchanges every offer into its record in ONE step
showing   each claimant is shown its published value before it runs again
release   the operator retires the record it has consumed
```

**Publication and application are separate facts.** A commit *publishes*: from
that instant every reader sees the new values. Whether each claimant then
*applied* what was published under its key is a second fact the bus records,
attributes and never assumes. **The record outlives the publication**: an
operation's record is kept until its operator releases it, so the notice that
wakes the operator always finds the record it names.

## What it is for, and what is not here

One consumer shaped it: an application whose document owner and presentation
owner must change their facts together when a source is opened — never a
frame in which one says the new document and the other the old layout. Loom
supplies the mechanism and no vocabulary of either.

### What is not here

- **No document, layout or application vocabulary.** The keys are latest-claim
  keys; what a claim means is the claimant's.
- **No queue, answer, retry, timeout, rollback or forced completion.** An
  operation is aborted or it commits; a claimant that could not apply is *held*
  until it is repaired by its owner (below), never retried by the bus.
- **No universal transaction.** Ordinary claims and weaves that never offer keep
  their contract and pay nothing: a weave that declares no `on_claim_published`
  hears nothing and is applied trivially.
- **No office keys.** An operation binds personal keys; a claimant may be *named*
  by the office it holds at `begin`, which resolves to the exact holder then.
- **No loaded coordination.** A loaded weave can offer; the operator's verbs are
  native ([what crosses the ABI](#what-crosses-the-abi)).
- **No durability across a process**, and single-threaded, FIFO, non-reentrant
  dispatch is assumed throughout ([MSG-01](../laws/messaging-laws.md)).

## Authority

The right to coordinate is a host-minted, operator-bound capability:

```cpp
loom::JointAuthority authority =
    bus.mint_joint_authority(operator_id, {"editor.office", "desk.office"});
```

Every operator verb checks that the presenter is the *exact* operator (weave,
life and incarnation) the authority names, that this Loom issued it, and that
it is spoken from inside the operator's own live delivery. Only claimants
holding one of the ceiling's roles at `begin` may be bound. **An operation id,
a role name, a payload field or a correlation is never authority** — a forged
`op` meets `NoSuchOperation` or `NotOperator`, and a copied authority in another
weave's hands meets `NotOperator`. A successor at the same address inherits
nothing.

## The operation

From `loom::Mail` (`zen/weave/weave.hpp`); every verb is authenticated against
the live delivery the Mail is.

| Verb | Who | Does | Refuses |
|---|---|---|---|
| `begin_joint(authority, keys)` | operator | binds each key's exact claimant (life + incarnation) and current revision; one live operation per key | `ForeignAuthority`, `NotOperator`, `OutsideCeiling`, `NoClaim` (no current claim, or a key naming both weave and role), `KeyBusy`, `Exhausted` (no free slot, or more than `kMaxJointKeys`) |
| `offer(op, next)` | claimant | admits `next` through the one gate against the claimant's declared claim-set, checks the bound revision, stores it until commit or abort. **Nothing is published here** | `NoSuchOperation`, `WrongState`, `NotBound`, `NotClaimant`, `StaleRevision` (aborts), `Undeclared`, `GateRefused`, `TooLarge` |
| `commit_joint(authority, op)` | operator | revalidates every participant, revision and offer, **then exchanges every offered value into its claim record in one protected step**: no participant code, gate, allocation, observer, I/O or lifecycle path runs between two exchanges. `ok` *is* the commitment; `false` means nothing changed and the operation is terminal with `why` | `WrongState`, `ParticipantChanged`, `NoClaim`, `StaleRevision`, `OfferMissing` |
| `cancel_joint(authority, op)` | operator | ends a Preparing operation; offers released; the record reads Aborted/`Cancelled` | `WrongState`, `NotOperator` |
| `joint_status(authority, op)` | operator | the record: state, reason, and after a commit the application (below) | `Missing`/`NoSuchOperation` after a release |
| `release_joint(authority, op)` | operator | retires a terminal record it has consumed; the slot is free, the id names nothing again | `WrongState` while Preparing (cancel it), `NoSuchOperation` once released, `NotOperator` |

A key is named with `loom::claim_key<Shape>(weave_id)` or
`loom::claim_key<Shape>("office")`. The host's ungated views are
`Switchboard::joint_status(op)`, `joint_pending()`, `joint_records()` (live or
unreleased — what counts against the bound), `joint_retained_bytes()`,
`has_unobserved_publication(id)`, `has_failed_application(id)` and
`application_of(id)`.

**Abort is automatic and exact.** A Preparing operation is aborted, its offers
released, the moment a bound participant's life or incarnation changes
(`ParticipantChanged`), or a bound claimant claims ordinarily over a bound key
(`StaleRevision` — the owner spoke, so the bound revision is stale). The
authorship of a published claim is the claimant's own, at its exact
incarnation, never the operator's.

## The showing and its three answers

After a commit each claimant is **shown** its published value once —
`Weave::claim_published`, routed by `WeaveBase` to the maker's
`on_claim_published(const T&)` — **before its next delivery and before its next
snapshot**, outside any dispatch. No reader can observe a weave standing behind
its own published claim. What the showing came to is recorded on the claim
record and on the operation, attributed to exactly this participant, this
publication and this revision:

| `JointApplication` | Meaning | What follows |
|---|---|---|
| `Pending` | published, not yet shown | shown before the claimant's next delivery or snapshot |
| `Applied` | shown; the hook completed and answered Applied | nothing owed |
| `Declined` | shown; the claimant **answered** that it does not apply this value and keeps or reconciles state of its own | functioning, **not held**; deliveries and the ordinary snapshot proceed; the published value stands on the record, attributed Declined, until the claimant's next ordinary claim replaces it (then `None`); the operator is told once, naming the claimant; a reload re-shows nothing |
| `Failed` | shown; the hook did not complete | the claimant is **held** (below); the operator is told once |
| `Lost` | the claimant was removed before it was shown | its record is gone with it; the operation keeps the word |

**The answer is the handler's return type**, and there are exactly three:

| Native handler | Loaded status | Recorded |
|---|---|---|
| returns `void`; returns `true`; returns `PublishedClaim::Applied` | `ZEN_OK` | Applied |
| returns `false`; returns `PublishedClaim::Declined` | `ZEN_CLAIM_DECLINED` | Declined |
| returns `PublishedClaim::Failed`; throws | any negative status (`ZEN_ERR` for an exception or a returned Failed; `ZEN_ERR_UNKNOWN_SCHEMA` / `ZEN_ERR_REFUSED` for bytes the library's gate would not admit) | Failed |

Any other return type is **refused at compile time** (the `hook_return_types`
entry pins it); any other positive loaded status is read as Failed. Only the
defined Applied and Declined answers establish those outcomes — nothing else
is read as success.

**Expected non-application is an answer; an unexpected exception is a
failure.** A functioning owner shown a value it did not prepare, or cannot
stand behind after its own checks, keeps or reconciles its own state and
answers Declined. A successor reloaded over a held predecessor is the typical
case: shown what its predecessor prepared, with nothing of its own to install,
it declines — and *a repaired owner is not proof that its old operation
applied*; what the successor answers is the fact, re-read from the record.

**Authoring the hook.** It runs outside any delivery: no Mail, no send, no
answer right. Apply the value or decline it; begin no work from it. A weave
that mirrors state into its claim updates its "what the bus holds under my
key" bookkeeping *whatever it answers*, so an owner that applied claims nothing
again and one that declined re-claims its own truth at its next delivery. A
warning followed by an ordinary return is an application; a keep-my-own-state
branch answers `Declined`, never `void`. Several pending publications under one
weave are shown in a fixed order (publishing operation, then key); an Applied
or Declined showing goes on to the next key; the first failure stops the
showing and leaves later keys Pending, never attempted.

**The operation's word is the worst part's**: Failed over Lost over Declined
over Pending over Applied, and `JointStatus::failed` / `failed_role` name the
participant a non-application is about, so an operator can tell a requester
*which* owner and not merely that something did.

## The hold, and diagnostic access

A claimant whose showing **failed** is held, attributably, at every door that
meets it:

- every delivery to it is refused `RefusalReason::ApplicationFailed` — step 9
  of the [delivery order](messaging.md#the-envelope-and-the-delivery-order) —
  in the journal, on the tap, and to a sender that accepts `zen.DispatchRefused`
  by exact attempt; **a Poke is a delivery** and is refused the same way;
- its ordinary snapshot (`snapshot_bytes(id)`, `SnapshotAccess::Ordinary`)
  throws — the weave's own exception at the showing that failed,
  `ApplicationFailedError` afterwards — rather than serving, as a normal
  snapshot, a state the weave no longer stands behind;
- **the hook is never re-run for that incarnation.** Observation must not
  retry whatever it half-did.

`SnapshotAccess::Diagnostic` reads the state **as it is** — nothing shown,
nothing recorded, nothing refused — for a diagnostic or a repair tool that
explicitly wants what a held weave holds. It is a host door, not a weave's.

## Repair

A reload is the repair of a held weave, and removal is the other.

- `Kernel::reload_from` reads the live weave with `SnapshotAccess::Reload`
  (pending publications are shown first as always; a failed one is not
  retried and does not refuse the read), and `Switchboard::swap_state` returns
  every Failed key to Pending for the successor, which is shown it at its first
  delivery or snapshot — new code's own attempt. That successor may apply,
  decline or fail; a re-settlement is told to the operator again, with the
  successor's own answer. A candidate image is judged (ABI version, descriptor,
  manifest) **before** the incumbent's snapshot is taken, so a refused
  replacement runs no code of the incumbent's and leaves it held exactly as it
  was.
- Removal takes the claim record with the weave; a Pending part becomes Lost.
  Removal and unresolved application stay distinct words.
- A Declined showing is an answer, not a failure: a reload re-shows nothing.

## The record's lifetime and release

A record — Committed with its application, settled or still owed, or Aborted
with its reason — **is kept until its operator releases it**
([SENSE-07](../laws/sense-laws.md)), or until the operator's own life or
incarnation changes. Publication is not the end of an outcome's lifetime, and
**a queued notification is not consumption**: the notice in the queue is a
promise the record will still be there.

- Preparing cannot be released; cancel it first. Committed/Pending, settled
  with a queued notice, and Aborted records all occupy a slot until released.
- **Only a released slot is reused.** `kMaxJointOperations` bounds the records
  live *or unreleased* on one bus; the next `begin` past it is refused
  `Exhausted`, in words, and takes nothing from any record. An operator that
  never releases meets that bound; it never causes another operator's
  outstanding outcome to be reused under it. No fairness or partition between
  operators is promised: what one keeps, every operator on that bus pays for,
  as a documented finite bound.
- **An authorized early release of a committed record deliberately relinquishes
  its later notices.** After a release, no re-settlement of that operation — a
  held owner's repair, for one — is told to anybody. It does not erase the
  claimants' application facts and does not lift a failure hold: those live on
  the claim records, so a held claimant is still held, still refused, still
  repaired by its own reload, and its next ordinary claim still replaces the
  value.
- **The operator's replacement, removal, death or revival retires every record
  it began**, whatever its state, at that transition: nobody is left to consume
  it, and a successor at the same address inherits no record, as it inherits no
  authority. The claimants lose nothing by it.
- A duplicate release is `NoSuchOperation`; a stale notice makes the operator
  consult a record that says Missing and decide nothing. **Missing is
  legitimate exactly after a release or after the operator's own lifecycle
  change, and never otherwise.**

An operator that wants a late word about a repair keeps the record until it
has it, and states the bound it keeps it under. The consumer is the owner of
that policy; Loom keeps what it is asked to keep, up to the bound.

## The operator's notices

Two shapes, in `zen/weave/dispatch_refusal.hpp`, delivered to the exact
operator that began the operation and only if it explicitly accepts them:

- **`zen.JointEnded {op, reason}`** — a bound participant of the operation was
  replaced or removed, ending it (`ParticipantChanged`) or ending the
  possibility of an answer to one the bus had already aborted. Once per
  operation. Deliberately *not* sent when a bound claim merely moves: that owner
  is alive and answers the operator in its own words.
- **`zen.JointApplied {op, applied, claimant, role, reason}`** — the application
  settled: every claimant applied, or one Declined, Failed or was Lost, named
  with the office it was bound through. Once per settlement; a repair that
  re-settles is said again.

A notice **wakes an operator to read authoritative state**; its ordinary
payload is not an authenticated outcome. The operator re-reads `joint_status`
for the bus's own record, which is the fact — and that record is kept until the
operator releases it, so the re-read always finds it. A forged notice makes the
operator consult a record that says otherwise; a stale one, a record that says
Missing; and it does nothing. Absence proves nothing.

## Mutation doors and the mirror's bookkeeping

A participant that derives its claim from its state keeps **one truthful owner
view** through every door that changes the state:

- `WeaveBase::handle` runs the maker's `after_delivery(Mail&)` after the
  handler, and also after a substrate door that **mutated** the state — a
  performed `zen.PokeWrite` or `zen.PokeResetState` — and after nothing else. A
  describe, a read, a refused write, a refused reset and a bad literal change
  nothing and invalidate nothing. So a written state re-claims at the end of
  that delivery, and a preparation bound to the old revision is aborted rather
  than committing over what a poker wrote.
- The showing hook updates the mirror's "what the bus holds under my key"
  whatever it answers (above), so publication hooks and owner mirrors never
  silently disagree about what applied.
- A participant that mirrors carries that bookkeeping **in its state**, or its
  reloaded successor re-claims at its first delivery and aborts an operation
  that had just bound it.

What a document and a presentation do to reconcile after a Declined or a Failed
showing is the consumer's; Loom records the answer and holds or releases the
weave.

## What crosses the ABI

ABI v8 carries **the claimant's surface only**: `ZenHostApi::sense_offer`
(offer) outbound and `ZenWeaveAbi::claim_published` (the showing) inbound,
with the two statuses `ZEN_ERR_JOINT_BASE` (a refused offer, by enumerator) and
`ZEN_CLAIM_DECLINED` (the third answer; the one positive status). The host
maps every status of the showing slot exactly as the [three-answer
table](#the-showing-and-its-three-answers) says; the raw mapping, including
undefined values, is on the slot in `zen/kernel/abi.h`.

The operator's verbs do not cross. A loaded weave's `begin_joint`,
`commit_joint`, `cancel_joint`, `joint_status` and `release_joint` inherit the
refusing defaults of `loom::Bus` and answer `NoLiveDelivery`: **loaded
coordination is unsupported by decision.** The authority is a native capability
the host mints; carrying it across the seam means the host keeping the minted
authority in a table keyed by the adapter and checking the operator's exact
incarnation at every verb, which is a design of its own with its own witnesses.
The isolated pipe supplies no offer door either: a child cannot present the
exact identity an operation binds.

Old v7 images are refused at load and at reload, naming both versions, and are
not adapted; rebuild hosts, libraries and images together
([dynamic ABI](dynamic-abi.md)).

## Host exception responsibility at the pump seam

A **native** hook that throws is recorded as Failed first and re-raised to the
host afterwards, exactly as a handler's exception is
([MSG-10](../laws/messaging-laws.md#msg-10--a-callback-that-throws-costs-the-delivery-not-the-bus)):
recorded, then never swallowed. The exception leaves `drain_until_idle()` /
`pump_pending()` at the delivery, or `snapshot_bytes()` at the snapshot, that
performed the showing; the record already says Failed, the hold already
protects the weave, the bus is not poisoned, and the next turn delivers. **The
host loop owns what happens next**: catch it at the pump, attribute it to the
held owner the record names, and continue — or terminate, if that is the
host's policy. Loom does not catch arbitrary exceptions on the host's behalf.

A native owner that expects it may fail returns `PublishedClaim::Failed`
instead of throwing: the same record, the same hold, and no exception to reach
the loop. That is a *choice about expected failure*, not a guarantee: an
unexpected throw is still a throw, and the host loop still owns it. A loaded
hook's exception is contained at the seam as `ZEN_ERR` and never reaches the
host as an exception; the record is the same.

## Bounds

| Bound | Value | Overflow behavior |
|---|---|---|
| `kMaxJointOperations` | 8 | records live *or unreleased* per bus; `Exhausted` at `begin`, nothing reused |
| `kMaxJointKeys` | 4 | keys per operation; `Exhausted` at `begin` |
| `kMaxJointOfferBytes` | 64 KiB | one offered value's serialized size; `TooLarge` at `offer` — a joint publication carries facts, not documents |
| one live operation per key | structural | `KeyBusy` at `begin` |

Offered bytes are retained only between offer and commit or abort
(`joint_retained_bytes()`); a retained terminal record holds parts and no
offers.

## Tests

Suite `joint` (`tests/test_joint.cpp`; kernel-gated, since it loads a real
claimant, `tests/weavelib/joint_probe.cpp`): J1 one boundary, J2 the showing
before the next handler and snapshot, J3–J5 abort on claim, reload and removal,
J6 every refusal by name, J7 the loaded claimant (Applied), J8 bounds,
retention until release and exhaustion in words, J9 nothing runs inside the
exchange, J10 `zen.JointEnded`, J11 the loaded failure held, J12 the native
throw recorded then re-raised and repaired by a swap, J13 several publications
under one weave, J14 Lost, J15/J16 the mutation doors native and loaded, J17
the committed record outlives an unrelated begin (both schedules), J18 the
aborted record outlives one, J19 the operator's lifetime, J20 Declined native
(and Failed by answer), J21 Declined loaded. The `hook_return_types` entry pins
the compile-time refusal of an unsupported hook return type; suite `kernel`
pins the v8 gate at load and at reload.
