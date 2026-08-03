# Handoff laws (HANDOFF)

Reference: [handoff](../reference/handoff.md). The ceremony these laws sit on
is [prepared replacement](../reference/prepared-replacement.md)
([PR-01..09](replacement-laws.md)).

**Authored handoff added no Loom API.** These laws describe what the substrate
already guarantees and what it deliberately refuses to do, so the pattern cannot
drift into a promise Loom never made. The negative half of the first one is
already law: [PR-09](replacement-laws.md) — prepared replacement preserves
nothing.

## HANDOFF-01 — Different schema identities require an explicit authored transformation

LAW — A value of one schema identity never becomes a value of another because
the versions look related. Only an explicit actor produces a valid new-schema
value, and the gate refuses everything else.

MEANS
- admission is keyed on **schema identity**, not field compatibility: a
  `LedgerV1` value fails `LedgerV2`'s gate even when every one of v2's fields is
  optional — so "different schemas remain different" is structural, not a
  consequence of the fields happening to disagree;
- the transformation is a first-class authored thing. The leading form is an
  ordinary **temporary migrator weave**, which wins because every property the
  transformation must have is a fact Loom already carries about a weave:
  inspectable (a real artifact), testable (ordinary tests), versioned (its own
  artifact), refusable (it answers a refusal), attributable (the bus stamps who
  transformed it);
- a migrator may **refuse**, and a refused migration reaches no candidate. There
  is nothing valid to offer, and manufacturing something would be exactly the
  invisible coercion this law forbids;
- the migrator is temporary in a provable sense: it unloads afterwards and the
  successor keeps serving without it.

DOES NOT MEAN
- that Loom holds a migration registry, graph or chain. Migration is an explicit
  authored transformation **before** normal admission, never coercion inside the
  gate ([ADR](../decisions/migration-is-authored-not-inferred.md));
- that a migrator is a Loom concept — it is an ordinary weave, and Loom knows
  nothing about its role in the ceremony;
- that the candidate should accept old-schema input. That would be the two
  schemas ceasing to be different, which is this law failing.

PROVEN BY — `loom::admit` (identity-keyed); suite `handoff` (the both-directions
gate case, the full authored handoff, the refused migration, the unload
witnesses).

## HANDOFF-02 — FIFO provides the boundary; the domain owns post-boundary policy

LAW — A handoff boundary is an **ordinary domain message** at an exact FIFO
position. Loom gives it no special standing. What the incumbent does on either
side of it is the domain's decision, and Loom chooses none of it.

MEANS
- everything delivered before the boundary is handled under ordinary policy;
  everything after it meets whatever the domain declared — refuse, defer, buffer,
  redirect through an explicit adapter, or degrade. The substrate carries all of
  them identically;
- because nothing further changes the incumbent after it quiesces, the value it
  authors **at** the boundary is exact — which is the difference between an
  ordinary snapshot and a final one;
- a snapshot taken while the incumbent is live is a snapshot, and must be
  labelled one. PR-09's "no atomic incumbent snapshot" is unchanged; the boundary
  is a *domain* construction on top of FIFO, not a substrate ceremony;
- a domain refusal after the boundary is **not** a Loom refusal: the message was
  delivered, the holder was unchanged, the shape was accepted, and the service
  declined to act. Keeping those two kinds of "no" apart is the point.

DOES NOT MEAN
- that Loom knows a boundary happened, or that any shape is privileged;
- that quiescing is required — a domain may hand off from a live service and
  accept the staleness, which is what most Night Lab applications did;
- that the boundary makes the replacement atomic. It makes the *authored value*
  exact; the ceremony's atomicity is still [PR-08](replacement-laws.md)'s.

PROVEN BY — suite `handoff` (H2 the exact-boundary witness with A/B/C before and
D/E after, H1 the stale-snapshot witness that is made to go stale on purpose).

## HANDOFF-03 — Protocol compatibility is not state migration

LAW — Transforming a persisted value and accepting an old wire shape are
different problems. Loom migrates neither, and never silently converts a queued
message because a replacement happened.

MEANS
- state migration (`StateV1 → StateV2`) is a one-time authored transformation; a
  temporary migrator is ideal, and it disappears;
- ongoing protocol compatibility (old clients still send `OldCommand`) is a
  standing obligation, and needs a persistent adapter, dual accepted versions,
  explicit producer migration, quiescence, or refusal — a **domain** choice with
  a lifetime, not a migration;
- queued old-protocol traffic gets four honest outcomes, not one: before the
  boundary it means what it meant; after the boundary with the incumbent still in
  office it is delivered and the domain declines it; queued around commit and
  after the role moves it reaches a successor that does not accept the shape and
  refuses `NotAccepted`, visibly and by name;
- **the lesson, stated plainly:** Loom gave the developer a boundary and a
  refusal. It did not pretend to know whether the old command still meant
  anything.

DOES NOT MEAN
- that dual-accepting both shapes is wrong — it is one of the legitimate
  patterns, and it is the domain's to choose and to carry;
- that a refusal is a failure of the handoff. A visible refusal is the designed
  outcome for a command whose meaning did not survive.

PROVEN BY — suite `handoff` (the four-position queued-old-protocol witness);
`Switchboard` accept-set routing (`NotAccepted`), unchanged by this phase.
