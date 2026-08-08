# Dynamic weaves

The same weave class, loaded from a shared library at runtime — a stranger's
code, indistinguishable on the bus once admitted.

## What loading it in-process means

`Kernel::load` is `dlopen`. **The library's native code runs inside the host
process's own address space, and nothing on this page changes that.** It
shares the heap, the stack, the loader's namespace and every mapping the host
holds; it can read and write host memory directly, without sending a message.

Admission, schema validation and capability routing do **not** create memory
isolation for that code. They are the same mechanisms whether a weave is
native or loaded, and they govern the *bus* — which is a different boundary
from the *process*:

```text
bus authority             what a weave may SAY, to whom.
                          Mediated by Zen: the grant is checked at delivery,
                          the sender is stamped from the connection, and the
                          gate admits the bytes. A loaded weave gets exactly
                          what a native one gets, and no more.

process-memory authority  what native code may TOUCH.
                          Inherent in executing native code in-process. Zen
                          imposes nothing here, and there is nothing it could
                          impose: an in-process library that wants host memory
                          simply reads it.
```

So: **an ordinary in-process dynamic weave is trusted at the process-memory
level.** `Kernel::containment_note()` says so in one line —
*"in-process; trusted; no OS sandbox"* — and `Kernel::load`'s default grant is
`Grant{}.allow_any()`, a deliberately permissive *bus* default that is
consistent with a participant already trusted with the address space. Load
artifacts you would run as your own code.

Running a stranger's artifact under an OS boundary instead is a **different
mechanism with a different threat model**: the out-of-process host
(`IsolationHost` → `zen-weave-host`), where the projection is the syscall
boundary rather than the message boundary — namespaces, a cgroup leaf, an
authored exec boundary, positively re-confirmed
([capabilities](../reference/capabilities.md#os-containment-out-of-process-linuxwsl)).
That tier is **abuse, not escape**: it stops buggy or greedy code, and it is
not a claim that hostile code cannot break out. Neither tier is a place to run
an artifact you actively distrust.

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
On **MSVC** you additionally have to pass `/Zc:preprocessor` yourself — Loom's
public shape macros are C++20 `__VA_OPT__` and the default traditional
preprocessor mis-expands them. The supported CMake targets apply it for you;
this is the one thing the package cannot hand a consumer who never links it.

These are two different laws and they do not travel together. The unique-symbol
mitigation is about a **loadable image's static lifetime** and is ELF/GNU-only
(`loom_weave_build_contract`, which you call). The preprocessor requirement is
about **compiling Loom's public headers at all**, is MSVC-only, and binds every
consumer of `ZEN_SHAPE` whether or not they ever build a weave — which is why it
rides `loom::core` and arrives on its own.

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
authorship works identically too (since ABI v5): `mail.as_role(...)`
requests, the host verifies the membership at that moment, and incoming
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
[dynamic-abi](../reference/dynamic-abi.md) (current: **v6**) ·
[capabilities](../reference/capabilities.md#the-grant-in-process) (the
in-process trust statement above, in its own reference). Real artifacts to
crib:
`tests/weavelib/` (e.g. `versioned_service.cpp`, `office_worker.cpp`).
