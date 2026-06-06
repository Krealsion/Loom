// The smallest end-to-end use of zen-core: define a schema, register it, build a
// value, admit it at the bus, serialize, read it back as Unverified, and re-admit
// through the same gate. Mirrors the README. Uses only the public API.

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
