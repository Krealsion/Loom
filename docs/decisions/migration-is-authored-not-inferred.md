# Migration is an authored transformation, not an inferred one

**Status: current (R2E-0).** Supersedes the speculative "automatic gate
migration" direction contemplated in earlier exploration. Laws:
[HANDOFF-01..03](../laws/handoff-laws.md).

## What was previously contemplated

Historical design exploration sketched a world where **admission itself** could
resolve a schema mismatch: a candidate value arrives shaped as `StateV1`, the
gate notices a registered relationship to `StateV2`, and a migration graph is
searched — possibly multi-hop — to coerce the value into the door's shape. Under
that sketch, a replacement across an evolved schema would "just work", and the
migration would be a registry the substrate consulted.

Those documents are frozen history and are **not edited** to pretend we always
believed otherwise. This ADR records the supersession.

## What new evidence appeared

The first real consumers arrived: the Workshop wants live code editing, and
Workshop values have already evolved. That fired the standing "migration layer"
trigger in
[known-seams](../reference/known-seams.md#deferred-with-intent-the-standing-trigger-map),
and the phase built the thing rather than continuing to reason about it — two
real loadable ledgers whose state schemas disagree on every field, a temporary
migrator, and a full prepared replacement across them.

Three facts came out of building it.

**1. The gate's wall is identity, not fields.** A `LedgerV1` value fails
`LedgerV2`'s gate *even when every one of v2's fields is made optional*. Admission
is keyed on schema identity. So "different schemas remain different" was never a
consequence of the fields happening to disagree — it is structural, and an
inference layer would have had to deliberately punch through it.

**2. Every property a migration must have is already a property of a weave.**
The phase required migrations to be inspectable, testable, versioned, refusable
and attributable. An ordinary temporary weave has all five as facts Loom already
carries. A callback registry has none of the last three; a plain library function
cannot be attributed at all; a gate hook is invisible by construction. The
alternative that looked closest — letting the *candidate* accept old-schema input
— fails on a different axis: it makes the two schemas stop being different, which
is the premise collapsing.

**3. Nothing needed to be added.** Prepared replacement already verifies the
successor; FIFO already supplies an exact boundary; the accept-set door already
refuses old traffic loudly. The Handoff Garden calls no API that R2E-0 invented,
because R2E-0 invented none for continuity.

## Why the current design differs

An inferred migration is a **hidden continuity promise**. It would mean the
substrate deciding, at a boundary the developer cannot see, that one meaning is
another meaning — with no author, no version, no refusal, and nobody to attribute
the result to when it is wrong. The failure mode is not a crash; it is a value
that is quietly, plausibly wrong, produced by machinery nobody chose.

A migration graph would also compound it: a multi-hop chain is a sequence of
inferences, each individually plausible, with a result no participant authored.
Debugging that means reconstructing a search the substrate performed and did not
record.

The current design puts the transformation where it can be argued with:

> **Migration is an explicit authored transformation *before* normal admission,
> not invisible coercion inside the gate.**

The migrator is a real artifact with a name, a version, a bus identity, tests,
and the ability to say no. It runs, it answers, and it unloads. The gate is
unchanged, and still refuses everything it always refused.

## What this rules out

- an admission-time migration registry, graph, or chain search;
- automatic multi-hop migration;
- any coercion of a queued **message** because a replacement happened —
  developers author compatibility (a temporary adapter, a persistent
  compatibility weave, dual accepted versions, explicit producer migration,
  quiescence, or refusal), and
  [HANDOFF-03](../laws/handoff-laws.md) keeps protocol compatibility and state
  migration separate on purpose;
- a Loom-owned identity allocator. The namespace obligation is the domain's, and
  the witness proves both outcomes — carried, and deliberately not carried.

## What remains intentionally unresolved

- **No migration vocabulary is canonized.** The Handoff Garden's `MigrateV1ToV2`
  / `MigrationResult` pair is a *test domain's* protocol, not a Loom shape. If
  many packages converge on the same spelling, that is when a shared vocabulary
  is earned — and it would be a package, not the substrate.
- **Schema-as-value stays out of scope.** A migrator that wanted to reason about
  shapes generically would need it; this one does not, because it knows both
  schemas statically. Recorded as pressure, not built.
- **Whether a persistent compatibility adapter deserves package support** is
  untested here. The phase proved refusal is honest and visible; it did not build
  the adapter pattern, and does not claim to know its shape.

## If this is ever wrong

The falsifier is concrete: an application where the authored migration is pure
mechanical repetition across many schema pairs, with no domain judgment in any of
them, and where the repetition — not the judgment — is the cost. That would argue
for a generated or declared transformation, and it would still be *authored*
somewhere. It would not argue for the gate inferring anything.
