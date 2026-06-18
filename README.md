# zen-core

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

A third, **`zen-kernel`**, loads Shards from dynamic libraries across a true C
ABI: everything a `.so` hands back crosses as bytes and is re-admitted through
the same gate, so the DLL seam is just another boundary the one gate guards (with
hot-reload that survives a library swap). See the Kernel section of `DESIGN.md`.

A fourth, header-only **`zen-author`** (`include/zen/author/`), is pure sugar: an
author writes each shape once as a plain C++ struct (`ZEN_SHAPE`) and the runtime
`Schema`, the typed conversions, and the derived `snapshot`/`revive`/dispatch all
follow — a struct-derived schema shares a door with the hand-built one by
content-id. See `examples/heartbeat_authored.cpp` and the authoring section of
`DESIGN.md`.

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

## Wire formats

The **native** format is canonical binary: compact, positional, schema-guided,
and byte-identical for equal values (so native bytes are content-addressable).
`serialize` / `parse` are the native entry points. The original self-describing
**JSON** format is retained as a compatibility / debug codec under
`zen::compat::serialize` / `zen::compat::parse` — inspectable, but larger and not
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
    using namespace zen;

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
