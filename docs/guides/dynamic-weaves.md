# Dynamic weaves

The same weave class, loaded from a shared library at runtime — a stranger's
code, indistinguishable on the bus once admitted.

## Export it

In the library source, after the class from
[writing a weave](writing-a-weave.md):

```cpp
#include <zen/kernel/export.hpp>
ZEN_EXPORT_WEAVE(Responder)   // writes the whole C-ABI descriptor for you
```

Build it as a shared library linking `loom::switchboard` and `loom::core`,
with `-fno-gnu-unique` (the law that keeps unloading real on GCC).

## Load it

```cpp
loom::Switchboard bus;
loom::Kernel kernel(bus);
loom::LoadResult lr = kernel.load("responder-v1", path, "responder");  // role optional
if (!lr.ok) { /* lr.error carries the loader's words */ }
bus.send_to_role("responder", loom::Message(loom::to_value(Ping{1})));
bus.pump();
```

Everything the library emits crosses as **bytes** and is re-admitted through
the one gate host-side ([KERN-01](../laws/kernel-laws.md)) — a malformed
message from a library is refused, never routed. Schemas agree across the seam
by content-id, so both sides compile the same `ZEN_SHAPE` header or they do
not talk.

## Live evolution

- `kernel.reload_from(name, new_path)` — hot-reload behind the same id:
  refused before the incumbent is touched unless the contract matches exactly.
- Replacing a *role-holding service* with a **different** contract, verified
  first, with no gap: that is [replacing a service](replacing-a-service.md).
- `kernel.unload(name)` / `unload_role(role)` — teardown, exact-once by
  construction.

## What is different from native (and what is not)

Answers work identically — a dynamic `mail.answer()` succeeds or is refused
*with the weave told* ([ANS-06](../laws/answer-authority-laws.md)). Office
authorship works identically too (v5): `mail.as_role(...)` requests, the host
verifies the membership at that moment, and incoming
`mail.authored_from_role(...)` reads the same stamped fact a native recipient
reads ([MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit)).
One structural difference: an ordinary dynamic `send` returns no bus ticket
(no seq crosses the C seam), so delivery fate is observed at recipients and
taps, not at the sender — which is the substrate-wide truth anyway
([the seam](../reference/known-seams.md#sender-cannot-observe-send-fate)).
An office-authored dynamic send is sharper: its ticket-validity does cross as
a status, so a refused authorship is *told* rather than silent.

Platforms: canonical on Linux/WSL; the opt-in Windows backend is
development-only and says so ([reference/kernel](../reference/kernel.md)).

## Deeper

Reference: [kernel](../reference/kernel.md) ·
[dynamic-abi](../reference/dynamic-abi.md) (v5). Real artifacts to crib:
`tests/weavelib/` (e.g. `versioned_service.cpp`, `office_worker.cpp`).
