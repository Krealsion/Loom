# Dynamic ABI — reference

The C seam a weave library and the host agree on. Current version: **v<!-- value ZEN_ABI_VERSION -->14<!-- /value -->**
(`ZEN_ABI_VERSION` in `include/zen/kernel/abi.h`), and the oldest a host loads is
`ZEN_ABI_OLDEST`, beside it ([the number and the sizes](#the-number-and-the-sizes)). Laws:
[KERN-01, KERN-04](../laws/kernel-laws.md),
[GATE-04](../laws/admission-laws.md#gate-04--immutable-published-schemas),
[ANS-06](../laws/answer-authority-laws.md),
[MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit),
[SENSE-01..07](../laws/sense-laws.md).

**v14 appends the creation value**, the first append since the seam grew: two descriptor
slots, `describe_creation` and `create_with`, and nothing else moved. A weave may be made with
one value, data and never a door ([the creation value](#the-creation-value)). An image built
against v13's header (`tests/abi_kept/v13/`) loads and works on this host, made with none: its
descriptor's size ends before the two slots, which the host reads as NULL. A v14 image on a v13
host is refused before any callback, as needing abi_version 14.

**v13 makes the seam grow.** Every struct that crosses it and can grow says its size, and a
reader takes what that size reaches. The descriptor's first four fields are the version the
image needs, its own size, the size of the host table it was built against and the version of
the manifest `describe` writes (`ZEN_ABI_REACHES` reads a field only where a size reaches it).
`ZenHostApi` and `ZenSenseBy` start with their size. `handle` takes the delivery's facts as one
`ZenDelivery` that says its size, in place of its six positional ones (sender, reply address,
correlation, provenance, attested sequence, authored role), and the role the delivery was
addressed to is its first new field, read as `Mail::addressed_role()` by a native, a loaded, a
boxed and an isolated weave alike. A manifest's version moves without the tables' number now.
An image from before v13 is refused at load and at reload, both versions named, before any
callback into it: it is the last version refused for being another number.

**v12 names the per-sender waiting bound.** No slot changes. A send that bound refuses
returns `ZEN_ERR_PENDING_PER_SENDER` on `send`, `send_to_role`, `publish`, `role_send`,
`role_send_to_role` and `role_publish`. A publication that does not fit the sender's
remaining room is refused whole, so `publish` and `role_publish` return that status and
queue nothing. It is not `ZEN_ERR_ROLE_AUTHORSHIP_DENIED` and not `ZEN_ERR_REFUSED`. An
answer is not this bound: `answer` and `answer_deferred` still return `ZEN_ERR_REFUSED`
only when there was no answer to spend. A v11 image is refused at load and at reload,
both versions named, before any callback into it.

**v11 names the role doors by role.** The slots a library calls to speak or claim as a role it
holds are `role_send`, `role_send_to_role`, `role_publish`, `sense_role_claim` and
`sense_observe_role`, the error a claim as an unheld role returns is
`ZEN_ERR_SENSE_ROLE_NOT_HELD`, and a reading's flag is `role_holder_is_current`: each in the
place, with the meaning, its earlier name had. A native table's layout is unchanged; a box imports
the doors from `zen_host` by these names, so a box built before them imports names this host does
not define. The version says it once for both: a v10 image is refused at load with both versions
named, before any callback into it.

**The manifest describes unions and open fields, and its version is not the ABI's.** A record is
described by `zen.SchemaDesc` and a union by `zen.UnionDesc`, its cases (`zen.Case`) each carrying
a type or nothing; the manifest's lists hold `zen.ShapeDesc`, the union of the two, so Loom's own
meta-schema is a union. A type token names a `Union` field's shape as it names a `Message`
field's, and an `Any` field by its kind alone
([values](values-and-admission.md#unions-and-the-open-field)). A record's description is the one
it always was, so a shape of anyone's that nests one keeps its identity, and so does every value
stored in it. The descriptor's manifest is the next version of `zen.Manifest`, beside the same ABI
version and the same tables. An image built against the previous manifest is refused by the gate in
`reconstruct`: after its static initializers and `create()` have run, with a sentence about a
meta-schema (`SchemaMismatch`, pinned in suite `schema_codec`). From v13 the descriptor names its
manifest's version, and such an image is refused by it before `create()`.

**v10 carries the words of a making refused.** `ZenWeaveAbi::create` takes a `ZenByteSink`: a
weave that makes no instance returns NULL having written why, and `ZEN_EXPORT_WEAVE` writes there
what its constructor threw (`std::exception::what()`; an exception that is no `std::exception`
writes nothing). The host keeps the first `kMaxMakingWordsBytes` of them
(`include/zen/kernel/making_words.hpp`, [bounds](bounds.md#a-weaves-making)) and says them in the
load's refusal, `library create() refused: <words>`, at load and at a replacement over a live
participant, whose incumbent stands; a weave that wrote none is refused as `library create()
returned null`. The isolated host's child sends the words in a `Refused` frame in place of
its `Hello`, and the mount is refused in them. No other slot changes; a v9 image is refused at load
with both versions named, before any callback into it.

**v9 carries the declared emit-set, by definition** ([declared vocabulary is
agreed at admission](../decisions/declared-vocabulary-is-agreed-at-admission.md)).
No slot changes in either table; the descriptor's manifest is `zen.Manifest`
**v5**, which adds the optional `emits` list beside `referenced`, `accepted`,
`state`, `requests` and `claims`, and the host claims those definitions — with
every component they nest — into its agreement wall at load, beside the
accept-set. A loaded emitter of `Pong v1 {a, b}` and an acceptor of `Pong v1 {a}`
refuse at load in either order; an image unable to say what it emits would be
admitted, and the disagreement would surface as its first refused delivery. The
section is descriptive vocabulary: the host reads no grant from it, and it is not
an exhaustive send list. It is a version break although the tables are
unchanged, because the version is the one compatibility boundary this seam
declares: a v8 image's manifest claims `zen.Manifest` v4, a different identity
from the v5 door, and the gate would refuse it (`SchemaMismatch`, pinned in suite
`schema_codec`) — but only in `reconstruct`, after the image's static
initializers and `create()` had run, with a sentence about a meta-schema. The
version gate refuses the image in `fetch_abi` instead, before any callback into it, with both versions named, at load and
at reload over a live participant, and the incumbent stands. "Declared nothing"
and "could not declare" are two versions, not two readings of one gate sentence.

**v8 carries a joint publication's claimant surface** ([joint
publication](joint-publication.md#what-crosses-the-abi)). Two slots are
appended, one to each table: `ZenHostApi::sense_offer` — the loaded claimant
offers the next value of its own latest claim for an exact operation; the bytes
are admitted host-side against its declared claim-set and handed to the gated
bus — and `ZenWeaveAbi::claim_published` — the host shows the library a value a
joint operation published under its key, before its next `handle` and before
its next `snapshot`, and **the library's status is the application fact**. Two
statuses join the table: `ZEN_ERR_JOINT_BASE` (a refused offer, minus the
`JointRefusal` enumerator, so the library can name it) and `ZEN_CLAIM_DECLINED`,
the one positive status — neither OK nor an error — for a library that,
functioning, did not apply the value and keeps state of its own.

The host maps every status of the showing slot, and the mapping is on the slot
in `abi.h`: `ZEN_OK` is Applied; `ZEN_CLAIM_DECLINED` is Declined (not held; the
initiator told; the library re-claims its own truth at its next delivery); every
negative status is Failed (`ZEN_ERR` for an exception that escaped the library's
handler or a returned `PublishedClaim::Failed`, `ZEN_ERR_UNKNOWN_SCHEMA` /
`ZEN_ERR_REFUSED` for bytes the library's gate would not admit) and the weave is
held; every other positive status is undefined for the slot and read as Failed.
A descriptor whose slot is NULL is shown nothing and read as Failed. Each of
the three answers is witnessed through the real image (suite `joint`: a loaded
claimant that applies, one whose showing fails, and one that declines).

Only the claimant's verbs cross. The initiator's — begin, commit, cancel,
status, release — and the authority they need are native; a loaded initiator's
Bus inherits the refusing defaults and answers `NoLiveDelivery`
([known seams](known-seams.md#loaded-coordination-of-a-joint-publication)). The
isolated pipe supplies no offer door. No compatibility with v7 is claimed: a v7
image is refused at load **and at reload** with both versions named, before any
callback of anybody's runs, and an incumbent it was meant to replace stands
untouched.

**v7 carries sender-visible dispatch refusal.** The four ordinary addressed
send callbacks (`send`, `send_to_role`, `role_send`, `role_send_to_role`)
return their actual queued sequence through `attempt_out`; NULL is allowed and
zero means no queued identity. The C++ wrappers return that sequence as Ticket,
so matching needs no invalid or success-sentinel tickets. Immediate
seam failures queue nothing and receive no later dispatch notice.

The accept-set declares zen.DispatchRefused through the existing manifest.
The host generates its payload and sets ZEN_PROV_DISPATCH_REFUSAL; handle's
existing provenance argument carries that fact and the library adapter rebuilds
it. Ordinary outbound callbacks have no provenance parameter. Answer and
publication callbacks keep their separate existing status/return contracts.

No binary compatibility with v6 is claimed: a real pre-v7 image is refused at
load with both versions named. The isolated pipe returns no attempt identity and
refuses a manifest accepting zen.DispatchRefused, naming its unsupported
attestation reach. Nonparticipating child speech is unaffected. The bridge/wire
payload format carries no trusted provenance field.

**v6 carries Senses across the seam, both ways**, so a loaded weave has exactly
the surface a native one has: `claim` / `role_claim` outbound, `observe` /
`observe_role` inbound, and the declared **claim-set in the manifest** — which
is what makes a loaded artifact's Sense capability discoverable at load rather
than after some runtime claim accidentally reveals a shape. The claim-set rides
the existing manifest rather than a second descriptor entry point: it is the same
kind of fact as the accept-set, so one manifest still means one decode and one
gate crossing.

The trust split is v5's, unchanged: the library REQUESTS "claim as R" and the
host verifies membership at the claim moment; the library asks to observe and the
host authorizes against the loaded weave's own grant. A library attests nothing
about itself in either direction. A reading crosses as **bytes** the library
re-admits against its own definition of the shape — no host pointer into the
repository reaches a library, so a loaded reader has exactly the reach a native
one has: none. Sense refusals cross as their own statuses
(`ZEN_ERR_SENSE_NO_CLAIM`, `_NOT_AUTHORIZED`, `_UNDECLARED`,
`_ROLE_NOT_HELD`), so four different problems stay four different answers
rather than one silence.

**An observed reading loses nothing to the seam.** The authorship a loaded reader
receives carries every fact a native one gets: author id, life and incarnation as
of the claim, whether that **life** is still current, whether that
**incarnation** is still current (a separate question — a live replacement moves
the code without ending the life), the authored role, whether that role's
holder is still current, and the claim's revision. The shape's identity is not
carried back because it is the *query* — a reading answers for the (name,
version) that was asked for.

**The role name crosses exactly, at any length.** It travels through a
caller-provided `ZenByteSink` — the same mechanism the claim's value uses — and
is *not* a field in `ZenSenseBy`. A fixed `char role[128]` would truncate at the
bound and let an observation report a role identity nobody ever authored: a
200-character role arriving as a plausible 127-character prefix, and two roles
agreeing for their first 127 bytes indistinguishable. A bigger buffer only moves
that lie further out, so there is no bound, no representability limit and no
truncation refusal. A role sink that is never written means the claim was
personal, which no real role name can imitate.

No binary compatibility is claimed: a v5 artifact refuses at load, naming both
versions — the honest failure, since it would otherwise load compiled against a
Bus whose claim/observe verbs silently return the refusing defaults: unable to
claim, unable to read, and unable to say so.

**v5 carried role-authored delivery provenance and gave loaded weaves the
explicit role-authorship doors native weaves already have**, for the same
reason and with the same honest refusal of its predecessor.

## Shape

A library exports one versioned descriptor (`ZenWeaveAbi`): create/destroy,
manifest reconstruction (accepted schemas + state schema, delivered as
**bytes** the host re-admits), handle (bytes in), snapshot (bytes out via
`ZenByteSink` — the library allocates into the host's sink; no cross-allocator
free, no host pointer into library memory), revive, policy. `ZEN_EXPORT_WEAVE`
writes all of it for a `Weave` subclass; library authors almost never touch
the ABI directly ([guide](../guides/dynamic-weaves.md)).

An artifact the host does not load is refused before any slot of it is called, by what it needs
and what the host has ([KERN-04](../laws/kernel-laws.md),
[the number and the sizes](#the-number-and-the-sizes)).

## The creation value

A weave is handed one value at its making, with its load: what it needs before its first turn,
such as a path a host fixed or the vocabulary it serves. A value, because a value is all that can
be judged before the weave runs; never a door, a handle or an authority, since a door handed at
creation is reach given to code nobody has admitted yet. Its capabilities are the load's grant.

- **Declared.** `ZEN_MADE_WITH(Setup)` in the weave and a constructor taking `const Setup&`. The
  descriptor's `describe_creation` writes the shape as `zen.Creation` (the shapes it nests, then the
  shape) without making an instance, because the manifest is read from one, after `create()`.
- **Gated before it runs.** The host admits the load's value against that shape before any of the
  weave's code sees it (`loom::judge_creation`, `zen/kernel/creation.hpp`, one function for the
  kernel, the box and the isolation host), then calls `create_with` with the admitted bytes, which
  the library re-admits against its own definition as it does state. A value that does not admit
  refuses the load in the gate's words; so does a load naming none for a weave made with one, and a
  load naming one for a weave made with none. Nothing is made.
- **Named by each door.** `Kernel::load(..., Creation)`, `load_candidate(..., Creation)`,
  `LoadLibrary` v4 and `zen.LoadWeave` v3 (`creation`, the value's JSON form, empty for none: text,
  since the value's shape is the artifact's and the bus holds no definition of it until the artifact
  is loaded), a boot plan row's `creation`, the console's `start ... --with <value>`, and a native
  weave through `loom::mount_made<W>(bus, value, why)`, which judges by the same function.
- **Kept.** A reload makes the new image with the value the load named: the same participant in new
  code. A prepared candidate is a new weave and names its own. An isolated child is told its
  value after it says its declared shape (`Creation` and `Make` frames), and a respawn is made
  with the same. `Kernel::made_with(name)` says it, the door's `ListLibraries` answers
  `name@role<Setup v1>`, and history keeps the load messages that carried it.

## The number and the sizes

From v13 the version says what an image needs: a host loads every version from
`ZEN_ABI_OLDEST` to its own `ZEN_ABI_VERSION`. Every change to `abi.h` still moves the number,
and none breaks an image already built: a field or a slot is appended at the end of its struct,
and a status, a provenance value and a slot keep their number and their meaning for ever. A host
reads an image's descriptor (`zen/kernel/descriptor.hpp`, shared by the kernel, the box and the
isolated child) before calling any slot of it, and refuses, naming what the image needs and what
the host has:

| the image | refused as |
|---|---|
| from before `ZEN_ABI_OLDEST` | `unsupported abi_version ...`, both versions named |
| newer than the host | `the image needs abi_version ...`, which the host lacks |
| a descriptor whose size does not reach `handle` | its size, and the size `handle` needs |
| built against a larger host table | both tables' sizes |
| writing a manifest version the host does not read | both manifest versions, before `create()` |

What it loads it reads as far as each size reaches. A slot past the descriptor's size is a NULL
slot, whose meaning the slot's comment says (`claim_published`: shown nothing, read as Failed). The
host hands a table of its own size, at least the size the image named, and the library calls no
door the table's size does not reach (`ZEN_ABI_REACHES`). A delivery's facts and a reading's
authorship are read and written the same way: a fact past the size is absent. A provenance value
is handed only with a shape the library declares it accepts for that value, so a value appended
later, with a shape of its own, never reaches an image built before it; a status a reader does not
know reads as its slot says, never as success.

A box judges the same facts: a module's version and manifest version through its glue's exports
(`zw_abi_version`, read first, so a module built against another table is refused by its version
and not by the exports it lacks, and `zw_abi_manifest_version`), and its host table by its
imports, each by name. The delivery's facts are written into the module's memory in wasm32's
layout and say their size. The isolation pipe carries a delivery's facts as one block that says
its length, so a reader takes the facts it knows and a later one is appended.

## A handler that does not finish

The library catches everything at its own boundary: `do_handle` wraps the whole
call and returns `ZEN_ERR` where an exception escaped, so a loaded weave's
exception never reaches the Switchboard and a pump over one still returns
normally. The message was validly delivered, and the library's internal error is
the library's own concern — but discarding the status would have the bus record
and announce a plain `Delivered` for a handler that never completed, which is the
one thing an observer most needs not to be told.

So the status crosses back as a fact and nothing else. `HostAdapter::handle`
reports a non-OK status through `Switchboard::note_handler_failure()`, the bus
emits `EventKind::HandlerFailed` instead of `Delivered`, and it records no
journal outcome — exactly what the native throwing path already did
([MSG-10](../laws/messaging-laws.md#msg-10--a-callback-that-throws-costs-the-delivery-not-the-bus)).
Nothing is refused, nothing is retried, nothing is translated into a
`RefusalReason`, and the status reaches no weave.

## Constructing the tables

**Every in-tree construction of `ZenWeaveAbi` and `ZenHostApi` names its fields**
— designated initializers, in declaration order. That is `ZEN_EXPORT_WEAVE`, the
in-process host (`src/kernel/kernel.cpp`), the isolation child
(`src/isolation/weave_host_main.cpp`) and the two hand-written descriptor
fixtures. The form is deliberate: designators in declaration order are standard
C++20 *and* standard C99+, so one spelling serves both sides of a seam whose
header promises to be valid in each. Out-of-order designators are legal C and
ill-formed C++, so order is not a style preference here.

**Why it is not merely tidiness.** `describe`, `snapshot` and `policy` are three
different doors that share one type, `ZenStatus (*)(void*, ZenByteSink)`. They
are the only three fields of `ZenWeaveAbi` that share a type with anything, and a
positional initializer can permute them and compile without a single diagnostic
under `-Wall -Wextra -Wpedantic -Werror`. A designator states which door each
function is and makes the compiler check that the door exists.

**What that does and does not buy.** Three separate protections, which do not
imply one another:

| protection | mechanism | scope |
|---|---|---|
| refusal by what an image needs | the descriptor's version, sizes and manifest version read at load before any slot is called (KERN-04) | an artifact built against an ABI this host does not load, or needing what it lacks, meeting this host |
| rebuilt-source compatibility | declared function-pointer types; `-Wmissing-field-initializers` with `-Werror` on in-tree C++ producers | incompatible assignments and omitted appended fields; outside producers choose their own warnings |
| correct current wiring | named initializers plus semantic witnesses through the real host and loaded image | this build's field-to-function mapping, including assignment-compatible mistakes |

A designator can still name the wrong function. For the descriptor's three
byte-emitting doors, the gate catches that mistake: manifest, state and lifecycle
policy have different schemas. A `kernel` case, the descriptor's three
same-signature doors each answering for themselves, pins the successful
consequences; a swap of any two is refused by the gate, since each door's answer
has its own schema.

**The host table has one compatible pair.** `send_to_role` and `role_publish`
share a function-pointer type since v7 added `attempt_out` to the addressed door:
both end in `uint64_t*`, but one returns an attempt and the other a recipient count.
They are the only pair among the host's fifteen callbacks (seventeen fields with
`size` and `ctx`), and they are not adjacent. Both can admit the same payload shape, so the
descriptor-schema argument does not distinguish their operations. v8's appended
`sense_offer` (`void*, uint64_t, const uint8_t*, size_t`) shares its type with
nothing in the host table, and the descriptor's appended `claim_published`
(`void*, const uint8_t*, size_t`) is assignment-compatible with `revive` — the
same shape, bytes in. A designator can still name the wrong one; what tells
them apart is their consequence: `revive` admits the bytes against the STATE
schema and `claim_published` against the CLAIM-SET, so a swap is a gate
refusal at the first real showing (the loaded claimant is shown a `DocFact`,
which the probe's `ProbeState` gate refuses), never a silent success.

The `dispatch_loaded` case for loaded role sends and role publications loads
the real image and checks the host's actual
destination set, personal versus verified role provenance, and exact attempt
versus recipient count. It chooses an addressed role the author also holds and
primes sequences above the fanout count, so both wrong callbacks can succeed and
an accidental numerical equality cannot hide the swap. Results are read through
the image's snapshot, independent of the callbacks being tested. Ordinary direct
send/publication and unauthorized-role controls remain. Cross-wiring the real
host's two named initializers compiles and makes this witness fail semantically;
restoring that host makes it pass.

### When fields or signatures evolve

`abi.h` promises valid C. There is no standing C-producer compilation gate: the
project is `LANGUAGES CXX`, and the promise is checked when the header changes,
by compiling it with a minimal named C producer under strict C99/C11/C17 and the
export path under C++20 (step 4 below). A real C/non-C++ producer or public
binding is the trigger to add that infrastructure.

For an appended field or slot, the one change that from v13 breaks no image already built (a
changed signature, a removed field and a slot whose meaning changes each break one, and the seam
makes none of them):

1. Inventory assignment-compatible callbacks across **all** fields, including
   nonadjacent pairs. A signature change can create a collision without adding
   or moving a field: v7's `attempt_out` made `send_to_role` and `role_publish`
   one type.
2. Keep each construction site explicit and in declaration order: export macro,
   in-process host, isolation child, the box's descriptor and its module glue, and the manual
   descriptors in `tests/weavelib/`. Append new fields at the end and document their meaning in
   the header, including what a reader does where a size does not reach the field: an appended
   slot is NULL to a host reading an older image, and an appended fact absent to an older
   library. Null unsupported child doors deliberately.
3. Check each interchangeable pair through its public semantic consequences.
   Different descriptor schemas distinguish the three descriptor doors; two
   operations accepting the same schema need destination/provenance/result
   witnesses. A same-type swap in a fake table or compilation alone is insufficient.
   Use an isolated real-host/image mutation when the changed surface leaves a gap,
   prove it reached the executed artifact, and require a successful restored control.
4. Compile the header and named producer in the promised C standards and the
   export path in C++20; check the changed assignments at every construction site.
   Preserve the missing-field warnings, while keeping source compatibility
   separate from semantic wiring and stale-binary evidence.
5. Move the version, and preserve rejection before callbacks — at load and at
   reload, where a refused candidate must leave the incumbent untouched. The
   manual fixtures pin the gate's ordering; a current image relabeled old does
   not replace evidence from an actual earlier artifact. An append owes what the
   [compatibility discipline](#compatibility-discipline) names, and the opening of
   this page says what each version carried: a new one adds its paragraph there.
6. **State what the bus records for every status of a new callback slot** —
   including the one that is neither OK nor an error — and witness each with a
   loaded image whose callback answers it. A slot whose status the host
   discards, or maps by default to success, turns a library that failed into
   one that silently succeeded: a host that `(void)`s `claim_published`'s
   `ZEN_ERR` has a library whose hook threw stand behind a claim it never
   applied. Document the raw mapping on the slot, including undefined values,
   and pin each defined value through the real image (suite `joint`, one loaded
   image per answer).

## Host services (what a loaded weave's `Bus` really is)

The library-side `Bus` shim forwards through host callbacks: `send`,
`send_to_role`, `publish` (addressed sends carry actual queued tickets since v7;
publication counts retain their existing behavior), since
v4 the **answer doors** with real success/failure: `answer`, `defer_answer`
(opaque token), `answer_deferred`, `release_deferred` — a refused dynamic
answer is *told* to the weave (`ZEN_ERR_REFUSED`), the parity law
([ANS-06](../laws/answer-authority-laws.md)) — and since v5 the **role
doors**: `role_send`, `role_send_to_role`, `role_publish`. The library
REQUESTS "speak as R"; the host knows the exact weave bound to the context,
verifies membership at that moment, and stamps. A refused authorship crosses
back as the precise `ZEN_ERR_ROLE_AUTHORSHIP_DENIED` (never downgraded to a
personal send). A holder at its waiting bound is `ZEN_ERR_PENDING_PER_SENDER`
instead, on those doors and on an ordinary send. `role_publish` carries its
recipient count out-of-band so "authorized, zero listeners" and "denied" stay
distinct.

**No exception crosses a door, in either direction.** A loaded image may carry
its own C++ runtime ([KERN-05](../laws/kernel-laws.md)), under which an
exception thrown on the other side is undefined. So every host door answers what
stopped the host's own work — its delivery sequence at its end, a host observer
that threw while a refusal was being recorded, memory — as `ZEN_ERR_REFUSED`;
`defer_answer` answers no token (0); `release_deferred` answers nothing and keeps
it on the host's side. A byte sink never throws into its caller; it records a
write it cannot hold. When the host's sink, handed to `describe`, `snapshot` and
`policy`, records one, the host refuses that load or call in its own words ("the
host could not hold the bytes library ... wrote"); when the library's sink,
handed to an observe door, records one, the library refuses that reading. Suite
`dispatch_loaded` drives each door through a real image.

Delivery-side, the host passes the delivery's facts as one `ZenDelivery`:
provenance, the attested sequence, the authored role (NULL = personal — a
separate axis, deliberately not another `ZEN_PROV_*` kind) and the role the
delivery was addressed to, into the library shim, so `mail.answers_ask()` /
`mail.lifecycle_attested()` / `mail.authored_from_role()` /
`mail.addressed_role()` read the same on both sides. The addressed role is no
attestation: it says where the delivery went, and an isolated child is told it too. A library could lie
to *itself* here; it buys nothing — provenance has no wire form and the host
recomputes its own truth.

Since v8 the **claimant's joint-publication door**: `sense_offer` outbound
(`Mail::offer` on the library side), and `claim_published` inbound — not a
delivery, no host table crosses, and the status crosses back as the
application fact ([joint publication](joint-publication.md#what-crosses-the-abi)).
The initiator's verbs keep the refusing defaults across the seam.

Out-of-process children receive **null** capability doors and fail closed
(cross-process attestation is deliberately out of scope) — including
the v5 role doors, in both directions: an isolated weave genuinely holding
its role is still refused role authorship at the pipe, and told so. The v6
Sense doors join the same standing law: the claim doors are null because the pipe
carries no verified identity for the parent to check a claim-set or a role
against, and the **observe** doors are null for the mirror reason — reading is
authorized against the reader's own grant, and the child cannot present one the
parent minted.

## Compatibility discipline

A version number is structurally unobservable within one self-consistent build —
it protects **mixed** artifacts, so each protection is witnessed by an image built
apart from the host that judges it.

- **An image from before the tables said their size is refused.** The stale-artifact
  fixture declares `ZEN_ABI_OLDEST - 1`, so the pin keeps its meaning at every
  append — at load, and at reload over a live participant, where the incumbent must
  stand. It is a current-layout image that *claims* that version: it pins the gate's
  ordering, and it is not evidence about an old binary's descriptor. That evidence is
  `tests/weavelib/abi_v12_image.cpp`, compiled against ABI 12's header as it was,
  kept byte for byte in `tests/abi_kept/` and held to its hash at configure (suite
  `seam`, at load and at reload).
- **An image needing what the host lacks is refused by what it needs**: the next
  version, a larger host table, an unread manifest and a descriptor short of
  `handle`, each its own fixture (`tests/weavelib/needs_more.cpp`, suite `seam`).
- **An image built against an earlier header loads and works.** Every append owes
  an image compiled against the previous header, loading and working on the new
  host: the header of each version is kept in `tests/abi_kept/<version>/` as the
  version leaves it, the current one held to the live header byte for byte until the
  version moves. Until the first append, a descriptor that *says* it was built
  against smaller tables stands in for it (`ADDRESSED_PROBE_SMALLER_TABLES`, suite
  `seam`), and the read of a descriptor and of a host table past their sizes is
  pinned directly.

## Tests

Suite `seam` (the descriptor and the host table read as far as their sizes, each
refusal by what an image needs, the ABI 12 image at load and at reload, a descriptor
saying smaller tables loading and working, the addressed role read natively and
loaded, and boxed in suite `box`); suite `kernel` (descriptor gate, byte-sink
ownership, dynamic answer parity, provenance across the seam, role-authorship parity
+ the stale fixture's refusal at load and at reload — it declares
`ZEN_ABI_OLDEST - 1`, so those cases never name a frozen number; the "schema
admission" cases: a loaded emitter's definition meeting acceptors at load, in
both orders); suite `schema_codec` (the manifest carries the emit-set and
its components; a v4 manifest does not pass the current door; a manifest carrying two
definitions of one component is refused at the second); suite `joint` (the v8
claimant doors: offer across the seam, and the showing's three statuses); suite
`isolation` (the fail-closed pipe, both directions; a child's emit-set; the
addressed role across the pipe);
[Night Lab](../evidence/night-lab.md)'s `marathon/repro_answer_seam.cpp` as the
application-shaped witness.
