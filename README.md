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

```
include/zen/   public headers          src/      implementation
tests/         suite (doctest)         examples/ runnable quickstart
DESIGN.md      the full design rationale
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

## End to end

Define a schema → register it → build a value → admit it → serialize →
deserialize → re-admit. (This is `examples/quickstart.cpp`.)

```cpp
#include <zen/zen.hpp>

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

    // 5. Serialize for persistence — the bytes carry the schema's identity.
    std::string bytes = serialize(v);
    std::cout << bytes << "\n";

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

    // A corrupted candidate is refused, never repaired.
    Unverified corrupt =
        parse(R"({"zen":1,"schema":"PlayerState","version":1,"fields":{"hp":"oops"}})");
    Admission refused = admit(corrupt, registry);
    std::cout << "corrupt admitted? " << std::boolalpha << refused.ok() << "\n";
    if (!refused.ok()) {
        std::cout << "  " << refused.first_error().message() << "\n";
    }
    return 0;
}
```

Running it prints the self-describing bytes, the revived value, and a precise
refusal for the corrupted one:

```
{"zen":1,"schema":"PlayerState","version":1,"content_id":"0x...","fields":{"hp":"30","name":"Ami"}}
revived hp=30 name=Ami
corrupt admitted? false
  hp: MalformedField (expected Int, got json:string) — not a base-10 64-bit integer
```

See [DESIGN.md](DESIGN.md) for the public API, the ownership/threading model and
why, the wire format, the version policy, and the seams left open for codegen,
schema-as-value reflection, and behavioral contracts.
