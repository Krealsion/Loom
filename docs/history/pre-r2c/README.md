> **Historical architecture record — Status: FROZEN.** Source commit: Loom `78d64ea`
> (pre-R2C-0 consolidation, preserved unabridged). This document keeps the reasoning,
> experiments, intermediate laws, rejected designs and architecture state that produced
> the current system. It is not the recommended introduction to Zen.
> Current behavior → `docs/reference/` · authoring → `docs/guides/` · concise
> invariants → `docs/laws/` · router → `docs/README.md`.

# loom

The foundational layer of **Zen**: the message-representation and validation
core every other part of the system links against.

A Zen value **carries its own shape** — a reference to the schema it claims to
be. That makes it typed enough to be *challenged* at any boundary, and dynamic
enough to be *built at runtime* from a schema that was discovered (say, from a
DLL loaded seconds ago) rather than compiled in. Exactly one gate, `admit`,
guards every boundary: the live message bus and the persistence layer reach the
same validator. Nothing crosses a boundary it cannot prove it belongs across.

The library holds the **grammar, never the answers**: it provides schema, value,
gate, registry, and serialization, and hard-codes no application message type
and no policy.

A second library, **`zen-switchboard`**, builds the first *live* boundary on top
of the core: an in-process message bus that gates every delivery through the same
`admit`. See `examples/heartbeat.cpp` and the Switchboard section of `DESIGN.md`.

A third, **`zen-kernel`**, loads Weaves from dynamic libraries across a true C
ABI: everything a `.so` hands back crosses as bytes and is re-admitted through
the same gate, so the DLL seam is just another boundary the one gate guards (with
hot-reload that survives a library swap). See the Kernel section of `DESIGN.md`.

A fourth, header-only **weave** layer (`include/zen/weave/`), is pure sugar: a
maker writes each shape once as a plain C++ struct (`ZEN_SHAPE`) and the runtime
`Schema`, the typed conversions, and the derived `snapshot`/`revive`/dispatch all
follow — a struct-derived schema shares a door with the hand-built one by
content-id. See `examples/heartbeat_woven.cpp` and the weaving section of
`DESIGN.md`.

**Capabilities (B1):** every Weave carries a host-assigned **grant** (default
empty), and the bus authorizes each Weave-originated send against it *before* the
gate — a denied send is `CapabilityDenied`, never delivered, and never reaches the
gate, so "one gate" stays literally true. The kernel's control Weave makes the
load surface a gated message door. See the Capabilities section of `DESIGN.md`.

```
include/zen/             public headers (core)
include/zen/switchboard/ public headers (bus)
src/                     implementation        tests/   suite (doctest)
examples/                quickstart, heartbeat  DESIGN.md  the full design rationale
```

## The spine

- **One gate, every boundary.** A single validator admits live messages and
  bytes read back from storage. There is no second code path.
- **Untrusted until proven.** Deserialization yields an `Unverified` with no
  field accessors; the only road to a usable `Value` is through `admit`. You
  cannot forget to validate.
- **Published schemas are immutable.** A registered `(name, version)` is frozen;
  you register a new version, never mutate the old.
- **No undefined behavior on hostile input.** Deserializing arbitrary or
  malicious bytes never crashes, leaks, or produces a trusted value — it yields a
  valid `Value` (post-gate) or a clean, machine-readable error.

## Build & test

Requires CMake ≥ 3.16 and a C++20 compiler (verified on GCC 11.4).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build

# with AddressSanitizer + UndefinedBehaviorSanitizer
cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DZEN_SANITIZE=ON
cmake --build build-san
ctest --test-dir build-san
```

The library builds clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Werror`, and the suite is green under the sanitizers.

**The `isolation` and `policy` suites need a delegated cgroup-v2 scope** — real OS
containment requires an unprivileged user namespace plus a delegated cgroup subtree, so
these suites are launched via `tests/run-under-scope.sh` (which ctest invokes for them).
Run outside such a scope — e.g. a plain `wsl bash` in the root cgroup — the OS-enforcement
cases **fail hard by design**, naming the missing capability, rather than pass having
verified nothing (see `tests/enforcement_gate.hpp`). On a host that genuinely cannot
enforce, `ZEN_ALLOW_UNENFORCEABLE=1` converts those into marked-degraded skips. The
portable suites (core, switchboard, bridge, …) need none of this and run everywhere,
including the Windows/MinGW build.

## Consuming loom from another project

`loom` installs as a CMake package, so a separate project consumes it the way it
would any third-party library — no sibling includes, no vendoring:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(loom 0.1 REQUIRED)

