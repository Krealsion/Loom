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
slot in.)

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
