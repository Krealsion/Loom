# loom — design

`loom` is the self-describing-value-and-gate foundation of Zen. A value
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
   (`loom::admit(Value, Schema)`) and the persisted-bytes path
   (`loom::admit(Unverified, …)`) both reach it and only it to decide
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
| `serialize.hpp` | native binary `serialize(Value)` / `parse`; `Unverified`; `admit(Unverified, …)`; compat JSON `loom::compat::serialize` / `loom::compat::parse` |

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
- **Compat** — the original self-describing JSON text (`loom::compat::serialize` /
  `loom::compat::parse`). Inspectable for debugging and tooling, but larger and
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

`zen-switchboard` is a separate library that links `loom` and builds the
first place where a value actually *crosses* a boundary: an in-process message
bus. It reimplements no validation, schema, or serialization logic — it routes
`Value`s and calls `admit()`. The whole point of the core (one gate, untrusted
until proven) now does real work guarding live delivery.

### In-memory delivery, gated at the recipient's door

Delivery is in-memory: the bus moves `Value` payloads between in-process Weaves.
Each delivery is validated by `admit(Value, recipient_accept_schema)` — the live
path of loom's *one* validator. The gate runs **at delivery, against the
recipient's accept-schema**: each Weave's boundary is its own door, so a publish
to N accepters is N independent boundary crossings. The bus writes no validator;
`gate_invocations()` proves a live delivery advances the same counter the
persistence (bytes) path does (`tests/test_switchboard.cpp`).

A handler is invoked **only** with an already-gated payload. A refused delivery
never reaches the handler — it is recorded and surfaced to observers instead.

### The `Message` envelope

A payload `Value` (its own schema is its routing shape) plus routing metadata:
`sender` (a `WeaveId`), an optional `reply_to` `WeaveId`, and an optional opaque
`correlation` token. Replies are ordinary sends — a handler sends to `reply_to`.
Synchronous request-and-await is a deliberate seam; the envelope already carries
what it needs.

### Addressing: accept-sets, directed, and publish

A Weave declares the schemas it accepts (its accept-set, keyed by
`(name, version)`). It is reachable two ways:

- `send(WeaveId, Message)` — directed. Refused unless the target's accept-set
  includes the payload's `(name, version)` **and** the payload passes the gate.
- `publish(Message)` — by shape. Enqueues one delivery for every alive Weave
  whose accept-set includes the payload's shape, in registration order; returns
  the recipient count (`0` is legal, not an error).

The bus owns a `loom::Registry` and registers every accept- and state-schema in
it, so all Weaves must agree on what a given `(name, version)` means — a
disagreement is a `loom::SchemaConflict` at registration.

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
the same outcomes as they happen. The outcome journal is a **bounded ring**
(`kJournalCapacity`, seq-tagged slots): it retains the most recent window of outcomes,
not one per message ever sent, so a bus running for weeks stays bounded by design rather
than by lifetime throughput (audit F-6). Every consumer reads a `Ticket` in the same
`submit`→`pump`→`outcome` breath, far inside the window; a `Ticket` older than the window
(or never issued) reads `Pending`.

### Refusals are structured and observable

