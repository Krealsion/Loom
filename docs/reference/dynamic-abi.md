# Dynamic ABI — reference

The C seam a weave library and the host agree on. Current version: **v7**
(`ZEN_ABI_VERSION` in `include/zen/kernel/abi.h`). Laws:
[KERN-01, KERN-04](../laws/kernel-laws.md),
[ANS-06](../laws/answer-authority-laws.md),
[MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit),
[SENSE-01..05](../laws/sense-laws.md).

**v7 carries sender-visible dispatch refusal.** The four ordinary addressed
send callbacks (`send`, `send_to_role`, `office_send`, `office_send_to_role`)
return their actual queued sequence through `attempt_out`; NULL is allowed and
zero means no queued identity. The C++ wrappers return that sequence as Ticket,
so matching no longer relies on invalid or success-sentinel tickets. Immediate
seam failures queue nothing and receive no later dispatch notice.

The accept-set declares zen.DispatchRefused through the existing manifest.
The host generates its payload and sets ZEN_PROV_DISPATCH_REFUSAL; handle's
existing provenance argument carries that fact and the library adapter rebuilds
it. Ordinary outbound callbacks have no provenance parameter. Answer and
publication callbacks keep their separate existing status/return contracts.

No binary compatibility with v6 is claimed: a real pre-v7 image is refused at
load with both versions named. The isolated pipe returns no attempt identity and
refuses a manifest accepting zen.DispatchRefused, naming its unsupported
attestation reach. Nonparticipating child speech retains its existing behavior.
The bridge/wire payload format gains no trusted provenance field.

**v6 carries Senses across the seam, both ways**, so a loaded weave has exactly
the surface a native one has: `claim` / `office_claim` outbound, `observe` /
`observe_office` inbound, and the declared **claim-set in the manifest** — which
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
`_OFFICE_NOT_HELD`), so four different problems stay four different answers
rather than one silence.

**An observed reading loses nothing to the seam.** The authorship a loaded reader
receives carries every fact a native one gets: author id, life and incarnation as
of the claim, whether that **life** is still current, whether that
**incarnation** is still current (a separate question — a live replacement moves
the code without ending the life), the authored office, whether that office's
holder is still current, and the claim's revision. The shape's identity is not
carried back because it is the *query* — a reading answers for the (name,
version) that was asked for.

**The office name crosses exactly, at any length.** It travels through a
caller-provided `ZenByteSink` — the same mechanism the claim's value uses — and
is *not* a field in `ZenSenseBy`. The first v6 draft carried it as a fixed
`char office[128]` and truncated at the bound, which let an observation report an
office identity nobody ever authored: a 200-character role arrived as a plausible
127-character prefix, and two offices agreeing for their first 127 bytes were
indistinguishable. R2E-0a removed the bound rather than raising it — a bigger
buffer only moves the lie further out — so no representability limit remains and
no truncation refusal was needed. An office sink that is never written means the
claim was personal, which no real role name can imitate.

No binary compatibility is claimed: a v5 artifact refuses at load, naming both
versions — the honest failure, since it would otherwise load compiled against a
Bus whose claim/observe verbs silently return the refusing defaults: unable to
claim, unable to read, and unable to say so. R2E-0a changed v6's layout **without
bumping to v7**, deliberately: v6 was never published, so no released artifact
could observe the difference, and a bump would assert a compatibility boundary
that does not exist.

**v5 carried role-authored delivery provenance and gave loaded weaves the
explicit office-authorship doors native weaves already have**, for the same
reason and with the same honest refusal of its predecessor.

## Shape

A library exports one versioned descriptor (`ZenWeaveAbi`): create/destroy,
manifest reconstruction (accepted schemas + state schema, delivered as
**bytes** the host re-admits), handle (bytes in), snapshot (bytes out via
`ZenByteSink` — the library allocates into the host's sink; no cross-allocator
free, no host pointer into library memory), revive, policy. `ZEN_EXPORT_WEAVE`
writes all of it for a `Weave` subclass; library authors almost never touch
the ABI directly ([guide](../guides/dynamic-weaves.md)).

An artifact declaring any other version is refused at load, naming both
versions ([KERN-04](../laws/kernel-laws.md)).

## A handler that does not finish

