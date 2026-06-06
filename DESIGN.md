# zen-core — design

`zen-core` is the self-describing-value-and-gate foundation of Zen. A value
carries a reference to the schema it *claims* to be — typed enough to be
challenged at any boundary, dynamic enough to be built at runtime from a schema
that was discovered rather than compiled. Exactly one gate guards every
boundary. This document records the public surface, the ownership and threading
model and why, the wire format, the version policy, and the seams deliberately
left open for later.

The library hard-codes **no** application message types and **no** policies. It
provides machinery — schema, value, gate, registry, serialization — and nothing
domain-specific. Everything in `tests/` named `Move`, `SetColor`, `PlayerState`,
`ReloadPolicy`, … is a fixture, never part of the library.

---

## The spine (operational invariants)

1. **One gate, every boundary.** There is a single structural validator,
   `detail::validate_into` (in `src/gate.cpp`). The live-message path
   (`zen::admit(Value, Schema)`) and the persisted-bytes path
   (`zen::admit(Unverified, …)`) both reach it and only it to decide
   conformance. `gate_invocations()` exposes a process counter so a test can
   prove both paths advance the same gate (see `tests/test_integration.cpp`,
   "one gate").

2. **Untrusted until proven.** Deserialization yields an `Unverified`, a type
   with no field accessors. The *only* way to obtain a usable `Value` from bytes
   is `admit(Unverified, …)`, which returns the `Value` only on success. It is
   not possible to forget to validate: there is no API that turns `Unverified`
   into `Value` without passing the gate.

3. **The kernel holds the grammar, not the answers.** No application type lives
   in the library.

4. **Published schemas are immutable.** A registered `(name, version)` is frozen.
   You never mutate it; you register a new version. `Registry` enforces this:
   identical re-registration is a no-op, a same-key/different-shape registration
   throws `SchemaConflict`.

---

## Public surface

Headers live under `include/zen/`. Prefer including the specific one;
`<zen/zen.hpp>` is an umbrella.

| Header | Provides |
|---|---|
| `kind.hpp` | `Kind` (the 7 primitive kinds), `name_of(Kind)` |
| `schema.hpp` | `TypeRef`, `Field`, `Schema`, `SchemaBuilder`, `make_schema`, `type_of/type_message/type_list`, `ContentId`, `same_identity` |
| `value.hpp` | `Cell`, `Value`, `Bytes`, `construct_blind`, `CellSource` |
| `admission.hpp` | `ErrorKind`, `Error`, `Admission` |
| `gate.hpp` | `admit(Value, Schema, Report)`, `diagnose`, `Report`, `gate_invocations` |
| `registry.hpp` | `Registry`, `SchemaConflict` |
| `serialize.hpp` | `serialize(Value)`, `parse`, `Unverified`, `admit(Unverified, …)` |

The surface is deliberately narrow. A foundational library earns trust by being
unsurprising.

### Primitive kinds (permanent)

`Int` (i64), `Float` (IEEE-754 binary64), `Text` (UTF-8), `Bool`, `Bytes`
(opaque), `Message` (nested value of a named schema), `List` (homogeneous
sequence of one element type). The set is intentionally small; every kind is a
forever commitment. `List` elements are described by a recursive `TypeRef`, so
`List<List<Int>>` and `List<Message>` are expressible without new kinds.

---

## Schema and content identity

A `Schema` is an ordered set of `Field`s plus a `name`, a `version`, and a
**content id**: a 64-bit FNV-1a hash folded over the schema's normalized
structure (name, version, then for each field in declared order: name,
required flag, and type — Message types fold in the *precomputed* content id of
their nested schema, so identity is a shallow, cheap recursion even for deep
trees). Field **declaration order is part of identity**.

Why a content id:

- The gate's identity question ("does your claim match this door") becomes a
  single integer compare instead of a recursive structural walk on the hot path.
- Two *separately built* but structurally identical schemas have the *same* id —
  exactly what we want, since a value built against one should pass the other's
  door.
- It detects shape drift: two schemas sharing a `(name, version)` but differing
  in shape have different ids, which the registry and the wire reader both catch.