add_executable(my_weave main.cpp)
target_link_libraries(my_weave PRIVATE loom::core)          # values, schemas, the gate
# target_link_libraries(my_weave PRIVATE loom::switchboard) # ...and the live bus
```

```sh
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/prefix
```

The **exported surface is deliberately smaller than the build tree**: `loom::core`,
`loom::switchboard`, and `loom::kernel`, plus the headers they need. (The package
also defines `loom::sanitize` and `loom::warnings` — two INTERFACE targets that
*must* ride the export set because they survive in the libraries' link interface.
They are plumbing, not surface: `warnings` is carried `LINK_ONLY`, so the Loom's
`-Werror` and `-Wconversion` never reach your sources.) The UI vocabulary, the
console, the TUI, the bridge and the SDL skin build here today but are
Zengine-destined — each moves out in its own port phase — so exporting them now
would publish a surface about to be relocated.

`loom::kernel` joined the export when its first hosting consumer appeared
(Zengine's snake slice). It exists on Linux always; **on Windows only when the
Loom was configured with `LOOM_ENABLE_WINDOWS_KERNEL=ON`** — an explicit
development/demo backend (`LoadLibrary`) that hosts weaves with **no isolation
and says so** (`Kernel::containment_note()`; the sandbox and the honesty
lattice's enforced rungs are Linux-only, and the security story remains
WSL-hosting). Gate on `if(TARGET loom::kernel)` — the honest question is "can
this install host loadable weaves?", not "which OS is this?". Isolation still
waits for its first out-of-process consumer.

Exported target names match the in-tree aliases exactly, so a consumer can swap a
sibling-source build (`add_subdirectory`) for the installed package without touching
a single `target_link_libraries` line.

## Where this lives

The Loom is one of two repositories under a shared `Zen/` root:

```
Zen/
  Loom/        this repo — the substrate, everyone's
  Zengine/     the default set of weaves; the Loom's first external consumer
  playground/  your own weaves
```

Zengine consumes the Loom by the stranger's path, which is what keeps the dependency
arrow un-invertible: **the Loom's build cannot see Zengine.** Rough edges in the
public surface therefore hit the house before they hit a guest.

**Per-repo green.** The Loom's suite runs in the Loom; Zengine's lane runs Zengine's
tests against its pinned/installed Loom and does *not* re-run this suite — a
dependency's proof rides its version. Every report-back states which repo's green was
proven; "green" must never silently mean "green in one of two." *Today-note:* the Loom
is still under active development, so its delegated-scope suite runs here every phase;
the don't-re-prove economy arrives as the Loom stabilizes.

Assistant sessions are launched from the `Zen/` root, never from inside a sub-repo —
the memory graph is keyed to that path. Run git per-repo (`git -C Loom status`).

## Wire formats

The **native** format is canonical binary: compact, positional, schema-guided,
and byte-identical for equal values (so native bytes are content-addressable).
`serialize` / `parse` are the native entry points. The original self-describing
**JSON** format is retained as a compatibility / debug codec under
`loom::compat::serialize` / `loom::compat::parse` — inspectable, but larger and not
byte-canonical. Both formats funnel through the same gate; deserializing either
yields an `Unverified` that the same `admit` validates.

## End to end

Define a schema → register it → build a value → admit it → serialize →
deserialize → re-admit. (This is `examples/quickstart.cpp`.)

```cpp
#include <zen/zen.hpp>

#include <cstdio>
#include <iostream>

int main() {
    using namespace loom;

    // 1. Define a schema (frozen once published).
    auto player = SchemaBuilder("PlayerState", 1)
                      .field("hp", Kind::Int)
                      .field("name", Kind::Text)
                      .build();

    // 2. Register it — e.g. discovered at runtime from a freshly loaded module.
    Registry registry;
    registry.register_schema(player);

    // 3. Build a value against the schema.
    Value v(player);
    v.set("hp", Cell::integer(30)).set("name", Cell::text("Ami"));

    // 4. Admit it at the bus boundary (consumes the candidate, re-emits it trusted).
    if (Admission live = admit(Value(v), *player); !live.ok()) {
        std::cerr << "refused: " << live.first_error().message() << "\n";
        return 1;
    }

    // 5. Serialize to the native canonical binary format (compact; the header
    //    carries the schema identity). Show the size and the "ZN" magic.
    std::string bytes = serialize(v);
    std::printf("native: %zu bytes, magic '%c%c' v%d\n", bytes.size(), bytes[0], bytes[1],
                static_cast<int>(static_cast<unsigned char>(bytes[2])));

    // 6. Read it back. Untrusted until proven: this is an Unverified, not a Value.
    Unverified candidate = parse(bytes);

    // 7. Re-admit through the SAME gate, resolving the claim via the registry.
    Admission revived = admit(candidate, registry);
    if (!revived.ok()) {
        std::cerr << "refused: " << revived.first_error().message() << "\n";
        return 1;
    }
    std::cout << "revived hp=" << revived.value().get("hp")->as_int()
              << " name=" << revived.value().get("name")->as_text() << "\n";

    // The compat JSON codec gives an inspectable view of the same value.
    std::cout << "compat json: " << compat::serialize(v) << "\n";

    // A corrupted candidate is refused, never repaired (shown via the readable
    // compat path so the diagnosis is legible).
    Unverified corrupt =
        compat::parse(R"({"zen":1,"schema":"PlayerState","version":1,"fields":{"hp":"oops"}})");
    Admission refused = admit(corrupt, registry);
    std::cout << "corrupt admitted? " << std::boolalpha << refused.ok() << "\n";
    if (!refused.ok()) {
        std::cout << "  " << refused.first_error().message() << "\n";
    }
    return 0;
}
```

Running it prints the compact native size, the revived value, the inspectable
JSON view, and a precise refusal for the corrupted one:

```
native: 34 bytes, magic 'ZN' v1
revived hp=30 name=Ami
compat json: {"zen":1,"schema":"PlayerState","version":1,"content_id":"0xb1d69bad13ae83d6","fields":{"hp":"30","name":"Ami"}}
corrupt admitted? false
  hp: MalformedField (expected Int, got json:string) — not a base-10 64-bit integer
```

See [DESIGN.md](DESIGN.md) for the public API, the ownership/threading model and
why, the wire format, the version policy, and the seams left open for codegen,
schema-as-value reflection, and behavioral contracts.
