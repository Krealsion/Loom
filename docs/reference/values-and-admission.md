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

Authorization (may this *sender* say this, to this target) is a distinct,
prior step and never folds into the gate
([GATE-03](../laws/admission-laws.md), [capabilities](capabilities.md)).

## Registry

Immutable `(name, version) → schema` registrations; a conflicting
re-registration of the same identity throws `SchemaConflict`. The bus
registers every weave's accept-set and state schema at registration, so all
parties agree on what a name means before anything routes. Reads are
lock-free-immutable; there is no mutation after publish.

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
