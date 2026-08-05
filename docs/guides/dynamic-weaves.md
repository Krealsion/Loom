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

## Build it

A weave is a shared library linking `loom::switchboard` and `loom::core` — the
kernel is the *host's* dependency, not yours. Hand the target to Loom's
reloadable-weave build contract and it applies whatever this platform and
compiler require for `dlclose` to genuinely end that image's static lifetime
([KERN-05](../laws/kernel-laws.md)):

```cmake
find_package(loom REQUIRED)

add_library(my-weave SHARED my_weave.cpp)
target_link_libraries(my-weave PRIVATE loom::core loom::switchboard)
loom_weave_build_contract(my-weave)   # <- the one line that is not yours to skip
```

You do not need to know which compiler option that is, and you should not spell
one yourself. On ELF/GNU it stops your vague-linkage statics — every
`schema_of<T>()` the maker path instantiates for your shapes — from being
emitted `STB_GNU_UNIQUE`, a binding glibc resolves through a process-wide table
that ignores `RTLD_LOCAL` and outlives `dlclose`. Skip it and the second library
sharing your vocabulary header silently reads the first one's destroyed statics;
`dlclose` returns success and `kernel.unload()` returns true while it happens.
On PE-COFF there is nothing to apply and the function applies nothing.

The contract covers a **compilation**, not a file: every translation unit inside
the image needs it. The installed `loom::core` and `loom::switchboard` already
carry it, so linking them costs you nothing extra — but a static library of your
own that you link into a weave is yours to hand over too.

**Building without CMake?** Reproduce the equivalent non-unique symbol semantics
for your toolchain yourself, and then verify the artifact rather than trusting
the flag: `nm -D --defined-only my-weave.so | grep ' u '` must print nothing.

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
