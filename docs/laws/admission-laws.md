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

## GATE-05 — Baseline authority is admission-time; delegated authority is live; effective authority decides

LAW — A weave's **baseline** message authority is attached by the host at
admission and never changes. **Delegated** live authority may be replaced at any
time, only by a holder of a host-minted `GrantAuthority`, only on the one subject
that capability names, and only within the ceiling it carries. **Effective**
authority — baseline ∪ delegated — is what the bus checks, at the moment of
delivery.

MEANS
- the host remains the only origin of authority: a capability that can widen a
  subject comes from `host_grant_authority(bus, subject, ceiling)`, which needs a
  `Switchboard&`. Nothing in-band mints one, and no grant confers one — a weave
  holding `allow_any()` may say anything and may administer nothing;
- **deny-by-default survives**: a newly mounted weave holds empty delegated
  authority. It gains delegation-eligibility from nothing except a valid
  capability explicitly acting on it;
- **revocation is effective at delivery**, including for a message queued while
  the authority was still held. Nothing on an envelope remembers what was true
  when it was authored, so approval changes authority, not history — and so does
  withdrawal;
- delegated authority is a **union**, never an override, so revoking it cannot
  remove what the host granted at admission; and it is scoped to the **WeaveId**,
  exactly as the baseline is, so it survives a code swap or a revival and is
  destroyed with the record;
- the ceiling relation is **semantic**: "any" contains each exact selector, and
  an office rule and a WeaveId rule contain one another in neither direction —
  whichever weave holds the office today — so a routing decision can never become
  permanent authority;
- only the two domains that are read at the moment of use are delegable. Send
  rules and observe rules are; the containment fields of a `Grant` are not, and
  `LiveAuthority` has no vocabulary for them.

DOES NOT MEAN
- **that grants are now mutable.** OS capabilities, `FsAccess` and
  `ResourceLimits` are consumed once, at `IsolationHost::mount`, into a namespace,
  a mount view and a cgroup leaf that this process cannot revisit. They remain
  admission-time, and the delegation door cannot express them;
- that the administrator becomes the sender. Administration queues no message;
  the governed subject retries its own action, under its own identity, and the
  target sees the subject (MSG-02 is untouched);
- that a capability is a lease — destroying one revokes nothing it established;
- that a capability outliving its subject is dangerous: WeaveIds are never
  reused, so it names nothing and refuses `NoSuchSubject` permanently;
- that authorization moved. It is still checked before role resolution and before
  the gate (GATE-03); only the value it reads is now a union of two.

PROVEN BY — `include/zen/switchboard/grant.hpp` (`LiveAuthority::contains`,
`effective_permits*`), `include/zen/host/grant_wiring.hpp`,
`src/switchboard/switchboard.cpp` (`delegate_authority_as`, `deliver_one`,
`observe_as`); suite `grant`, and the unchanged `capabilities`/`policy` suites.
