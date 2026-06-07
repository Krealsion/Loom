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
| `serialize.hpp` | native binary `serialize(Value)` / `parse`; `Unverified`; `admit(Unverified, …)`; compat JSON `zen::compat::serialize` / `zen::compat::parse` |

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
translation-loss boundary this library exists to remove. There are two codecs,
both Zen's own:

- **Native** — a canonical binary format (`serialize` / `parse`). Compact,
  positional, schema-guided, and **canonical**: a given `Value` has exactly one
  encoding, so native bytes are content-addressable.
- **Compat** — the original self-describing JSON text (`zen::compat::serialize` /
  `zen::compat::parse`). Inspectable for debugging and tooling, but larger and
  not byte-canonical. Demoted from native; retained, not deleted.

Both produce/consume the same `Unverified` and reach the same gate. The format
swap touched only this layer — the gate, schema, value, identity, and the
`parse → Unverified → admit` contract are unchanged.

`src/detail/binary.*` (native), `src/detail/json.*` and `src/detail/base64.*`
(compat) are the only code that touches raw external bytes. All are **total**:
every input — hostile, truncated, random — yields either a parse result or a
clean error, never a crash, an overread, an over-allocation, or unbounded
recursion.

### Native envelope (binary, little-endian)

A self-describing header a reader can challenge without a schema, then the body:

```
5A 4E                 magic "ZN"
01                    format version (u8; unknown → reject)
LL LL                 schema-name length (u16) + that many UTF-8 bytes (validated)
VV VV VV VV           schema version (u32)
CC CC CC CC CC CC CC CC   content_id (u64)  — MANDATORY
…body…                the value (kept raw in Unverified; decoded only in admit)
```

`content_id` is **mandatory** in native (it is optional in compat JSON).
Positional decode has no field-name safety net, so the content id is the only
pre-decode guard against a positional misread — a value written against a
different shape of the same name/version. A mismatch is `SchemaMismatch`, exactly
as in JSON. A header that cannot supply all eight bytes is `MalformedBytes`.

### Native body (positional, schema-guided, per message incl. nested)

A **presence bitmask** of `ceil(num_fields / 8)` bytes (bit *i*, LSB-first, set
iff field *i* in declared order is present; padding bits beyond the last field
must be zero), then each *present* field, in declared order:

| Kind | Encoding |
|---|---|
| `Int` | zigzag → **minimal** unsigned LEB128 (non-minimal / overlong rejected) |
| `Float` | 8 bytes IEEE-754 binary64 LE; NaN normalized to one canonical quiet-NaN; −0.0 preserved |
| `Text` | varint length + UTF-8 bytes (length bounds-checked; UTF-8 validated) |
| `Bytes` | varint length + raw bytes (length bounds-checked) |
| `Bool` | one byte, `0x00` or `0x01` only |
| `Message` | the nested body inline (its own bitmask + fields) — no per-nested header |
| `List` | varint count + that many encoded elements of the element type |

Required-but-absent fields are not special-cased by the decoder — a clear bit
just means "absent", and `validate_into` reports any missing required field as
`MissingField`, reusing the live-path diagnostic.

### Canonicality (a `Value` has exactly one native encoding)

Fields in declared order; deterministic presence bitmask with zero padding bits;
minimal varints; `Bool ∈ {0,1}`; NaN normalized; −0.0 preserved; no padding or
reserved slack. Result: **byte-identity ≡ value-identity**, so native bytes are
content-addressable. The reader enforces this in both directions — it *rejects*
non-canonical encodings (non-minimal varints, out-of-range bool bytes,
non-canonical NaN payloads, set padding bits, trailing bytes) — so every accepted
byte string is the unique encoding of its value. Tested in `tests/test_serialize.cpp`.

### Totality, caps, and safety

`parse` is `noexcept` and total; it reads only the header and keeps the body
opaque until `admit` supplies the door. Every length and count is bounds-checked
against the remaining input *before* any read or allocation — a length field can
never size an allocation beyond what remains. Named caps:

- `kMaxBinaryDepth = 64` — nesting depth (note: native nesting depth is bounded by
  the *door's* schema, which is trusted; the cap guards against a pathologically
  deep schema, and against deep list-of-list bodies).
- `kMaxListCount = 1<<20` — elements in one list (guards the one zero-byte element
  case: a list of zero-field messages, which a remaining-bytes check cannot bound).
- `kMaxFieldBytes = 1<<28` — bytes in one `Text`/`Bytes` (secondary to the
  remaining-input check).

The native decoder is **fatal on desync**: a positional misread cannot be
resynced (there are no field names), so a low-level read failure yields one
precise error and stops — it never invents spurious follow-on errors. The compat
JSON decoder, having field names, still collects multiple errors in `Report::Full`.

