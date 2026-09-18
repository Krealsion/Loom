# Declared vocabulary is agreed at admission — every list, every component, and never as authority

**Status: current (2026-09-18; ABI v9, `zen.Manifest` v5).** Laws:
[GATE-04](../laws/admission-laws.md#gate-04--immutable-published-schemas),
[LIFE-08](../laws/lifecycle-laws.md#life-08--a-schema-is-retained-by-a-live-claim-never-by-having-been-registered),
[SENSE-04](../laws/sense-laws.md#sense-04--claiming-as-an-office-is-explicit-the-claim-set-is-a-contract),
[KERN-04](../laws/kernel-laws.md#kern-04--the-abi-seam-refuses-loudly). Reference:
[values and admission](../reference/values-and-admission.md#registry),
[kernel](../reference/kernel.md), [dynamic ABI](../reference/dynamic-abi.md).
Supersedes the recorded meaning of `Emit<...>` as "informational; does not register".

## The pressure

A native Workshop declared the newer `PaneInventory v1` only in `Emit<...>`. An older loaded
desktop accepted the same name and version over a different nested `InventoryPane v1`. Both
admitted. The first publication was refused at the gate — `claimed shape does not match this
door` — and Pane Manager stayed waiting, with the refusal visible only on the tap, because a
publication carries no addressed refusal back to its author. In the shipped load order the
stale desktop won the wall and the *current* Info pane was the artifact refused.

The wall the guide promised existed and worked — for every shape a participant **accepts,
claims or persists**. What had no wall was a shape a participant only **emits**: the bus pinned
an emitter's shapes by key and never took its definition; a library's manifest carried no
emit section at all, so a loaded emitter had nothing to offer either. Two further gaps sat
beside it. A native declaration was claimed as its top-level roots only, so two weaves that
nested `Part v1` under *different* outer names (`Box` and `Box2`) were never compared and two
definitions of one `(name, version)` lived in one process. And the manifest encoder
deduplicated nested components by name, so one weave declaring `Box { Part {a} }` beside
`Box2 { Part {a, b} }` lost its second `Part` before reconstruction and **loaded, advertising
a `Box2` it never declared** — a false contract, not a refused one.

## The decision

**Every shape a participant declares — accepted, claimed, emitted, persisted — and every
component those shapes nest is claimed, by definition, in the one registration transaction,
through the one wall, natively and across the seam alike.** Concretely:

- `loom::Weave` gained `emitted_schemas()` (defaulted empty); `WeaveBase` writes it from
  `Emit<...>`, `final`. `Switchboard::register_weave` and the code-swap re-claim take the
  **closure** of the accept-set, claim-set, emit-set and state shape.
- The closure is walked by one traversal, `collect_referenced` in `zen/schema.hpp` — moved
  down from the kernel codec to the layer that owns schemas, same name and signature —
  deduplicated by **identity** (name, version *and* content-id), never by name alone. Two
  definitions of one key both survive the walk; the Registry's existing same-key/different-
  content comparison refuses them, inside one declaration or against a live one.
- `zen.Manifest` v5 adds the optional `emits` list; `do_describe` writes it from the same
  virtual; `Kernel::reconstruct` claims it beside the accept-set; the host adapter and the
  isolation proxy answer `emitted_schemas()` from it. The encoder's `referenced` section carries
  both definitions of a contradictory component, and `decode_referenced` refuses at the second.
- A reload candidate's whole closure is checked against the bus's live vocabulary **before**
  the incumbent is touched, with the registry's own sentence — the one place validate-then-
  commit had a gap once nested components and emits were claimed.
- The ABI is bumped to **v9** although no table slot changed: a v8 image's manifest would pass
  a v5 meta-schema whose new section is optional and load with its emit-set silently absent.
  "Declared nothing" and "could not declare" must not be the same bytes.

## What it deliberately does not decide

- **Vocabulary is not authority.** Declaring a shape registers what it *means*. Send authority
  is still the grant the host attached at admission (GATE-03, GATE-05); a declared emitter with
  no rule for a shape is refused `CapabilityDenied` at delivery, pinned as the denied-send
  control beside the discovery case. The admission policy is shown the declaration as advice
  and derives nothing from it.
- **`Emit<...>` is not an exhaustive send list.** Publishing a shape absent from it is not
  refused for that reason; a router or forwarder that speaks shapes chosen at runtime meets the
  seam and its grant exactly as before. The emit-enforcement seam on `Mail` stays open.
- **No permanent global registry.** A declaration retains its vocabulary for its claim's
  lifetime (LIFE-08): the last holder leaving reclaims the shape, and a different definition may
  then publish. "Once per process" was never the contract and is not now; it is once per
  Registry, while something live requires it.
- **No automatic migration, no second wall.** The Kernel keeps its decoding registry for a
  manifest's nested references and compares content the same way; the Switchboard's registry is
  where every participant meets. Evolving a shape is still publishing a new version, or an
  authored handoff ([migration is authored](migration-is-authored-not-inferred.md)).

## Alternatives considered

- **Narrow the documentation to what the code did** (Emit stays informational; the guide,
  GATE-04 and the registry reference say only accepted/claimed/persisted shapes agree). Rejected:
  it leaves the product to explain a publication's gate refusal from the tap, since no author is
  told; and it keeps a declaration that means one thing natively and nothing across the seam.
- **Native half only, manifest later.** The experiment that proved the direction did exactly
  this; a loaded emitter still slipped through to a delivery-time seam refusal with a different
  sentence for the same cause. Rejected as the landed shape: one phase, both halves.
- **Keep name-keyed deduplication and refuse contradictions at describe time.** Rejected: the
  refusal would surface as `describe() failed` with no shape named, and a hand-built manifest
  would still be trusted. Carrying both definitions lets the *loader* refuse with the shape
  named, whoever wrote the manifest.
- **Merge the Kernel's registry into the Switchboard's.** Rejected as unnecessary: the gap was
  the closure and the emit-set never reaching the bus, not the existence of a decoding registry.
- **Add a `SchemaDesc`-level identity (content-id on the wire).** Not needed: identity is
  deep, so a component's content folds into its owner's, and the loader recomputes it.

## Consequences

- A stale acceptor is refused at load, in both load orders, with the shape named; the current
  artifact is never the one refused for a predecessor's drift.
- An emitted-only shape resolves from its emitter's declaration: a `AcceptMode::AnyRegistered`
  weave (the console) is now offered it on a directed send, where before the send was refused
  `NotAccepted`; publication still fans out to listed doors only. The known-seams reply-shape
  gap closes on the descriptive side; the approval-interface side of that seam remains.
- A consumer compiled against older headers rebuilds (the `loom::Weave` vtable grew); every
  image built against ABI v8 is refused at load and at reload with both versions named.
- Zengine's three seams that reuse `collect_referenced` (operator descriptors, provider
  contributions, maker definitions) inherit identity deduplication: a self-contradictory
  definition now refuses at its decoder instead of substituting.

## Current laws supported

GATE-04 (agreement across a seam is on content-id, for every declared shape and component);
LIFE-08 (a producer that declared a shape claims its definition; a grant-only producer still
claims by key); SENSE-04 (the claim-set is enforced at the claim doors, which is what makes it
particular — all three lists register); KERN-04 (v9 is paid as a break, rejection before
callbacks, with both versions named).

## Evidence

Suites `schema` (the traversal), `registry` (a contradiction inside one request),
`switchboard` (emitter/acceptor both orders, the accept/claim/emit/state routes to a nested
disagreement, a self-contradicting declaration, discovery and the denied-send control, swap
and reclamation), `weave` (the guide's copied weave), `schema_codec` (manifest v5, the
Box/Box2 manifest refused at the second Part, a hand-built manifest, the v4 door),
`kernel` (native/loaded and loaded/loaded emitters, nested-only across artifacts and against a
native weave, one artifact contradicting itself in both orders with the agreeing control,
refusal cleanup and reclamation, reload against a native-only definition, prepared candidate,
P-LOOM-06's copied weave), `isolation` (a child's emit-set against the bus and against another
mount), and `tests/package/stranger_host.cpp` (the installed package carries the emit-set and
the nested closure). Phase record: `loom-schema-admission-implementation` in the Zen workspace;
research: `loom-schema-admission-research`, with its independent review.