A delivery either conforms or is refused — no partial or best-effort delivery,
no silent drop. A `Refusal` distinguishes bus-level routing reasons
(`NoSuchTarget`, `TargetUnavailable`, `NotAccepted`) from a gate refusal
(`GateRefused`, which carries the loom `Error` with its field path and
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

- `snapshot_bytes(id)` serializes the Weave's `snapshot()` with native `serialize`.
- `kill(id)` marks it dead (it stops receiving deliveries) and emits `Died`.
- `reload(id, bytes)` runs `parse` → `admit(Unverified, state_schema)` — the
  self-set lock from the first nucleus. On success it calls `revive(state)` and
  refreshes last-known-good. On refusal, the Weave's `policy()` decides.
- `swap_state(id, bytes)` is the **intentional** sibling of `reload`: same gate
  (`parse` → `admit(Unverified, state_schema)`), same `revive` + last-known-good
  refresh, but it spends **no budget** and offers **no** last-known-good fallback —
  a gate refusal is a clean refusal. Emits `Revived`/`Refused`.

**Intentional swap ≠ crash revival (only crashes spend the budget).** `reload` is
the crash-revival path: it is *budgeted* — it reads the policy's `max_reloads` and
decrements a per-Weave counter, so a Weave that crash-thrashes cannot revive
forever, and on a refused candidate the policy may fall back to last-known-good.
`swap_state` is the deliberate code-swap path (what the kernel's `reload_from`
calls): no budget, no fallback. Sharing one counter would be backwards — a Weave
that spent its crash-revival allowance could not be hot-swapped to *fixed* code,
and every deliberate swap would draw down the very budget meant to stop crash
loops. The two operations are therefore separate methods, not a flag.

The **only** schema the bus hard-codes is its lifecycle-policy grammar —
`LifecyclePolicy v1 { max_reloads: Int, revive_from_last_good: Bool }`, exposed
as `lifecycle_policy_schema()`. The bus validates `policy()` against it and reads
only those two fields; everything else about a Weave is opaque to it.
Last-known-good is the last successfully-admitted snapshot (seeded at
registration by gating the Weave's initial snapshot, so a Weave is born valid).

### Weave contract (a frozen ABI surface)

`Weave` is an abstract base with exactly five methods — `accepted_schemas`,
`handle`, `snapshot`, `policy`, `revive` — kept minimal because it is a future
ABI surface. It is designed to survive a move to per-Weave mailboxes and
multi-threaded dispatch unchanged: a handler still *receives* a gated message and
*sends* (which enqueues); only `pump`'s internals would change. The bus owns
Weaves (`register_weave(std::unique_ptr<Weave>)`); a non-owning `weave(WeaveId)`
accessor serves queries and tests. `handle` sends through an abstract `Bus`
interface (which `Switchboard` implements), so the *same* Weave works whether it
is compiled in or loaded from a library — the kernel below relies on this.

---

## The Kernel (DLL loading · the C ABI · hot-reload)

`zen-kernel` is the OS/entry-point layer: it loads Weaves from dynamic libraries
and hosts them on a Switchboard. It reimplements no validation, routing,
serialization, or lifecycle — it links `loom` + `zen-switchboard` and adds
only the library boundary.

### Bytes are the boundary currency

The hard, permanent part is the seam. Only **C** crosses it: opaque instance
handles, plain function pointers, `const uint8_t*` + `size_t` buffers, and
integer status codes — no C++ types, no STL, no `std::any`, no exceptions. Every
Zen value/schema/message a library hands back crosses as **serialized bytes**,
and the host **re-admits those bytes through loom's gate** before trusting
them. So the DLL boundary is just another boundary the one gate guards, and
*untrusted-until-proven extends to loaded code for free*: a buggy or hostile
library can no more inject an unvalidated value than a hostile byte-stream from
storage can. A test proves a value crossing the DLL seam advances the same
`gate_invocations()` counter the persistence path does, in both directions
(a delivery *to* the library Weave and a message *emitted by* it).

This also supersedes a rejected prototype — a `std::any`-based service locator
whose `any_cast<T*>` carried no schema, was UB across the seam, and retained raw
library pointers that dangled on unload. Only its cross-platform `dlopen`
wrapper survived (GCC default visibility, no `__declspec`).

### The C ABI (`include/zen/kernel/abi.h`)

One exported symbol, `extern "C" const ZenWeaveAbi* zen_weave_abi(void)`,
returns a static descriptor:

- `uint32_t abi_version` — the ABI's own version (distinct from schema versions);
  the host rejects a descriptor whose version it does not support.
- `create` / `destroy` — construct/destroy the opaque instance.
- `describe` — emit the manifest (accepted schemas + state schema) as bytes.
- `snapshot` / `policy` — emit state / lifecycle policy as bytes.
- `revive` — receive already-host-admitted state bytes.
- `handle` — receive an already-host-gated inbound message (sender/reply_to/
  correlation + payload bytes) plus a **host callback table** (`ZenHostApi`)
  through which the Weave `send`/`publish`es by handing the host serialized
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
`(name, version)` and resolved against the host registry. This is the minimal
**schema-as-value** precursor — the one place that seam is lightly touched.

**The manifest is self-contained (`zen.Manifest` v3).** "The manifest lists
referenced schemas first" was documented here from the start — and unbuilt: no
encoder section carried the nested components, which no fixture noticed because
every shape that had ever crossed the ABI was flat. The **first real consumer
with a nested shape** (Zengine's snake — a state carrying `List<Pos>` and a
`Pos` field) was refused at load with `unresolved nested schema 'Pos'`, and the
prose-over-promise surfaced. v3 completes it: an optional `referenced` list
carries every transitively-nested component schema in **post-order**
(dependencies before dependents, deduplicated), the decode side registers them
into the dependency registry before touching accepted/state, and the
cross-library **agreement wall applies to components** exactly as to doors (an
identical re-registration is a no-op; a conflicting one refuses the load). Flat
manifests emit no section and stay lean. The kernel's reconstruct consumes it;
the isolation host's manifest path deliberately does **not** yet (no
out-of-process consumer has a nested shape — its refusal stays clean, the
helper exists, the trigger will pull it).

### Weaving stays Zen-invisible

`ZEN_EXPORT_WEAVE(MyWeave)` (header-only `export.hpp`) generates the descriptor
and every thunk from a clean C++ `loom::Weave` subclass — the maker writes
the same Weave they would compile in, plus one line, and hand-writes no thunk.
The thunks have C language linkage (matching the descriptor's pointer types),
forward to C++ template helpers, serialize Values to the host sink, rebuild a
`Bus` that forwards the Weave's `send`/`publish` across the callback table, and
**never let a C++ exception cross the seam** (all caught, turned into status
codes).

**Build a weave library with `-fno-gnu-unique` (GCC).** Found by the first
maker-path `.so`s (Zengine's snake), not by the fixtures: the woven layer
instantiates loom's inline templates — `schema_of<T>()`'s function-local
statics — with vague linkage, which GCC emits as `STB_GNU_UNIQUE` symbols, and
glibc resolves those through a **program-wide** table that ignores `RTLD_LOCAL`
and can outlive a `dlclose`. Two libraries sharing a vocabulary header, one
unloaded before the other loads, left the second silently aliasing the first
one's *destroyed* statics — a use-after-free in `describe()` at load time (or
garbage manifest bytes when the read survived). The kernel's `RTLD_LOCAL`
promise ("the host and the library never interpose") is only whole when the
library is compiled `-fno-gnu-unique`, demoting those statics to ordinary
vague linkage so each library owns its own, whole lives. The fixtures dodged
this for years by accident — their hand-built schemas live in anonymous
namespaces, and internal linkage is never unique; any real maker using
`ZEN_SHAPE` + `WeaveBase` in a `.so` needs the flag. Pinned by Zengine's
load-unload-load linkage cases; a Loom-native pin (a `WeaveBase` fixture pair)
is a named follow-on.

### The host adapter

`HostAdapter` (host-side) **implements the existing `Weave` interface** by
translating each method to its C thunk: serialize arguments to bytes, call
across, and on the way back `parse` + `admit` through the gate (manifest,
snapshot, policy, and every emitted message). From the Switchboard's side it is
simply a Weave — a loaded Weave mounts, routes, and reloads exactly like a native
one. The host callbacks resolve an emitted message's schema against the
Switchboard's system-wide registry (`resolve_schema`) and gate it before routing.

### Teardown and hot-reload order

The adapter owns the instance and destroys it (`abi->destroy`) in its destructor;
the Kernel owns the library handle and closes it **only after** the adapter is
gone. Unload is therefore: `unregister_weave` (stop delivery, take ownership) →
destroy the adapter (→ destroy the instance, library still open) → `close` — no
call ever lands in a closed library (clean under ASan).

`reload_from(name, new_path)` snapshots the live Weave to **host-owned** bytes,
opens and validates the new library, then swaps `(abi, instance)` **in place
behind the same adapter and WeaveId** (so senders keep their handle), destroys
the old instance while its library is still open, closes the old library, and
revives the new instance from the snapshot **through the gate**. State survives
because the snapshot bytes are host-owned, independent of either library. A new
library whose **state-schema version differs** is a **clean refusal** — the old
library keeps running. (This is exactly where the deferred migration layer will
slot in.) The revive after the swap goes through `Switchboard::swap_state`, the
**unbudgeted** intentional-swap path, so a deliberate hot-reload neither draws
down nor is blocked by the Weave's crash-revival budget (see *Intentional swap ≠
crash revival* above).

### The one switchboard change for the kernel, and a note on hosting

The kernel needed two small `Switchboard` additions: `unregister_weave` (to
destroy an adapter before closing its library) and `resolve_schema` (to gate a
library's emitted messages against the system registry). `Weave::handle` taking
the abstract `Bus` (rather than the concrete `Switchboard`) is what lets one
Weave be hosted either natively or from a `.so`.

**Crash isolation is a non-goal here:** this kernel is in-process, so a crashing
Weave takes the host down (accepted for now). The wire format already makes the
eventual process boundary cheap — in-process (fast) and out-of-process (isolated)
become the two permanent hosting modes, and a cross-boundary link is just
`serialize` at the sender and `parse` → `admit` at the receiver, with no change
to the Weave contract.

---

## The weaving layer (schema-from-struct, low ceremony)

The **weave** layer (header-only, `include/zen/weave/`) is the first layer whose job is
to make apparatus *disappear*. It is **pure sugar**: no new schema type, no new
value type, no second validator, no change to the gate, the wire format, the bus,
or the kernel. It emits schemas only through `SchemaBuilder` and hands `Value`s to
the same `admit`. A struct-derived `Foo v1` and a hand-built one are the *same*
`Schema` — identical content-id, shared door — proven by `tests/test_weave_shape.cpp`.

### Schema-from-struct

A maker writes a plain C++ struct (real members) plus one in-class line:

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
  `loom::Bytes→Bytes`, `std::vector<T>→List<T>` (recursive; a byte vector resolves
  to Bytes first), and a nested registered shape `→Message`;
- `to_value(const T&)` and `from_value<T>(const Value&)` (the latter assumes an
  already-gated value — conversion to a struct happens *only after* the gate).

The struct stays a plain aggregate; the maker never touches a `Cell` or a
`set("...")`, so a field typo is a compile error, not a runtime throw.

### Explicit version (built now — migration seam #3)

The macro **requires** a version and folds it into identity: `v1::Ping` and
`v2::Ping` (same `zen_name`, different version) are distinct content-ids by
construction. There is no way to evolve a shape in place; a new version is a new
identity. Omitting the version fails to compile. The whole migration chain keys on
these stable versions, so this is the one migration seam unsafe to leave loose —
and it is built.

### Low-ceremony Weave-making + `mount`

`WeaveBase<Self, State, Accept<Shapes...>, Emit<Shapes...>>` (CRTP) derives:
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
wires the self-id, and returns the `WeaveId` in one call.

**Dispatch selects by true identity (`same_identity`), the same key the bus
admitted the message under — not by a bare content-id hash.** A delivered payload
has already passed the gate against the accept-set entry the Switchboard chose by
`(name, version)` (`accept_match`); `handle()` picks the handler the same way, so
`from_value<S>`'s precondition — every field present and well-typed — is
*guaranteed*, not merely probable. Selecting by `content_id()` *alone* would be a
latent **null dereference**: `content_id` is a 64-bit FNV hash, and a collision
*within one Weave's accept-set* would route a message to the wrong `on()`, whose
`from_value<S>` reads `*v.get(field)` for each of `S`'s fields — and `get()`
returns null for a field the colliding shape does not carry. `same_identity`
checks `(name, version)` as well as `content_id`, so it closes that collision —
its `content_id` term is the redundant-but-true integrity check — and that is
exactly how the door was chosen.
Because the delivered message is gated against one accept-set entry and the
handler set is the *same* `Accept<A...>`, **exactly one** handler matches; a
no-match is an internal-invariant violation, so `handle()` throws a clear
`std::logic_error` rather than silently dropping the message.

**Ceremony delta** (`examples/heartbeat.cpp` → `heartbeat_woven.cpp`): ~89 → ~58
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
  via a function, chainable so a `v1` value walks `v1→v2→v3`. The weaving layer is
  shaped so such a transform naturally **consumes and produces the typed structs**
  (`Player_v2 migrate(const Player_v1&)`) and is **keyed by content-id** (which
  already exists and is now derivable from a struct). It hooks into the *single*
  identity-mismatch decision points that already exist — `admit_against` (wire) and
  `reload_from` (kernel), both reject-by-default — which stay isolated and untouched.
- **Emit-set (enforcement reserved at `Mail`).** `Emit<Shapes...>` is declared
  alongside `Accept<…>` and surfaced as `emitted_schemas()`, **informational and
  unenforced** for now. `Mail::send`/`reply`/`publish` are the *sole* outbound path
  for a woven Weave's **maker code**, so **`Mail` is the single reserved chokepoint** where
  emit-enforcement (a sent `T` must be in `Emit<...>`) would later sit. (The
  construction layer's own poke answers go directly through the gated bus —
  substrate machinery, not a maker emission; a future `Mail` emit-gate would not
  govern them, and their authority is still the grant — see the Poke section.) It stays
  off **with intent**: a Weave's emit-set is not yet known to be statically
  enumerable (a router/forwarder may emit shapes chosen at runtime), so the
  substrate must not commit to it. The *declaration* is kept honest by test
  (`tests/test_weave.cpp` pins that each Weave's observed emits equal its
  `emitted_schemas()`) meanwhile. Completing the silhouette later (gating against
  the declared contract, drawing the bus wiring graph, feeding a dependency-mapper)
  needs *no* weaving change. **B1 (capabilities) closes the in-process half of
  this** — see below.

---

## Capabilities (B1): the in-process grant model

Delivery is gated on message *shape*, but a Weave could send anything to anyone.
B1 closes that: a Weave's reach is a **grant** the host assigns, default nearly
empty, and the bus authorizes every Weave-originated send against it. (This is
**B1 of two**: B1 is the *message* boundary; **B2 — the next phase — is
isolation-as-enforcement**, out-of-process hosting that projects the grant onto OS
sandbox primitives at the *syscall* boundary. The grant is the one source of truth
projected onto whatever boundary the hosting mode provides.)

### The grant

A `Grant` (`include/zen/switchboard/grant.hpp`) has two parts:

- **Send-permissions (enforced in B1):** a list of `SendRule`s, each a
  `(shape-selector, target-selector)` where each selector is a specific value or
  "any" — e.g. "`Pong` to any accepter", "`LoadLibrary` to the control Weave only".
- **OS-capability flags (hard, binary):** `Network` (enforced out-of-process in B3)
  and `SpawnProcess` (reserved). B1 **does not consult** them — they govern
  instruction-level behaviour a loaded `.so` reaches directly, which only process
  isolation stops. **Filesystem is not a flag** — it is the *graduated* `FsAccess`
  level (enforced in B4); the old binary `FilesystemRead/Write` flags were removed in
  B4 so files have a single source of truth.

The default grant is **empty**: minimal authority by default. Grants flow from the
host (the root of trust) at registration, out-of-band; there is no in-band path by
which a Weave widens its own grant.

### The trust boundary: `Bus` gates, `Switchboard` is root

The split that already existed *is* the trust boundary. A handler only ever
receives a `Bus&` — and `deliver_one` now hands it a per-delivery **`WeaveBus`**
bound to the handling Weave's id, *not* the concrete `Switchboard`. The `WeaveBus`
stamps the authoritative sender on every message (a Weave cannot send as anyone
else) and enqueues it **gated**. `Switchboard::send`/`publish` — held only by the
host program — enqueue **ungated** root authority (test setup, the trusted host
shell). `Mail` wraps the `WeaveBus`; the kernel's loaded-Weave host callbacks
route through the same `WeaveBus` — so native and loaded Weaves are authorized
identically, keyed by id.

### Authorization is a distinct step from the gate

At delivery, for a *gated* (Weave-originated) message and **before** the gate, the
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
attempted target — the supervisor can *see* a Weave overreach.

### Grant ↔ Emit, and the closed emit seam

`loom::mount<Self>(bus)` defaults the grant to the Weave's self-declared
`Emit<E…>` (each emitted shape → any accepter) — the *trusted* in-process default.
This **closes the in-process emit-enforcement seam**: delivery now checks a real
authority, and a Weave that sends a shape outside its declared `Emit` is denied.
`mount_granted` supplies an explicit grant for an untrusted Weave, whose
declaration is not trusted. (Emit-set *as a wiring contract* — enumerating a
runtime router's emits, drawing the graph — remains the deferred seam.)

### The kernel's message door (the teeth)

The kernel registers a **control Weave** (`include/zen/kernel/control.hpp`)
accepting `LoadLibrary` / `ReloadLibrary` / `UnloadLibrary` (`ZEN_SHAPE`s) whose
handlers call the kernel's `load` / `reload_from` / `unload`. Operating the kernel
is now just sending it messages. The **load capability** — the right to send those
shapes to the control Weave — is the canonical dangerous grant: a Weave holding it
drives the kernel by message; one without it is denied at the control Weave's door
(`CapabilityDenied`), gating the single most dangerous surface in the system with
the same mechanism as everything else.

### Loaded `.so` Weaves, and the B1 → B2 → B3 split

A loaded `.so` can bypass the bus and reach syscalls directly, so a restrictive
*bus* grant on it is not real containment in B1. Containment is staged: **B2**
(below) puts the Weave in its own **process** — crash isolation and memory
separation — and **B3** adds the **OS sandbox** the reserved OS-capability flags
drive (the syscall boundary). So the kernel grants loaded Weaves **permissive bus
sends** in B1; the kernel *door* is demonstrated fully gated against **native**
Weaves (the woven mod logic, the console's elevated instance). B1 makes the
grant *real at the message boundary* and shapes it so B2 can host out-of-process
and B3 can enforce its OS-relevant parts at the syscall boundary, each with no
rework.

---

## Isolation (B2): out-of-process hosting & crash supervision

B2 makes hosting mode a **mount choice**: a Weave can run in a child process,
indistinguishable to the bus from an in-process one, and when it crashes the host
survives, contains the blast, reloads it a bounded number of times, then
quarantines it. This is the *isolation* half of "capabilities + isolation". It is
honestly **isolation, not sandboxing** — see the status note below.

### What changed, and what deliberately did not

Nothing in the gate, the wire format, the `Value`/`Schema` model, or the
single-threaded FIFO bus changed in behavior. The only new bus surface is the pair
`send_as`/`publish_as` (root authority, added in B1's prep), which the host uses to
re-enter a child's output with its identity stamped **from the connection**. The
new code is a library (`zen-isolation`) and a child executable (`zen-weave-host`);
the bus simply hosts one more kind of `Weave`.

### The child is a byte shuttler (`zen-weave-host`)

The child reuses the **kernel C ABI** unchanged: it `dlopen`s the `.so`, gets its
`zen_weave_abi()`, and drives the same `create`/`describe`/`policy`/`snapshot`/
`revive`/`handle` thunks. It links **no loom** — it neither validates nor
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
`admit` + `decode_schema`. **Reconstruction is depth-bounded.** A type reference is a
*flat* token stream — bounded by the list cap, not the value-tree depth cap — so a
malicious manifest could type a field `List<List<…>>` tens of thousands deep, pass the
gate, then drive `decode_type`'s per-`List` recursion to a host **stack overflow at mount
time, before the mod runs** (a SIGSEGV no `try/catch` catches; audit F-19). `decode_type`
now caps nesting at `kMaxTypeDepth` (mirroring `kMaxBinaryDepth=64` — a type nested deeper
could only describe values the gate already rejects) and **refuses on the way down**, so a
hostile descriptor becomes an ordinary `Refused` at every `decode_schema` call site
(out-of-process mount, in-process load, and the bridge peer).

### Sender integrity: stamped from the connection

The `Emit` frame carries **no sender field by construction** — a child has no way
to express one. The host stamps the proxy's `WeaveId` (the identity of the
*connection* the bytes arrived on) via `send_as`/`publish_as`, and the bus then
authorizes that message against **that Weave's grant** at delivery, yielding
`CapabilityDenied` on a violation — identical to the in-process `WeaveBus` path. A
child that wants to send as someone else cannot get it; a child granted nothing
can emit, but its emissions are denied before the gate.

### The proxy is a `Weave`; the host loop keeps the bus single-threaded

The host-side `OutOfProcessWeave` *is* a `Weave` on the bus, so the Switchboard is
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

On a child's death the host marks the Weave dead and emits `Died`, then drives the
**existing lifecycle mechanics**: `reload` from the **host-owned snapshot**, which
is budgeted by the Weave's own `max_reloads`. Reload re-enters through the proxy's
`revive()`, which respawns a fresh child and ships it the last-good state. A child
that crashes again on `revive` simply dies again next `step`, spending one unit of
budget each cycle; when the budget is exhausted, `revive` is never called and the
Weave stays dead — **quarantined**, surfaced on the tap. The host process is never
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

**Threat tier (stated plainly): this is abuse-tier, not escape-tier.** The sandbox's threat model is
**buggy or greedy code** — a mod that over-reaches its grant (touches the network/filesystem it
wasn't given, forks without bound) — not a determined adversary attempting **kernel escape**. The
mechanisms here (namespaces, cgroups, the fail-safe mount) contain *misbehavior*; they are **not**
hardened against a syscall-level exploit chain. **seccomp is the named, currently *unbuilt*
escalation to escape-tier.** Where this document says a mod is "untrusted" or "hostile," read it at
abuse-tier — the value proven is that the *bus/grant* makes most bad behavior unsayable and the OS
sandbox contains the rest, not that a kernel exploit is repelled.

B3 turns "isolated" into "isolated **and** sandboxed" by projecting the grant's
OS-capability flags onto a real syscall-level profile applied to the child *before*
it runs the mod's code. It enforces exactly **one** flag — **Network** — and, more
importantly, builds the **permanent detection-and-honesty structure** every future
primitive plugs into. The seam B2 left is honoured exactly: hosting is already
out-of-process, the grant already carries the flags, the child is the single place
a profile installs — so B3 is additive, with **no rework** to the gate, the bus, the
wire format, or the supervision loop.

### The lattice: detect → apply → know → refuse-or-proceed

The real deliverable is not "configure a namespace" — it is that the system *always
knows whether it actually sandboxed a Weave*, on every platform, and **never claims
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
Weave **visibly marked uncontained** for each unenforced capability (so a WSL/CI box
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

B4 closes the highest-value remaining harm a stranger's Weave can do once the network is
shut: reach your **files** — read `~/.ssh` or a password store, destroy data, or plant an
executable. It is the **first *graduated* capability**: the grant's `FsAccess` level picks
a point on a safe→dangerous axis, defaulting to the safe end.

### The level model (allow-list, not deny-list)

The restricted view is an **allow-list** — the Weave sees only what it is granted, built by
`pivot_root`-ing into a fresh, minimal, read-only root. A deny-list (hiding sensitive paths)
fails *open* the moment you forget one; an allow-list fails *closed*. The levels:

- **None** (default) — a minimal read-only view: the dynamic-loader closure and the Weave's
  own `.so`, nothing writable, no home, no `/tmp`. A default-grant Weave cannot read your
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
4. **Remount the root read-only** so a Weave cannot write — or plant-and-exec — at `/`;
   only the scratch submount stays writable. (Without this the writable root tmpfs would
   leak past the read-only/noexec intent — a real gap the behavioural test caught.)
5. `pivot_root` into the new root, `chdir("/")`, detach the old root.

### Confirmation, fail-safe, dev-mode (the B3 discipline, extended)

After the handshake the host **confirms** the child is in a distinct mount namespace
(`/proc/<pid>/ns/mnt` inode differs); a failure fails safe (the mount refuses), so
"contained at level X" rests on confirmation, never inference. Detection **probes** the real
mechanism (a throwaway child that builds a minimal view); an unenforceable host refuses by
default, and `set_dev_mode(true)` converts that to a loud warning with the Weave marked
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
Weave's read of a secret outside scope, a write outside `/scratch`, and an `execve` from a
`noexec` scratch all return the OS's `ENOENT`/`EROFS`/`EACCES` — while the probe **still
emits** its result (sandbox ≠ muzzle) — and `WriteAnywhere` proves the opt-out reaches host
paths and is reported *not contained*. **Sandboxed-by-default now means network *and* a
restricted filesystem view.** Next: **B5 — cgroups**.

---

## Isolation (B5): the resource primitive — a quantitative capability, cgroup-v2 enforced

B5 closes the last threat-model harm: a Weave that **hogs resources** — allocates until the
host OOMs, pegs every core, or fork-bombs. It is the **first *quantitative* capability**:
not binary (network), not a safe→dangerous level (filesystem), but a *limit*. With network
+ filesystem + resources the mechanism ladder covers the mod-ecosystem threat model end to
end. **seccomp is a separate, later decision** (it guards a different tier — kernel-exploit
*escape*), not an assumed B6.

### The grant's resource limits and computed defaults (no knob)

`ResourceLimits` adds **memory** (bytes), **pids** (the fork-bomb stop), and **cpu_weight**;
`0` means "use the host-computed conservative default," a positive value raises it, and
`with_unlimited_memory()` is the **only** opt-out — it removes the *memory* cap alone. Defaults
are **computed from the host, not a config knob** (the stinginess bar): memory = a bounded
fraction of RAM (1/8) capped at 1 GiB and floored at 128 MiB so one Weave can't OOM the host;
pids = 512 (room for threads, stops a bomb); cpu_weight = 100 (fair share, not a hard quota — a
quota would waste idle cores). **A structural invariant: no grant can license a fork bomb.**
There is no wholesale "no limits" opt-out; the only opt-out reaches *memory*, and **pids stays
bounded for every Weave cgroups can reach** (a heavily-parallel Weave raises its pids cap, never
removes it). A forgotten/empty grant lands on the *bounded* default.

### cgroup-v2 mechanism (parent-applies-at-the-sync-point)

Reusing the proven fork+handshake — **no `clone3` rewrite** (the handshake already closes
the attach race: the child runs nothing real until released):

- The host **discovers its delegated base** from `/proc/self/cgroup` and, once, builds the
  hierarchy the **no-internal-processes** rule forces: create a `zen-supervisor` leaf, **drain
  the base's processes into it**, then enable `+memory +pids` — **and `+cpu` where the
  controller is delegated** — on the base's `cgroup.subtree_control` (you can't enable
  controllers on a cgroup that holds processes). Per-Weave leaves are created **alongside** the
  supervisor.
- At **mount**, a per-Weave leaf is created with its limits (`memory.max` unless opted out by
  grant, `memory.swap.max=0` so swap can't escape the cap, `pids.max` **where the pids
  controller is delegated**, and `cpu.weight` where the cpu controller is delegated — a
  fair-share weight, set-and-confirmed, not a hard cap). Each dimension is written only where
  its controller is present, and the note/attestation say so per dimension. At the **sync point** (child unshared, blocked on
  "go") the parent writes the child's pid into the leaf's `cgroup.procs` — moving the whole
  subtree it execs/spawns under the limits — then maps it (if it made a userns), then releases.
  The child consumes nothing until released, so it is in the cgroup before it can.
- A Weave exceeding `memory.max` is **OOM-killed within its cgroup** by the kernel; the child
  dies and flows through the **existing** death → bounded-reload → quarantine path unchanged.
  `pids.max` makes `fork()` fail (`EAGAIN`) rather than bomb the host. The leaf is removed on
  teardown (after the process is reaped — `rmdir` needs it empty) and recreated on respawn.

### Resolution, confirmation, and the delegation reality

Resolution joins the tree: enforceable → **Enforced** (a leaf bounded by the delegated
controllers — memory capped or opted-out by grant, pids capped, cpu weighted, *each where its
controller is delegated* — create+limit+move+confirm); else dev-mode → **Uncontained**; else
**fail-safe refuse**. Resources **never resolve to Granted** — there is no wholesale opt-out, so
a leaf is always created when cgroups work (bounded by whatever controllers are delegated).
Confirmation reads `/proc/<pid>/cgroup` (pid is in the leaf) and reads the limits back — each
where its controller is delegated (memory where capped, pids where delegated, cpu where
delegated); a mismatch fails safe.

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

The Resources detection (establish-the-base probe), the per-Weave cgroup-v2 leaf with
memory/pids limits (and `cpu.weight` where the cpu controller is delegated) applied at the sync
point and confirmed (pid-in-leaf + limits read back), leaf cleanup/recreate across
teardown/respawn, the resolution + dev-mode + fail-safe + the **memory-only opt-out**, and the
honest per-Weave `containment()` all ship, green in Debug and under ASan/UBSan (the suite runs
under a delegated scope). The OS-enforced proof passes **with its negative control**: a
memory-bomb Weave under a 64 MiB cap is **OOM-killed within its cgroup** (the host survives and
quarantines it), while the *same* allocation under a 512 MiB cap **survives** — proving the cap,
not the allocation, is the cause; and a fork-bomb is **bounded by `pids.max`** (≤64 of its 4000
attempts) **where the pids controller is delegated**. **The structural invariant is pinned (in
that posture): a fork-bomb stays bounded *even with `with_unlimited_memory()`*** (memory
uncapped, pids still 64-bounded), and the memory opt-out lets a bomb survive the cap that would
OOM a default-capped one — no grant can license a fork bomb. Where pids is *not* delegated the
substrate now **says so** rather than claim the stop absolutely (the F-20 pids mirror, below). `cpu.weight` is **set-and-confirmed where the cpu controller is delegated** (present but
unexercised on this WSL host, which delegates only memory+pids — honestly reported absent), a
*fair-share weight, never a hard cap*; an absolute `cpu.max` quota is a possible **future
opt-in**, not built. **Memory _and pids_ are reported the same honest way as cpu:** the
containment note prints `memory<=…` / `pids<=…` only where that controller is delegated
(`cgroup_memory_available()` / `cgroup_pids_available()`), and positively states the dimension is
*uncapped* where it is not — `cgroup_create_leaf` writes `memory.max` / `pids.max` only when the
controller is present, so the note must never claim a cap it did not set. **The attestation
mirrors the note:** `resource_attestation()` qualifies the fork-bomb-stop claim on
pids-delegation — where pids is not delegated it says the fork-bomb stop is *not enforceable on
this host*, retiring the former absolute `"pids.max ALWAYS bounds a fork-bomb"`. This is the
honesty lattice's one absolute rule (audit F-20 **and its pids mirror, N-1**), now watched in
**both** delegation directions: the original F-20 pin exercised only the pids-only posture, so
the memory-only mirror over-claimed unwatched — the pids×memory **posture matrix**
(test_isolation.cpp) closes that, pinning every posture to tell the truth or fail. `containment()` now leaves **only
syscalls** unenforced. The mechanism ladder is **complete for the threat model** — as a
*mechanism* set, and conditionally: the OS-enforced rungs need userns + delegated cgroup-v2, and
where the host lacks them a mount **fails safe (refuses)** outside dev-mode. What remains is a
**deliberate seccomp decision** (kernel-exploit-escape defense) — the policy phases (P1, the
StorageBroker) have since shipped.

---

## Policy phases P1–P2 — the powerbox (StorageBroker, then NetworkBroker)

The B-series built **mechanism** — a grant projected onto an OS-enforced boundary. This phase
builds the **policy that produces the grant**, in the **powerbox / object-capability** shape:
a privileged capability (real disk) is held by a small, hard-rooted **broker** Weave; an
untrusted mod never holds the raw capability — it holds only a *send-rule to talk to the
broker*, which mediates and scopes by the authoritative sender the bus already stamps. The
core barely changes; the broker is *ecosystem*. **Both parts are built**: Part A the reusable
grant-policy core hooks (ask, floor + grant-record, role-addressing), Part B the broker itself
(the role-send ABI, persistent-scoped write, the StorageBroker, and the mod-vs-mod proofs).

### The wall: advice, not authority (ask ≠ grant)

A Weave may **ask** for capabilities; the **host alone** decides the grant. The ask is
*conformance data* — gated, untrusted, surfaced — never authority. This is Pillar 1
(conformance ≠ authorization) applied to the policy layer, and it is kept absolute.

- **The ask, in the manifest.** `zen.Manifest` is bumped to **v2** with an optional
  `requests` field carrying a `zen.CapabilityAsk` (`network`, a `filesystem` level name,
  `roles`). It is optional — a Weave with no ask emits no section and admits unchanged; and
  bumping the version (not mutating v1 in place) preserves the invariant that a published
  `(name, version)` is a frozen shape. The ask rides the manifest through the **same
  meta-schema gate** as the accept-set and state schema (`schema_codec.hpp`); nothing in the
  path lets a declaration become a grant.
- **The maker macro.** `ZEN_ASK(.network = true, .filesystem = "write-scoped", .roles =
  {"storage"})` is a `ZEN_SHAPE`-sibling on `WeaveBase`: it shadows a floor default
  (`ask_config()` → empty) with designated initializers, defaulting to nothing. The export
  layer reads it only if present (`if constexpr (requires { s->zen_requested_capabilities(); })`),
  so a bare `Weave` without it simply has no ask.
- **The kernel ignores the ask; the host reads it only as advice.** The kernel never consults the
  manifest ask for anything — it is never an input to a grant. Only the **host** reads it, purely as
  advice: `IsolationHost::declared_ask(name)` surfaces what the mod *wanted*; the gap between it and
  `containment(name)` is the advice-vs-authority wall made observable. Proven: a mod whose manifest
  declares `network` **and** `filesystem` write still mounts on the floor (`network: contained`,
  `filesystem: contained`). The ask is never consulted for a grant.

### The floor, and where authority above it comes from

- **Floor by default, no ceremony.** An unknown mod mounts via `mount_mod(name, so_path)` on
  the **floor**: a default `Grant` (no network, `FsAccess::None`, bounded resources, empty
  sends) plus a single send-rule to the storage broker **role** — enough to persist and
  retrieve on messages alone, with zero disk access. It never holds a raw privileged
  capability; it holds only a send-rule to the broker that does.
- **The grant-record (the host holds the pen).** A persisted, per-install ledger keyed by the
  mod's **`.so` content-hash** (**SHA-256 truncated to 128 bits** — stable across path moves, so
  a rebuild is a new identity and re-floors, the honest default) maps identity → a granted
  **delta** above the floor. The key is a **cryptographic** digest, not FNV-1a: because this key
  alone decides a mod's authority above the floor, a weak hash would let an attacker forge a
  second build onto an existing grant — FNV's ~2^32 collision resistance was too weak to name a
  security-relevant identity (audit F-1). It is still content-addressing, not authentication: it
  names a build by its bytes; a *signed* author identity remains the identity phase's job. It is
  **TCB data**: only the host writes it (`record_grant_delta`), never a Weave; it persists as a
  gated `Value` in Zen's JSON (inspectable, editable per-install), written **durably** — the temp
  file is `fsync`'d before the atomic `rename`, and the directory `fsync`'d after, so a crash
  cannot leave the ledger's name pointing at unsynced bytes and brick the host's next load (audit
  F-8). This stands in for the consent UX (deferred). Proven: with an empty record a greedy mod floors;
  with a recorded network delta the *same* mod mounts `network: granted` — the pen, not the
  declaration, is the authority.

### Role-addressing (the seam, baked in, resolved trivially)

Power is a granted send-rule to a **role**, not a `WeaveId`:

- A Weave may be **registered under a role** (`register_weave(weave, grant, "storage")`); v1
  is **singleton** — a role has exactly one holder, and binding a held role throws.
- A send may target a **role** (`Bus::send_to_role`); a grant may permit `shape → role`
  (`Grant::allow_to_role` / `permits_role`). Authorization is **by role** — the stable slot
  the rule names — decided in `deliver_one` **before** the role is resolved to a holder, so
  an unauthorized sender cannot even learn whether the role is held, and a role-rule and a
  WeaveId-rule are **distinct walls** (neither authorizes the other's send).
- The role resolves to its holder at delivery; the holder's `WeaveId` is **stable across
  reload**, so a role send-rule survives the broker reloading. An **unheld role degrades to
  `NoSuchTarget`** — exactly like an unknown WeaveId, never the gate: a crashed/unmounted
  broker is *unavailable*, not a hole.
- **Out-of-process role-send (Part B).** A new wire frame `Op::EmitRole` (role + reply_to +
  correlation + payload, **no sender**) and a `ZenHostApi::send_to_role` callback close the
  seam: `HostApiBus::send_to_role` ships the frame, and `handle_child_frame` re-admits the
  payload and routes via `send_as_to_role(link.id, role, msg)` — the sender stamped from the
  connection, never on the wire. `reply_to` defaults to `link.id` (a child cannot know its own
  id), so a broker can reply to the mod. A floored, out-of-process mod reaches the broker end
  to end on messages alone.

### The broker (Part B): a real capability on a mod's behalf

- **Persistent-scoped write (TCB-only).** `build_view_plan` binds the host's `storage_root`
  **writable** at `/scratch` when `WriteScoped` carries a `scoped_path`, instead of the
  ephemeral tmpfs it binds without one. Still least-privilege — the broker reaches only that
  one dir, never the host home — but the data survives. A mod is `FsAccess::None` and never
  gets a `scoped_path`; the path cannot leak to it.
- **The StorageBroker** (an *ecosystem* Weave, not host code). Host-mounted out-of-process via
  `mount_broker` at the TCB tier: `WriteScoped(storage_root)` only, permitted to reply
  `StorageValue` to any mod, registered under role `"storage"`. It accepts
  `StoragePut{key,value}` / `StorageGet{key}` → `StorageValue{value}`, and derives the keyspace
  from the **stamped sender** (`mail.sender()`) — `storage_root/<sender>/<hex(key)>` — never a
  payload field, so a mod reaches only its own data and a hostile key cannot escape its subdir.
  Value is opaque bytes. A **hot-reloadable singleton**: `reload()` re-spawns the child in
  place keeping WeaveId/grant/role; the on-disk data is durable regardless.
- **Session-scoped, stated honestly.** The sender is the *ephemeral* runtime `WeaveId`, so a
  mod's subdir changes across host restarts — storage is **session-scoped**, not
  save-across-restart. `containment()` says so (the persistent-bind fs note). Persistent
  identity is the named successor phase.

### P2: the NetworkBroker (the powerbox, generalized)

The same pattern, a different capability — proving the powerbox is general, not bespoke to
storage. A mod that needs the network reaches it **only through a broker**, never as a raw grant.

- **Net is a recorded delta, never the floor.** Persistence is benign, so the floor grants the
  storage role to all; network is dangerous, so the floor grants the net role to **no one**. A
  mod reaches the NetworkBroker only via a host-recorded `GrantDelta.roles` entry (`"net"`) —
  closing the deferred roles extension. Critically, that delta grants the **role send-rule only,
  not `os_cap::Network`**: the mod stays OS-network-denied (no-interface netns), powerless to
  `connect()` directly; it reaches the network solely by messaging the broker.
- **The NetworkBroker** (an *ecosystem* Weave). Host-mounted via `mount_net_broker` at the TCB
  tier with `os_cap::Network` (so it runs in the host netns, real network), `FsAccess::None`,
  bounded resources, role `"net"`, permitted to reply `NetResponse` to any mod. Protocol:
  `NetRequest{host,port,payload}` → `NetResponse{ok,data}`. It **validates `host:port` against an
  allow-list** (v1: loopback only, exact-match then `inet_pton` the same string — no TOCTOU) and,
  for an allowed destination, performs **raw TCP** connect/send/recv (no HTTP/TLS/DNS, no
  dependency), replying to the **stamped sender** (the confused-deputy fix, identical to storage).
- **A higher-trust broker, stated honestly.** Network is binary — there is no OS-scoped network
  (no-interface or the whole net) — so the broker holds the **full host network** and
  per-destination scoping is **software-enforced by its own allow-list, not the OS**. Its blast
  radius if compromised is the whole network; its validation is therefore kept tiny and auditable.
  `containment()` says the truth: `network: granted — full host network …, NOT OS-scoped …; any
  per-destination limit is the holder's own software policy`. An OS-level tightening (nftables
  inside the broker's netns) is a named future hardening, not built.

### Named follow-ons (designed, not built)

- **OS-scoping the NetworkBroker** (nftables/firewall rules inside the broker's netns to limit
  its reach at the kernel) — the defense-in-depth for the higher-trust broker, whose scoping is
  software-only today. **Per-mod network policy** (different mods reach different destinations,
  keyed by `mail.sender()`) — v1 is a broker-level allow-list.
- **First-class persistent Weave identity** — the **save-file successor**: a stable identity
  (beyond the ephemeral `WeaveId` and the `.so` content-hash) that lets storage survive a host
  restart. Storage is honestly session-scoped until this lands.
- **Authority-transfer** — promoting a *different* Weave into a broker role (loud, user-gated,
  with a state handoff + transition-window quiescing). P1/P2 ship **reload-in-place only**.
- **The concurrent-instance version resolver** — multiple versioned holders of a role resolved
  by a send-time constraint. The role-target is real from day one; the resolver is a later
  addition with no migration. All brokers stay singletons.

### Status: built (P1 and P2)

Everything above ships, green in Debug and under ASan/UBSan (the policy suite runs under the
delegated scope). **P1 (StorageBroker):** the ask (`zen.Manifest` v2 + `zen.CapabilityAsk` +
`ZEN_ASK` + `declared_ask`), the floor-factory + content-hash-keyed JSON grant-record + the host
grant-API, role-addressing (registry, `send_to_role`, `allow_to_role`/`permits_role`,
authorize-before-resolve, singleton, reload-stable, `NoSuchTarget` on an unheld role), the
`Op::EmitRole` / `ZenHostApi::send_to_role` seam (sender stamped host-side, never on the wire),
the persistent-`WriteScoped` extension, and the out-of-process `StorageBroker`. **P2
(NetworkBroker):** the `GrantDelta.roles` extension + floor-denies-net wiring, and the
out-of-process NetworkBroker (`os_cap::Network`, `FsAccess::None`, role `"net"`, a software
allow-list, raw TCP). The powerbox is proven end to end with negative controls — **storage:**
mod-vs-mod scoping (A and B write the same key; each reads only its own, B never A's),
floor-without-disk (a `None` mod's direct open fails at the syscall level while its
broker-mediated put/get succeeds), ask-is-not-a-grant, reload-keeps-state; **network:** mediation
+ negative control (a net-denied mod reaches the allowed loopback listener *only* through the
broker — its own direct `connect()` returns ENETUNREACH), allow-list scoping (a disallowed
destination is refused, never connected), and floor-denies-net (a pure-floor mod, even one that
*asks* for network, is `CapabilityDenied` to role `"net"`) — plus broker-down → `NoSuchTarget`.
The powerbox is proven **general** (two brokers, two capabilities). What remains: OS-scoping the
net broker and per-mod network policy; the first-class-identity (save-file) successor;
authority-transfer; the version resolver.

---

## The Console: the doing-layer (Stage 1 engine + Stage 2 dataflow + Stage 3 UI-as-data)

Everything before this made the substrate impossible to misuse; the Console is the first thing a
*human* uses. It is a fully message-native bus participant — it discovers what's registered,
composes and gate-sends messages, and receives, indexes, and shows replies — so the Zen ideal
(apparatus disappears, intent remains) becomes something a person *does*, not just a property the
architecture quietly has. **Stage 1 is the engine + a deliberately plain terminal**; the
dataflow/reference layer (Stage 2) and the rich TUI (Stage 3) are named successors.

### The engine / frontend split (the durable spine)

The `ConsoleEngine` (`zen-console`) is **frontend-agnostic and fully testable with no terminal**.
It returns **domain data** — lists of weaves, field descriptors, received `Value`s, the buffer —
never formatted text and **never a widget tree**. The terminal is the first **replaceable skin**:
it formats that data as plain text. Care and correctness live in the engine; a GUI later inherits
it whole and only the skin is new. (UI-as-data is Stage 3, born from felt behavior; when it comes
it will describe **intent and relationship, never absolute position/size**.)

### Discovery-first; the console presumes nothing

The engine drives shapes it has never seen: `weaves()` enumerates live Weaves (id + accept-set);
`describe(name, version)` reads a shape's fields from the **registry**. So a person explores with
no prior knowledge, and a *new* Weave with *new* shapes is immediately drivable. The console bakes
in **no opinion** about what a shape means — the kernel holds grammar not answers, and the console
must too. This is how emergence is protected.

### Two layers, both free: the registry guides, the gate enforces

The console *knows the schemas* (it reads the registry), so it **guides at compose-time** — shows
fields and types, type-checks each value set. The **gate enforces at send-time**: the engine
assembles a `Value` (via `construct_blind`) and `send_as(console, target, reply_to=console)` admits
it against the target's accept-set at delivery. A field left unset slips compose-time and is
cleanly **gate-refused** (surfaced as the gate's verdict — `MissingField`), never silently
mis-sent. Compose-time makes it smart; send-time makes it safe — the same one-gate spine.

### Wildcard-accept (the one bus change): accept-any-known-shape, gated

A normal Weave's accept-set is specific `(name, version)` matches, so a reply of an unanticipated
shape would be refused `NotAccepted` — but the console must receive **whatever** a Weave replies.
`register_weave(…, AcceptMode::AnyRegistered)` is a deliberate, opt-in capability: on a shape it
does not explicitly list, the bus **resolves the payload's claimed `(name, version)` from the
registry and admits against that registered schema**. An *unregistered* shape resolves to null and
is still refused — an unknown shape reaches no one, not even the console. It widens the door set; it
**never skips the gate**, and a payload claiming a registered shape but carrying a different
content_id is still caught (SchemaMismatch). Ordinary Weaves (`Listed`, the default) are unchanged.

### The console as a participant, not an exception

The console is registered **in-process** as a raw `loom::Weave` (it does not dispatch by shape;
its generic handler buffers every received `Value` into an indexed buffer — `m1`, `m2`, …,
retrievable by index, the substrate the Stage-2 `$m1.field` references read from). It is the
**most-granted** participant — broad send (`Grant{}.allow_any()`), wildcard-accept, the observer
tap, and discovery (registry read) — but each is a deliberate **grant**, not a bypass: its sends go
through the *gated* `send_as` path, bounded by its grant, never the ungated root authority. The
operator's hands on the bus — powerful, still a participant. (Discovery via a direct bus query is
pragmatic here; "discovery as messages to a registry Weave" is a noted future purification.)

### Status: Stage 1 built

`zen-console`'s `ConsoleEngine` (discovery, compose-by-named-field, gated send, the reply buffer,
the tap), the console-as-Weave (broad grant + `AnyRegistered`), the wildcard-accept bus capability,
and a plain terminal REPL all ship, green in Debug and under ASan/UBSan. The four proofs pass
**frontend-free**: the **participant loop** (discover → gate-send → reply read back from the
buffer), the **gated-send backstop** (a malformed command → a clean `GateRefused`/`MissingField`,
no mis-send), **discovery on an unseen shape** (a shape the console code has no knowledge of —
listed, described, driven, its reply buffered), and **wildcard-accept** (a non-pre-declared shape
buffered, gated against its registry schema; an unregistered shape refused, not buffered). Named
successors: **Stage 2** — the dataflow/reference layer (`$m1.result_int`) and the assumption ladder
(named → positional → type-directed → prompt-on-ambiguous); **Stage 3** — the UI-as-data TUI
(panes, focus, the live guided-input redraw), describing **intent and relationship, never absolute
position/size**.

### Stage 2: the dataflow layer — references and the assumption ladder

Stage 2 turns the console from a sender-of-one-shot-messages into a **dataflow surface**. It is
the **text-mode prototype of the flowchart crown**: the logic built here is the dataflow brain the
later visual graph will *render*, not replace.

**A reference is a wire.** `$m1.count` reads a field of the buffered reply `m1` and routes it into a
new message — one message's output into another's input, by typing. This is exactly the wire the
visual flowchart will have you *draw*; doing it in text first **validates the dataflow model** before
it is committed to a canvas. Resolution lives in the **engine** (`resolve_ref` — standalone,
independently tested); the terminal only lexes the `$label.field` token. The label format is the
engine's (`buffer_at` produces `mN`); the resolver parses it back. A reference only ever **reads** an
immutable, already-gated `Value` from the buffer — it copies a scalar `Cell` out; it cannot mutate
the buffer. So `$m1.count` is *reading a field of a typed, schema'd Value you already hold* — safe by
construction. Errors are clean, never crashes: missing entry (`$m9.x`), missing field (`$m1.nope`),
a reference into an empty buffer. Stage 2 resolves **scalar** fields only (parity with compose);
nested/non-scalar (`$m1.items`, `$m1.a.b`) is a named future refinement.

**The assumption ladder (the durable dataflow brain).** `compose(target, name, version, args)` takes
a list of **arguments** — each a literal or a reference, each optionally **named** (`field=…`) — and
assigns them to the target shape's declared fields by a fixed resolution order, each rung a coherent
strategy:

1. **Named wins.** Every `field=value` is assigned to that field. Unknown or doubly-assigned field →
   a clean compose `Error`. The rest are **open** fields; the rest are **bare** args.
2. **Positional.** Bare args fill open fields in **declaration order** (bare[i] → open[i]),
   type-checked. If *every* positional placement type-checks, accept it (fewer bare args than open
   fields just leaves trailing fields open). If **any** mismatches, positional **fails as a whole**
   and falls through — this is what lets `send foo 5` do the right thing when foo's first field is
   Text and its second Int: the Text/Int mismatch fails through and type-directed lands the 5.
3. **Type-directed.** Each bare arg seeks the open field(s) its type fits. If every bare arg matches
   **exactly one** open field uniquely (no two args claim the same field), accept it. Otherwise (an
   arg matches *no* open field, or *several*) fall through to prompt.
4. **Prompt (`NeedsInput`).** Return the still-open fields and the unplaced args as **structured
   data** (`open_fields` + `unplaced`) — *never a printed string*; the frontend prompts, the operator
   re-composes with explicit `field=value`. After any successful rung, a still-open **required** field
   *also* yields `NeedsInput` (prompt rather than knowingly send incomplete). The ladder **never
   guesses on genuine ambiguity and never silently mis-sends.**

**Coercion** is narrow and predictable: a numeric *literal* widens among numeric field types
(Int→Float ok); Text only to Text, Bool only to Bool; a **reference matches its resolved type
exactly** (no coercion — wiring stays predictable). 

**The gate is the backstop, so the ladder guesses fearlessly.** The console knows the schemas, so it
guides at compose-time; the **gate enforces at send-time**. On `Ready` the ladder assembles the
`Value` (`construct_blind`) and gate-sends via the same `send_as` path — a wrong guess that ever
reached assembly would be a clean `GateRefused`, never a silent mis-send. In practice Stage 2 catches
a wrong-typed value (literal or reference) earlier still — at compose, as a clean `Error` — because
the engine knows both types; the gate remains the unconditional floor beneath it. This is the
week-one one-gate spine paying for the smart layer's boldness: the assumption logic can be as
aggressive as it likes because the safe layer refuses a bad guess loudly.

**The engine/frontend boundary holds exactly.** Reference *resolution* and the *ladder* are engine
logic, driven entirely with no terminal (the proofs below are all frontend-free). The terminal
(`console_term.cpp`) only **lexes** text to structured `Arg`s — each token to its narrowest type
(digits → Int, `d.d` → Float, `true`/`false` → Bool, `$mN.field` → reference, quoted or non-numeric
→ Text; **quote a numeric string to force Text** — `"5"` is text, `5` is Int), recognizes
`field=value` — and **renders** a `NeedsInput` result as a plain prompt. So Stage 3 replaces the skin
and inherits the dataflow brain whole.

**Status: Stage 2 built.** References resolve `$label.field` to a typed scalar `Cell` with clean
errors; the assumption ladder assigns named → positional → type-directed and returns structured
`NeedsInput` on genuine ambiguity; the terminal lexes narrowest-type literals, references, and named
args and renders the prompt plainly. Green in Debug and under ASan/UBSan. The proofs pass
frontend-free: the **reference round-trip** (a reply lands in `m1`; `$m1.seq` feeds a new message
that carries m1's value — reply→reference→send, the wire conducts), **each ladder rung** (named;
positional; **positional fall-through** rerouting a type-mismatched value to its unique type-directed
field; **prompt-on-ambiguous** returning `NeedsInput` and sending nothing), the **gate-backstop on a
wrong-typed reference** (caught with no mis-send), and **reference resolution errors** (missing
entry/field/empty buffer, all clean). Named successor: **Stage 3** — the UI-as-data TUI (panes,
focus, the live guided-input redraw), describing **intent and relationship, never absolute
position/size**.

### Stage 3: the UI-as-data layer — the renderer-agnostic widget tree

Stage 3 is the capstone of the doing-layer: the console's **own interface becomes data**. The
engine emits a **semantic widget tree** describing the console — and the *same tree* a terminal
renderer resolves to box-characters, a GUI later resolves to rectangles. This is where "the GUI
inherits the engine" stops being a slogan and gets real structural support.

**The bet, made structural: no geometry member exists on `Widget`, plus a compile-time tripwire.**
The widget types (now `include/zen/ui/tree.hpp` — lifted to the zen-ui target in Phase B; the
Stage-3 record below describes their console-era home) express **intent and relationship — never
coordinates or sizes**. There is no `x`/`y`/`width`/`height`/`row`/`col` member on a `Widget`, and a
name-based compile-time fence (member-detection `static_assert`s on ~10 coordinate spellings + an
`equality_comparable` guard) makes *adding* one of those names fail to build. This is **defense in
depth, not unrepresentability**: `int x;` fails to compile, but `int px;` compiles clean. The real
guarantee is the **closed, geometry-free member set** — no positional field exists to write; the
fence is one honest layer on top, not a proof that geometry is unrepresentable. Layout — resolving
intent into a medium's positions — happens **only in a renderer**.

**The vocabulary (general, used here only for the console).** A small semantic set:
arrangement `VStack`/`HStack` (children in a vertical/horizontal *relationship*, each carrying an
optional **weight grow-hint** — a *relative* hint, never an *absolute* size) and `Region{title,
child}`; content `List`
(selectable lines + a `selected_index` that is an index *into items*, never a y), `Log`
(append-oriented), `Text`, and `Field` (an input affordance carrying an **engine-produced
guidance hint**). Cross-cutting: an **overflow policy** (`Scroll`/`Wrap`/`Truncate`/`Grow`) and a
**focus marker** — both resolved per-renderer in that medium's own units. A `Widget` is a single
value type with a defaulted `operator==`, so the whole tree is **one value**: trivially asserted
in tests and diffable by `region_id` for retained-mode partial redraw.

**One real renderer, plus a test-only proof of renderer-agnosticism.** The engine library builds the
tree from its **public domain data** (`weaves()`, the reply buffer, the tap, registry-derived
guidance) — a fresh value each call, renderer-agnostic, fully testable with no terminal. There is
**one real (production) renderer**, the full-screen **TUI** (`src/console/console_tui.cpp`), which
lays the tree out to a character grid (hand-rolled ANSI + POSIX termios raw mode, **no ncurses, no
new dependency**) and is the **only** place positions, sizes, and cells exist. Alongside it is a
**~50-line test-only outline walk** (`render_outline`) that consumes the *same tree* and emits a
plain indented outline carrying no medium — a **renderer-agnosticism *proof*, not a second
production renderer**. It deliberately ignores `weight` (proving that hint is not tree content); the
TUI resolves `weight` **as a relative size in cells**. Two consumers of one tree is the evidence
that a GUI later is just another renderer of the same description — like a screen reader and a
browser over one HTML DOM.

**Engine-produced guidance, the live guided-input.** As the operator types a partial command
(`<weave id> <Shape> <version> [args]`), the `Field`'s hint shows the **next choice** —
`guidance_for(partial)`, derived by the engine from the registry + the partial input (empty →
"choose a weave"; an id → its shapes; a shape+version → its fields). The guidance is
engine-produced, so it is renderer-agnostic (the GUI shows the same string as a dropdown). The
Stage-1 show-then-choose, now a live interaction.

**Retained-mode, message-driven dirty-tracking (the Zengine idea's home).** The change signal is
**bus messages**: the single bus observer (`record_tap`) sets per-region dirty flags as events
arrive during `pump()` — `buffer` on a reply delivered to the console, `weaves` on a Weave
dying/reviving, `tap` on any event — drained by `take_dirty()`. A reply arriving patches the
buffer pane; a tap event patches the tap log. (The current single-threaded TUI consults the flags
then full-repaints each frame, which is correct and cheap at terminal sizes; the flags are the
mechanism a future async/multi-weave or GUI renderer uses for true per-region partial redraw —
minimal cell-diffing is the named refinement.)

**Symmetric input seam.** Input is factored as renderer-agnostic semantic **actions**
(`FocusNext`/`FocusPrev`/`SelectUp`/`SelectDown`/`Activate`/`Edit`/`Backspace`/`Submit`/`Cancel`)
that a `ConsoleUi` controller applies; the raw-key→action table is the *only* terminal-coupled
input code and lives in the TUI alone. So a future GUI inherits the **input** abstraction too,
symmetric with the output tree, and the interaction is driven headlessly by scripted actions in
tests.

**Status: Stage 3 built.** The widget vocabulary, the engine's tree emission + guidance, the
headless outline renderer, the `ConsoleUi` controller + semantic-action input, and the full-screen
termios TUI all ship, green in Debug and under ASan/UBSan. The proofs pass **frontend-free**: the
**tree's semantic structure asserted with no renderer** (a `VStack` of a Bus `Region` [`HStack` of
a Weaves `List` + a Tap `Log`], a Buffer `List` with `m1`, a Compose `Field` carrying the engine's
guidance); **no geometry member on `Widget`** (the closed member set + the name-based compile-time
fence on ~10 coordinate spellings, plus a runtime structural-`==` proof — defense in depth, not
unrepresentability); **one tree, two consumers** (the test-only outline walk reflects the same tree
the TUI lays out, mutates nothing, ignores `weight`); **engine-produced guidance advancing with
input**; a **bus message driving a buffer-pane
update** (a reply dirties + grows the buffer; a refusal dirties only the tap); and a **TUI smoke
test** (scripted actions move focus, compose a guided send, buffer the reply; an ambiguous command
surfaces a `NeedsInput` prompt region and sends nothing). Named successors: a **general
Weave-emitted-UI protocol** (any Weave describes its own UI as a tree the console/host renders —
the vocabulary is built general *for* this, but the console validates it first); the **GUI
renderer** (the same tree to pixels); **geometric/canvas UIs** (geometric by nature — they bridge
at the sandboxed-Weave *fabric* level, not the semantic-rendering level); and the **result-graph
buffer** (Stage 2's seam — the flat `m1,m2…` buffer becoming an addressable result-graph).

*(The vocabulary described above has since been **evolved in place** by the UI-Builder component
phase — interaction intent replacing `focusable`, the `Slot` kind, bindings/routes, and
serialization as gated Values. This section stays as the Stage-3 record; the current vocabulary
is §"The UI-Builder component vocabulary".)*

### The terminal-backend seam (cross-platform frontends)

The console's engine and UI-as-data layers are pure portable C++; the *only* platform-specific code
in the whole console was the TUI's terminal control. That is now extracted into a **terminal-backend
seam** — a small `TerminalBackend` interface (`src/console/terminal.hpp`, in the TUI executable, NOT
the engine library) the shared TUI talks to: `is_interactive` / `size` / `read_byte` /
`read_byte_timeout`, plus a RAII raw-mode ctor/dtor, with `make_terminal()` the one symbol selected
per platform. After this, `console_tui.cpp` is **platform-header-free** (no `termios`/`unistd`/
`windows.h`); every platform difference lives in a `terminal_*.cpp`, and `zen-console` stays
terminal-free.

- **POSIX backend (`terminal_posix.cpp`), preserved by moving.** The existing termios raw mode (the
  `isatty` gate + the `atexit` restore), `ioctl(TIOCGWINSZ)` size, the blocking byte read, and the
  ESC-continuation VTIME-grace read (now `read_byte_timeout(ms)`) moved behind the interface
  **behavior-identical**. The Linux path is preserved by construction, not rewritten — the safety
  property that makes this low-risk.
- **Windows backend (`terminal_windows.cpp`), by-the-book, Josh-verified.** The Win32 Console API
  (no new dependency): raw mode via `GetConsoleMode`/`SetConsoleMode` (clear line/echo/processed
  input, set `ENABLE_VIRTUAL_TERMINAL_INPUT`; set `ENABLE_VIRTUAL_TERMINAL_PROCESSING` on output —
  so the *same* ANSI renderer and the *same* escape-sequence key parsing run unchanged); `size` via
  `GetConsoleScreenBufferInfo`'s `srWindow`; `read_byte` via `ReadFile`; `read_byte_timeout` via a
  `WaitForSingleObject` deadline loop that `PeekConsoleInput`-drains non-key-down records (key-up /
  focus / resize signal the handle but yield zero VT bytes, so a naive wait-then-`ReadFile` could
  block past the bound); a graceful VT-unavailable fallback (report not-interactive, never pretend).
  The renderer is **ASCII-only** on purpose — a non-ASCII byte (e.g. a `—`) would be decoded by the
  console code page as mojibake; there is no UTF-8/code-page handling and none is needed.
- **Build-verify division (the cpu-seam arrangement, since narrowed).** The Linux build + POSIX
  backend are *proven* green on the WSL box (ctest 20/20 Debug + ASan/UBSan). The **portable Windows
  subset is now verified natively too** — built with CLion's bundled MinGW (cmake + ninja + g++ 13.1)
  into a separate `build-win`: `terminal_windows.cpp` compiles clean under the full `-Werror` set,
  and the portable suites pass (15/15 ctest, the **console** suite 152/152 incl. every Stage-3
  UI-as-data proof). What remains *Josh-verified in CLion* is only the **interactive raw-mode TUI
  behavior** (arrow/Tab focus, Enter/Backspace edits, resize reflow, bare-ESC-vs-arrow disambiguation,
  clean exit) — which a non-interactive shell cannot drive.
- **Trusted-code, in-process, no containment surface.** The Windows TUI runs **in-process Weaves
  only** — there is no `IsolationHost` in the TUI build (the sandbox is Linux/WSL, by design), so
  this phase adds **no out-of-process hosting and no containment surface**. The Windows TUI is the
  trusted-code console (you trust what you run); containment honesty and the relocate-to-WSL flow
  belong to the WSL-bridge phase, where hosting actually enters.

**CMake platform-gating.** `zen-kernel`, `zen-isolation`, `zen-weave-host`, and the Linux-only test
sources/weavelibs/suites (`test_kernel`, `test_capabilities`, `test_policy`, `test_isolation`, the
loadable `.so`s, the `isolation`/`policy`/`all` ctest entries) are gated behind `if(NOT WIN32)`, so
the portable subset (core, switchboard, console, both terminal frontends, and the portable suites
incl. **console**) configures on Windows. On Linux every gate is taken — the full target set and
full suite build and run exactly as before; the gating is invisible. (`test_capabilities` is gated
Linux-only because it exercises capability enforcement *through the kernel loader* — it includes
`<zen/kernel/kernel.hpp>` and loads a real `.so` — so it cannot build without the kernel + dlopen,
despite "capabilities" reading as portable.)

**Seams appreciated (latent power, not debt).** This boundary is the **hook the next two frontends
plug into**, which is the whole point of doing it cleanly: the **WSL remote console** becomes
*another `TerminalBackend`* (a socket transport, not a parallel codebase), and the **GUI** becomes
*another renderer* of the same widget tree — both behind the same kind of seam. Two shape-notes for
the remote phase, named now so it lands as a hook and not a refit: **output is not yet behind the
seam** (the shared `draw()` writes the frame to stdout — fine for POSIX + Windows VT; a socket
backend will route output over the wire, the natural extension being a `write()`/`flush()` pair),
and **the TUI owns the I/O loop synchronously** (blocking `read_byte` + a timeout-read maps cleanly
onto `recv`/`poll`, but a fully async transport would invert loop ownership). Separately, the
platform gate makes the **native-Windows Weave-loading** port a *visible* seam (`dlopen` →
`LoadLibrary` in the kernel) — hooked right there, but deferred to a trigger that likely **never
fires**, because WSL-hosting dominates native execution for anything needing the sandbox.

---

## The remote-operator bridge — the WSL crossing (the first thing across the host boundary)

The console's engine is a bus participant; a *remote* operator is the **same pattern as an
out-of-process weave** — a proxy that *is* a participant, bridging a socket to the bus — pointed at an
**operator** instead of a hosted weave. The bridge makes a Windows console drive a WSL-hosted bus
across the real kernel boundary, and it is the first time anything in Zen crosses the host boundary as
a participant. Built **complete**; both altitudes (the local mechanism and the real Windows→WSL
crossing) are **proven**.

### The architecture: fat-client + an operator-protocol of messages

A remote console **cannot hold a `Switchboard&`** across a socket, so the engine runs **client-side**
and its three host interactions — discovery, the tap, and sends — become an **operator-protocol of
framed messages** the host *answers* and *streams*. This is the twice-deferred *discovery-as-messages*
purification, pulled in now by completeness (a complete remote operator needs all three over the wire,
and the only honest way is as messages). It **purifies invariant 1** — with a precise scope. A remote
operator's **sends** are value-carrying: they meet the one gate at admission on the receiving (host)
side (`send_as` re-admits the assembled Value, exactly as an out-of-process weave's Emit does). The
protocol's **control frames** (discovery requests, the tap subscription) are *not* gated Values — they
are `Cursor` bounds-checked wire frames the host answers; and **host-side discovery stays the bridge's
own granted direct reads** of the registry (the same broad grant the in-process console holds), not a
bus message it sends to itself. So "discovery becomes messages" means the *operator* asks over the wire
and the host answers on its behalf. The deeper purification — **discovery as a literal bus weave** (a
discovery participant the operator queries by an ordinary *gated* message) — is newly named here and
pulled by nothing yet.

- **The frontend is unified across transports.** The console's frontend surface was extracted into a
  `loom::Console` interface (`weaves`/`describe`/`compose`/`buffer*`/`tap`/`take_dirty`/`pump`); the
  in-process `ConsoleEngine` implements it directly, and a client-side `RemoteConsole` implements it
  over the socket. The **same** `ConsoleUi` + the **same** shared renderer (`tui_render.cpp`) drive
  both — `zen-console-remote` is `zen-console-tui` with the engine behind a socket and the loop
  inverted. The assumption ladder (`run_compose_ladder`), the reference resolver (`resolve_ref_from`),
  and the shape describer (`describe_schema`) are shared free functions, so the ladder is not
  duplicated across transports. The in-process console stays **direct** (no serialization) — the
  unification is at the interface, not forced through bytes.
- **The send is an out-of-process `Emit`.** Compose runs client-side against a schema fetched over the
  wire (`Describe` → an encoded `Schema`, reconstructed with the schema codec — the IPC currency) and
  the local reply buffer (filled by `Delivered` frames); the assembled message is serialized and
  shipped as a `Send` frame the host **re-admits through the one gate and `send_as`-stamps** — exactly
  the `IsolationHost::handle_child_frame` Emit path, pointed at an operator.

### The connect-authority chokepoint (model A, hook for B/C)

The WSL host and the connecting side are **one trust domain the operator controls**, so reachability
of the bridge socket **is** authority — a party that can reach it holds operator power, exactly as a
local operator at the host does. Every connection's acquisition of power routes through **one
function**, `authorize_connection(connection) → OperatorGrant`, which **today returns full grant,
always**. The seam is that function **plus the one call in `accept_new` that consumes its result** —
the `register_weave(std::move(proxy), Grant{}.allow_any(), AnyRegistered)` line. Together they are the
*entire* connect-authority future-proofing: when the trigger fires — *model B* (a bearer capability
token: possession-is-authorization, ocap-style, no identity) or *model C* (per-connection
graduated/differential grants that narrow the `allow_any()` per operator, **where "Weaver" is born**,
at the first relation needing differential persistent authority — i.e. the first place authorization
needs *authentication*, the identity phase) — **only those two points change**, and the operator
wielding the grant is untouched. Model B makes the *unauthorized-connection forge* sayable: its pin
must prove a connect that fails the token registers no proxy and processes no frame. (Note under model
A: operator proxies are hidden from discovery but *are* addressable by their small-integer `WeaveId`s —
harmless while every operator is the same principal, but a cross-principal channel the identity phase
must treat as surface.)

### Provenance: the sender is stamped from the connection, never the wire

The proxy stamps the operator's sender **from the connection** (the proxy's `WeaveId`), never from the
wire payload — exactly as a child's sender is stamped from its link. A remote operator may send *any*
message (it holds operator grant), but its **sender is the bridge's stamp**, so provenance stays honest
even for the most-granted participant; `reply_to` is likewise forced to the operator (so its replies
route back to it, and a forged `reply_to` cannot redirect them — the confused-deputy guard, the same
posture as `EmitRole`). The `Send` frame carries `wire_sender`/`wire_reply_to` fields the bridge
**always overwrites** — an honest client sets them to 0; a malicious one forges them, and the stamp
wins. This is pinned by a test that **forges the wire frame** a raw client manufactures (the honest
`RemoteConsole` cannot express the attack) and proves the connection-stamped id is what the bus saw —
the unsayable-attack discipline, mirroring the `forge_client` confused-deputy pin.

### Loop ownership: an event-driven single-threaded multiplexer (NOT threaded dispatch)

The transport drives. Input is **one event source among several**: a socket delivers bytes when the
*far side* decides, the tap pushes events unbidden, and **disconnect is an event with no `read` to
return it** — an absence the loop must notice. A synchronous block-on-read cannot represent any of
that. The bridge server is a **single-threaded multiplexer**: `bridge_wait_readable` (**`poll` on
POSIX / `WSAPoll` on Winsock** — no `FD_SETSIZE` ceiling, so a reconnecting fd-hog cannot walk the
server into UB; *greedy* is in the threat tier) waits for any source, then `step()` accepts / drains /
dispatches / pumps / flushes / reaps. The client multiplexer waits over {terminal input, socket} the
same way (POSIX `poll`; the Windows `WaitForSingleObject` deadline-loop — the generalization of
`read_byte_timeout`). **Critically, this is the client's readiness-to-receive, not bus concurrency:**
the bus stays single-threaded FIFO, operator sends enter through the one gated path, and `pump()`
processes them in order — no threads added, the reentrancy guard untouched. (Multi-threaded dispatch
remains a separate, deferred maybe/never seam; conflating it here would be a real error.) **Disconnect
is handled as an event** — a closed/killed peer surfaces as a readable-then-EOF socket, and
`reap_dead()` unregisters its proxy gracefully, proven by both a closed socket and a real `SIGKILL` of
a forked peer process.

**Squared edges (bounds stated + pinned; no dark fates).** A stated **connection cap**
(`kMaxOperatorConnections = 32`): `accept_new` sheds past it (accept-then-close, counted by
`declined_count()`), so a reconnecting fd-hog is contained and the shedding is observable, not silent.
The **handshake is load-bearing** — a frame other than `Hello` before `Hello` completes *severs* the
connection (marks the channel failed; the existing `reap_dead` path takes it), anti-Postel. The three
payload-level drops on the `Send` path (malformed header / unknown schema / gate-refused) — the only
dark fates, since everything after `send_as` is tap-visible — now emit a **`SendRefused`** frame
(per-frame, non-fatal), which the client surfaces as a `"BridgeRefused"` tap kind (honestly *not* a bus
event — the send never entered the bus). And the client **bounds what a host can make it hold**
(`kMaxPendingDelivered = 64` on the fetch-the-reply-schema queue, drained on `SchemaNone`) — the
`kMaxBacklog` principle applied to a more-trusted peer. The **observer stays copy-only**: `on_tap`
copies event fields (safe mid-`pump()`), but the weave-list refresh — a bus *registry read* — is
**deferred** to after `pump()` returns (a `needs-push` flag drained in `step()`), because
reads-during-dispatch is not a *stated* Switchboard guarantee (the in-process `record_tap` copies only;
the bridge does not lean on an unstated bus property).

### Output behind the seam; the transport

`TerminalBackend` gained a `write()`/`flush()` pair (output behind the seam, was a direct `std::cout`);
the POSIX/Windows backends write stdout. The socket-*frame*-output backend it enables is **hooked, not
built** — no consumer yet (the remote console renders client-side off the operator-protocol and draws
to its own real terminal). The transport (`BridgeChannel`) mirrors the proven isolation `Channel`
(non-blocking, length-framed, bounded, EOF-observable) but is **portable**: one `#ifdef` splits
recv/send/close/set-nonblocking and the socket setup between POSIX and Winsock, because the Windows
console connects across the boundary. **AF_UNIX** is the fast local (WSL↔WSL) transport; **AF_INET
127.0.0.1** is the real Windows↔WSL crossing (WSL2 forwards localhost). The protocol reuses the
portable wire primitives (`put_u*`, `Cursor`, `kMaxFrameLen`).

### Honest containment

The bridge's containment honesty states it plainly: **the security boundary is the reachability of the
bridge socket** — a party that can reach it holds operator power; securing that reachability (don't
expose the bridge to an untrusted network — it binds `127.0.0.1`) is a **deployment responsibility**,
not something the bridge enforces, and it does **not authenticate connectors**. The same honest posture
as "a local operator is trusted," stated for the remote case. Threat tier unchanged: **abuse, not
escape**.

### Status: built and proven (both altitudes)

Targets: `zen-bridge` (portable: transport + server + `RemoteConsole`), `zen-bridge-host` (the WSL
demo host), `zen-bridge-probe` (a non-interactive crossing prover), `zen-console-remote` (the
interactive console), `zen-tui` (the shared renderer + terminal seam, linked by both consoles). Proven:
the **local mechanism** — the operator-protocol (discovery + tap + send as messages), the event-driven
multiplexer, the proxy-participant + connection-stamped sender against a forged frame, and
disconnect-as-an-event (close + a real `SIGKILL`) — green on the `bridge` suite (12 cases / 135
assertions) **Debug + ASan under the scope, `-Werror` clean**, *and* natively on Windows via MinGW (10
cases there — the fork-kill and AF_UNIX cases are POSIX-gated). And the **real Windows→WSL crossing**: a MinGW-built Windows
process (`zen-bridge-probe.exe`, and the interactive `zen-console-remote.exe`) drives the WSL-hosted
bus across the boundary — discovery + describe + gate-send + the echoed reply + the tap + a graceful
disconnect — proven end-to-end (a step up from the prior phases: no Josh-verified-only half, except the
interactive raw-mode *input* on Windows, which stays the established CLion-verified division).

### Relocation — argued to fall out (true by construction, not yet pinned); the one primitive it needs

Relocation (*"this weave is `uncontained` here → host it in WSL → it runs sandboxed without skipping a
beat"*) was **not built** — this phase **argued the litmus** against what the bridge made real (an
argument true by construction, *not yet pinned by a test*). The honest decomposition: relocation is (a)
snapshot the weave's state — **exists** (`IsolationHost` already host-owns snapshots; state crosses as
bytes); (b) ship it across the boundary — **now real** (the bridge proved the cross-kernel ship); (c)
revive in a WSL-hosted sandbox keeping the same identity — **exists** (`mount`/`respawn_and_revive`,
identity-preserving); and (d) **re-point everyone messaging it to the new location**.

(d) is the seam — and reading the Switchboard sharpens the earlier "zero new runtime" claim to **one
honest primitive, not zero.** A forwarding proxy on the old bus would need to sit *behind the departed
weave's `WeaveId`* so existing senders reach it — but **no Switchboard operation binds a replacement
participant behind an existing id**: `register_weave` always mints a fresh id (`WeaveId
id{next_weave_id_++}`), and `reload`/`swap_state` are id-preserving *but* `admit` the candidate against
the record's **unchanged** `state_schema` (a divergent shape is gate-refused, never bound). So
relocation's (d) costs exactly **one new primitive** — either a `WeaveId`-rebind (put a new
participant, possibly with an evolved state shape, behind an existing id), or **location-transparent
addressing** (invariant 3, contract-addressing pointed at *location*: senders address by a stable
contract-address that resolves to wherever the weave currently is, so the id need not be preserved at
all). The latter is the cleaner long-term answer, named as the seam **relocation** will pull, not built
speculatively. The confirmed crossing is the latent capability relocation cashes in; its litmus is now
**behaviorally checkable**, not merely reasoned-about — *one new primitive, then pinned.*

### Seams appreciated (latent power)

The **chokepoint** hooks B/C (graduated operator authority — the grant model generalizing to people);
**location-transparent addressing** hooks relocation and later multi-runtime coexistence (a Python
weave, a GUI weave, and a sandboxed weave coexisting by address regardless of where they run); the
**event-driven loop** is the ontology-fix the GUI, relocation, and any async transport inherit with no
refit (paying it here, while the only consumer is the bridge, is why they are cheap later); the
**socket-frame `TerminalBackend`** (host-rendered output over the wire) is hooked-not-built; and a
neutral shared `wire.hpp` (the two protocols' framing primitives) is a clean future factoring.

---

## The Poke weave — live inspect/manipulate under the `ZEN_EXPOSE`/`ZEN_HIDE` access model

The first debugging capability, built without a debugger: **poke by message, never past the
weave boundary.** The Poke weave is an ordinary participant with no special powers — it
inspects and manipulates other weaves by *sending them messages*, and the enforcement lives
in the **target's own construction layer**. A weave that didn't expose something cannot be
poked into it; that is the safety property, not a limitation. (The "call a function directly
with provided values" power is real but deliberately elsewhere: it arrives with the
auth/identity phase as *authority + inclusion* — "having a, not being a" — never as a
privileged debugger. Message-poking is *a* poke path, not the only one.)

### The access model (`ZEN_EXPOSE` / `ZEN_HIDE`, in `weave/shape.hpp`)

Two author-control tags around a sensible default. **With no tag a field is read-exposed
(inspectable) and write-hidden (not manipulable).** `ZEN_EXPOSE` opts *in* to write;
`ZEN_HIDE` opts *out* of raw read — the value becomes message-only ("don't scrape my raw
counter mid-update; ask me"). Each tag has one spelling and two scopes: field scope replaces
`ZEN_FIELD` in the registration list (`ZEN_SHAPE(S, 1, ZEN_EXPOSE(rate), ZEN_HIDE(raw),
ZEN_FIELD(label))`), and **whole-state scope** is a bare `ZEN_EXPOSE();`/`ZEN_HIDE();` inside
the state struct (the `ZEN_SHAPE` type — *not* the weave class; `WeaveBase` `static_assert`s a
misplacement, which would otherwise silently no-op — a fail-open for `HIDE`) —
**whole-state IS apply-to-all** (`shape_access_bits()` OR'd onto every field at derivation;
one primitive, two spellings — pinned by a test comparing the two spellings' derived tables).
Whole-state detection is **value-checked, not presence-checked** (a nested `requires`): a
`zen_expose_all = false`, or a field merely named like the flag, does *not* widen access — the
dangerous direction never fails open (pinned). The bits ride `FieldEntry` and are
**deliberately invisible to `build_schema()`**: a tagged struct derives a byte-identical
schema — same content-id — as an untagged twin (pinned against a hand-built `SchemaBuilder`
equivalent). The tags are
authored metadata *beside* the shape, never part of wire identity; they seed the
introspection system without touching invariant 3.

**The honesty boundary (load-bearing): the tags govern value access only.** Neither tag can
hide a field's existence, name, type, or its own tag-state — `access_of<T>()` returns every
field, nothing filters it, and hiding a value is itself declared, inspectable metadata.
There is no way to make state invisible; a weave cannot lie about what it is.

### The doors (substrate-answered, in `WeaveBase`)

Every woven weave gains four **substrate doors** appended to its accept-set —
`zen.PokeDescribe` → `zen.PokeStructure` (identity + *every* field's name/type/tag-state),
`zen.PokeRead` → `zen.Result`|`zen.Refused`, `zen.PokeWrite` →
`zen.Ack`|`zen.Refused`, `zen.PokeResetState` (default-construct the state; requires
*every* field writable) — the replies are the **standard shapes** (see "The standard reply
shapes" below), not a poke dialect; only `zen.PokeStructure` stays bespoke — answered by
`handle()` **before maker dispatch**, from pure
standalone-tested functions (`poke_structure/read/write/reset` in `weave/poke.hpp`) over the
declared access model. **A `WeaveBase` weave cannot intercept them**, and that is enforced,
not merely asked: `handle()` and `accepted_schemas()` are `final` (a `Self` subclass cannot
override them to answer pokes dishonestly or drop the doors from its advertisement), and a
`static_assert` refuses the protocol shapes — or an inheriting alias of one — in
`Accept<...>`. So an answered structure is trustworthy. (A maker who wants raw dispatch
implements `loom::Weave` directly — trusted in-process code that then *transparently
advertises no doors it will not answer*; the dishonest case this closes is "advertise the
doors via `WeaveBase`, then lie in the answers.") Every refusal is an honest answer carrying
its reason — never silence (the one exception: a poke with no reply address and no sender
has nowhere to answer, by the asker's choice).
Values cross the boundary **as text**, converted against the field's *declared kind* at the
target (`to_chars`/`from_chars`, exact and locale-free): a bad literal is a clean refusal,
never a mis-write, and the shapes stay console-composable today. **Scalar fields only this
phase** — a non-scalar field is fully visible in the structure, just not message-read/
written yet.

**No path around the gate.** A poke answer is an ordinary *gated* send checked against the
answering weave's grant: `mount()` adds `allow_poke_answers` (plain `Grant::allow_to_any`
rules — the same kind an `Emit<...>` declaration confers) alongside the Emit-derived grant,
while `mount_granted` stays sovereign — an ungranted weave's answers are `CapabilityDenied`
at delivery, visible on the tap (pinned: the substrate's own answering machinery bows to the
host's grant). This is a real, if minor, grant the maker's own code shares (it may now emit
those four answer shapes too) — precisely as declaring `Emit<Result>` would grant it —
and it is **inert** not by enumerating today's consumers but by the **standing consumer
obligation** (recorded in `standard_shapes.hpp`, since the standard replies are universal
vocabulary any granted participant can emit): a consumer of a standard reply matches each
arrival against its own outstanding requests by **correlation and bus-stamped sender**
(`loom::relay` implements exactly this wall) and treats an unsolicited answer as data at
best. Makers whose own code replies with a standard shape still declare it in `Emit<...>`
— the ride-along grant is the answering machinery's, and the carve-out (an undeclared
standard-reply emit *is* deliverable under `mount()`) is pinned as known, not latent. A finer
*per-send* principal (separating the substrate's answer from the maker's own sends under one
`WeaveId`) is the sub-weave-identity seam the auth phase pulls, not this one.
`emitted_schemas()` remains the maker's declaration alone. Host-side authority
(`snapshot_bytes`, `swap_state`, root sends) is untouched and out of scope: the host owns
its weaves; `ZEN_HIDE` binds the *message plane*.

### The Poke weave (`weave/poke_weave.hpp`)

An ordinary `WeaveBase` participant: accepts four operator **commands** (`zen.PokeInspect/
PokeGet/PokeSet/PokeReset{target,…}` — a command names a third party; a protocol message
arriving at a weave means "you"), forwards the matching protocol shape to the target with a
fresh `seq` correlation, and relays the answer to the original asker (taken from the
command's `reply_to`-else-stamped-sender, never its payload) with the original correlation.
That forward/relay dance is the **standard request/reply relay pattern** (`weave/relay.hpp`)
— `loom::forward`/`loom::relay` over a `loom::RelayState` — and the Poke weave's whole state
*is* the relay bookkeeping; its handlers are one-liners.
**A relayed answer must come from the poked target:** the relay matches pending pokes by
correlation *and* the bus-stamped sender — an ordinary participant *can* emit a
perfectly-shaped forged `zen.Result` (sayable through the honest API, and the test says
it), but it cannot speak *as* the target, so the forgery is dropped and the pending poke
stays parked. Pending pokes are honest state (a `zen.RelayPending` list — itself
poke-inspectable), bounded at 64 with oldest-shed; a forwarded poke whose answer never
comes (no such target, a raw non-woven weave with no doors, an answer-denied grant) parks
until shed, with the underlying refusal on the tap — a participant cannot observe the fate
of its own sends today, a named seam this phase does not build.

Driven **end-to-end from the existing console with zero console changes** (pinned): the
operator composes `zen.PokeInspect 3` at the Poke weave, the structure lands in the buffer
with the hidden field visible *as hidden*; `zen.PokeSet` on an exposed field acks and the
write is live; on an un-exposed field the refusal comes back with its reason; the hidden
value refuses a raw read while the target's own front-door query still serves it computed —
**the message interface stays sovereign; the tags never touch the front door.** The
protocol shapes are ordinary messages: the console can also poke a target directly, no Poke
weave involved (pinned by stamped-sender).

### The standard reply shapes (`weave/standard_shapes.hpp`)

One vocabulary for the answers every protocol sends, so a reader recognizes a reply
instantly instead of re-learning each protocol's dialect. **The rule (the
least-complete-information razor, applied to replies): standardize the contentless and
simple-payload replies — ack / refusal / result; keep a bespoke reply type ONLY where the
reply carries genuinely protocol-specific structure whose absence would break the image.**
Test each field by its absence: if removing it leaves the reader *confused*, it is
load-bearing; if merely *less-informed*, it is sediment.

- **`loom::Ack`** (`zen.Ack`) carries **nothing** — "done." A reply's correlation already
  ties it to its request, so the correlation carries *what* was done. Zero fields is the
  complete image.
- **`loom::Refused`** (`zen.Refused`) carries **one field** — "no, and here is why." A
  refusal without its reason is an incomplete image; the reason is written self-contained,
  for a stranger. (Deliberately *not* named `Error`: `loom::Error` is the gate's admission
  error — a malformed claim, a fault — while a `zen.Refused` is a deliberate answer by
  policy; the two must not read as one thing. "Refused" is also the word the protocols had
  already independently converged on — `PokeRefused`, `SendRefused` — canonized.)
- **`loom::Result`** (`zen.Result`) carries **the payload**, as text — "here is what you
  asked for." (Not named `Value`: `loom::Value` is the substrate's value type itself.)

They are ordinary shapes — registered, gated, content-id'd, hand-registered with the
substrate's `zen.` prefix — and every protocol using them derives the same schema from the
same struct: one content-id everywhere, by construction (pinned against hand-built twins).
Because the vocabulary is universal, it comes with a **consumer obligation** (recorded in
the header): accepting a standard reply obliges matching correlation **and** bus-stamped
sender against your own outstanding requests; an unsolicited reply is data at best.
Producers still declare standard replies in `Emit<...>` like any shape they send.
The poke protocol's original `PokeValue{field,type,value}` / `PokeAck{op,field}` /
`PokeRefused{op,field,reason}` collapsed into them: the `op`/`field`/`type` members restated
what the reply's correlation, the request itself, and the structure (`zen.PokeDescribe`)
already carried. `zen.PokeStructure` **survives by the razor** — a weave's full structure
(identity + every field's name/type/tag-state) is genuinely protocol-specific; remove its
fields and the reader is *confused*, not merely less-informed.

### The request/reply relay pattern (`weave/relay.hpp`)

The command→forward→answer dance every fronting weave repeats, expressed once:
`loom::forward(mail, state, target, req)` stamps a fresh `seq` on the forward and remembers
who asked (from routing metadata, never the payload) in a bounded `loom::RelayState`;
`loom::relay(mail, state, answer)` sends a correlated answer on to the original asker —
**only if its bus-stamped sender is the forwarded-to target** (the anti-forgery wall,
written once in the substrate). It carries only what the pattern needs — the correlation
bookkeeping and the two moves; no expected-reply-type registry, no timeouts, no knobs — and
it removes bookkeeping, never the contract: the maker's `Accept<...>`/`Emit<...>`
declarations stay visible on the weave.

### The weave-layer files (the naming fix)

Three files once read as "weave.hpp" at three layers; the raw contract is now
`switchboard/weave_contract.hpp`, so the layers are legible at a glance:
`zen/switchboard/weave_contract.hpp` — the raw `Weave` contract (the frozen five-method
virtual ABI the bus dispatches through); `zen/weave/weave.hpp` — the `WeaveBase` authoring
sugar (what a maker writes against); `zen/weave.hpp` — the umbrella include.

### Scope + seams (hooked, not built)

The **standard-shape follow-on** (sharpened by a whole-repo shape audit): the storage
broker (`StorageValue{value: Bytes}` — a bytes payload, so it stays bespoke by the rule
unless/until a bytes result earns its way into the standard set; its empty-bytes-means-
absent sentinel conflates two images — a *stored empty value* and an *absent key* are
indistinguishable — and the absent case could become an honest `zen.Refused`; **`StoragePut`
has no reply at all** — a persistence write whose disk failure the client never learns →
`zen.Ack`|`zen.Refused`), the net broker (`NetResponse{ok, data}` — the ok-flag is an
inline refusal-bit carrying **no reason**: an allow-list refusal and a TCP connect failure
are byte-identical replies, an incomplete image → split into a result|`zen.Refused{reason}`),
and the bridge's `SendRefused` (a socket-layer framed op, not a bus shape — adopting the
vocabulary there is a design question, not a mechanical swap) all reinvent ack/refusal
today; migrating them is named, mechanical follow-up, deliberately not this phase. The
audit's sharpest find is the **kernel control weave** (`kernel/control.hpp`): the canonical
dangerous surface performs load/reload/unload and **discards the outcome**
(`(void)kernel_->load(...)` — `LoadResult{ok, id, error}` / `ReloadResult`'s
version-mismatch never reach the sender). The standard replies + `mount()`'s existing
answer grant make honest outcomes nearly free (`zen.Result{id}`|`zen.Refused{error}`), but
replying is a *behavior change*, not a collapse — its trigger is the first operator who
drives the kernel from the console and needs to see what happened. Whether a **bytes
result** joins the standard set (three of the four sites carry `Bytes`, which `zen.Result`'s
text field cannot) is the follow-on phase's one real design decision. Also: out-of-process pokes (a sandboxed `.so` weave now *accepts* the doors via its manifest, but
its kernel-decided grant does not include the answer shapes, so answers are denied until a
host grants them — consistent, tap-visible, unexercised); the authorized-direct-call path
(auth-phase trigger); composed/granular tag rules (`expose-all-except`, both-tags-on-one-
field at field scope — weave-scope-one + field-scope-other says it today); non-scalar
poke I/O (the structure already lists those fields honestly); send-fate observability for
participants; registering declared emissions at mount (today a reply shape reaches the
console's wildcard-accept only if some accepter lists it — the console suite pins that as
intended; the Poke weave's accept-set is what registers the answer grammar in practice).

---

## The UI-Builder component vocabulary (Phase A — the semantic tree, evolved; shapes only)

The UI Builder produces **components: schematics with typed slots** — reusable UI that is
incomplete *by design*, declaring the data shape it consumes and leaving open holes to be bound
later. This phase built **only the vocabulary** (`include/zen/console/component.hpp` at the
time — since lifted to `include/zen/ui/component.hpp` in Phase B — + the
evolved `ui.hpp`): the shapes, their serialization as gated Values, and the unification with the
console's existing widget tree. No renderer (SDL2 is the next phase), no Builder panels, no live
binding, no routing/presenter runtime.

### ONE tree, evolved — not a second vocabulary

The console's `Widget` tree **is** the component tree; this phase evolved it in place. What
changed on the node vocabulary itself:

- **Abstract interaction intent** — `activatable` / `editable` / `reorderable` flags: what the
  operator may *do* here, never which key or gesture does it (the TUI maps keys, a GUI maps
  clicks onto the same declarations; `reorderable` is declared intent no built renderer consumes
  yet). These **replaced `focusable`**, which the least-complete-information razor cut: no
  renderer or controller ever read it, and focus-eligibility is derivable from interaction
  intent — the intent is the load-bearing fact, the eligibility was sediment.
- **A new node kind `Slot`** — a typed open hole (`slot_name` + `slot_accepts`: `"Component"`,
  `"Route"`, or a scalar Kind spelling). A slot is a *position* in the tree, so it is a node,
  not a side-table; its `children` are the design-time placeholder preview.
- **Data binding** — `from_field` names the contract field feeding a node's content/items
  (declared, not yet resolved: live binding is the Inspector phase's; design-time shows
  placeholder). **Route addressability** — a component's `name` is its address; a node's
  `route_to` declares navigation intent; the routing *runtime* is deferred.
- The stable spellings (`name_of(WidgetKind)` / `name_of(Overflow)` + parsers) became public
  contract — they are the wire spellings, and an unknown spelling is **refused on decode**,
  never a silent blank.

The TUI still renders the evolved tree (the standing renderer-agnosticism proof), gaining only a
`Slot` projection (`[slot name: accepts T]` + the placeholder beneath). The outline proof now
*prints* interaction intent, bindings, routes, and slots — they are the tree's **meaning** — and
still *ignores* `weight`/overflow — they are renderer **hints**. That two-renderer discipline
(meaning shows, hints don't) is pinned in the component suite.

### A schematic is data: components serialize as gated Values

`zen.ui.Node` v1 / `zen.ui.Component` v1 / `zen.ui.Presenter` v1 (hand-registered blocks, like
the standard reply shapes, so the substrate's dotted prefix is unspellable by a maker's macro).
A component round-trips `to_value → serialize → parse → admit → from_value` through the **same
single validator as the bus path** — no second format, no bespoke codec. "A schematic shared is
a toy others can play with" is thereby mechanical: save, send, load, gate.

**The wire form is flat.** A schema cannot reference itself (immutable published schemas), and
nested-Message decoding is depth-capped — so the tree crosses as a flat `nodes` list whose
`children` are indices (node 0 the root; pre-order is `flatten()`'s canonical layout, not a
decode requirement — any valid-tree layout rebuilds deterministically). `flatten()`/`tree_of()`
are the lossless pair **within `kMaxUiDepth`** (flatten of a deeper tree yields a component
`tree_of` refuses — the bound is pinned, not hidden), with the round-trip pinned by `Widget`'s
structural `==` on real trees including the console's own live tree and a maximal
every-field-non-default node pair (the mutation pin for codec omissions).

**Honest layering (load-bearing):** the gate proves *shape-conformance*; it cannot see
*tree-ness*. Whether the flat list is actually a tree — indices in range, every node reached
exactly once, no cycles or orphans, known spellings, sane ranges (`contract_version` a u32),
depth ≤ 256, and per-kind **child arity** (a Region wraps exactly one child; List/Log/Text/Field
carry none — child structure is where the TUI and the outline would otherwise silently
*diverge*, so it is tree structure and is refused; per-kind-unused *scalar* fields stay lenient
and round-trip verbatim — canonical authoring zeroes them; a strict field-canonicality check is
a named seam awaiting a consumer) — is the vocabulary's own decode check: `tree_of()` **refuses
with a reason** naming the offending node. The component suite forges hostile shape-conforming
frames — a detached cycle, a two-parent child, a root back-edge, unknown spellings, out-of-range
integers, arity violations, a 300-deep chain — and **every class runs the full wire path**: the
real gate admits it, `tree_of` refuses it (the unsayable-attack discipline applied to the
vocabulary, pinned per class). The TUI's weight `split()` was hardened to 64-bit accumulation
alongside — a wire-legal wide-and-heavy stack could wrap the old 32-bit weight sum.

### Contracts, the view/presenter split, and stress placeholders

- **Data contracts.** A component declares the shape it consumes as `(contract_name,
  contract_version)` — registry-resolved identity, like every schema agreement in Zen.
  `check_bindings(component, contract_schema)` makes typed slots *checkable*: every
  `from_field` must name a real contract field of a displayable kind (Text/Field ↔ scalars,
  List/Log ↔ List fields **with scalar elements** — rows are text, a `List<Bytes>` has no row
  form; containers bind nothing), a Slot's `accepts` must be a known type, slots must be named
  **uniquely** (filled by name later — a nameless or duplicate hole is unfillable), and
  `route_to` requires `activatable` (a route that can never fire is a dead declaration).
  Pinning the contract's *content-id* into the component is the migration/identity layer's
  business — a named seam.
- **The view/presenter split, as separable data.** A component is pure display: it declares
  what it consumes but **never names what feeds it**. `zen.ui.Presenter{view, source_role}`
  is a distinct value binding a source (by *role* — the persistable addressing; never a
  session-scoped WeaveId) to a view (by name). Pinned at the schema level: the component
  schema has no source field, the presenter schema has no tree. This is what lets a crashed
  program-weave *not* take the UI down with it later — the source dying is an event the
  display can outlive; the presenter runtime is a later phase.
- **Stress placeholders are the default.** Design-time previews use the value that *reveals
  the seam*: `stress_text()` (a long paragraph + an unbroken 64-char word), `stress_number()`
  (`-9223372036854775808`, the widest Int spelling), `stress_rows()` (the empty list — the
  zero-case), `stress_nested()` (a deep alternating-stack ladder). The design-time
  constructors (`bound_text`/`bound_field`/`bound_list`/`open_slot`) pick them **by
  default** — no happy-path preview exists to pick. ASCII on purpose (the TUI is
  byte-per-cell); the Unicode stress case arrives with a renderer that can draw it. A
  hand-chosen fill-out example-data section is a named follow-on.

### Status: built (Phase A)

Suite `component` (15 cases / 193 assertions): schema twins pinned against hand-built
`SchemaBuilder` equivalents; the gate round-trip incl. canonical-bytes re-serialization and the
Registry path; the console's live emitted tree crossing the wire bit-for-bit (ONE tree pinned);
the gate-admits/`tree_of`-refuses hostile-frame matrix, **every class through the real gate**;
the maximal every-field-non-default round-trip; accept-side boundary edges (weight 65535,
`selected_index` INT_MAX, a cursor over an empty list); the bounded-lossless pin; stress
defaults; outline-reads-as-intent (weight AND overflow pinned as outline-ignored hints); the
TUI projecting a rebuilt schematic; the split's schema-level separability; the binding checks.
Green Debug + ASan/UBSan under the delegated scope and on the Windows/MinGW portable subset.
Named successors (not built): the **SDL2 renderer** (Phase B — same tree to pixels; it also
pulls lifting the vocabulary out of the console target when a non-console consumer arrives),
the **Builder panels** (Phase C — palette/canvas/inspector composing these shapes), **live
data binding** (the Inspector phase), the **routing runtime**, the **presenter runtime**
(subscription, update application, crash-as-event), the **fill-out example-data section**, and
**contract-content-id pinning** (identity/migration phase).

---

## The SDL2 projection (Phase B — same tree, to pixels; the agnosticism test made real)

Phase A proved renderer-agnosticism *by the shape*; with only the TUI consuming the tree, a
skeptic could still say the vocabulary grew up around its home medium. Phase B is the real
test: **a second, radically different renderer — pixels, a mouse, real fonts — consuming the
identical tree.** The headline result: **the SDL renderer needed ZERO additions to the node
vocabulary.** Every node kind, every field, every hint projected to pixels as-is. The one
shared-vocabulary addition the phase made is on the INPUT side — `Action::SelectAt` (+
`InputEvent::index`): the pointer medium's basic act is *naming* a row where keys can only
*walk* to one. That is intent ("select THIS row"), not medium (a terminal with mouse reporting
could emit it); the console's controller gives it real semantics under the same single-writer
clamp as SelectDown. No SDL-only node field exists — the litmus held.

### The vocabulary-target lift (the seam Phase A named, pulled by its trigger)

The tree + component vocabulary now lives in **its own target `zen-ui`** (`include/zen/ui/
tree.hpp` + `ui/component.hpp`), depending only on the core — never on the console, the bus,
or any renderer. The console is a *consumer* (its `console/ui.hpp` keeps what is genuinely the
console's: `UiState`/`Focus`, `guidance_for`, `emit_ui_tree`, `ConsoleUi`); the TUI's `zen-tui`
now links `zen-ui`, not `zen-console` — the same one-way dependency the SDL renderer has. A
boring, behavior-identical move: identical suite counts on all three environments were the
safety proof.

### The projection split: a suite-provable brain, a thin SDL skin

The renderer is split exactly the way the TUI is:

- **`zen-ui-pixel`** (`include/zen/ui/pixel.hpp`) — the layout *brain*: tree → paint-ordered
  **draw commands** (`Fill`/`Text`/`PushClip`/`PopClip`, with semantic `PxRole`s so the command
  list stays theme-free) + **interactive targets** (rect → node/row) for pointer hit-testing.
  Pure and SDL-free: text metrics are *injected* (`PxMetrics`), so the whole projection logic
  is deterministic and pinned in the ordinary suite on every platform — including Windows,
  where the SDL skin doesn't build. Pixel geometry exists ONLY here and below, as cells exist
  only in the TUI's Grid.
- **`zen-ui-sdl`** (`src/ui/sdl/`, gated) — the *skin*: a window, TTF-backed metrics, a
  command executor (surface → texture → copy, the Zengine-borrowed path with its leaks fixed),
  and `sdl_map_event` — raw SDL events to the SAME semantic `InputEvent`s `tui_map_key`
  produces. **Abstract interaction maps to the medium in the renderer, never in the tree:**
  `activatable` → click-to-select (`SelectAt`) / double-click-to-`Activate`; `editable` →
  `SDL_TEXTINPUT` as byte-`Edit`s (UTF-8 sequences reassemble exactly); keys mirror the TUI's
  bindings. The controller/engine never see an `SDL_Event`.
- **`zen-ui-sdl-demo`** — Josh's visual verify: renders the stress-placeholder schematic
  (Phase A defaults + the Unicode case) with a demo-only input shim; the SDL *console
  frontend* (`ConsoleUi` over the engine with this renderer) falls out nearly free and is a
  named seam, deliberately not built (render, don't operate — this phase's scope).

### Overflow made real, and the Unicode stress case

The overflow policy — the hint the TUI ignores (it aliases Wrap/Truncate to Grow) — gets its
**real meaning** here: `Wrap` lays a Text node out as greedy word-wrapped lines that
hard-break inside an over-wide word only at a **codepoint boundary** (never mid-UTF-8-
sequence); `Truncate` ellipsizes at a codepoint boundary; `Grow` stays natural-size. One tree,
one renderer ignoring the hint, one honoring it — pinned from both sides (the outline equality
on overflow-only differences, and the wrapped/truncated/grown command shapes). The stress
canon gained **`stress_text_unicode()`** (CJK, emoji, a combining sequence, an RTL run, an
unbroken mixed-script word — spelled as explicit UTF-8 byte escapes so no editor can reshape
it): the DEFAULT placeholders stay ASCII (they must stress every projection, not mojibake
one), and the Unicode value is the graphical renderers' additional case. What glyphs *look*
like (shaping, bidi) is the text stack's affair; the pins are "never break, never split".

### Gating and platform decision

**SDL2 is a dependency of the SDL renderer target ONLY** — the vocabulary, pixel logic,
console, bus, and core stay SDL-free (pinned by Windows/MinGW building and running the `pixel`
suite with no SDL present). The target is **WSL/Linux-first** (`ZEN_SDL` defaults ON there,
OFF on Windows): prefers a system SDL2, else fetches **pinned release tarballs by checksum**
(static SDL2 2.30.11 + SDL2_ttf 2.22.0 with vendored FreeType) — nothing installs on the host.
On WSL building from a `/mnt` checkout, the fetched trees live on the WSL-native filesystem
(drvfs cannot hold SDL's internal symlinks; keyed per build dir so Debug and ASan objects
never mix). The dummy-driver smoke (`SDL_VIDEODRIVER=dummy`) runs the FULL pipeline — real
SDL video, real TTF metrics, executed commands — headless in the suite; on-screen pixels are
Josh's visual verify (the established division).

### Status: built (Phase B)

Suite `pixel` (SDL-free, every platform incl. Windows-without-SDL): wrap greedy, space-
preferring, codepoint-safe on the Unicode gauntlet (reassembly + no-line-starts-mid-codepoint
pins); truncate boundary-safe with a clean-prefix pin; the one-tree-two-media command shape
(paint order, focus/selection fills before their text, clip balance, interactive targets);
overflow-made-real (wrap lines / ellipsis / natural size) against the outline's pinned
hint-blindness; the slot marker + placeholder preview; hit-testing to the named row; weight
splitting pixels exactly as it splits cells; layout determinism. Suite `sdl` (dummy-driver,
where the SDL target builds): the full pipeline headless — real SDL video, real TTF metrics,
executed commands, the emoji surviving un-split, the executor's clip stack balanced — plus
raw-SDL-events → semantic InputEvents (click→SelectAt with the row index, double-click→
Activate, UTF-8 TEXTINPUT reassembling exactly through byte-Edits, quit as the one false
return — the tui_map_key contract shape). The TUI stays alive as the continuing second
projection, and the console suites are count-identical to Phase A (the lift's boring-move
proof). Green Debug + ASan/UBSan under the delegated scope; Windows/MinGW portable green
without SDL. Named successors (not built): the **Builder panels** (Phase C), **live binding**
(Inspector), the **routing/presenter runtimes**, the **SDL console frontend** (ConsoleUi over
the engine with this renderer — nearly free), **focus-by-pointer** (a FocusTo-style event, cut
by the razor until a frontend needs it), and **richer text** (kerning-aware wrap, real
bidi/shaping, codepoint-aware Edit).

---

## The Weave Manager — the lifecycle steward

Operating the system used to be a *different gesture* from using it. Using it meant
sending messages; operating it — load, reload, unload — meant the host calling C++ on
its `Kernel`. That asymmetry is the thing this phase removes: **operating the system
becomes the same gesture as using it.**

**The door answers.** `ControlWeave` (`kernel/control.hpp`) already turned the kernel's
dangerous surface into an accept-set gated by the sender's `load_capability`. But it
*discarded every outcome* — `(void)kernel_->reload_from(...)`. The single most
dangerous surface in the system was also the only one that answered nothing, so a
reload's state-schema mismatch — a real, deliberate, well-shaped refusal — died as an
unread C++ return value. Every op now replies with a standard shape
(`standard_shapes.hpp`) to `reply_to`-else-stamped-sender, echoing the correlation:
`LoadLibrary → zen.Result{id} | zen.Refused{why}`, `ReloadLibrary`/`UnloadLibrary`/
`UnloadRole → zen.Ack | zen.Refused{why}`, `ListLibraries → zen.Result{"a,b@role"}`.
The door's `Emit<...>` declares all three, so its authority to answer is an ordinary
emit-default grant — nothing rides the poke carve-out.

**The Manager is an ordinary participant.** `WeaveManager` (`kernel/manager.hpp`) is a
`WeaveBase` weave whose whole state is `RelayState` — the relay bookkeeping and nothing
else, itself poke-inspectable. It accepts four ops (`zen.LoadWeave`, `zen.SwapWeave`,
`zen.ReloadWeave`, `zen.ListLoaded`), forwards each to the door with `loom::relay`
(its second real consumer), and relays the answer to the original asker under the
asker's own correlation. **No privilege:** its authority over the kernel is exactly
`load_capability(control)` — target-scoped to the door — assembled by the host at
mount and handed in whole (`manager_capability`). Any participant could hold that same
grant and drive the door with no Manager in the path; the `manager` suite pins that
directly. It is *replaceable default tooling* — orchestration, never exclusivity — and
the host keeps the pen: the Manager cannot widen its own grant, cannot reach the
`Kernel` object, and cannot register or kill anything itself.

**The door executes primitives; the orchestrator composes.** `SwapWeave` is the
composite that justifies the Manager's existence: it is *not* a kernel primitive. Had
`SwapRole` been added to the door instead, replacing the Manager would not let you
change swap policy. The composite belongs to the orchestrator.

**Swap and reload are two ops, deliberately.** They are different machines and deserve
different names. `ReloadWeave` is reload-**in-place**: same weave, same `WeaveId`,
state snapshotted and transplanted through the gate; a differently-shaped library is a
clean refusal and the incumbent runs on. `SwapWeave` **replaces the role holder**: the
incumbent is unloaded, a successor is loaded into the role, state starts fresh; a
differently-shaped successor is the *normal case*. Folding them into one op would
invite exactly the quiet growth of "reload" into "replace" that the two names prevent.

**Addressing is role-first.** A consumer that must survive its provider being replaced
addresses it by **role**, never by `WeaveId` — the successor is a different weave with
a different id, and only the role slot carries the consumer's reach across. `Kernel::load`
therefore gained an optional `role`, because registration is the only moment a role
*can* be bound (`Switchboard::register_weave` is the sole binder, and roles are
singletons). Binding a role already held is a clean `LoadResult` failure, not a throw.

**The swap window, stated honestly.** Swap issues two messages — `UnloadRole{role}`,
then `LoadLibrary{name, path, role}` — and the bus's single-threaded FIFO, non-reentrant
dispatch guarantees the order. Three properties, all pinned:

- **Inbound traffic already queued still reaches the incumbent.** The swap's own
  messages go to the *tail*, so a role-send enqueued before them resolves against the
  incumbent. A swap does not steal traffic already addressed to the role.
- **The incumbent's in-flight *replies* die with it.** A gated message is authorized by
  looking its sender up at *delivery* time, so once the incumbent is unregistered its
  still-queued answers fail the `sender != nullptr` term and are refused
  `CapabilityDenied`. Fail-closed and correct, but real: an in-flight request to the
  incumbent can be answered into the void. This is a property of unregistering **any**
  live weave mid-queue, not something the swap invented — the swap is simply the first
  op that makes it routine. It is the concrete thing an invisible/atomic rebind would
  have to solve, and the honest reason that refinement is named rather than dismissed.
- **A failed swap leaves the role unheld**, the asker gets the `Refused` with its
  reason, and sends to the empty slot refuse cleanly (`NoSuchTarget`, exactly as an
  unmounted provider does) — the optional-participation floor doing its job. Felt
  friction, admitted at floor tier.

**One request, one answer.** The unload half of a swap is deliberately fire-and-forget
(correlation 0, which no relay sequence can equal — they start at 1), so its reply is
dropped by the consumer obligation rather than relayed. That is not a dark fate: its
outcome is fully subsumed by the load's. If the role was unheld, the unload "fails" and
the load then binds it — precisely what was asked. And because the unload is
**role-addressed**, it cannot destroy a weave the asker did not name: the only thing it
can unload is the role's holder. Both are pinned.

**Homing (the `schema_codec` lesson, applied prospectively).** The Manager must speak
the door's wire shapes, which live in un-exported kernel headers. It therefore homes
**inside** `include/zen/kernel/` — already excluded from the install — so **no exported
header reaches an un-exported one**. Zero new export edges; the Manager joins the
installed surface at the same trigger the kernel does (a hosting consumer).

**Loading is path-addressed, honestly.** Naming a weave by the file it lives in is what
the kernel can do today; content-addressed identity belongs to the identity phase.
`ListLoaded` is answered from the kernel's own live map, never a Manager ledger — there
is no cache here that could drift from the loading authority's truth.

Suite `manager`: 16 cases / 171 assertions, green Debug + ASan/UBSan under the delegated
scope; Linux-gated with the rest of the kernel, and the Windows portable subset is
count-identical (194 / 80203). Named successors (not built): `PrepareShutdown` + the
cooperative handoff letter (1b, Loomstd-homed); snapshot configurability; invisible/atomic
rebind (pulled only by felt window); multi-multiplicity roles; the conflict-triage brain.

---

## The letter — cooperative handoff (Manager 1b)

1a proved the floor: swap-with-reset, an honest window, loud refusals. What could not
yet happen was **continuity across succession** — a differently-shaped successor
inheriting what its predecessor knew. Reload transplants state across the *same* shape;
the letter **converses** across a different one. The predecessor writes a letter to its
heir.

The protocol is **Loomstd-tier** and lives in `weave/lifecycle.hpp` beside the standard
reply shapes — deliberately **not** kernel-homed. It is universal lifecycle conversation
any weave may choose to have; the Weave Manager is *a* consumer of it, not its owner.

**Two walls gave the design its shape, and both are 1a's own pins rather than guesses.**

**W1 — the letter dies with its sender.** A gated message is authorized by looking its
sender up at *delivery* time, so a fire-and-forget graceful swap (ask, then immediately
queue unload+load) would post the letter into the void: the incumbent's reply would be
an in-flight send from a weave about to be unregistered, refused `CapabilityDenied`.
The graceful path is therefore **two-stage by construction** — ask, *receive the
letter*, and only then unload. The suite reads the ordering off the bus's own tape:
`PrepareShutdown` delivered → `Bequest` delivered → **then** `UnloadRole`, with zero
`CapabilityDenied` refusals of `zen.Bequest`.

**W2 — the Manager cannot push.** Delivering arbitrary domain shapes to an heir would
need shape grants unknowable at mount, and `allow_any` on the steward is exactly the
transitive reach its broker note refuses. So delivery is **pull: the heir claims.** The
steward is granted the two *lifecycle* shapes and nothing else — it can conduct a
succession without being able to say a single domain word. Pull is also **gap-agnostic
by construction**, which is the protocol's law: *the letter must not know the gap.*
Nothing in it assumes immediacy, wall-clock, or that the predecessor's `WeaveId` still
means anything. The letter waits; the heir asks when it wakes — a microsecond or a
month. An heir reaches the steward by its well-known **role** (`zen.manager`), because a
weave that just woke knows nothing else that outlives a swap.

**Messages only, no state blob.** A deliberate deviation from the original
`{state, wake_messages[]}` sketch: a state blob would be a second, shadow transplant
path with none of reload's shape agreement — precisely the quiet growth of "reload" into
"replace" the two ops exist to prevent. A weave that wants its state to carry says so
**in its own vocabulary, as an item**.

**The items are bytes, and the gate stays the sole admitter.** A list cannot hold
heterogeneous messages: a List's element is ONE `TypeRef`, and the gate pins a nested
Message to a single schema by `content_id`. So the letter is a `List<Bytes>`, each item
serialized by the predecessor and **re-admitted through the real gate by the heir when
it reads it** (`claim_item`). The escape hatch buys heterogeneity without buying a
second admission path — inherited mail is untrusted input like any other.

**The participation check.** The steward asks the door `QueryRole{role}` **before**
asking the incumbent anything, so a weave that never declared `zen.PrepareShutdown` is
never waited on — it simply falls through to the 1a hard swap, automatically. The answer
comes from data the kernel already holds (the manifest it reconstructs at load, plus the
bus's published accept-set); **no Switchboard API was added.** `holder == 0` honestly
conflates "unheld" with "held by a native weave" — the kernel cannot see a native
weave's accepts, no caller needs the distinction, and both are non-participants.

**The non-participation floor, both ends.** An incumbent that never declared → hard
swap. An heir that never claims → fresh start, safe, letter held and visible. An
incumbent that *declared* and then never replies wedges **its own** swap only; the
escape is a second, non-graceful `SwapWeave` — the ordinary op, not a knob. There is
**no timeout machinery**, on doctrine.

**What the steward keeps.** The letter store is bounded, keyed by role, latest-only (a
newer swap replaces an unclaimed letter), answered exactly once, authorized only from
the weave recorded as that role's successor, and **poke-inspectable** — the steward
keeps no secret mail. A letter whose load *failed* is discarded: no successor exists to
authorize a claim, and unclaimable mail is a leak wearing the costume of a feature. That
does mean a failed graceful swap loses the letter along with the incumbent — the honest
extension of 1a's failed-swap friction, pinned rather than papered over.

`graceful` is a **field** on `SwapWeave` (v2), not a sibling op: this is the same
machine with one extra stage in front of it, and a sibling would duplicate role/name/path
and let the two drift. (Contrast `SwapWeave` vs `ReloadWeave`, which are different
*mechanisms* and so are different ops.)

`loom::forward_for` generalizes `relay.hpp`'s `forward` for multi-stage orchestration:
when the answer that finally satisfies the asker is triggered by some *other* weave's
message, the inbound `Mail` no longer describes the asker, so the caller supplies the
asker and correlation it captured earlier. `forward` is now expressed in terms of it,
behavior-identical (the `poke` suite is count-identical at 157 assertions — the
boring-diff proof).

Suite `manager`: 27 cases / 447 assertions; the Loomstd vocabulary's own round-trip and
gate-refusal pin lives in the **portable** `weave` suite on purpose — a header that
claims to be portable while only ever compiling behind `if(NOT WIN32)` is a claim nothing
checks. Green Debug + ASan/UBSan under the delegated scope; Windows MinGW portable
195 cases / 80209 assertions.

**Named successor — the snapshot opt-out satellite ships as 1c** (the prompt's own
cut-order). Pricing it surfaced the reason it deserves its own phase:
`OutOfProcessWeave::snapshot()` returns the host-owned cached value, so under a `Never`
policy that cache would hold only the *handshake* snapshot and silently serve stale state
as current — a vacuous-green of exactly the kind the honesty lattice exists to prevent.
Deciding what `snapshot()` means under `Never`, and making `containment()` attest the
degradation, is a real honesty-lattice decision in the most safety-critical subsystem,
not a flag.

---

## Future seams (designed for, not built)

- **Reflection migration of the macro.** Under C++26, the `ZEN_FIELD` block in
  `ZEN_SHAPE` becomes a reflect-over-members derivation. Everything downstream
  consumes only the abstract `zen_fields()` tuple, so this is a single-seam swap
  with no change to any consumer.

- **Codegen marriage.** A build-time generator should emit, from one schema
  definition, *both* a compiled C++ struct (zero-overhead static path) *and* this
  runtime `Schema`. The weaving layer is the **first half** of this: a shape
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
  *faithfulness*: a Weave that declares one policy and behaves against it passes
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

- **Multi-threaded dispatch (per-Weave mailboxes).** The single-threaded FIFO
  loop is an implementation of the dispatcher, not part of the contract. The
  `Weave` surface (receive a gated message; send, which enqueues) is identical
  under per-Weave mailboxes and worker threads — only `pump`'s internals change.
  Nothing in the Weave ABI or the `Message` envelope forecloses it.

- **Request/response correlation and await.** The envelope already carries
  `reply_to` and `correlation`; replies work today as ordinary sends. A
  synchronous `request(...)` that blocks until a correlated reply arrives is a
  layer over the same enqueue/deliver path — not built here.

- **Schema-as-value over the bus.** A Weave answering "what do you accept?" with
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
  in loom — with no change to the Weave contract.

- **Crash isolation via per-process hosting.** Surviving a segfault in a loaded
  Weave needs the Weave in its own process under supervision (IPC). The C ABI's
  bytes-as-currency is already the cross-process currency, so in-process (fast)
  and out-of-process (isolated) become the two permanent hosting modes behind the
  same `Weave` contract — the next phase, not built here.

- **Migration at the version-mismatch reload point.** Today a new library whose
  state-schema version differs is a clean refusal. The migration layer slots in
  exactly there: resolve `(name, claimed_version, content_id) → door`, transcode
  the host-owned snapshot, and revive through the same gate.

- **Cross-language libraries.** The C ABI is *designed* to admit a Weave woven
  in another language: it exports only C, and Zen values cross as bytes. Only a
  C++-woven test library is built now, but nothing in the descriptor or the
  buffer discipline forecloses a Rust/C/other maker.

- **Full schema-as-value.** The accepted-schemas manifest is the minimal
  precursor: schemas already round-trip as gated Values. Generalizing it lets the
  console introspect the whole system and a Weave answer "what do you accept?"
  with schemas rendered as Values.

---

## Level 0 hardening — closed seams and the seam-readiness review

Before Weave-based "Level 1" development begins — at which point every Level 0
surface a Weave touches gets expensive to move — one tightening pass closed the
handful of seams whose shape is *proven* and that Level 1 will immediately lean
on, and **left the rest open on purpose**. The discipline is symmetric: closing a
seam before its shape is proven is the same mistake as leaving a sharp one open.

### Closed in this pass (code)

1. **Dispatch selector → true `same_identity`; the `same_identity` misnomer
   closed.** `WeaveBase::handle` selects the handler the same way the bus selected
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
| Emit enforcement (as a wiring *contract*) | partly closed in B1 | capability-gated delivery now authorizes every send against a real grant (Emit-defaulted for trusted Weaves), so the *in-process* emit gate is live; emit-set *as an enumerated contract / wiring graph* is still deferred (a runtime router's emits are not statically known) |
| Schema-as-value beyond the manifest precursor | not-ready | only the manifest slice is exercised; the general "schema of schemas" has no consumer yet |
| Multi-threaded dispatch (per-Weave mailboxes) | not-ready | single-threaded FIFO is correct and sufficient; the `Weave` contract already survives the swap, so early closure buys nothing |
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

**Precondition — the `isolation` and `policy` suites need a delegated cgroup-v2
scope.** ctest launches them via `tests/run-under-scope.sh`
(`systemd-run --user --scope -p Delegate=yes`), because real B3–B5 enforcement needs
an unprivileged user namespace plus a delegated cgroup subtree. Run those suites
without such a scope (a plain `wsl bash` lands in the root cgroup) and the
OS-enforcement cases **fail hard by design**, naming the missing capability — the
harness will not report a pass it did not earn (`tests/enforcement_gate.hpp`). Set
`ZEN_ALLOW_UNENFORCEABLE=1` to convert those into marked-degraded skips on a host that
genuinely cannot enforce. The portable suites need none of this and run everywhere,
including the Windows/MinGW build.
