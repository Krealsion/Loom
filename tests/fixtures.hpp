#ifndef ZEN_TESTS_FIXTURES_HPP
#define ZEN_TESTS_FIXTURES_HPP

// Domain message types used by the tests. They live here, in the test code,
// and NOWHERE in the library: the kernel holds the grammar, never the answers.
// Each is just data fed through the public API.

#include <zen/zen.hpp>

#include <memory>

namespace fx {

using zen::Kind;
using zen::Schema;
using zen::SchemaBuilder;

// A 2-D movement message.
inline std::shared_ptr<const Schema> Move() {
    static const auto s = SchemaBuilder("Move", 1)
                              .field("dx", Kind::Float)
                              .field("dy", Kind::Float)
                              .build();
    return s;
}

// A differently shaped message, for "you claim the wrong door" tests.
inline std::shared_ptr<const Schema> Spawn() {
    static const auto s = SchemaBuilder("Spawn", 1).field("kind", Kind::Text).build();
    return s;
}

// The "discovered at runtime" message of scene B.
inline std::shared_ptr<const Schema> SetColor() {
    static const auto s = SchemaBuilder("SetColor", 1)
                              .field("r", Kind::Int)
                              .field("g", Kind::Int)
                              .field("b", Kind::Int)
                              .field("named", Kind::Bool)
                              .build();
    return s;
}

// A Shard's persisted state — the lock the self chooses (scene C).
inline std::shared_ptr<const Schema> PlayerState() {
    static const auto s = SchemaBuilder("PlayerState", 1)
                              .field("hp", Kind::Int)
                              .field("name", Kind::Text)
                              .build();
    return s;
}

// A policy declared as a value, validated by the gate (scene C).
inline std::shared_ptr<const Schema> ReloadPolicy() {
    static const auto s = SchemaBuilder("ReloadPolicy", 1)
                              .field("max_reloads", Kind::Int)
                              .field("revive_from_last_good", Kind::Bool)
                              .build();
    return s;
}

// A wholly different policy shape, proving the grammar is open (scene C).
inline std::shared_ptr<const Schema> EphemeralPolicy() {
    static const auto s =
        SchemaBuilder("EphemeralPolicy", 1).field("wipe_on_reload", Kind::Bool).build();
    return s;
}

// Exercises Bytes.
inline std::shared_ptr<const Schema> Blob() {
    static const auto s = SchemaBuilder("Blob", 1).field("data", Kind::Bytes).build();
    return s;
}

// Exercises lists of primitives.
inline std::shared_ptr<const Schema> Inventory() {
    static const auto s = SchemaBuilder("Inventory", 1)
                              .field("owner", Kind::Text)
                              .list("items", zen::type_of(Kind::Text))
                              .list("counts", zen::type_of(Kind::Int))
                              .build();
    return s;
}

// Exercises nested messages and a list of messages.
inline std::shared_ptr<const Schema> Squad() {
    static const auto s = SchemaBuilder("Squad", 1)
                              .field("name", Kind::Text)
                              .message("leader", PlayerState())
                              .list("members", zen::type_message(PlayerState()))
                              .build();
    return s;
}

// A schema with an optional field.
inline std::shared_ptr<const Schema> Greeting() {
    static const auto s = SchemaBuilder("Greeting", 1)
                              .field("to", Kind::Text)
                              .field("note", Kind::Text, /*required=*/false)
                              .build();
    return s;
}

} // namespace fx

#endif // ZEN_TESTS_FIXTURES_HPP