The FNV-1a algorithm, seed, and prime are **frozen** (`src/detail/hash.hpp`): a
content id appears in the wire header, so changing the hash would silently
reinterpret every persisted value's identity. It is not cryptographic — it
identifies schemas, it does not authenticate bytes.

Schemas are immutable once constructed (`SchemaBuilder::build()` /
`make_schema`); the constructor rejects malformed `TypeRef`s (a Message with no
schema, a List with no element, a primitive carrying either), empty field names,
and duplicate field names. Schemas are assumed acyclic; they are built
bottom-up from already-built children, so a cycle cannot form.

---

## Value model and ownership

A `Value` always carries a non-null `shared_ptr<const Schema>` — there is no
shapeless value. Field data is stored **positionally**, in a vector of
`optional<Cell>` aligned 1:1 with `schema().fields()`; `set`/`get` resolve a
field name to its slot by a linear scan (field counts are small, and this keeps
the model index-free and the hot path allocation-free). A value can only hold
fields its schema declares; `set` on an undeclared field throws.

A `Cell` holds exactly one of the seven kinds in a `std::variant`. `Message` is
held as `shared_ptr<Value>` and `List` as `vector<Cell>`, giving a finite,
recursive type. Consequences:

- **Moves are cheap** (pointer/variant moves); the hot validation path performs
  no copies.
- **Copying a `Value` shares its nested sub-values** (the `shared_ptr`). Values
  are intended to be treated as immutable after they pass the gate; do not mutate
  a sub-value that may be shared. (A future deep-clone is a possible addition if
  mutable value trees are ever needed.)

Schemas are owned by whoever builds them and, canonically, by the `Registry`.
Values hold a `shared_ptr<const Schema>`, so a value's schema can never dangle.

### Blind construction

`construct_blind(schema, source)` walks a runtime-discovered schema's fields in
order and fills each from a caller-supplied `source` (which may return
`nullopt` to leave an optional field absent). This is the in-engine console's
path — building a value for a type nothing was compiled to know — and it is a
first-class entry point, not an afterthought.

---

## The gate

`admit(Value claimant, const Schema& door, Report)` asks two questions with one
function:

1. **Identity** — `claimant.schema().content_id() == door.content_id()`? A
   mismatch is fatal (a different shape cannot be structurally compared to this
   door) and returns immediately with `SchemaMismatch`.
