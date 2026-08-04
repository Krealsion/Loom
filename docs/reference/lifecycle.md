# Lifecycle — reference

How a weave lives, dies, revives, is replaced in place, and is told it went
live. Laws: [LIFE-01..06](../laws/lifecycle-laws.md). For replacing a *role
holder* with a verified successor, see
[prepared-replacement](prepared-replacement.md).

## Lives and incarnations — two counters, deliberately

| Counter | Advances when | Nowhere else |
|---|---|---|
| `incarnation` | `swap_state` commits new **code** behind a stable id | reload-in-place |
| `life` | a **dead → alive** transition (revival) | every revival path |

One field cannot carry both: a live code reload must *not* invalidate queued
speech (same weave, still alive, mid-sentence), while a death-and-revival must.
Queued envelopes bind to the **life** ([MSG-03](../laws/messaging-laws.md));
deferred answer rights bind to the **incarnation**
([ANS-02](../laws/answer-authority-laws.md)).

## The transitions

- `kill(id)` — marks dead; ends every unfinished conversation the weave was
  party to *before* announcing `Died` ([ANS-04](../laws/answer-authority-laws.md)).
- `reload(id, bytes)` — **crash revival** of the same code: gated against the
  state schema, budgeted by the weave's own policy (`max_reloads`,
  last-known-good fallback). Advances the life.
- `swap_state(id, bytes)` — **intentional** state swap / code hot-reload
  commit: gated, spends no crash budget, no fallback (a malformed candidate
  fails visibly). Advances the incarnation.
- `unregister_weave(id)` — permanent removal; hands the weave back to the
  caller. Pending deliveries to it refuse `NoSuchTarget`; its own queued
  speech dies with it (fail-closed). Returns `nullptr` — changing nothing — for
  an unknown id **and** for the weave whose callback is running (below).

## Permanent removal and the active callback

*A weave outlives its own callback* ([LIFE-06](../laws/lifecycle-laws.md)).

| | |
|---|---|
| success | transfers ownership: a `unique_ptr` the caller may destroy at once |
| unknown id | `nullptr`, nothing mutated |
| the id is the weave whose callback is running | `nullptr`, nothing mutated |

Success is what makes the refusal necessary rather than tidy: the caller
receives a **unique** owner, so there is no honest "erase now, destroy later" —
Loom cannot both hand over unique ownership and secretly retain it. Refusing is
the only answer that keeps the returned owner true.

The rule is **exact to the active target**:

- retry after the callback exits — by return *or* by exception unwind — and the
  removal succeeds normally, with every ordinary effect (role released, claims
  forgotten, deferred conversations ended, transactions invalidated);
- another weave may still be removed **during** the callback, immediately and
  with its ordinary cleanup. This is not `in_dispatch_`, and the distinction is
  load-bearing — the transaction layer removes a *different* weave from inside a
  delivery whenever a candidate refuses its preparation;
- it covers both callbacks Loom makes: ordinary `handle` and the
  committed-activation `handle`. The second matters most — the topology has
  already moved by then, and there is no rollback to reach for;
- Loom's own discard obeys it. `finish_txn` unregisters an aborted transaction's
  sealed candidate; when that candidate *is* the running weave, the discard
  fails and leaves sealed wreckage (nonpublic, owned by no live transaction)
  rather than destroying a live handler.

**Authority, honestly:** ordinary weave code is handed a `WeaveBus`, which has no
`unregister_weave`. This case exists only where host wiring deliberately gives a
weave concrete `Switchboard&` access — a supported pattern, not ambient weave
authority.

## `zen.Activated`

`zen.Activated v1 {sequence}` — Loom's authenticated statement that one exact
incarnation entered the living conversation. Exactly that
([LIFE-01](../laws/lifecycle-laws.md) pins ten non-meanings). Participation is
declared (list the shape in your accept-set), never attempted
([LIFE-02](../laws/lifecycle-laws.md)).

Trust is **provenance, not shape**: `mail.lifecycle_attested()` +
`mail.attested_sequence()` are delivery facts no payload can carry and no
ordinary enqueue can preserve ([ANS-07](../laws/answer-authority-laws.md)). An
ordinary `zen.Activated` — however well-granted — is a costume; consumers
(e.g. Zengine's `ActivationCursor`) ignore it. Consumer identity is the pair
(attested sender, sequence), monotonic per operator lineage
([LIFE-03](../laws/lifecycle-laws.md)).

Two attesting roads exist:

1. **`announce_lifecycle(authority, target, msg, sequence)`** — an ordinary
   *gated send* that carries the attestation. The control door uses it on
   `LoadLibrary`/`ReloadLibrary`. Ordinary grant applies; ordinary answer
   semantics apply.
2. **The committed-admission activation** — not a send at all: Loom's own act,
   part of the admission dispatch, no grant consulted, not answerable
   ([LIFE-05](../laws/lifecycle-laws.md), [PR-08](../laws/replacement-laws.md)).

Minting `LifecycleAuthority` requires the `Switchboard&` itself
(`zen/host/lifecycle_wiring.hpp` — one friend function, host-wiring only), and
an authority is honored only by the board that issued it
([LIFE-04](../laws/lifecycle-laws.md)).

## Graceful swap — the legacy ceremony, and when to prefer it

The Weave Manager's `SwapWeave` composite and the cooperative-handoff **letter**
(`zen.PrepareShutdown` → `zen.Bequest` / `zen.ClaimBequest`) predate prepared
replacement and remain supported. The two ceremonies are **disjoint**: graceful
swap talks to the *outgoing* holder, preserves authored work, verifies nothing
about the successor, and has an observable window; prepared replacement talks
to the *incoming* holder, verifies everything, has no window — and tells the
incumbent nothing. Neither implies the other; applications that need both
verification *and* continuity author the bridge
([known-seams](known-seams.md#continuity-is-authored)).

## Tests

Suites `switchboard` (kill/reload/swap), `provenance` (lives, incarnations,
attestation), `manager` (the door, the letter), `kernel` (admission
activation).
