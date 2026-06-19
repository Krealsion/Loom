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

**Two comparisons, named for what they are.** `same_identity(a, b)` is *true
identity*: `name == && version == && content_id ==`. It cannot be fooled by a
hash collision, nor by an unregistered schema claiming a taken `(name, version)`
with a different shape — so reaching for it is always correct. Bare
`a.content_id() == b.content_id()` is the narrower **integrity/drift** check: a
single-integer comparison the gate, the wire reader (`admit_against`), and the
registry use *inline* on the hot path, where the `(name, version)` is already
established upstream by door selection. Those inline checks are unchanged; the
helper carries the full identity so callers reaching for "identity" get it.

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

## The Switchboard (the first live boundary)

`zen-switchboard` is a separate library that links `zen-core` and builds the
first place where a value actually *crosses* a boundary: an in-process message
bus. It reimplements no validation, schema, or serialization logic — it routes
`Value`s and calls `admit()`. The whole point of the core (one gate, untrusted
until proven) now does real work guarding live delivery.

### In-memory delivery, gated at the recipient's door

Delivery is in-memory: the bus moves `Value` payloads between in-process Shards.
Each delivery is validated by `admit(Value, recipient_accept_schema)` — the live
path of zen-core's *one* validator. The gate runs **at delivery, against the
recipient's accept-schema**: each Shard's boundary is its own door, so a publish
to N accepters is N independent boundary crossings. The bus writes no validator;
`gate_invocations()` proves a live delivery advances the same counter the
persistence (bytes) path does (`tests/test_switchboard.cpp`).

A handler is invoked **only** with an already-gated payload. A refused delivery
never reaches the handler — it is recorded and surfaced to observers instead.

### The `Message` envelope

A payload `Value` (its own schema is its routing shape) plus routing metadata:
`sender` (a `ShardId`), an optional `reply_to` `ShardId`, and an optional opaque
`correlation` token. Replies are ordinary sends — a handler sends to `reply_to`.
Synchronous request-and-await is a deliberate seam; the envelope already carries
what it needs.

### Addressing: accept-sets, directed, and publish

A Shard declares the schemas it accepts (its accept-set, keyed by
`(name, version)`). It is reachable two ways:

- `send(ShardId, Message)` — directed. Refused unless the target's accept-set
  includes the payload's `(name, version)` **and** the payload passes the gate.
- `publish(Message)` — by shape. Enqueues one delivery for every alive Shard
  whose accept-set includes the payload's shape, in registration order; returns
  the recipient count (`0` is legal, not an error).

The bus owns a `zen::Registry` and registers every accept- and state-schema in
it, so all Shards must agree on what a given `(name, version)` means — a
disagreement is a `zen::SchemaConflict` at registration.

### Single-threaded FIFO dispatch; the reentrancy guarantee

`send`/`publish` **enqueue**; `pump()` dequeues and delivers until the queue
drains. Dispatch is single-threaded and FIFO, so ordering is deterministic. A
handler that sends during handling enqueues a *later* delivery — never a nested
one: a reentrancy guard makes a reentrant `pump()` a no-op, so delivery depth
never exceeds one. Tests assert both the deterministic order and the
non-reentrancy (a shared depth counter that never exceeds 1).

Because `send` enqueues, a delivery's fate is read *after* `pump()`:
`send` returns a `Ticket`, and `outcome(Ticket)` yields
`Delivered` / `Refused{Refusal}` (or `Pending` before the pump). Live taps see
the same outcomes as they happen.

### Refusals are structured and observable

A delivery either conforms or is refused — no partial or best-effort delivery,
no silent drop. A `Refusal` distinguishes bus-level routing reasons
(`NoSuchTarget`, `TargetUnavailable`, `NotAccepted`) from a gate refusal
(`GateRefused`, which carries the zen-core `Error` with its field path and
expected/actual). The two never blur: routing is the bus's, conformance is the
gate's.

### Observation (the IDE-as-a-node seed)

An observer/tap registers via `add_observer` and is told of every delivery
(`Delivered`/`Refused`) and every lifecycle transition (`Died`/`Revived`)
through one `BusEvent` hook — without being a recipient. It is cheap and present:
the seed the self-documenting console grows from. (`BusEvent::payload` is valid
only during the callback; taps copy out the durable fields.)

### Lifecycle / reload — orchestrated, mechanics reused

The Switchboard owns the death→revive cycle and reinvents nothing:

- `snapshot_bytes(id)` serializes the Shard's `snapshot()` with native `serialize`.
- `kill(id)` marks it dead (it stops receiving deliveries) and emits `Died`.
- `reload(id, bytes)` runs `parse` → `admit(Unverified, state_schema)` — the
  self-set lock from the first nucleus. On success it calls `revive(state)` and
  refreshes last-known-good. On refusal, the Shard's `policy()` decides.
- `swap_state(id, bytes)` is the **intentional** sibling of `reload`: same gate
  (`parse` → `admit(Unverified, state_schema)`), same `revive` + last-known-good
  refresh, but it spends **no budget** and offers **no** last-known-good fallback —
  a gate refusal is a clean refusal. Emits `Revived`/`Refused`.

**Intentional swap ≠ crash revival (only crashes spend the budget).** `reload` is
the crash-revival path: it is *budgeted* — it reads the policy's `max_reloads` and
decrements a per-Shard counter, so a Shard that crash-thrashes cannot revive
forever, and on a refused candidate the policy may fall back to last-known-good.
`swap_state` is the deliberate code-swap path (what the kernel's `reload_from`
calls): no budget, no fallback. Sharing one counter would be backwards — a Shard
that spent its crash-revival allowance could not be hot-swapped to *fixed* code,
and every deliberate swap would draw down the very budget meant to stop crash
loops. The two operations are therefore separate methods, not a flag.