2. **Structure** — is the claimant a well-formed instance, recursing into nested
   messages (checking the nested value's claimed identity, then its fields) and
   into list elements (each against the element `TypeRef`)?

`admit` takes the candidate **by value** and, on success, moves it out as the
trusted result — the same value, now blessed. On failure it yields a structured
`Admission` carrying one or more `Error`s. Each `Error` has a machine-readable
`ErrorKind`, a dotted/indexed `path` to the offense (`members[0].name`),
`expected`, and `actual`, plus a one-line `message()` rendering — enough for the
console to point exactly at the fault.

- **`Report::FirstError`** (default, hot path) stops at the first offense.
- **`Report::Full`** collects every offense. `diagnose(const Value&, Schema)` is
  the non-consuming full-report form.

Both modes are the *same* validator with a `collect_all` flag; there is no second
validation code path.

### Version policy

Version handling is **reject-by-default**. A claimant is admitted only if its
content id equals the door's; since the content id folds in the version, a
different version is a different door and is refused. There is **no** compatibility
or migration scheme, by design. The documented seam for one: a future
`admit` overload (or a `CompatibilityPolicy` passed to the gate) could, on an
identity mismatch, consult a registered relation between `(name, vA)` and
`(name, vB)` before the structural walk. Nothing in the current API forecloses
adding that; the identity check is the single, isolated place it would hook in.

---

## Registry — threading and immutability

`Registry` is the kernel's grammar store. It owns schemas as the canonical
`shared_ptr<const Schema>` that values reference. It supports registering schemas
discovered at runtime (the DLL case), lookup by `(name, version)`, and
idempotent re-registration.

**Concurrency: copy-on-write.** The map is an immutable snapshot. A reader takes
a shared lock only long enough to copy the snapshot `shared_ptr`, then traverses
that forever-immutable map with no lock held. A writer takes an exclusive lock,
builds a new map with the addition, and swaps it in; existing reader snapshots
keep the old map alive, untouched. Registration is expected to be rare; lookups
ride the hot bus path.

Ideally the snapshot pointer would be a `std::atomic<std::shared_ptr<const Map>>`
for wait-free reads. **GCC 11 (the available toolchain) lacks that
specialization** (it landed in GCC 12), so the snapshot load/store is guarded by
a `std::shared_mutex` whose critical section is a single `shared_ptr` copy —
O(1), no traversal under lock. On GCC 12+ the field can become
`atomic<shared_ptr>` with **no change to any caller**; the guarantee (immutable
snapshots, swap-on-write, traversal off-lock) is identical.

**Immutability.** Re-registering identical content returns the existing canonical
schema (`inserted == false`). Re-registering a `(name, version)` with different
content throws `SchemaConflict`. A new *version* coexists with the old.

---

## Serialization — the persistence boundary

Zen owns its wire format end to end. **No third-party serializer** is used:
their type models are foreign to Zen's and would reintroduce the
translation-loss boundary this library exists to remove. `src/detail/json.*` and
`src/detail/base64.*` are the only code that touches raw external bytes, and they
are written to be **total**: every input, hostile or truncated, yields a parse
tree or a clean error — never a crash, an overread, or unbounded recursion.

### Envelope (v1, JSON text)

```json
{
  "zen": 1,
  "schema": "PlayerState",
  "version": 1,
  "content_id": "0x4d2f...e91",
  "fields": { "hp": "30", "name": "Ami" }
}
```

- `zen` — envelope format version. Readers reject envelopes they do not
  understand. Bumped only if the envelope shape (not a payload schema) changes.
- `schema`, `version` — the claimed identity; the registry resolution key.
- `content_id` — the door-identity / integrity hash (hex). When present and it
  disagrees with the door, the bytes are refused (`SchemaMismatch`): they were
  written against a different shape of this name and version.
- `fields` — the payload. Only present fields are written; absent (optional)
  fields are simply omitted.

A text format was chosen for v1 for inspectability and console use. A compact
binary format is a documented future optimization; the envelope's `zen` version
field is where a format negotiation would live.

### Encoding (lossless-explicit)

| Kind | On the wire |
|---|---|
| `Int` | JSON **string** of the decimal i64 (`"30"`) — avoids the 2^53 precision loss of a JSON number |
| `Float` | JSON **number** (shortest round-trip via `to_chars`); the non-finite values are the string tokens `"NaN"`, `"Infinity"`, `"-Infinity"` |
| `Text` | JSON string (UTF-8, validated on read) |
| `Bool` | JSON `true`/`false` |
| `Bytes` | JSON string, base64 (RFC 4648, strict decode) |
| `Message` | a **bare** JSON object of its fields — no per-nested header; the parent field's declared schema drives decoding |
| `List` | JSON array of encoded elements |

Because a JSON string can be an `Int`, `Text`, `Bytes`, or a `Float` token, the
**decoder is schema-guided**: it decodes each node as the door's declared kind.
This is unavoidable given lossless encoding and is what makes the boundary safe —
untrusted bytes are coerced strictly to the shapes the door declares.

Parser hardening: nesting deeper than 64 is rejected (bounds stack on
adversarial `[[[[…`); duplicate object keys are rejected (no ambiguity);
unescaped control characters in strings are rejected; `\uXXXX` with surrogate
pairs is handled; base64 and UTF-8 are validated strictly.

### `parse` → `Unverified` → `admit`

`parse(bytes)` is `noexcept` and total. It extracts the claim (`schema`,
`version`, optional `content_id`) and keeps the raw `fields` tree. Malformed
bytes produce an `Unverified` whose `well_formed()` is false and whose `admit`
refuses with `MalformedBytes`. `Unverified` exposes only its *claim*
(`claimed_name`, `claimed_version`) — no payload accessors.

`admit(Unverified, door)` (door taken by `shared_ptr<const Schema>` so the
resulting `Value`'s schema cannot dangle) and `admit(Unverified, Registry)`
(resolve the claim; unknown → `UnknownSchema`):

1. Check the claim's identity against the door (name, version, and `content_id`
   if present).
2. **Decode** the untrusted payload under the door's shape, recording byte-level
   faithfulness errors (bad base64, non-integer `Int` string, invalid UTF-8, a
   JSON shape that cannot be the declared kind) into a candidate `Value`.
3. Run the **single** structural validator (`validate_into`) on the candidate —
   the same one the live bus path uses.
4. Admit iff no decode error and no structural error; otherwise refuse.

**On "one gate" and the decode step.** Decode is *deserialization*, not a rival
validator: it answers "can these bytes form a value of this shape," a question
the live path (whose `Value`s are already typed) never faces. The authoritative
*conformance* judgment — required fields, kind matches, nested identity,
recursion — is made solely by `validate_into`, which the live path uses in its
entirety. `validate_into` re-checks types on the decoded candidate, so it is
complete on its own; decode is belt-and-suspenders plus byte-level diagnostics.

**Threat model.** The hostile input is *bytes*, not in-memory `Value`s (those are
your own). Schema-guided decode + the gate make deserialization sound: anything
that survives is, by construction, a valid instance of the door's schema. The
fuzz suite (`tests/test_fuzz.cpp`) feeds ~120k random, JSON-shaped, bit-flipped,
truncated, and pathologically nested inputs and asserts no crash and that every
admitted value re-validates clean — run green under ASan/UBSan.

---

## Future seams (designed for, not built)

- **Codegen marriage.** A build-time generator should later emit, from one
  schema definition, *both* a compiled C++ struct (zero-overhead static path)
  *and* this runtime `Schema`. Nothing here assumes schemas exist only at
  runtime: `Schema`/`Value` are ordinary types a generator can emit and
  populate, and content identity means a generated struct and a runtime value
  share a door. The generator would target the same `SchemaBuilder`/`Value` API
  the tests use.

- **Schema-as-value (reflection).** The schema model is plain data
  (`name`, `version`, ordered `Field`s with `TypeRef`s). It can be described by a
  Zen schema and represented *as* a `Value`, letting the console introspect the
  whole system, not just messages. Nothing here precludes a "schema of schemas":
  the value model is expressive enough (Text, Int, Bool, List, nested Message) to
  carry a `Schema`'s structure, and `ContentId` gives such reflected schemas a
  stable identity.

- **Behavioral contracts (kept faith).** `admit` checks *shape*, not
  *faithfulness*: a Shard that declares one policy and behaves against it passes
  shape validation cleanly. This is a known, accepted gap. The place a
  behavioral-contract check would sit is the boundary itself — after structural
  `admit` succeeds, before the value is acted on — e.g. an optional
  `Contract`/predicate keyed by schema identity, consulted by the bus once the
  gate has admitted a value. The gate's structured `Admission` (it yields the
  trusted `Value`, not a bool) is the hook: a contract layer receives exactly the
  admitted value to judge behavior over.

