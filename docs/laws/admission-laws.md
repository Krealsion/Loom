# Admission laws (GATE)

Reference: [values-and-admission](../reference/values-and-admission.md).

## GATE-01 — One gate

LAW — `loom::admit()` is the sole conformance authority at every boundary. Every
delivery, revival, persistence load and cross-library payload funnels through it.

MEANS
- the bus gates each delivery against the *recipient's* accepted schema, at
  delivery time;
- kernel-loaded weaves' bytes are re-admitted host-side before anyone sees them;
- there is no fast path: the content-id shortcut is deliberately untaken so
  "one gate, every delivery" stays literally true.

DOES NOT MEAN
- that authorization happens here — a grant check is a distinct step (GATE-03);
- that a well-shaped message is a trusted message (provenance is a third
  question again — see [ANS-01](answer-authority-laws.md#ans-01--answer-authority-is-the-delivery)).

PROVEN BY — `src/gate.cpp` (`gate_invocations()` counter); suites `gate`,
`switchboard` (delivery-gating cases), `kernel` (bytes re-admitted).

## GATE-02 — Untrusted until proven

LAW — Unvalidated bytes have no accessors. `parse()` yields `Unverified`, which
is unusable until `admit()` accepts it.

MEANS
- there is no road from bytes to a readable `Value` that skips the gate;
- a gate refusal drops the candidate entirely — no partial acceptance;
- decoding untrusted bytes costs a **bounded** amount of host structure: one
  shared, host-owned allowance per top-level decode, spent before the structure
  exists, so a compact value cannot command unbounded host allocation
  ([bounds](../reference/bounds.md#the-decode-materialization-bound)).

DOES NOT MEAN
- that parsing is validation — `parse` only decodes structure; conformance to a
  *door* happens at `admit`;
- that an in-budget value is cheap, or that the bound judges what a value
  *means* — semantic ranges stay the receiving service's contract.

PROVEN BY — `include/zen/admission.hpp` (`Unverified`'s accessor-free surface is
a compile-time fact); suites `gate`, `serialize`, `fuzz`, `schema_codec`,
`bridge` (the R2F-A cases).

## GATE-03 — Authorization is not conformance

LAW — A grant check (`CapabilityDenied`) is a distinct step *around* the gate
(`GateRefused`), never folded into it, and runs first.

MEANS
- an unauthorized sender's message reaches neither the gate nor role
  resolution — it cannot even learn whether a role is held;
- the two refusals are distinct observable reasons, sending an operator to
  different fixes.

DOES NOT MEAN
- that holding a grant makes a message conform, or vice versa.

PROVEN BY — `src/switchboard/switchboard.cpp` `deliver_one` (order: sender-life
→ seal → grant → resolution → gate); suites `capabilities`, `policy`.

## GATE-04 — Immutable published schemas

LAW — A published `(name, version)` is frozen; identity across any boundary is
the content-id derived from the shape, never the C++ type.

MEANS
- evolving a shape means publishing a new version;
- two parties agree across a `.so` seam iff their content-ids match
  (`SchemaConflict` otherwise);
- `same_identity` is the full `(name, version, content_id)` triple.

DOES NOT MEAN
- that old versions vanish — both stay registered and distinct.

PROVEN BY — `src/registry.cpp`, `src/schema.cpp`; suites `registry`, `compat`,
`schema_codec`.
