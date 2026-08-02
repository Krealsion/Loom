# Dynamic ABI — reference

The C seam a weave library and the host agree on. Current version: **v4**
(`ZEN_ABI_VERSION` in `include/zen/kernel/abi.h`). Laws:
[KERN-01, KERN-04](../laws/kernel-laws.md),
[ANS-06](../laws/answer-authority-laws.md).

## Shape

A library exports one versioned descriptor (`ZenWeaveAbi`): create/destroy,
manifest reconstruction (accepted schemas + state schema, delivered as
**bytes** the host re-admits), handle (bytes in), snapshot (bytes out via
`ZenByteSink` — the library allocates into the host's sink; no cross-allocator
free, no host pointer into library memory), revive, policy. `ZEN_EXPORT_WEAVE`
writes all of it for a `Weave` subclass; library authors almost never touch
the ABI directly ([guide](../guides/dynamic-weaves.md)).

An artifact declaring any other version is refused at load, naming both
versions ([KERN-04](../laws/kernel-laws.md)).

## Host services (what a loaded weave's `Bus` really is)

The library-side `Bus` shim forwards through host callbacks: `send`,
`send_to_role`, `publish` (fire-and-forget across the seam — **no bus ticket
crosses**; an ordinary dynamic send always returns an invalid `Ticket`), and
since v4 the **answer doors** with real success/failure: `answer`,
`defer_answer` (opaque token), `answer_deferred`, `release_deferred`. A
refused dynamic answer is *told* to the weave (`ZEN_ERR_REFUSED`) — the
parity law ([ANS-06](../laws/answer-authority-laws.md)).

Delivery-side, the host passes provenance flags + the attested sequence into
the library shim, so `mail.answers_ask()` / `mail.lifecycle_attested()` read
the same on both sides. A library could lie to *itself* here; it buys nothing
— provenance has no wire form and the host recomputes its own truth.

Out-of-process children receive **null** capability doors and fail closed
(a cross-process answer capability is deliberately out of scope).

## Compatibility discipline

The stale-artifact test fixture always declares `ZEN_ABI_VERSION - 1`, so the
refusal pin keeps meaning "the previous ABI refuses" after every bump. A
version number is structurally unobservable within one self-consistent build —
it protects **mixed** artifacts.

## Tests

Suite `kernel` (descriptor gate, byte-sink ownership, dynamic answer parity,
provenance across the seam); Night Lab `repro_answer_seam.cpp` as the
application-shaped witness.
