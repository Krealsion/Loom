# Published bounds — reference

Every deliberate capacity in the current system, in one place. A bound
refusing is visible (`Exhausted`, a named `TxnReason`, or a stated shed) —
never a silent drop. Values are the source's; the constant names are grep-able.

## Switchboard

| Bound | Value | Overflow behavior |
|---|---|---|
| `kMaxDeferredAnswers` | 64 | `Exhausted` refusal on the tap; the immediate answer right survives. **Loom-wide, not per weave** |
| `kJournalCapacity` | 1024 | ring: older ticket outcomes read as `Pending`, exactly like unknown seqs |
| `kMaxPreparedReplacements` | 8 | `CapacityExhausted` before anything is inspected |
| `kMaxPreparationBudget` | 1024 | larger requests refused at `begin` |
| `kMaxTerminalOutcomes` | 16 | oldest outcome dropped (evidence, not authority) |
| activation sequence | `INT64_MAX` | the door refuses further activations, naming the boundary; it does not brick |
| deferred-answer tokens | 2^64, monotonic | deliberately unguarded: process-local, never persisted, +1 per deferral |

Also structural (not knobs): one active replacement transaction per incumbent
*and* per candidate; one preparation conversation per transaction.

## Console (operator history)

Both windows are **history**: past observations kept for inspection, on which
nothing is owed. That is what makes discarding the oldest legitimate here and
illegitimate for a backlog. They are shared by the in-process `ConsoleEngine`
and the client-side `RemoteConsole` -- the same two constants, the same window
type, so the local and remote operator see the same horizon.

| Bound | Value | Unit | What it bounds | Overflow behavior |
|---|---|---|---|---|
| `kConsoleTapCapacity` | 1024 | bus events | the tap window (`ConsoleEngine::tap_`, `RemoteConsole::tap_`) | ring: oldest evicted, counted in `Console::evicted().tap` |
| `kConsoleBufferCapacity` | 64 | received `Value`s | the m1/m2/... reply buffer (`ConsoleWeave::received_`, `RemoteConsole::buffer_`) | ring: oldest evicted, counted in `Console::evicted().buffer`; its **label** then refuses |

**Why those two numbers.** The tap matches `kJournalCapacity` deliberately: one
tap entry is roughly one journal entry, so an operator who can still *see* an
event on the tap can still *ask* the journal what became of it. The buffer is
sixteen times smaller because the unit is far heavier -- a `TapEvent` is a few
short strings, while a wire-arrived `Value` is bounded only by
`kMaxDecodedCells` -- and it matches `kMaxPendingDelivered`, the *pending* half
of the same client-side reply path.

**Ownership / reset.** Host-owned and automatic: not a constructor parameter,
not configurable, not widenable by a message. There is no clear operation --
the reset is object lifetime, and a fresh console starts a fresh window with
both counters at zero. `evicted()` is therefore a statistic about *this*
window, never a lifetime total a later console inherits.

**Labels are identities, not positions.** `buffer_at(N)` answers for the reply
labelled `mN`; the retained range is `m(evicted+1) .. m(evicted+size)`. An
evicted label refuses and says it was evicted -- it never re-binds to whatever
reply now occupies that slot, so a reference an operator wrote down either
still means what it meant or fails loudly. Eviction is visible without asking:
the tap and buffer panes carry it in their headings.

## Bridge (remote operator)

| Bound | Value | Behavior |
|---|---|---|
| `kMaxOperatorConnections` | 32 | accept-then-shed, `declined_count()` visible |
| `kMaxPendingDelivered` (client) | 64 | pending unknown-schema replies bounded, drained on `SchemaNone`. An **active backlog**, so it is bounded by refusal (a visible `BridgeRefused`), never by eviction -- dropping the oldest would discard an obligation |
| `kMaxAbsentSchemas` (client) | 64 | remembered `SchemaNone` answers; FIFO, oldest evicted. A **memo**, so eviction costs at most one repeated `Describe` -- and stops a host from growing the client one entry per novel unknown shape |

## Transport channels (framed byte channels)

Both framers -- the isolation `Channel` (parent side of an out-of-process Weave
host) and the portable `BridgeChannel` -- share these.

| Bound | Value | Scope | Behavior |
|---|---|---|---|
| `kMaxFrameLen` | 64 MiB | one frame's payload | over-cap: the channel is marked `failed()` (send) / the framer fails cleanly, no over-read (receive) |
| `kMaxBacklog` | 64 MiB | the **unsent** send backlog, and the unread receive buffer | `failed()` -- a peer that will not drain is contained, never allowed to block, hang or OOM the host |
| live send storage | < 2x the unsent backlog | the outbox buffer | not a knob: an invariant of the amortized prefix reclamation ([LIFE-07](../laws/lifecycle-laws.md)) |
| live receive storage | the unread/incomplete suffix | the inbox buffer | not a knob: the decoded prefix is erased at the end of every poll that decoded it |

