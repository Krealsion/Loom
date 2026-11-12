# Values and admission — reference

The core: self-describing values, one gate, immutable published schemas. Laws:
[GATE-01..04](../laws/admission-laws.md).

## Schema

A `Schema` is a frozen, named, versioned shape: `(name, version)`, a form, and
ordered fields of the nine permanent kinds (`Int` i64, `Float` f64, `Text`,
`Bool`, `Bytes`, `List`, `Message` and `Union` — nested by schema — and `Any`,
nested by the value's own shape). Its form is a **record**, whose value holds
any of its fields, or a **union**, whose value holds exactly one of them
([below](#unions-and-the-open-field)). Identity across any
boundary is the **content-id** (FNV over the canonical shape), never the C++
type; `same_identity` is name, version and content-id, confirmed field by field
(`same_structure`), since the content-id is a fast hash rather than a
cryptographic one.
Published schemas are immutable: evolving a shape is publishing a new version
([GATE-04](../laws/admission-laws.md)). Build one with `SchemaBuilder`, or
derive one from a struct with `ZEN_SHAPE`
([guide](../guides/writing-a-weave.md)).

### Required and optional fields

Each field is required or optional (`Field::required`; `SchemaBuilder` takes it
as its last argument and defaults to required). The gate refuses a value that
leaves a required field absent (`MissingField`) and admits one that leaves an
optional field absent; a present optional field is judged exactly as a required
one. Requiredness belongs to a field, never to a list's element type, and it is
part of the content id.

Absent is its own state, never a stand-in value: `Value::get` returns null for
it, the canonical binary clears its presence bit and writes nothing, and the
compat JSON leaves its key out (a JSON `null` is refused for every kind), as the
Python session client does both ways. A present `0`, `""`, `false` or empty list
is a value.

With `ZEN_SHAPE`, a member of type `std::optional<T>` is an optional field of
`T`'s type, for any `T` a field takes; every other member is required.
`to_value` leaves the field absent when the member holds `std::nullopt`, and
`from_value` gives `std::nullopt` for an absent field, whatever the member's
initializer holds. A `std::optional` anywhere else, as a list's element or as an
optional's value, is refused at compile time. Live inspection reads a present
optional scalar field as its value and refuses an absent one as absent; a hidden
or non-scalar optional field is refused as any hidden or non-scalar field is
([observation](messaging.md#observation)).

An optional field is not a versioning tool: adding one is a new version
([GATE-04](../laws/admission-laws.md)), and a value of one version fails
another's gate however optional its fields
([handoff](handoff.md#different-schemas-remain-different)).

## Unions and the open field

### A union

A union is a named shape whose value is exactly one of its cases: a pointer
moment is a move, or a button, or a wheel, never two and never none. Its fields
are its cases, in declared order; each case carries a value of any type a field
takes, or carries nothing (`Field::carries`, `case_bare`). A union whose cases
all carry nothing is an **enumeration**, and it is the same form. A union is a
shape like any other: it has a name, a version and a content id, the registry's
one wall refuses two definitions of it, a manifest carries it once in
`referenced` before whatever nests it, and a new case is a new version. It may
stand wherever a shape stands: at a door, nested in a field, as a list's
element, a claim or a weave's state.

A field holding a union is of kind `Union`, a field holding a record of kind
`Message`; `type_message(schema)` gives the one the shape's form says, and a
`TypeRef` naming a shape of the other form is refused when the schema is built.
`Kind` gained the two at its end, so every `switch` over it with no default
fails to compile until its reader decides what a union and an open field mean
to it. A record's content id folds exactly what it folded before unions
existed; a union's folds its form as well, so a record and a union with the same
fields are two identities.

```cpp
struct Moment {
    std::variant<KeyPressed, PointerMoved, PointerWheel> is;
    ZEN_UNION(Moment, 1, is);                      // cases named by their shapes
};
struct Pick {
    std::variant<std::int64_t, std::int64_t, std::monostate> is;
    ZEN_UNION(Pick, 1, is, "left", "right", "none"); // or every case by the line
};
enum class Outcome { delivered, refused, handler_failed };
ZEN_ENUM(Outcome, 1, delivered, refused, handler_failed);
```

A case is named by its shape's name, or every case by the names the line lists,
in order: a scalar case, two cases of one type and a `std::monostate` case (one
that carries nothing) need them. `to_value`, `from_value`, `schema_of` and a
door's accept list take a union and an enumeration as they take a record, and a
handler reads one with `std::visit`. By hand, `make_union(name, version,
{case_of(...), case_bare(...)})` builds the shape, `Value::set` holds a case that
carries a value and `Value::choose` one that carries nothing; either replaces the
case held, so a value never holds two, and `Value::chosen()` says which it holds.

### The open field

A field of kind `Any` holds a value of **any shape the registry holds**, named by
the value itself: an element's data, a carried item, a thing whose shape is
chosen at run time and may be a stranger's. It is for a set that is truly open;
a set closed at a version is a union. With `ZEN_SHAPE` the member is a
`loom::Any`: `Any::of(obj)` holds a struct's value, `holds<T>()` and `as<T>()`
read it back, and an empty `Any` leaves its field absent.

The gate checks the value an open field holds against a registry: its shape must
be registered there as one definition with it (`same_identity`), and the value
must conform to it, at its path. `admit(value, door, open)` and
`admit(unverified, door, open)` take the registry; the bus admits every delivery
against its own, and a host that admits bytes at a seam passes the bus's
(`Switchboard::registry()`). Without one, an open field is refused
`UnknownSchema`, never trusted. **A weave nests only a shape it may itself
send**: before the gate, the bus asks the sender's effective authority for every
shape an open field of the payload holds, however deep, to the same destination,
and refuses the delivery `CapabilityDenied` if any is not one it may send
([capabilities](capabilities.md)). A loaded weave reads an open field against the
shapes it declares itself (its doors, state, claims and emits and what they
nest): a shape it does not declare is not one it can read, and the value is
refused at its seam.

### On the wire

- **Native**: a record's body is a presence mask and its present fields; a
  union's is its case's index as a minimal varint, then that case's value, or
  nothing for a case that carries nothing — an enumeration's value is one byte up
  to 128 cases. An index past the last case is `MalformedField`, never skipped.
  An open field's value is its shape's name, version and content id, then its
  body, decoded under the registry's definition: one it does not hold is
  `UnknownSchema`, another content id `SchemaMismatch`. A union holding no case
  cannot be written: `serialize` throws.
- **Compat JSON**: a union is an object with one member, its case's name and
  value (`{"moved": {"x": "1", "y": "2"}}`), or a case that carries nothing as its
  name alone (`"idle"`), at a door as in a field. No member is `NoCase`, several
  are `SeveralCases`, a name the union does not declare is `UnknownField`, and a
  case in the other form is refused by its kind. An open field's value is
  `{"schema", "version", "content_id", "fields"}`, the envelope's own members,
  `content_id` optional as it is there.

## A value never changes once admitted

A nested value is **made, never adopted**: `Cell::message(Value)` takes the value
and makes the cell's own node, and no overload takes a pointer. Nothing outside a
value holds a way to change what is inside it, so the value the gate admitted is
the value every recipient is handed, and a sender that keeps its own copy keeps a
copy. Copying a value copies its own slots and shares everything beneath them,
which is safe because nothing beneath can change. An open field's value is held
the same way, and so is a union's case.

## Value

A `Value` carries the schema it *claims* plus positional cells; a union's value
holds at most one case. Values are ordinary C++ values (copyable);
`construct_blind` builds one at runtime from a discovered schema — the console's
road, which asks a union for one case. Claiming is not conforming: only the gate
says a value is what it claims.

## The gate

```text
bytes ──parse()──► Unverified ──admit(door)──► Value      (or a structured refusal)
```

`loom::admit(candidate, door)` is the **sole** conformance authority
([GATE-01](../laws/admission-laws.md)). `Unverified` says only what it claims
to be (`well_formed()`, `claimed_name()`, `claimed_version()`) and has no accessor
for its data — there is no road from bytes to readable data around the gate
([GATE-02](../laws/admission-laws.md)). Admission is total (caps on depth/size
make hostile input safe — see [bounds](bounds.md)). Refusals carry kind, field
path, expected/actual. `gate_invocations()` exists so tests can prove every
boundary funneled through.

**Admission is strict, and this is how strict.** A door admits **one exact
shape**: the claimed name, version and content id are its own (`SchemaMismatch`
otherwise), and a nested value's are its field's. Then the value is whole: every
required field present (`MissingField`), every present one of its declared kind
(`TypeMismatch`), a union holding exactly one of its cases (`NoCase`), an open
field's value of a registered shape. Nothing is skipped and nothing is dropped.
Each format refuses an added field in its own way: compat JSON names its fields,
so its decoder refuses a member the door does not declare (`UnknownField`, and
only that decoder raises it); the native body is positional and names none, so a
value written against a shape with another field list carries another content
id and is refused by the identity question (`SchemaMismatch`). A reader meeting
a newer shape is told so at the door, never inside the value.

**A live value is bounded as a decoded one is.** The gate counts the cells a
decode of the value would materialise, as the decoder counts them, and refuses a
value past `kMaxDecodedCells` (`TooLarge`), so no live value is larger than a
boundary would take back. A nested value placed in several fields is counted at
each, a visit: a value whose parts are shared many times over costs the walk it
takes, and the walk stops at the bound
([bounds](bounds.md#the-decode-materialization-bound)).

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
re-registration of the same identity throws `SchemaConflict`. It is also what an
open field's value is checked against ([above](#the-open-field)). The bus takes
every weave's accept-set, declared claim-set, declared emit-set and state schema
at registration — **with every component those shapes nest**, walked by
`collect_referenced` (`zen/schema.hpp`) — so every party that declares a name,
whichever list it declares it in and however deep, agrees on what the name means
before anything routes, and a declaration that contradicts itself never
registers ([decision](../decisions/declared-vocabulary-is-agreed-at-admission.md)).
A loaded artifact's manifest carries the same four lists and the same closure,
so the wall is one wall for native and loaded participants alike. Agreeing on a
shape grants nothing: what a weave may *send* is its grant's business
([capabilities](capabilities.md)). Reads take an immutable snapshot and traverse
it lock-free.

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

Who holds the claims: a `WeaveRecord` for what its weave hears, declares it
says, may say and persists; a Kernel's loaded-artifact record for its manifest;
an isolation `Link` for its mount. The Registry itself knows nothing of weaves,
artifacts or mounts.

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

Suites `schema`, `value`, `gate`, `union` (the union, the open field and the
live budget, through the gate, both codecs, the authoring layer, the manifest's
codec, the composer and the bus), `registry`, `serialize`, `compat`,
`integration`, `fuzz` (deterministic-seeded regression corpus), `weave_shape`
(struct-derived shapes, optional members among them); the entry
`optional_member_types` holds what a `std::optional` member compiles to and
where one is refused.
