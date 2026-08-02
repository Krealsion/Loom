# Dynamic ABI — reference

The C seam a weave library and the host agree on. Current version: **v5**
(`ZEN_ABI_VERSION` in `include/zen/kernel/abi.h`). Laws:
[KERN-01, KERN-04](../laws/kernel-laws.md),
[ANS-06](../laws/answer-authority-laws.md),
[MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit).

**v5 carries role-authored delivery provenance and gives loaded weaves the
explicit office-authorship doors native weaves already have.** No binary
compatibility is claimed: a v4 artifact refuses at load, naming both versions
— the honest failure, since loading it would leave office speech silently
unspeakable and unobservable on one side of the seam.

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
crosses**; an ordinary dynamic send always returns an invalid `Ticket`), since
v4 the **answer doors** with real success/failure: `answer`, `defer_answer`
(opaque token), `answer_deferred`, `release_deferred` — a refused dynamic
answer is *told* to the weave (`ZEN_ERR_REFUSED`), the parity law
([ANS-06](../laws/answer-authority-laws.md)) — and since v5 the **office
doors**: `office_send`, `office_send_to_role`, `office_publish`. The library
REQUESTS "speak as R"; the host knows the exact weave bound to the context,
verifies membership at that moment, and stamps. A refused authorship crosses
back as the precise `ZEN_ERR_ROLE_AUTHORSHIP_DENIED` (never downgraded to a
personal send), and `office_publish` carries its recipient count out-of-band
so "authorized, zero listeners" and "denied" stay distinct.

Delivery-side, the host passes provenance flags + the attested sequence + the
authored role (v5: its own NUL-terminated parameter, NULL = personal — a
separate axis, deliberately not another `ZEN_PROV_*` kind) into the library
shim, so `mail.answers_ask()` / `mail.lifecycle_attested()` /
`mail.authored_from_role()` read the same on both sides. A library could lie
to *itself* here; it buys nothing — provenance has no wire form and the host
recomputes its own truth.

Out-of-process children receive **null** capability doors and fail closed
(cross-process attestation is deliberately out of scope in V1) — including
the v5 office doors, in both directions: an isolated weave genuinely holding
its role is still refused office authorship at the pipe, and told so.

## Compatibility discipline

The stale-artifact test fixture always declares `ZEN_ABI_VERSION - 1`, so the
refusal pin keeps meaning "the previous ABI refuses" after every bump. A
version number is structurally unobservable within one self-consistent build —
it protects **mixed** artifacts.

## Tests

Suite `kernel` (descriptor gate, byte-sink ownership, dynamic answer parity,
provenance across the seam, office-authorship parity + the v4 refusal); suite
`isolation` (the fail-closed pipe, both directions); Night Lab
`repro_answer_seam.cpp` as the application-shaped witness.