The two caps overlap on the send side: since `kMaxFrameLen == kMaxBacklog`, a
single frame at exactly the frame cap already exceeds the backlog cap by its
5-byte header, so the largest frame that can actually be queued is
`kMaxBacklog - 5`. The per-frame check is therefore **masked** by the backlog
check for any one frame -- deliberate overlap, not a redundant guard: the frame
cap is what a *receiver* enforces on a length it was told.

Neither live-storage row is a capacity a caller can exhaust; both are statements
about what the channel is allowed to keep. They are bounds on *history*, and
that is the whole content of [LIFE-07](../laws/lifecycle-laws.md).

## Gate / serialization

Depth and size caps make hostile input total (see
`values-and-admission` and the fuzz suite); the UI vocabulary pins tree depth
≤ 256 and per-kind child arity.

| Bound | Value | Scope | Overflow behavior |
|---|---|---|---|
| `kMaxBinaryDepth` | 64 | one nesting chain | `MalformedBytes` |
| `kMaxListCount` | 2^20 | **one** list | `MalformedField`, "list count exceeds cap" |
| `kMaxFieldBytes` | 2^28 | **one** `Text`/`Bytes` | `MalformedField` (also capped by remaining input) |
| `kMaxDecodedCells` | **65,536** | **the whole decoded value** | `MalformedBytes`, "…exceeds the materialization budget…" |
| `kMaxTypeDepth` | 64 | one schema descriptor's type | thrown refusal from `decode_schema` |

### The decode-materialization bound

**Wire-size limits bound serialized bytes. Decode-materialization limits bound
the trusted host structure those bytes may create. Neither implies
application-semantic validity.**

Serialized size and decoded structural size are different facts. A zero-field
`Message` has a zero-byte presence bitmask, so it costs no body bytes at all — a
list of them commands a host-side population unrelated to the input's length. A
compact value may legitimately represent many values; it may not command
effectively unbounded host work.

- **Unit** — one *decoded cell*: one `Cell`-sized slot the decoder
  materialises. One per **declared** field of every message it enters (a `Value`
  allocates exactly that many `std::optional<Cell>` slots, present or not) and
  one per element of every list it decodes. So a message of *n* fields costs *n*
  whether or not those fields arrive; a list of *k* elements costs *k*, plus
  whatever each element then costs. `Text`/`Bytes` **payload** bytes are not
  counted — a 1 MiB `Bytes` field is one cell (its size is `kMaxFieldBytes`'
  business).
- **Scope** — one allowance per **top-level decode**, shared by every nested
  message, list, field and recursive helper. It is never reset per container, so
  two individually-modest lists cannot be summed past it. Schema descriptors are
  ordinary values and spend the same budget.
- **Boundary** — **inclusive**: a decode totalling exactly 65,536 cells is
  accepted; the first cell beyond it is refused.
- **When** — spent *before* the cells exist. A list charges its whole declared
  element count before building the first element. Exhaustion is a refusal, not
  an allocation regretted afterwards, and never a `std::bad_alloc` backstop.
- **Failure** — the ordinary parse-failure model: `admit()` returns a rejection
  with `ErrorKind::MalformedBytes` and a detail naming the budget. No partial
  `Value` escapes, none is admitted, no registry entry is derived from it, no
  message is delivered.
- **Ownership** — host-owned and automatic. Not a `parse()` parameter, not
  widenable by a message, grant, schema, or payload; changing it is a build
  decision (`src/detail/binary.hpp`).

65,536 cells is roughly 6 MiB of worst-case decoded structure — an order of
magnitude above the largest value any current consumer sends. A value larger
than the bound cannot cross a serialized boundary at all: an in-process `Value`
may exceed it, but nothing will re-admit its bytes.

## Isolation resource defaults (computed, no knob)

memory = RAM/8 (cap 1 GiB, floor 128 MiB) · pids = 512 · cpu_weight = 100.
`with_unlimited_memory()` removes the memory cap **alone** — pids stays
bounded; no grant can license a fork bomb.

## Zengine Timer (owned there, listed for reach)

`kMaxHandoffEntries = 32` · `kPreparedClaimBeats = 8` (derived + published) ·
`kBeatCapMs = 10`. See
[Zengine timer reference](../../../Zengine/docs/reference/timer-continuity.md).