### `parse` → `Unverified` → `admit` (one gate)

`parse(bytes)` extracts the claim (`schema`, `version`, `content_id`) and holds
the still-opaque body. Malformed input → `Unverified` with `well_formed() == false`
and an `admit` that refuses with `MalformedBytes`. `Unverified` exposes only its
*claim* (`claimed_name`, `claimed_version`) — no payload accessors.

`admit(Unverified, door)` (door by `shared_ptr<const Schema>` so the resulting
`Value`'s schema cannot dangle) and `admit(Unverified, Registry)` (resolve the
claim; unknown → `UnknownSchema`) both:

1. Check the claim's identity against the door (name, version, content id).
2. **Decode** the opaque body under the door's shape into a candidate `Value`,
   dispatching on the format tag (native binary or compat JSON).
3. Run the **single** structural validator `validate_into` on the candidate — the
   same one the live bus path uses. `gate_invocations()` proves both formats and
   the live path funnel through it (`tests/test_compat.cpp`, `tests/test_integration.cpp`).
4. Admit iff decode and structure are both clean; otherwise refuse.

A fatal binary desync skips step 3 (there is no coherent candidate to judge) and
returns the precise decode error directly; this is byte-level rejection,
analogous to a malformed-envelope rejection in `parse`, not a second conformance
authority.

### Strict core — no partial acceptance

A decoder produces a value that is an *exact* instance of the door's schema, or a
precise structured `Error`. There is no warnings channel on `Admission`, no
silent drop, no best-effort. In particular the compat JSON decoder now **rejects
any field the door does not declare** (`ErrorKind::UnknownField`) instead of
silently dropping it — strictness applies in every format. (Native binary cannot
express an extra field, so this is structurally impossible there.) Graceful
degradation is deliberately a *consumer* concern, handled over the bus, not a
core feature: the core's job is a clean yes/no with a reason.

**Threat model.** The hostile input is *bytes*, not in-memory `Value`s (those are
your own). Schema-guided decode + the gate make deserialization sound: anything
that survives is, by construction, a valid instance of the door's schema. The
fuzz suite (`tests/test_fuzz.cpp`) feeds random, valid-header-plus-garbage,
bit-flipped, truncated, length-lying, huge-count, non-minimal-varint, and deeply
nested inputs to the native decoder (plus a compat JSON pass) and asserts no
crash, no over-read/over-allocation, and that every admitted value re-validates
clean — run green under ASan/UBSan.

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

- **Migration chain (version graph).** Cross-version reads (admitting `v2` bytes
  against a `v3` door via a registered relation) are still being designed and are
  not built. The native header carries exactly what such a system keys on — the
  schema `version` and the `content_id` — readable before the body is decoded. The
  current policy is reject-by-default on any identity mismatch; the isolated place
  a migration step would hook in is that identity check in `admit_against`, before
  the structural walk. Nothing in the binary layout forecloses it: a migrator
  would resolve `(name, claimed_version, content_id) → door` and transcode the
  decoded value, then submit it to the same `validate_into`.

---

## Smaller decisions on record

- **A `Value` is never default-constructible / never shapeless.** It always
  carries a schema. This makes "a value that cannot say what it is" unrepresentable
  and pushes the only untyped state into `Unverified`.
- **`admit` consumes the candidate** (by value) and re-emits it on success. This
  gives the live path the same "you receive a *trusted* value" ergonomics as the
  persisted path and avoids aliasing a caller's value behind an `Admission`.
- **Nested messages carry no per-nested header on either wire.** The top-level
  header fully describes the tree; nested values are decoded under their parent
  field's declared schema. Compact and unambiguous in both formats.
- **`content_id` is mandatory in native, optional in compat.** Native is
  positional and untagged, so the content id is the only pre-decode guard against
  a positional misread — there is no field-name safety net — hence mandatory. JSON
  is self-describing by field name, so it can tolerate the id's absence. In both,
  a *present* id that disagrees with the door is `SchemaMismatch`; the hash
  algorithm is frozen. The resolvable identity is `(name, version)`; the hash is
  the integrity/drift check.
- **Native byte buffer is `std::string`.** The public API is already
  `std::string` / `std::string_view`; using it throughout means one buffer type
  and zero conversions at the boundary. `std::string` holds arbitrary bytes
  (embedded NULs included).
- **Float NaN is normalized on encode and non-canonical NaN is rejected on
  decode.** This keeps the format content-addressable (every accepted byte string
  is the unique encoding of its value), consistent with rejecting non-minimal
  varints and out-of-range bools. −0.0 is a distinct value and round-trips.
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
