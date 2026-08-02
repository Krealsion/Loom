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

## Bridge (remote operator)

| Bound | Value | Behavior |
|---|---|---|
| `kMaxOperatorConnections` | 32 | accept-then-shed, `declined_count()` visible |
| `kMaxPendingDelivered` (client) | 64 | pending unknown-schema replies bounded, drained on `SchemaNone` |

## Gate / serialization

Depth and size caps make hostile input total (see
`values-and-admission` and the fuzz suite); the UI vocabulary pins tree depth
≤ 256 and per-kind child arity.

## Isolation resource defaults (computed, no knob)

memory = RAM/8 (cap 1 GiB, floor 128 MiB) · pids = 512 · cpu_weight = 100.
`with_unlimited_memory()` removes the memory cap **alone** — pids stays
bounded; no grant can license a fork bomb.

## Zengine Timer (owned there, listed for reach)

`kMaxHandoffEntries = 32` · `kPreparedClaimBeats = 8` (derived + published) ·
`kBeatCapMs = 10`. See
[Zengine timer reference](../../../Zengine/docs/reference/timer-continuity.md).
