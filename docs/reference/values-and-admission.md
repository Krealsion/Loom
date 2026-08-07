# Values and admission — reference

The core: self-describing values, one gate, immutable published schemas. Laws:
[GATE-01..04](../laws/admission-laws.md).

## Schema

A `Schema` is a frozen, named, versioned shape: `(name, version)` plus ordered
fields of the seven permanent primitive kinds (`Int` i64, `Float` f64, `Text`,
`Bool`, `Bytes`, `List`, `Message` — nested by schema). Identity across any
boundary is the **content-id** (FNV over the canonical shape), never the C++
type; `same_identity` is the full `(name, version, content_id)` triple.
Published schemas are immutable: evolving a shape is publishing a new version
([GATE-04](../laws/admission-laws.md)). Build one with `SchemaBuilder`, or
derive one from a struct with `ZEN_SHAPE`
([guide](../guides/writing-a-weave.md)).

## Value

A `Value` carries the schema it *claims* plus positional cells. Values are
ordinary C++ values (copyable); `construct_blind` builds one at runtime from a
discovered schema — the console's road. Claiming is not conforming: only the
gate says a value is what it claims.

## The gate

```text
bytes ──parse()──► Unverified ──admit(door)──► Value      (or a structured refusal)
```

`loom::admit(candidate, door)` is the **sole** conformance authority
([GATE-01](../laws/admission-laws.md)). `Unverified` has no accessors — there
is no road from bytes to readable data around the gate
([GATE-02](../laws/admission-laws.md)). Admission is strict (no partial
acceptance) and total (caps on depth/size make hostile input safe — see
[bounds](bounds.md)). Refusals carry kind, field path, expected/actual.
`gate_invocations()` exists so tests can prove every boundary funneled through.

**Decoding is bounded, and that is a separate fact from the wire size.**
Wire-size limits bound how many *serialized bytes* a value may carry.
The decode-materialization limit bounds how much *trusted host structure* those
bytes may create — one shared allowance of `kMaxDecodedCells` per top-level
decode, spent before the cells exist, in both encodings
([bounds](bounds.md#the-decode-materialization-bound)). The two are different
because a compact encoding can legitimately stand for many values: a zero-field
`Message` costs no body bytes, so without the second bound a few dozen bytes
could command millions of cells in the receiving process.

Neither limit implies application-semantic validity. A width, an interval, a
board dimension: the gate does not judge whether those are *sensible*, only
whether the value conforms to its declared shape and fits the decoder's own
work bound. Ranges remain the receiving service's own contract.

Authorization (may this *sender* say this, to this target) is a distinct,
prior step and never folds into the gate
([GATE-03](../laws/admission-laws.md), [capabilities](capabilities.md)).

## Registry

Immutable `(name, version) → schema` registrations; a conflicting
re-registration of the same identity throws `SchemaConflict`. The bus takes
every weave's accept-set, declared claim-set and state schema at registration,
so all parties agree on what a name means before anything routes. Reads take an
immutable snapshot and traverse it lock-free.

**A schema is discoverable while something live requires it**
([LIFE-08](../laws/lifecycle-laws.md#life-08--a-schema-is-retained-by-a-live-claim-never-by-having-been-registered)).
Two doors say how long:

| | what it means | released by |
|---|---|---|
| `register_schema(s)` | publish for this Registry's lifetime | nothing — it is a claim with no end |
| `claim(schemas)` | publish while this claim lives | the returned `SchemaClaimScope` dying |
| `claim_known(scope, keys)` | keep an *existing* shape resolvable; publish nothing | the same scope |

`SchemaClaimScope` is move-only and RAII, so cleanup is structural rather than
paired: there is no `unregister_schema` a failure exit can forget. Acquiring
several schemas is one transaction and one publication — a conflict on the last
leaves no claim on the first — and releasing a scope removes every shape whose
last claim it held in one publication too.

Who holds the claims: a `WeaveRecord` for what its weave hears, may say and
persists; a Kernel's loaded-artifact record for its manifest; an isolation
`Link` for its mount. The Registry itself knows nothing of weaves, artifacts or
mounts.

Reclamation is about **discoverability, not memory**. A `Value` owns its schema
strongly and `lookup` returns a strong owner, so a schema that has left the
Registry stays valid everywhere it is already held; what changes is only that a
fresh lookup can no longer find it. The consequence to design against: when the
last weave that accepts a shape leaves and no authorized producer claims it, an
emission naming that shape meets the seam
([MSG-08](../laws/messaging-laws.md)) instead of being routed to nobody.

## Serialization

The wire form is the **native canonical binary**: a little-endian envelope +
positional, schema-guided body; a `Value` has exactly one encoding
(canonicality is pinned). A compatibility JSON codec exists for tooling.
`serialize(value)` → bytes; `parse(bytes)` → `Unverified` → the gate. This is
the persistence boundary and the dynamic-library currency
([KERN-01](../laws/kernel-laws.md)) alike: bytes in, gate, then trust.

## Tests

Suites `schema`, `value`, `gate`, `registry`, `serialize`, `compat`,
`integration`, `fuzz` (deterministic-seeded regression corpus).