The **only** schema the bus hard-codes is its lifecycle-policy grammar —
`LifecyclePolicy v1 { max_reloads: Int, revive_from_last_good: Bool }`, exposed
as `lifecycle_policy_schema()`. The bus validates `policy()` against it and reads
only those two fields; everything else about a Shard is opaque to it.
Last-known-good is the last successfully-admitted snapshot (seeded at
registration by gating the Shard's initial snapshot, so a Shard is born valid).

### Shard contract (a frozen ABI surface)

`Shard` is an abstract base with exactly five methods — `accepted_schemas`,
`handle`, `snapshot`, `policy`, `revive` — kept minimal because it is a future
ABI surface. It is designed to survive a move to per-Shard mailboxes and
multi-threaded dispatch unchanged: a handler still *receives* a gated message and
*sends* (which enqueues); only `pump`'s internals would change. The bus owns
Shards (`register_shard(std::unique_ptr<Shard>)`); a non-owning `shard(ShardId)`
accessor serves queries and tests. `handle` sends through an abstract `Bus`
interface (which `Switchboard` implements), so the *same* Shard works whether it
is compiled in or loaded from a library — the kernel below relies on this.

---

## The Kernel (DLL loading · the C ABI · hot-reload)

`zen-kernel` is the OS/entry-point layer: it loads Shards from dynamic libraries
and hosts them on a Switchboard. It reimplements no validation, routing,
serialization, or lifecycle — it links `zen-core` + `zen-switchboard` and adds
only the library boundary.

### Bytes are the boundary currency

The hard, permanent part is the seam. Only **C** crosses it: opaque instance
handles, plain function pointers, `const uint8_t*` + `size_t` buffers, and
integer status codes — no C++ types, no STL, no `std::any`, no exceptions. Every
Zen value/schema/message a library hands back crosses as **serialized bytes**,
and the host **re-admits those bytes through zen-core's gate** before trusting
them. So the DLL boundary is just another boundary the one gate guards, and
*untrusted-until-proven extends to loaded code for free*: a buggy or hostile
library can no more inject an unvalidated value than a hostile byte-stream from
storage can. A test proves a value crossing the DLL seam advances the same
`gate_invocations()` counter the persistence path does, in both directions
(a delivery *to* the library Shard and a message *emitted by* it).

This also supersedes a rejected prototype — a `std::any`-based service locator
whose `any_cast<T*>` carried no schema, was UB across the seam, and retained raw
library pointers that dangled on unload. Only its cross-platform `dlopen`
wrapper survived (GCC default visibility, no `__declspec`).

### The C ABI (`include/zen/kernel/abi.h`)

One exported symbol, `extern "C" const ZenShardAbi* zen_shard_abi(void)`,
returns a static descriptor:

- `uint32_t abi_version` — the ABI's own version (distinct from schema versions);
  the host rejects a descriptor whose version it does not support.
- `create` / `destroy` — construct/destroy the opaque instance.
- `describe` — emit the manifest (accepted schemas + state schema) as bytes.
- `snapshot` / `policy` — emit state / lifecycle policy as bytes.
- `revive` — receive already-host-admitted state bytes.
- `handle` — receive an already-host-gated inbound message (sender/reply_to/
  correlation + payload bytes) plus a **host callback table** (`ZenHostApi`)
  through which the Shard `send`/`publish`es by handing the host serialized
  message bytes.

**Buffer ownership (the safety property).** Library→host returns go through a
host-provided `ZenByteSink` — the library hands bytes, the host copies them
immediately into host memory, and the library allocates nothing host-visible and
frees nothing across the seam. Host→library inputs are `const ptr + len` valid
only for the call. There is therefore **no cross-allocator free and no host
pointer into library memory** — the prototype's "return a pointer and hope" is
gone. The only library-owned thing the host holds is the opaque instance handle.

### Schemas cross as gated values

A library's accepted schemas (and its state schema) cross as a **manifest** —
encoded *as a Value* of a fixed kernel meta-schema (`zen.Manifest` →
`zen.SchemaDesc` → `zen.Field` → `zen.TypeToken`, in `schema_codec.hpp`), so a
schema travels as ordinary bytes and is re-admitted through the gate exactly like
any other value before the host reconstructs it with `SchemaBuilder`. A type
reference is a flat, prefix-order token list, so nested Lists/Messages need no
recursive meta-schema; Message/List nested schemas are referenced by
`(name, version)` and resolved against the host registry (the manifest lists
referenced schemas first). This is the minimal **schema-as-value** precursor —
the one place that seam is lightly touched.

### Authoring stays Zen-invisible

`ZEN_EXPORT_SHARD(MyShard)` (header-only `export.hpp`) generates the descriptor
and every thunk from a clean C++ `zen::sb::Shard` subclass — the author writes
the same Shard they would compile in, plus one line, and hand-writes no thunk.
The thunks have C language linkage (matching the descriptor's pointer types),
forward to C++ template helpers, serialize Values to the host sink, rebuild a
`Bus` that forwards the Shard's `send`/`publish` across the callback table, and
**never let a C++ exception cross the seam** (all caught, turned into status
codes).

### The host adapter

`HostAdapter` (host-side) **implements the existing `Shard` interface** by
translating each method to its C thunk: serialize arguments to bytes, call
across, and on the way back `parse` + `admit` through the gate (manifest,
snapshot, policy, and every emitted message). From the Switchboard's side it is
simply a Shard — a loaded Shard mounts, routes, and reloads exactly like a native
one. The host callbacks resolve an emitted message's schema against the
Switchboard's system-wide registry (`resolve_schema`) and gate it before routing.

### Teardown and hot-reload order

The adapter owns the instance and destroys it (`abi->destroy`) in its destructor;
the Kernel owns the library handle and closes it **only after** the adapter is
gone. Unload is therefore: `unregister_shard` (stop delivery, take ownership) →
destroy the adapter (→ destroy the instance, library still open) → `close` — no
call ever lands in a closed library (clean under ASan).

`reload_from(name, new_path)` snapshots the live Shard to **host-owned** bytes,
opens and validates the new library, then swaps `(abi, instance)` **in place
behind the same adapter and ShardId** (so senders keep their handle), destroys
the old instance while its library is still open, closes the old library, and
revives the new instance from the snapshot **through the gate**. State survives
because the snapshot bytes are host-owned, independent of either library. A new
library whose **state-schema version differs** is a **clean refusal** — the old
library keeps running. (This is exactly where the deferred migration layer will
slot in.) The revive after the swap goes through `Switchboard::swap_state`, the
**unbudgeted** intentional-swap path, so a deliberate hot-reload neither draws
down nor is blocked by the Shard's crash-revival budget (see *Intentional swap ≠
crash revival* above).

### The one switchboard change for the kernel, and a note on hosting

The kernel needed two small `Switchboard` additions: `unregister_shard` (to
destroy an adapter before closing its library) and `resolve_schema` (to gate a
library's emitted messages against the system registry). `Shard::handle` taking
the abstract `Bus` (rather than the concrete `Switchboard`) is what lets one
Shard be hosted either natively or from a `.so`.

**Crash isolation is a non-goal here:** this kernel is in-process, so a crashing
Shard takes the host down (accepted for now). The wire format already makes the
eventual process boundary cheap — in-process (fast) and out-of-process (isolated)
become the two permanent hosting modes, and a cross-boundary link is just
`serialize` at the sender and `parse` → `admit` at the receiver, with no change
to the Shard contract.

---

## The authoring layer (schema-from-struct, low ceremony)

`zen-author` (header-only, `include/zen/author/`) is the first layer whose job is
to make apparatus *disappear*. It is **pure sugar**: no new schema type, no new
value type, no second validator, no change to the gate, the wire format, the bus,
or the kernel. It emits schemas only through `SchemaBuilder` and hands `Value`s to
the same `admit`. A struct-derived `Foo v1` and a hand-built one are the *same*
`Schema` — identical content-id, shared door — proven by `tests/test_author_shape.cpp`.

### Schema-from-struct

An author writes a plain C++ struct (real members) plus one in-class line:

```cpp
struct Ping {
    std::int64_t seq;
    ZEN_SHAPE(Ping, /*version=*/1, ZEN_FIELD(seq));
};
```

`ZEN_SHAPE` adds `zen_name`, `zen_version`, and `zen_fields()` (a tuple of
`(name, &member)` entries); `ZEN_FIELD(seq)` captures the member pointer and the
name string. From that the layer derives, all through the public API:

- the runtime `Schema` (`schema_of<T>()`, built once via `SchemaBuilder`), with
  the **Kind deduced from each member's C++ type** (`type_ref_for<M>`):
  `int64_t→Int`, `double→Float`, `std::string→Text`, `bool→Bool`,
  `zen::Bytes→Bytes`, `std::vector<T>→List<T>` (recursive; a byte vector resolves
  to Bytes first), and a nested registered shape `→Message`;
- `to_value(const T&)` and `from_value<T>(const Value&)` (the latter assumes an
  already-gated value — conversion to a struct happens *only after* the gate).

The struct stays a plain aggregate; the author never touches a `Cell` or a
`set("...")`, so a field typo is a compile error, not a runtime throw.

### Explicit version (built now — migration seam #3)

The macro **requires** a version and folds it into identity: `v1::Ping` and
`v2::Ping` (same `zen_name`, different version) are distinct content-ids by
construction. There is no way to evolve a shape in place; a new version is a new
identity. Omitting the version fails to compile. The whole migration chain keys on
these stable versions, so this is the one migration seam unsafe to leave loose —
and it is built.

### Low-ceremony Shard authoring + `mount`

`ShardBase<Self, State, Accept<Shapes...>, Emit<Shapes...>>` (CRTP) derives:
`accepted_schemas()` from `Accept<…>`, `snapshot() = to_value(state_)`,
`revive() = from_value<State>`, and `policy()` from an overridable
`policy_config()`. Its `handle()` matches the gated payload to an accepted shape
by **true identity** (`same_identity`: `name`, `version`, `content_id`), converts
it, and dispatches to a **typed handler** `void on(const Ping&, Mail&)` — one per
accepted shape, so the
accept-set is named once, not a third time. `Mail` is the typed send context (it
carries the inbound envelope): `mail.reply(Pong{…})`, `mail.send(target, T)`,
`mail.publish(T)` — no `Value`/`Cell`/`Message` ceremony. `mount<Node>(bus)`
constructs, registers (the derived schemas flow into the registry as usual),
wires the self-id, and returns the `ShardId` in one call.

**Dispatch selects by true identity (`same_identity`), the same key the bus
admitted the message under — not by a bare content-id hash.** A delivered payload
has already passed the gate against the accept-set entry the Switchboard chose by
`(name, version)` (`accept_match`); `handle()` picks the handler the same way, so
`from_value<S>`'s precondition — every field present and well-typed — is
*guaranteed*, not merely probable. Selecting by `content_id()` *alone* would be a
latent **null dereference**: `content_id` is a 64-bit FNV hash, and a collision
*within one Shard's accept-set* would route a message to the wrong `on()`, whose
`from_value<S>` reads `*v.get(field)` for each of `S`'s fields — and `get()`
returns null for a field the colliding shape does not carry. `same_identity`
checks `(name, version)` as well as `content_id`, so it closes that collision —
its `content_id` term is the redundant-but-true integrity check — and that is
exactly how the door was chosen.
Because the delivered message is gated against one accept-set entry and the
handler set is the *same* `Accept<A...>`, **exactly one** handler matches; a
no-match is an internal-invariant violation, so `handle()` throws a clear
`std::logic_error` rather than silently dropping the message.

**Ceremony delta** (`examples/heartbeat.cpp` → `heartbeat_authored.cpp`): ~89 → ~58
code lines, and the hand-built schemas, the stringly-typed `set`s, and the
hand-written `snapshot`/`revive` are *gone* — same observable behavior.

### The reflection seam (what C++26 removes)

C++20 has no reflection, so "write once" is "write-once-and-a-half": the struct's
members plus the `ZEN_FIELD` list. That list is the **single seam**. Everything
downstream (Kind deduction, conversions, schema build, dispatch) consumes only the
abstract `zen_fields()` tuple, never how it was produced. Under C++26, `zen_fields()`
becomes a reflect-over-members derivation and **nothing else changes** — the swap
touches only that block.

### Reserved (documented, not built)

- **Migration transform registry.** A future registry maps `(name, vA) → (name, vB)`
  via a function, chainable so a `v1` value walks `v1→v2→v3`. The authoring layer is
  shaped so such a transform naturally **consumes and produces the typed structs**
  (`Player_v2 migrate(const Player_v1&)`) and is **keyed by content-id** (which
  already exists and is now derivable from a struct). It hooks into the *single*
  identity-mismatch decision points that already exist — `admit_against` (wire) and
  `reload_from` (kernel), both reject-by-default — which stay isolated and untouched.
- **Emit-set (enforcement reserved at `Mail`).** `Emit<Shapes...>` is declared
  alongside `Accept<…>` and surfaced as `emitted_schemas()`, **informational and
  unenforced** for now. `Mail::send`/`reply`/`publish` are the *sole* outbound path
  for an authored Shard, so **`Mail` is the single reserved chokepoint** where
  emit-enforcement (a sent `T` must be in `Emit<...>`) would later sit. It stays
  off **with intent**: a Shard's emit-set is not yet known to be statically
  enumerable (a router/forwarder may emit shapes chosen at runtime), so the
  substrate must not commit to it. The *declaration* is kept honest by test
  (`tests/test_author_shard.cpp` pins that each Shard's observed emits equal its
  `emitted_schemas()`) meanwhile. Completing the silhouette later (gating against
  the declared contract, drawing the bus wiring graph, feeding a dependency-mapper)
  needs *no* authoring change. **B1 (capabilities) closes the in-process half of
  this** — see below.

---

## Capabilities (B1): the in-process grant model

Delivery is gated on message *shape*, but a Shard could send anything to anyone.
B1 closes that: a Shard's reach is a **grant** the host assigns, default nearly
empty, and the bus authorizes every Shard-originated send against it. (This is
**B1 of two**: B1 is the *message* boundary; **B2 — the next phase — is
isolation-as-enforcement**, out-of-process hosting that projects the grant onto OS
sandbox primitives at the *syscall* boundary. The grant is the one source of truth
projected onto whatever boundary the hosting mode provides.)

### The grant

A `Grant` (`include/zen/switchboard/grant.hpp`) has two parts:

- **Send-permissions (enforced in B1):** a list of `SendRule`s, each a
  `(shape-selector, target-selector)` where each selector is a specific value or
  "any" — e.g. "`Pong` to any accepter", "`LoadLibrary` to the control Shard only".
- **OS-capability flags (hard, binary):** `Network` (enforced out-of-process in B3)
  and `SpawnProcess` (reserved). B1 **does not consult** them — they govern
  instruction-level behaviour a loaded `.so` reaches directly, which only process
  isolation stops. **Filesystem is not a flag** — it is the *graduated* `FsAccess`
  level (enforced in B4); the old binary `FilesystemRead/Write` flags were removed in
  B4 so files have a single source of truth.

The default grant is **empty**: minimal authority by default. Grants flow from the
host (the root of trust) at registration, out-of-band; there is no in-band path by
which a Shard widens its own grant.

### The trust boundary: `Bus` gates, `Switchboard` is root

The split that already existed *is* the trust boundary. A handler only ever
receives a `Bus&` — and `deliver_one` now hands it a per-delivery **`ShardBus`**
bound to the handling Shard's id, *not* the concrete `Switchboard`. The `ShardBus`
stamps the authoritative sender on every message (a Shard cannot send as anyone
else) and enqueues it **gated**. `Switchboard::send`/`publish` — held only by the
host program — enqueue **ungated** root authority (test setup, the trusted host
shell). `Mail` wraps the `ShardBus`; the kernel's loaded-Shard host callbacks
route through the same `ShardBus` — so native and loaded Shards are authorized
identically, keyed by id.

### Authorization is a distinct step from the gate

At delivery, for a *gated* (Shard-originated) message and **before** the gate, the
bus looks up the **sender's** grant and checks `(target, shape)`. Denied →
`RefusalReason::CapabilityDenied`: never delivered, and **the gate is not invoked**
(`gate_invocations()` is unchanged — `tests/test_capabilities.cpp` asserts it).
This is correct: authorization ("are *you* allowed to send an `X` to *them*") is a
categorically different question from conformance ("is this a well-formed `X`"), so
it sits *around* the gate, never inside it. **"One gate" stays literally true** —
there is still exactly one conformance validator, untouched; an authorized,
well-formed message still passes it. `send`, `publish`, and replies are all
authorized the same way (uniform-gated; an implicit-reply convenience is a possible
follow-up). Denials are observable on the tap with sender, attempted shape, and
attempted target — the supervisor can *see* a Shard overreach.

### Grant ↔ Emit, and the closed emit seam

`zen::author::mount<Self>(bus)` defaults the grant to the Shard's self-declared
`Emit<E…>` (each emitted shape → any accepter) — the *trusted* in-process default.
This **closes the in-process emit-enforcement seam**: delivery now checks a real
authority, and a Shard that sends a shape outside its declared `Emit` is denied.
`mount_granted` supplies an explicit grant for an untrusted Shard, whose
declaration is not trusted. (Emit-set *as a wiring contract* — enumerating a
runtime router's emits, drawing the graph — remains the deferred seam.)

### The kernel's message door (the teeth)

The kernel registers a **control Shard** (`include/zen/kernel/control.hpp`)
accepting `LoadLibrary` / `ReloadLibrary` / `UnloadLibrary` (`ZEN_SHAPE`s) whose
handlers call the kernel's `load` / `reload_from` / `unload`. Operating the kernel
is now just sending it messages. The **load capability** — the right to send those
shapes to the control Shard — is the canonical dangerous grant: a Shard holding it
drives the kernel by message; one without it is denied at the control Shard's door
(`CapabilityDenied`), gating the single most dangerous surface in the system with
the same mechanism as everything else.

### Loaded `.so` Shards, and the B1 → B2 → B3 split

A loaded `.so` can bypass the bus and reach syscalls directly, so a restrictive
*bus* grant on it is not real containment in B1. Containment is staged: **B2**
(below) puts the Shard in its own **process** — crash isolation and memory
separation — and **B3** adds the **OS sandbox** the reserved OS-capability flags
drive (the syscall boundary). So the kernel grants loaded Shards **permissive bus
sends** in B1; the kernel *door* is demonstrated fully gated against **native**
Shards (the authored mod logic, the console's elevated instance). B1 makes the
grant *real at the message boundary* and shapes it so B2 can host out-of-process
and B3 can enforce its OS-relevant parts at the syscall boundary, each with no
rework.

---

## Isolation (B2): out-of-process hosting & crash supervision

B2 makes hosting mode a **mount choice**: a Shard can run in a child process,
indistinguishable to the bus from an in-process one, and when it crashes the host
survives, contains the blast, reloads it a bounded number of times, then
quarantines it. This is the *isolation* half of "capabilities + isolation". It is
honestly **isolation, not sandboxing** — see the status note below.

### What changed, and what deliberately did not

Nothing in the gate, the wire format, the `Value`/`Schema` model, or the
single-threaded FIFO bus changed in behavior. The only new bus surface is the pair
`send_as`/`publish_as` (root authority, added in B1's prep), which the host uses to
re-enter a child's output with its identity stamped **from the connection**. The
new code is a library (`zen-isolation`) and a child executable (`zen-shard-host`);
the bus simply hosts one more kind of `Shard`.

### The child is a byte shuttler (`zen-shard-host`)

The child reuses the **kernel C ABI** unchanged: it `dlopen`s the `.so`, gets its
`zen_shard_abi()`, and drives the same `create`/`describe`/`policy`/`snapshot`/
`revive`/`handle` thunks. It links **no zen-core** — it neither validates nor
interprets values; it only moves bytes between the `.so` and a framed socket. Its
outbound `Bus` (the `ZenHostApi`) ships the `.so`'s emitted payload bytes as `Emit`
frames; **gating happens parent-side**. The `.so` is identical to the in-process
one and does not know it is hosted out-of-process. The child's I/O is blocking —
it has nothing to do but service the parent — so all the non-blocking machinery
lives on the host side.

### Bytes are the IPC currency; one gate, host-side

Exactly as for persistence and the DLL boundary, Zen's serialized values are the
IPC currency. Every message/snapshot/policy/manifest crosses the socket as bytes
and is re-admitted **host-side through the one gate** before the host trusts it.
A child's emitted message is `parse`d, its schema `resolve`d against the system
registry, and `admit`ted — only then is it routed. Malformed or hostile bytes are
refused and dropped; they never reach a recipient and never crash the host. The
manifest crosses as the same gated **schema-as-value** descriptor the kernel
reconstructs, so the host rebuilds the child's accept-set and state schema through
`admit` + `decode_schema`.

### Sender integrity: stamped from the connection

The `Emit` frame carries **no sender field by construction** — a child has no way
to express one. The host stamps the proxy's `ShardId` (the identity of the
*connection* the bytes arrived on) via `send_as`/`publish_as`, and the bus then
authorizes that message against **that Shard's grant** at delivery, yielding
`CapabilityDenied` on a violation — identical to the in-process `ShardBus` path. A
child that wants to send as someone else cannot get it; a child granted nothing
can emit, but its emissions are denied before the gate.

### The proxy is a `Shard`; the host loop keeps the bus single-threaded

The host-side `OutOfProcessShard` *is* a `Shard` on the bus, so the Switchboard is
unchanged. `handle()` serializes the message and ships a `Deliver` frame and
**returns at once** (fire-and-continue): a slow, flooding, or hung child can never
block, stall, or OOM the host. `snapshot()`/`policy()` return **host-owned cached**
values, refreshed from the child's proactive `Snapshot` frames (the child ships a
fresh snapshot after each `handle`/`revive`), so crash recovery never needs a
blocking round-trip. The IPC `Channel` is non-blocking and **bounded** (a per-frame
cap and an outbound-backlog cap); a peer that won't drain, or a frame over the cap,
marks the channel failed — the child is contained, not tolerated. EOF is observable
and signals child death.

`IsolationHost::step()` is the whole concurrency story, single-threaded:

1. **drain IPC** — flush queued frames to each child, read its output, re-enqueue
   emitted messages (gated), refresh cached snapshots, note EOF;
2. **`pump`** — the bus delivers FIFO, including to proxies, which fire-and-continue;
3. **supervise** — detect deaths (channel EOF/failure), and drive recovery.

The bus's FIFO ordering and non-reentrancy hold exactly as before; the only
asynchrony is the *timing* of a child's reply, which arrives on a later `step`.

### Supervision: bounded reload, then quarantine

On a child's death the host marks the Shard dead and emits `Died`, then drives the
**existing lifecycle mechanics**: `reload` from the **host-owned snapshot**, which
is budgeted by the Shard's own `max_reloads`. Reload re-enters through the proxy's
`revive()`, which respawns a fresh child and ships it the last-good state. A child
that crashes again on `revive` simply dies again next `step`, spending one unit of
budget each cycle; when the budget is exhausted, `revive` is never called and the
Shard stays dead — **quarantined**, surfaced on the tap. The host process is never
the thing that dies. (Crash-revival is budgeted; intentional hot-reload remains the
unbudgeted `swap_state` path — B2 reuses both without change.)

### Honest containment status

`containment(name)` reports the truth and never claims more. In B2 it was a fixed
**"isolated (process boundary) … Not sandboxed,"** because the grant's `os_cap`
flags were inert. As of **B3** the string is *generated from what was actually
imposed* on each child, iterated per capability — e.g. **"isolated (process
boundary): crash-contained … network: contained — private user+net namespace, no
external interface … (confirmed: child netns distinct from host) … filesystem/
syscalls/resources: not yet enforced (B4+)."** Process
isolation *alone* buys crash containment and memory separation; it does **not** stop
a child from opening a socket or a file directly. Making the Network flag *absolute*
is exactly what B3 adds (below); the rest of the `os_cap` reach stays honestly
reported as not-yet-enforced. Pretending otherwise would be the one thing this layer
must not do.

---

## Isolation (B3): the OS sandbox — the network primitive + the honesty lattice

B3 turns "isolated" into "isolated **and** sandboxed" by projecting the grant's
OS-capability flags onto a real syscall-level profile applied to the child *before*
it runs untrusted code. It enforces exactly **one** flag — **Network** — and, more
importantly, builds the **permanent detection-and-honesty structure** every future
primitive plugs into. The seam B2 left is honoured exactly: hosting is already
out-of-process, the grant already carries the flags, the child is the single place
a profile installs — so B3 is additive, with **no rework** to the gate, the bus, the
wire format, or the supervision loop.

### The lattice: detect → apply → know → refuse-or-proceed

The real deliverable is not "configure a namespace" — it is that the system *always
knows whether it actually sandboxed a Shard*, on every platform, and **never claims
enforcement it did not impose**. `detect_enforcement()` (`src/isolation/sandbox.cpp`)
**probes** — it does not assume: it attempts the real unprivileged operation in a
throwaway child and observes the result, then caches a per-capability
`EnforcementReport` (each capability is "enforced by «mechanism»" or "not enforceable
here" — never a bare bool). The same mechanism the probe uses is the one enforcement
uses, so a green probe means real enforcement. An **unrecognized platform is the
floor**: zero enforceable capabilities, every requested capability fails safe. You do
not enumerate platforms; "I have no native path I understand here" produces a loud
refusal automatically.

### The network primitive (the one real enforcement)

When a grant's `Network` flag is **clear** (the default — minimal authority), the
child is launched into a **user+network namespace with no usable interface**, so
`connect()` and friends fail at the syscall level *regardless of what the child links
or `dlopen`s* — closing the exact "linked-libcurl still works" gap B2 left open. When
`Network` is **set**, the child runs with host networking (a granted capability is
real power, by design). Network is the right *first* primitive because it is **binary
and coarse**: there is no "safer network," so it has no gradient to muddy the lattice,
and "there is no interface" cannot be subtly misconfigured.

`posix_spawn` cannot unshare namespaces, so the sandboxed branch uses a native
`fork()` → `unshare(CLONE_NEWUSER|…)` → (parent writes the child's uid/gid maps) →
`execve()` (in `IsolationHost::spawn_and_handshake`), with everything the child does
between fork and exec kept async-signal-safe (raw syscalls, no allocation). The
**granted** (unsandboxed) branch keeps B2's `posix_spawn` byte-for-byte — the new,
riskier code is confined to the new capability. The child's containment is recorded
on its `Link` so crash recovery respawns it identically.

*(Implementation note, settled in B4: this host **refuses a child's self-map** of
`/proc/self/uid_map` with EPERM — the standard container constraint — so the **parent**
writes `/proc/<child>/uid_map` (the way `unshare(1)` and runtimes do), synchronised by a
small pipe handshake: the child unshares and signals, the parent maps it and releases
it, then the child builds its view and execs. The parent only writes the release byte
when the child is alive and waiting, so a forced/early child death never raises SIGPIPE.
This also hardened B3's netns entry, which had relied on a self-map that happened to work
earlier.)*

### What "network: contained" means — scope, confirmation, and preconditions

"Contained" is a precise, **structural** claim, and `containment()` states exactly it:
*no external network reachability* — there is no veth/bridge/physical interface in the
namespace, so the child cannot reach the host network, other namespaces, or the
internet, and a new outbound `connect()` fails at the route/syscall level. It is robust
because it is structural (no interface ⇒ no route), not a firewall rule. A fresh
`CLONE_NEWNET` also scopes **abstract `AF_UNIX`** sockets.

It does **not** mean "no communication," and the claim must never be read that way:

- The inherited **host-control fd** (fd 3) stays open, **by design** — that is how the
  child talks to the host. "Contained" is *no external reachability*, not *no IPC*.
- **Pathname `AF_UNIX`** sockets on the shared filesystem are filesystem-scoped, not
  netns-scoped — closed only when filesystem sandboxing lands (B4+).
- Intra-namespace **loopback**: the child is root in its *own* user namespace and could
  bring `lo` up, but that reaches only itself — inert **while there is exactly one
  process in the namespace**. The guarantee rests on "no interface bridges outward," not
  on "lo stays down."

**Inferred → verified.** "Contained" does not rest on inference (report said enforceable
+ handshake succeeded). After spawn the host **positively confirms** the child is in a
distinct network namespace, comparing `/proc/<child>/ns/net` against `/proc/self/ns/net`
(different inode ⇒ different namespace) — host-side, no protocol change. If confirmation
fails, the mount **fails safe** (refuses); only then does `containment()` say "confirmed."

**Fail-safe on a surprise failure.** Detection probes in one child; real enforcement
runs later in another, and could fail when the probe didn't (e.g. `unshare(CLONE_NEWUSER)`
EINVAL in a since-threaded process). Because entry runs in the host's fork-child *before*
`execve` — and a failed entry `_exit`s before any untrusted code loads, failing the
handshake — such a failure **refuses the mount in both strict and dev mode**. Dev-mode
relaxes only *known* gaps (a capability detected unenforceable *before* launch); it never
downgrades an *intended* enforcement that failed at the last moment.

**Preconditions for "no network" to hold:** **one process per fresh namespace; no
namespace sharing; no in-namespace process spawning.** If a future primitive shares a
namespace between children, or lets a child spawn an in-namespace helper, "no network"
silently becomes "a private loopback network *between those processes*," and this claim
must be revisited then.

### Native-only — because a boundary you don't understand lies

There is no portable sandbox-abstraction dependency. Each platform's enforcement is
implemented directly and **known specifically**, because the system is the one telling
the operator "this is contained," and that claim must be backed by enforcement *we*
understand, not a library's promise. B3 implements **Linux** (the target); an
unimplemented platform detects as zero enforceable capabilities and hits the fail-safe
refusal. The seam is shaped so a macOS/Windows backend can be added later.

### The dev-mode override (the one knob, introduced here)

Strictness is **default-on**. When the safe floor for a requested capability cannot be
imposed on this host, the mount **refuses**, loudly, naming the gap — a forgotten flag
fails *safe* (refuses), never *open* (runs unprotected silently). **Dev-mode** is the
human override: it converts those refusals into loud warnings and proceeds, with the
Shard **visibly marked uncontained** for each unenforced capability (so a WSL/CI box
lacking a primitive never blocks development while production stays protected by
default). It is a deployment-level choice — the same binary, dev box vs prod,
uninferable by code — which is exactly why it earns a knob. It gates *only* the
fail-safe refusal; there is nothing else to override, and there is deliberately no
second knob.

### Hard vs graduated capabilities (filesystem's reserved home)

**Network is a *hard* capability** — enforce-or-refuse, no middle. **Filesystem is
*graduated*** — a spectrum with a safe default and louder-as-riskier widening: none →
read-only → write-to-a-scoped-dir → write-with-no-exec-bit → write-anywhere. B3 builds
the **vocabulary** only (`FsAccess` on `Grant`, defaulting to `None`), not the
mechanism, so filesystem is not a retrofit. The unifying rule the vocabulary encodes:
the *default* of a graduated capability is its **safe** end (a forgotten filesystem
grant fails to none/scoped, never to write-anywhere), and reaching a dangerous level is
an explicit, visible act. The intended filesystem phase (B4+) is **Linux-makes-it-
hard-and-loud-but-possible**: a mount namespace with bind mounts for the scoped tree,
the no-exec bit enforced via `MS_NOEXEC`, path scoping by what is (not) bound, and
loudness scaling with the level.

### Per-capability resolution (B4-ready)

Each `Link` holds a **vector** of per-capability resolutions (capability, outcome ∈
{enforced, granted, uncontained}, confirmed, note), and `containment()` **iterates**
them — it does not hardcode one sentence. B3 proved the plural shape with one entry
(Network); **B4 added Filesystem** as exactly "a probe + an enforcement call + a
`describe_resolution` arm," and a mixed verdict (network + filesystem) now appears
verbatim in `containment()`. The *application* step is per-capability by nature (a netns
and a mount-ns view are built pre-`execve`; a cgroup would be applied post-fork) — there
is no generic "apply-all," which is why each phase adds its own. `set_dev_mode` stays
**global** ("let *known* gaps slide everywhere"); the *reporting* is per-capability,
which is what the mixed verdict needs. **B5 (Resources/cgroups) slotted in exactly that way**,
and a three-capability mixed verdict (network + filesystem + resources) now appears verbatim in
`containment()`.

### Status: built

The detection lattice, the network primitive (sandboxed `fork`+`unshare` vs granted
`posix_spawn`), positive `/proc/<pid>/ns/net` confirmation, the honest generated
per-capability `containment()`, the dev-mode knob, and the graduated `FsAccess`
vocabulary all ship and are tested in Debug and under ASan/UBSan. The isolation suite
proves the OS enforcement end-to-end (a child without the Network grant gets
`ENETUNREACH` from a real `connect()`; one with it gets `ECONNREFUSED`), proves both
detection branches (enforceable → contained-and-confirmed; injected-unavailable → strict
refuses / dev-mode proceeds visibly uncontained, never falsely claiming containment), and
proves a **forced real-entry failure refuses in both strict and dev mode** (no
run-while-claiming-contained path). Deferred to later phases: **seccomp-bpf** syscall
filtering, **cgroups** CPU/memory caps, **filesystem** enforcement (B3 ships its
`FsAccess` vocabulary; the mechanism is B4, below), and the macOS/Windows backends.

---

## Isolation (B4): the filesystem primitive — a graduated capability, mount-namespace enforced

B4 closes the highest-value remaining harm a stranger's Shard can do once the network is
shut: reach your **files** — read `~/.ssh` or a password store, destroy data, or plant an
executable. It is the **first *graduated* capability**: the grant's `FsAccess` level picks
a point on a safe→dangerous axis, defaulting to the safe end.

### The level model (allow-list, not deny-list)

The restricted view is an **allow-list** — the Shard sees only what it is granted, built by
`pivot_root`-ing into a fresh, minimal, read-only root. A deny-list (hiding sensitive paths)
fails *open* the moment you forget one; an allow-list fails *closed*. The levels:

- **None** (default) — a minimal read-only view: the dynamic-loader closure and the Shard's
  own `.so`, nothing writable, no home, no `/tmp`. A default-grant Shard cannot read your
  secrets because they are **absent from the view**, not merely hidden.
- **ReadOnly** — None plus the grant's `scoped_path` bind-mounted read-only.
- **WriteScoped** — None plus a single writable scratch `tmpfs` at `/scratch`.
- **WriteNoExec** — WriteScoped, but the scratch is `MS_NOEXEC`: the kernel refuses to
  `execve` anything written there.
- **WriteAnywhere** — the **opt-out**: the host filesystem, unrestricted. Treated like a
  *granted* capability (à la `os_cap::Network` granted) and reported honestly as *not
  contained, by grant* — real power the operator chose to give.

### The mechanism (the same fork-child window as the netns)

The fork-child enters `CLONE_NEWNS` alongside `CLONE_NEWUSER` (and `CLONE_NEWNET` when the
network is contained). After the parent writes its id maps (so it holds `CAP_SYS_ADMIN` in
the userns), the child runs a **mount plan precomputed in the parent** — raw syscalls only,
no allocation — to build the view:

1. **Make the tree private first** (`mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL)`) — the
   reverse-leak guard. Without it a new mount namespace shares mounts with the host and the
   child's mount changes can **propagate back to the host**. The #1 invisible footgun.
2. A `tmpfs` new root; the loader closure (`/usr,/lib,/lib64,/bin,/etc`) and the exe/.so
   directories bind-mounted **read-only**, made recursively read-only via
   `mount_setattr(AT_RECURSIVE, MOUNT_ATTR_RDONLY)` (kernel 5.12+; the bind-then-remount-ro
   alternative is *non-recursive* and would leave submounts writable).
3. The scratch `tmpfs` for the write levels (`MS_NOEXEC` at `WriteNoExec`).
4. **Remount the root read-only** so a Shard cannot write — or plant-and-exec — at `/`;
   only the scratch submount stays writable. (Without this the writable root tmpfs would
   leak past the read-only/noexec intent — a real gap the behavioural test caught.)
5. `pivot_root` into the new root, `chdir("/")`, detach the old root.

### Confirmation, fail-safe, dev-mode (the B3 discipline, extended)

After the handshake the host **confirms** the child is in a distinct mount namespace
(`/proc/<pid>/ns/mnt` inode differs); a failure fails safe (the mount refuses), so
"contained at level X" rests on confirmation, never inference. Detection **probes** the real
mechanism (a throwaway child that builds a minimal view); an unenforceable host refuses by
default, and `set_dev_mode(true)` converts that to a loud warning with the Shard marked
filesystem-uncontained. A *surprise* entry failure refuses in both modes.

### Honest scope caveats (in the containment string)

- `WriteNoExec` blocks **native `execve`**, not a script run by an interpreter already in the
  view (`/bin/sh` is in the loader closure).
- **PIDs are not namespaced** (B4 does no PID namespace), so a host `/proc` would reflect host
  processes — therefore `/proc` is **deliberately not mounted**.
- "Contained" is *no reach beyond the allow-list*, not "no IPC": the inherited host-control fd
  remains, by design.

### drvfs / WSL findings, and the `FsAccess`/`os_cap` cleanup

Probing confirmed `drvfs` (`/mnt/...`) directories **bind-mount read-only into the view** and
the loader + `dlopen` resolve inside the pivot-rooted view — no copy-to-`tmpfs` fallback was
needed. `FsAccess` is now the **single source of truth** for files; the redundant binary
`os_cap::FilesystemRead/Write` flags were removed (`Network`/`SpawnProcess` stay hard flags).

### Status: built

The Filesystem detection probe, the per-level mount-namespace view, `/proc/<pid>/ns/mnt`
confirmation, the honest per-level `containment()` with its caveats, and the cleanup all ship,
green in Debug and under ASan/UBSan. The OS-enforced proof passes end-to-end: a fs-probe
Shard's read of a secret outside scope, a write outside `/scratch`, and an `execve` from a
`noexec` scratch all return the OS's `ENOENT`/`EROFS`/`EACCES` — while the probe **still
emits** its result (sandbox ≠ muzzle) — and `WriteAnywhere` proves the opt-out reaches host
paths and is reported *not contained*. **Sandboxed-by-default now means network *and* a
restricted filesystem view.** Next: **B5 — cgroups**.

---

## Isolation (B5): the resource primitive — a quantitative capability, cgroup-v2 enforced

B5 closes the last threat-model harm: a Shard that **hogs resources** — allocates until the
host OOMs, pegs every core, or fork-bombs. It is the **first *quantitative* capability**:
not binary (network), not a safe→dangerous level (filesystem), but a *limit*. With network
+ filesystem + resources the mechanism ladder covers the mod-ecosystem threat model end to
end. **seccomp is a separate, later decision** (it guards a different tier — kernel-exploit
*escape*), not an assumed B6.

### The grant's resource limits and computed defaults (no knob)

`ResourceLimits` adds **memory** (bytes), **pids** (the fork-bomb stop), and **cpu_weight**;
`0` means "use the host-computed conservative default," a positive value raises it, and
`with_unlimited_resources()` is the explicit opt-out. Defaults are **computed from the host,
not a config knob** (the stinginess bar): memory = a bounded fraction of RAM (1/8) capped at
1 GiB and floored at 128 MiB so one Shard can't OOM the host; pids = 512 (room for threads,
stops a bomb); cpu_weight = 100 (fair share, not a hard quota — a quota would waste idle
cores). A forgotten/empty grant lands on the *bounded* default; unbounded is the explicit,
honestly-reported opt-out.

### cgroup-v2 mechanism (parent-applies-at-the-sync-point)

Reusing the proven fork+handshake — **no `clone3` rewrite** (the handshake already closes
the attach race: the child runs nothing real until released):

- The host **discovers its delegated base** from `/proc/self/cgroup` and, once, builds the
  hierarchy the **no-internal-processes** rule forces: create a `zen-supervisor` leaf, **drain
  the base's processes into it**, then enable `+memory +pids` on the base's
  `cgroup.subtree_control` (you can't enable controllers on a cgroup that holds processes).
  Per-Shard leaves are created **alongside** the supervisor.
- At **mount**, a per-Shard leaf is created with its limits (`memory.max`, `memory.swap.max=0`
  so swap can't escape the cap, `pids.max`). At the **sync point** (child unshared, blocked on
  "go") the parent writes the child's pid into the leaf's `cgroup.procs` — moving the whole
  subtree it execs/spawns under the limits — then maps it (if it made a userns), then releases.
  The child consumes nothing until released, so it is in the cgroup before it can.
- A Shard exceeding `memory.max` is **OOM-killed within its cgroup** by the kernel; the child
  dies and flows through the **existing** death → bounded-reload → quarantine path unchanged.
  `pids.max` makes `fork()` fail (`EAGAIN`) rather than bomb the host. The leaf is removed on
  teardown (after the process is reaped — `rmdir` needs it empty) and recreated on respawn.

### Resolution, confirmation, and the delegation reality

Resolution joins the tree: grant unlimited → **Granted** (opt-out); else enforceable →
**Enforced** (create+limit+move+confirm); else dev-mode → **Uncontained**; else **fail-safe
refuse**. Confirmation reads `/proc/<pid>/cgroup` (pid is in the leaf) and reads the limits
back; a mismatch fails safe.

**Delegation is the make-or-break, and it is invocation-dependent.** cgroup write access needs
a *delegated* subtree the user owns (on systemd, the user session slice). A process launched
outside a login session (e.g. plain `wsl bash`) lands in the **root cgroup with no delegation**
— there resource containment is *not enforceable*, and a default mount fails safe. So the host
must run inside a delegated scope; the isolation test suite is launched via
`tests/run-under-scope.sh` (`systemd-run --user --scope -p Delegate=yes`) so enforcement is
real. **Partial controllers:** on this host systemd delegates `memory` and `pids` but **not
`cpu`** — B5 enforces what is present and reports the rest honestly (it does not refuse for a
missing controller).

### Status: built

The Resources detection (establish-the-base probe), the per-Shard cgroup-v2 leaf with
memory/pids limits applied at the sync point and confirmed (pid-in-leaf + limits read back),
leaf cleanup/recreate across teardown/respawn, the resolution + dev-mode + fail-safe + the
unlimited opt-out, and the honest per-Shard `containment()` all ship, green in Debug and under
ASan/UBSan (the suite runs under a delegated scope). The OS-enforced proof passes **with its
negative control**: a memory-bomb Shard under a 64 MiB cap is **OOM-killed within its cgroup**
(the host survives and quarantines it), while the *same* allocation under a 512 MiB cap
**survives** — proving the cap, not the allocation, is the cause; and a fork-bomb is **bounded
by `pids.max`** (≤64 of its 4000 attempts). `containment()` now leaves **only syscalls**
unenforced. The mechanism ladder is **complete for the threat model**; what remains is a
**deliberate seccomp decision** (kernel-exploit-escape defense) and the **policy phases**
(provenance→grant→hosting-mode; persistent scoped storage) — mechanism done, policy next.

---

## Future seams (designed for, not built)

- **Reflection migration of the macro.** Under C++26, the `ZEN_FIELD` block in
  `ZEN_SHAPE` becomes a reflect-over-members derivation. Everything downstream
  consumes only the abstract `zen_fields()` tuple, so this is a single-seam swap
  with no change to any consumer.

- **Codegen marriage.** A build-time generator should emit, from one schema
  definition, *both* a compiled C++ struct (zero-overhead static path) *and* this
  runtime `Schema`. The authoring layer is the **first half** of this: a shape
  declared once already yields the runtime `Schema` and typed accessors, sharing a
  door with the hand-built equivalent by content-id. The remaining half (a
  zero-overhead static path sharing the same door) is reachable from here
  unchanged — `Schema`/`Value` are ordinary types a generator can emit and
  populate against the same `SchemaBuilder`/`Value` API.

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

- **Multi-threaded dispatch (per-Shard mailboxes).** The single-threaded FIFO
  loop is an implementation of the dispatcher, not part of the contract. The
  `Shard` surface (receive a gated message; send, which enqueues) is identical
  under per-Shard mailboxes and worker threads — only `pump`'s internals change.
  Nothing in the Shard ABI or the `Message` envelope forecloses it.

- **Request/response correlation and await.** The envelope already carries
  `reply_to` and `correlation`; replies work today as ordinary sends. A
  synchronous `request(...)` that blocks until a correlated reply arrives is a
  layer over the same enqueue/deliver path — not built here.

- **Schema-as-value over the bus.** A Shard answering "what do you accept?" with
  its schemas rendered *as* `Value`s (the reflection seam above) turns the bus
  self-documenting and is the path to the IDE-as-a-node. `accepted_schemas()` and
  the observer hook are the surfaces it would build on.

- **Content-id fast-path.** When a payload's schema identity already equals the
  door's `content_id`, the structural re-validation is provably redundant and
  could be skipped. The gate's identity check is exactly where this would sit; it
  is an optimization, deliberately not taken so that "one gate, every delivery"
  stays literally true for now.

- **Cross-boundary delivery (process / DLL).** In-process delivery moves `Value`s
  directly; a cross-boundary link would serialize at the sender and `parse` →
  `admit(Unverified, door)` at the receiver — the bytes path that already exists
  in zen-core — with no change to the Shard contract.

- **Crash isolation via per-process hosting.** Surviving a segfault in a loaded
  Shard needs the Shard in its own process under supervision (IPC). The C ABI's
  bytes-as-currency is already the cross-process currency, so in-process (fast)
  and out-of-process (isolated) become the two permanent hosting modes behind the
  same `Shard` contract — the next phase, not built here.

- **Migration at the version-mismatch reload point.** Today a new library whose
  state-schema version differs is a clean refusal. The migration layer slots in
  exactly there: resolve `(name, claimed_version, content_id) → door`, transcode
  the host-owned snapshot, and revive through the same gate.

- **Cross-language libraries.** The C ABI is *designed* to admit a Shard authored
  in another language: it exports only C, and Zen values cross as bytes. Only a
  C++-authored test library is built now, but nothing in the descriptor or the
  buffer discipline forecloses a Rust/C/other author.

- **Full schema-as-value.** The accepted-schemas manifest is the minimal
  precursor: schemas already round-trip as gated Values. Generalizing it lets the
  console introspect the whole system and a Shard answer "what do you accept?"
  with schemas rendered as Values.

---

## Level 0 hardening — closed seams and the seam-readiness review

Before Shard-based "Level 1" development begins — at which point every Level 0
surface a Shard touches gets expensive to move — one tightening pass closed the
handful of seams whose shape is *proven* and that Level 1 will immediately lean
on, and **left the rest open on purpose**. The discipline is symmetric: closing a
seam before its shape is proven is the same mistake as leaving a sharp one open.

### Closed in this pass (code)

1. **Dispatch selector → true `same_identity`; the `same_identity` misnomer
   closed.** `ShardBase::handle` selects the handler the same way the bus selected
   the door — by `same_identity` (`name == && version == && content_id ==`), not
   by a bare content-id hash — a null-deref fix (see *Schema and content
   identity*). In the same spirit, the public `same_identity` helper itself was
   strengthened from hash-only equality (a function named for identity that did
   `content_id`-only equality — a loaded gun in the surface) to full
   `(name, version, content_id)` identity, so its name and behavior now agree and
   the selector calls it instead of re-deriving the comparison inline. The
   gate/wire/registry still compare `content_id` *inline* as the drift check —
   unchanged.
2. **Exactly-one-handler, made loud.** A delivered message matches exactly one
   handler; a no-match is an internal-invariant violation that throws, never a
   silent drop. Pinned by a multi-shape routing test.
3. **`swap_state` split from `reload`.** Intentional hot-reload no longer spends
   the crash-revival budget (see *Intentional swap ≠ crash revival*).
4. **Emit declaration proven honest by test; `Mail` reserved as the chokepoint.**
   No enforcement added (see *Emit-set*).
5. **Grep sweep of `content_id()`-equality sites.** The dispatch selector was the
   only site reaching type-punned/positional access (`from_value<T>`) without a
   structural `admit`/`validate_into` behind it. Every other site is backed by
   structure and was left as-is: the gate's top-level and nested-message identity
   checks (`src/gate.cpp`) — *reviewed and deliberately unchanged*, since a full
   structural walk stands behind each, and FNV-as-drift-check is settled — the
   wire identity check (`src/serialize.cpp`, decode + `validate_into` follow), the
   registry's idempotent-re-registration check (`src/registry.cpp`, no type-pun),
   and the kernel's state-schema compatibility guard (`src/kernel/kernel.cpp`,
   refuse-on-mismatch; the real revive goes through the gate).

### The readiness bar, and the verdict on every other seam

A seam is **ready** only if *its shape is proven* **and** *Level 1 will
immediately lean on it*. Judged against that bar, none of the remaining future
seams is ready — each is **deferred with intent** so nothing helpfully closes it
and couples the substrate to a guess:

| Seam | Verdict | Why not yet |
|---|---|---|
| Migration transform registry | not-ready | the transform signature and keying need a real cross-version case to fix; reject-by-default holds until then |
| Emit enforcement (as a wiring *contract*) | partly closed in B1 | capability-gated delivery now authorizes every send against a real grant (Emit-defaulted for trusted Shards), so the *in-process* emit gate is live; emit-set *as an enumerated contract / wiring graph* is still deferred (a runtime router's emits are not statically known) |
| Schema-as-value beyond the manifest precursor | not-ready | only the manifest slice is exercised; the general "schema of schemas" has no consumer yet |
| Multi-threaded dispatch (per-Shard mailboxes) | not-ready | single-threaded FIFO is correct and sufficient; the `Shard` contract already survives the swap, so early closure buys nothing |
| Request/response await | not-ready | replies-as-sends works; a blocking `request()` has no caller yet |
| Content-id fast-path | not-ready (intentionally untaken) | kept off so "one gate, every delivery" stays literally true |
| Cross-boundary / cross-process delivery | not-ready | the bytes path exists; there is no second process to talk to yet |
| Crash isolation (per-process hosting) | not-ready | needs the process/supervision layer; in-process is accepted for now |
| Cross-language libraries | not-ready | only a C++ test library exists; the C ABI already admits others when one appears |
| Behavioral contracts | not-ready | the hook (the post-`admit` trusted value) is identified; there is no contract language yet |
| Static-struct half of the codegen marriage | not-ready | the runtime half is built and shares the door; the generator is a separate build-time effort |

No seam beyond items 1–5 was judged newly ready in this pass — the expected,
correct outcome. Should a later judgment find one ready, it is to be **flagged for
an explicit decision**, not closed unilaterally.

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
