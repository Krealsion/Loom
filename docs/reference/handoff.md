# Authored handoff — reference

How one incarnation deliberately becomes another. Laws:
[HANDOFF-01..03](../laws/handoff-laws.md), sitting on
[PR-01..09](../laws/replacement-laws.md).

**This is a pattern, not an API.** R2E-0 added no Loom primitive for continuity,
because the substrate already had every piece:

| What handoff needs | What already provides it |
|---|---|
| a verified successor | prepared replacement ([PR-01..09](../laws/replacement-laws.md)) |
| an exact boundary | FIFO ([MSG-01](../laws/messaging-laws.md)) |
| a visible refusal for old traffic | the accept-set door (`NotAccepted`) |
| an inspectable, testable, versioned, refusable, attributable transformer | an ordinary weave |

The standing law is unchanged: prepared replacement **preserves nothing**
([PR-09](../laws/replacement-laws.md)). What crosses is what a migration carried.

## The shape

```text
LedgerV1{next_id, total, mode}                 the incumbent's meaning
        │
        │  HANDOFF_BOUNDARY  ── an ordinary message at an exact FIFO position
        ▼
  final authored v1 value      ── exact, because nothing further changes it
        │
        │  [ temporary migrator weave ]        ── authored, refusable, attributable
        ▼
LedgerV2{ids{high_water}, totals{count,sum}, modes:List<ModeFlag>}
        │
        │  preparation ask → authenticated answer → commit → admission
        ▼
  the successor, live, continuing the identity namespace
        │
        └─ the migrator and the retired incumbent unload
```

## Why a migrator weave

Compared before committing:

| Candidate | Inspectable | Testable | Versioned | Refusable | Attributable |
|---|---|---|---|---|---|
| **temporary migrator weave** | yes | yes | yes | yes | yes |
| host callback registry | no | awkward | no | no | **no** |
| candidate-owned migration | yes | yes | yes | yes | yes — a real option, see below |
| gate migration hook | no | no | no | — | no |
| plain library function | partly | yes | weakly | no | **no** |

The weave wins because every required property is a fact **Loom already carries**
about a weave, rather than a convention the domain must maintain. A gate hook is
invisible coercion inside the gate, and is forbidden
([ADR](../decisions/migration-is-authored-not-inferred.md)).

**Candidate-owned migration is a tradeoff, not an impossibility.** A candidate
can perfectly well declare an explicit migration protocol — a distinct shape
carrying the predecessor's state — and transform it itself. That does *not* make
the two schemas identical and does not erase the version boundary: the migration
shape is its own schema, admitted like any other, and `LedgerV1` still fails
`LedgerV2`'s gate. Saying it was "impossible" would be wrong.

What the separate migrator buys, and why this reference recommends it:

- **independent versioning** — the transformation has its own version, so
  v1→v2 and v2→v3 do not accumulate inside the successor;
- **independent testing** — it can be exercised without standing up a
  replacement ceremony;
- **independent refusal** — it can decline a state it cannot faithfully
  transform, and a refused migration reaches no candidate at all;
- **independent lifetime and unload** — it exists only for the ceremony, and
  is provably gone afterwards;
- **clear ownership of the old→new transformation** — "who transformed this?"
  has a bus identity as its answer, not a code path.

The cost of candidate-owned migration is that the successor carries knowledge of
every shape it may have to migrate *from*, for as long as it lives. That is a
real design choice a domain may reasonably make; it is not a rule Loom enforces
either way.

## Different schemas remain different

Admission is keyed on **schema identity**, not field compatibility. A `LedgerV1`
value fails `LedgerV2`'s gate even when every one of v2's fields is optional. So
HANDOFF-01 is structural: no "related-looking versions" heuristic exists to go
wrong.

A migrator may **refuse**, and a refused migration reaches no candidate — there
is nothing valid to offer, and manufacturing something would be the coercion this
design refuses.

## The FIFO boundary

```text
A  B  C   HANDOFF_BOUNDARY   D  E
└── ordinary ──┘      └── the domain's declared post-boundary policy ──┘
```

The boundary message is **ordinary**. Loom gives it no standing; what makes it a
boundary is that the incumbent's handler for it enters a quiescing state and
authors its final value there.

Post-boundary policy is the domain's, and Loom chooses none of it: **refuse ·
defer · buffer · redirect through an explicit adapter · degrade**. The reference
witness refuses, and counts the refusals, because a ledger that keeps minting
identities after handing its namespace away is precisely the failure the
namespace obligation exists to catch.

A domain refusal after the boundary is **not** a Loom refusal: the message was
delivered, the holder was unchanged, the shape was accepted, and the service
declined to act.

## Exact vs stale — keep them visibly apart

- **A snapshot taken while the incumbent is live is a snapshot.** It may go stale
  before it is used; PR-09's "no atomic incumbent snapshot" is unchanged. Label
  it.
- **A value authored at the boundary is exact**, because nothing further changes
  it — exact *according to the domain's chosen policy*, which is what quiescing
  buys.

The witness carries the label on the value itself (`LedgerV1Report{state,
quiesced}`), so a consumer that treats a snapshot as exact is choosing to, and
cannot do it by accident.

## Queued old-protocol traffic

Four positions, four honest outcomes, **no automatic migration ever**:

| Position | Outcome |
|---|---|
| before the boundary | means what it always meant |
| after the boundary, incumbent still in office | delivered; the **domain** declines it |
| queued around commit (role-addressed) | resolved at delivery → reaches the successor |
| after the role moved | `NotAccepted`, visibly, by name |

> **Loom gave the developer a boundary and a refusal. It did not pretend to know
> whether the old command still meant anything.**

Protocol compatibility is a *different problem* from state migration
([HANDOFF-03](../laws/handoff-laws.md)): it is a standing obligation needing a
persistent adapter, dual accepted versions, explicit producer migration,
quiescence, or refusal — with a lifetime, unlike a migrator that disappears.

## The minted identity namespace

An identity's namespace must live at least as long as references to it may live
([known-seams](known-seams.md#minted-identity-needs-a-surviving-namespace)). The
witness carries a high-water mark across an incompatible state schema — 47 issued
before, 48 next after — and the **control case** changes one flag in the migrator
so the successor mints `1` again, an id the predecessor already handed out.

The difference is the migration's. Loom has no identity allocator and no opinion,
and none is planned.

## Senses across a handoff

- a **sealed candidate** has no office claim and cannot make one;
- across the commit the incumbent's final claim stays the incumbent's, stamped
  `office_holder_is_current = false`;
- the successor is considered to have claimed nothing until its own
  `zen.Activated` handler claims — after which the role-bound view follows it, at
  the next revision of the same key;
- **an ordinary Sense is not an exact handoff snapshot.** Read mid-flight it goes
  stale like any other snapshot. A domain using Sense data as migration input
  explicitly accepts whatever staleness the Sense provides.

## Temporary means unloadable

Both the migrator and the retired incumbent unload afterwards, are gone
(`is_loaded()` is false, and the incumbent's Sense keys went with it), and the
successor keeps serving without either — which is what "temporary" has to mean to
be worth the word.

## Tests

Suite `handoff` — the incompatible-gate premise in both directions, H1 the
stale-snapshot witness, H2 the exact-boundary witness, the full authored handoff,
the namespace witness and its control, the refused migration, the four-position
queued-old-protocol witness, the Senses interaction, and the PR-09 re-proof.