The library catches everything at its own boundary: `do_handle` wraps the whole
call and returns `ZEN_ERR` where an exception escaped, so a loaded weave'''s
exception never reaches the Switchboard and a pump over one still returns
normally. What the host does with that status is the part RTH-1 changed. It used
to be discarded entirely — the message was validly delivered, and the library'''s
internal error was the library'''s own concern — which is still true and was still
not the whole truth: the bus then recorded and announced a plain `Delivered` for
a handler that never completed, which is the one thing an observer most needs not
to be told.

So the status crosses back as a fact and nothing else. `HostAdapter::handle`
reports a non-OK status through `Switchboard::note_handler_failure()`, the bus
emits `EventKind::HandlerFailed` instead of `Delivered`, and it records no
journal outcome — exactly what the native throwing path already did
([MSG-10](../laws/messaging-laws.md#msg-10--a-callback-that-throws-costs-the-delivery-not-the-bus)).
Nothing is refused, nothing is retried, nothing is translated into a
`RefusalReason`, and the status reaches no weave.

## Constructing the tables (BL-4)

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
| stale-binary refusal | `abi_version` checked at load before descriptor callbacks (KERN-04) | an artifact built against another ABI meeting this host |
| rebuilt-source compatibility | declared function-pointer types; `-Wmissing-field-initializers` with `-Werror` on in-tree C++ producers | incompatible assignments and omitted appended fields; outside producers choose their own warnings |
| correct current wiring | named initializers plus semantic witnesses through the real host and loaded image | this build's field-to-function mapping, including assignment-compatible mistakes |

A designator can still name the wrong function. For the descriptor's three
byte-emitting doors, the gate catches that mistake: manifest, state and lifecycle
policy have different schemas. The existing `kernel` case "BL-4: the descriptor's
three same-signature doors each answer for themselves" pins the successful
consequences. BL-4's v6 mutation evidence covers all three descriptor swaps;
v7 leaves those signatures, named mappings and schema distinctions unchanged.

**The host table now has a compatible pair.** `send_to_role` and `office_publish`
share a function-pointer type since v7 added `attempt_out` to the addressed door:
both end in `uint64_t*`, but one returns an attempt and the other a recipient count.
They are the only pair among the host's fourteen callbacks (fifteen fields including
`ctx`), and they are not adjacent. Both can admit the same payload shape, so the
descriptor-schema argument does not distinguish their operations.

The `dispatch_loaded` case "ABI v7: loaded role sends and office publications have
distinct successful semantics" loads the real image and checks the host's actual
destination set, personal versus verified office provenance, and exact attempt
versus recipient count. It chooses an addressed role the author also holds and
primes sequences above the fanout count, so both wrong callbacks can succeed and
an accidental numerical equality cannot hide the swap. Results are read through
the image's snapshot, independent of the callbacks being tested. Ordinary direct
send/publication and unauthorized-office controls remain. Cross-wiring the real
host's two named initializers compiles and makes this witness fail semantically;
restoring that host makes it pass.

### When fields or signatures evolve

`abi.h` promises valid C. Bounded v7 checks compiled the actual header and a
minimal named C producer under strict C99/C11/C17, and the export path under
C++20. There is no standing C-producer compilation gate: the project remains
`LANGUAGES CXX`. A real C/non-C++ producer or public binding is the trigger to
add that infrastructure; this compatibility witness does not create one.

For an appended field **or a changed existing signature**:

1. Inventory assignment-compatible callbacks across **all** fields, including
   nonadjacent pairs. A signature change can create a collision without adding
   or moving a field, as `attempt_out` did in v7.
2. Keep each construction site explicit and in declaration order: export macro,
   in-process host, isolation child and manual bad/stale descriptors. Append new
   fields at the end and document their meaning in the header; review existing
   mappings when a signature changes. Null unsupported child doors deliberately.
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
5. Decide the version deliberately and preserve rejection before callbacks.
   The manual previous/future-version fixtures pin the gate's ordering; a current
   image relabeled old does not replace evidence from an actual pre-change artifact.
   See the v2/v4/v5/v6/v7 notes in `abi.h` for the breaks already paid.

## Host services (what a loaded weave's `Bus` really is)

The library-side `Bus` shim forwards through host callbacks: `send`,
`send_to_role`, `publish` (addressed sends carry actual queued tickets since v7;
publication counts retain their existing behavior), since
v4 the **answer doors** with real success/failure: `answer`, `defer_answer`
(opaque token), `answer_deferred`, `release_deferred` — a refused dynamic
answer is *told* to the weave (`ZEN_ERR_REFUSED`), the parity law
([ANS-06](../laws/answer-authority-laws.md)) — and since v5 the **office
doors**: `office_send`, `office_send_to_role`, `office_publish`. The library
REQUESTS "speak as R"; the host knows the exact weave bound to the context,
verifies membership at that moment, and stamps. A refused authorship crosses
back as the precise `ZEN_ERR_ROLE_AUTHORSHIP_DENIED` (never downgraded to a
personal send), and `office_publish` carries its recipient count out-of-band
so "authorized, zero listeners" and "denied" stay distinct.

Delivery-side, the host passes provenance flags + the attested sequence + the
authored role (v5: its own NUL-terminated parameter, NULL = personal — a
separate axis, deliberately not another `ZEN_PROV_*` kind) into the library
shim, so `mail.answers_ask()` / `mail.lifecycle_attested()` /
`mail.authored_from_role()` read the same on both sides. A library could lie
to *itself* here; it buys nothing — provenance has no wire form and the host
recomputes its own truth.

Out-of-process children receive **null** capability doors and fail closed
(cross-process attestation is deliberately out of scope in V1) — including
the v5 office doors, in both directions: an isolated weave genuinely holding
its role is still refused office authorship at the pipe, and told so. The v6
Sense doors join the same standing law: the claim doors are null because the pipe
carries no verified identity for the parent to check a claim-set or an office
against, and the **observe** doors are null for the mirror reason — reading is
authorized against the reader's own grant, and the child cannot present one the
parent minted.

## Compatibility discipline

The stale-artifact test fixture always declares `ZEN_ABI_VERSION - 1`, so the
refusal pin keeps meaning "the previous ABI refuses" after every bump. A
version number is structurally unobservable within one self-consistent build —
it protects **mixed** artifacts.

## Tests

Suite `kernel` (descriptor gate, byte-sink ownership, dynamic answer parity,
provenance across the seam, office-authorship parity + the previous-ABI
refusal — the fixture always declares `ZEN_ABI_VERSION - 1`, so that case
never names a frozen number); suite
`isolation` (the fail-closed pipe, both directions); Night Lab
`repro_answer_seam.cpp` as the application-shaped witness.