---

## Smaller decisions on record

- **A `Value` is never default-constructible / never shapeless.** It always
  carries a schema. This makes "a value that cannot say what it is" unrepresentable
  and pushes the only untyped state into `Unverified`.
- **`admit` consumes the candidate** (by value) and re-emits it on success. This
  gives the live path the same "you receive a *trusted* value" ergonomics as the
  persisted path and avoids aliasing a caller's value behind an `Admission`.
- **Nested messages carry no per-nested header on the wire.** The root envelope's
  schema fully describes the tree; nested values are decoded under their parent
  field's declared schema. This is compact and unambiguous.
- **`content_id` in the wire header is advisory-but-enforced.** If present it must
  match the door; its hash algorithm is frozen. The resolvable identity is
  `(name, version)`; the hash is the integrity/drift check.
- **clang-tidy config is provided but not run in CI here** — the available WSL
  toolchain has no `clang-tidy`. The code is written to the configured policy;
  enabling the check is a drop-in once the tool is present.

---

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build

# sanitized
cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DZEN_SANITIZE=ON
cmake --build build-san
ctest --test-dir build-san
```

C++20, builds clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Werror`. Verified with GCC 11.4 (Ubuntu 22.04 via WSL);
targets the C++20 floor and avoids features that require GCC 12+. The suite is
green in Debug and under `-fsanitize=address,undefined`.
