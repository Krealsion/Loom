// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include <zen/kernel/schema_codec.hpp>
#include <zen/serialize.hpp>
#include <zen/zen.hpp>

// kMaxDecodedCells: the F-19 ceiling case meets the decode-materialization
// bound, and the two are pinned as ADJACENT below, so this reads the real constant.
#include "../src/detail/binary.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace loom;
using namespace loom;

namespace {

// encode a schema as a descriptor, send it through the gated bytes path, decode.
std::shared_ptr<const Schema> round_trip(const std::shared_ptr<const Schema>& s,
                                         const Registry& deps) {
    std::string bytes = serialize(encode_schema(*s));
    Unverified u = parse(bytes);
    REQUIRE(u.well_formed());
    Admission a = admit(u, schema_desc_schema());
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    return decode_schema(a.value(), deps);
}

// A schema descriptor whose single field's type is a FLAT stream of `n` List
// tokens followed by one Int token — i.e. List<List<…Int>> nested n deep, exactly
// what a hostile .so's describe() could emit. Built directly as tokens (not via
// encode_schema of a real n-deep type), because the whole point is that the token
// stream is flat: its length is bounded only by kMaxListCount (~1M), never by any
// value-depth cap, so it stays tiny and passes the meta-schema gate. Returns the
// serialized descriptor bytes — the same bytes the host admits then reconstructs.
std::string deep_list_descriptor_bytes(int n) {
    std::vector<Cell> tokens;
    tokens.reserve(static_cast<std::size_t>(n) + 1);
    for (int k = 0; k < n; ++k) {
        Value list_tok(type_token_schema());
        list_tok.set("kind", Cell::integer(static_cast<std::int64_t>(Kind::List)));
        tokens.push_back(Cell::message(std::move(list_tok)));
    }
    Value int_tok(type_token_schema());
    int_tok.set("kind", Cell::integer(static_cast<std::int64_t>(Kind::Int)));
    tokens.push_back(Cell::message(std::move(int_tok)));

    Value fd(field_desc_schema());
    fd.set("name", Cell::text("x"));
    fd.set("required", Cell::boolean(true));
    fd.set("type", Cell::list(std::move(tokens)));

    Value desc(schema_desc_schema());
    desc.set("name", Cell::text("Evil"));
    desc.set("version", Cell::integer(1));
    desc.set("fields", Cell::list({Cell::message(std::move(fd))}));
    return serialize(desc);
}

} // namespace

TEST_SUITE("schema_codec") {

TEST_CASE("a flat schema round-trips through the gated descriptor with identical identity") {
    auto s = SchemaBuilder("Ping", 1).field("seq", Kind::Int).build();
    Registry deps;
    auto back = round_trip(s, deps);
    CHECK(back->content_id() == s->content_id());
    CHECK(back->name() == "Ping");
    CHECK(back->version() == 1);
}

TEST_CASE("every primitive kind, required and optional, survives") {
    auto s = SchemaBuilder("Prims", 3)
                 .field("i", Kind::Int)
                 .field("f", Kind::Float)
                 .field("t", Kind::Text, /*required=*/false)
                 .field("b", Kind::Bool)
                 .field("y", Kind::Bytes)
                 .build();
    Registry deps;
    CHECK(round_trip(s, deps)->content_id() == s->content_id());
}

TEST_CASE("nested messages and nested lists survive, resolved via the dependency registry") {
    auto inner = SchemaBuilder("Inner", 2).field("x", Kind::Int).build();
    auto outer = SchemaBuilder("Outer", 1)
                     .message("in", inner)
                     .list("tags", type_of(Kind::Text))
                     .list("rows", type_list(type_message(inner))) // List<List<Message(Inner)>>
                     .build();

    Registry deps;
    deps.register_schema(inner); // a referenced schema must be resolvable first
    auto back = round_trip(outer, deps);
    CHECK(back->content_id() == outer->content_id());
}

TEST_CASE("a manifest carries the accept-set and state schema") {
    auto ping = SchemaBuilder("Ping", 1).field("seq", Kind::Int).build();
    auto pong = SchemaBuilder("Pong", 1).field("seq", Kind::Int).build();
    auto counter = SchemaBuilder("Counter", 1).field("count", Kind::Int).build();

    std::vector<std::shared_ptr<const Schema>> accepted{ping, pong};
    std::string bytes = serialize(encode_manifest(accepted, *counter));

    Unverified u = parse(bytes);
    REQUIRE(u.well_formed());
    Admission a = admit(u, manifest_schema());
    REQUIRE(a.ok());
    const Value& manifest = a.value();

    Registry deps;
    std::vector<std::shared_ptr<const Schema>> rebuilt;
    for (const Cell& c : manifest.get("accepted")->as_list()) {
        auto s = decode_schema(*c.as_message(), deps);
        deps.register_schema(s);
        rebuilt.push_back(s);
    }
    auto state = decode_schema(*manifest.get("state")->as_message(), deps);

    REQUIRE(rebuilt.size() == 2);
    CHECK(rebuilt[0]->content_id() == ping->content_id());
    CHECK(rebuilt[1]->content_id() == pong->content_id());
    CHECK(state->content_id() == counter->content_id());
}

TEST_CASE("a manifest is self-contained: nested component schemas travel in `referenced` "
          "and resolve into an EMPTY registry") {
    // The exact shape that found the hole (Zengine's snake): a state that nests
    // a component both as a List<Message> and as a message field — plus a
    // two-deep chain (Outer nests Mid nests Pos) to pin the post-order
    // guarantee, not just one level.
    auto pos = SchemaBuilder("Pos", 1).field("x", Kind::Int).field("y", Kind::Int).build();
    auto mid = SchemaBuilder("Mid", 1).message("at", pos).build();
    auto world = SchemaBuilder("SnakeWorldState", 1)
                     .field("width", Kind::Int)
                     .list("snake", type_message(pos))
                     .message("food", pos)
                     .build();
    auto outer = SchemaBuilder("Outer", 1).message("m", mid).build();
    auto tick = SchemaBuilder("Tick", 1).build();

    const std::vector<std::shared_ptr<const Schema>> accepted{tick, outer};
    const std::string bytes = serialize(encode_manifest(accepted, *world));

    Unverified u = parse(bytes);
    REQUIRE(u.well_formed());
    Admission a = admit(u, manifest_schema());
    REQUIRE(a.ok());
    const Value& manifest = a.value();

    SUBCASE("the reconstruct sequence resolves everything from the manifest alone") {
        Registry deps; // EMPTY: the manifest must bring its own components
        decode_referenced(manifest, deps);
        std::vector<std::shared_ptr<const Schema>> rebuilt;
        for (const Cell& c : manifest.get("accepted")->as_list()) {
            auto s = decode_schema(*c.as_message(), deps);
            deps.register_schema(s);
            rebuilt.push_back(s);
        }
        auto state = decode_schema(*manifest.get("state")->as_message(), deps);
        REQUIRE(rebuilt.size() == 2);
        CHECK(rebuilt[0]->content_id() == tick->content_id());
        CHECK(rebuilt[1]->content_id() == outer->content_id());
        CHECK(state->content_id() == world->content_id());
        // The components arrived with true identity, not just resolvability.
        REQUIRE(deps.lookup("Pos", 1) != nullptr);
        CHECK(deps.lookup("Pos", 1)->content_id() == pos->content_id());
        REQUIRE(deps.lookup("Mid", 1) != nullptr);
        CHECK(deps.lookup("Mid", 1)->content_id() == mid->content_id());
    }

    SUBCASE("the section is load-bearing: skipping it reproduces the original refusal") {
        Registry deps;
        // Straight to the state descriptor with no referenced pass — exactly
        // what every decode site did before v3, and why the first nested
        // consumer's load refused with "unresolved nested schema 'Pos'".
        CHECK_THROWS_AS(decode_schema(*manifest.get("state")->as_message(), deps),
                        std::runtime_error);
    }

    SUBCASE("a flat manifest stays lean: no referenced section is emitted at all") {
        auto counter = SchemaBuilder("Counter", 1).field("count", Kind::Int).build();
        const std::vector<std::shared_ptr<const Schema>> flat{tick};
        const std::string flat_bytes = serialize(encode_manifest(flat, *counter));
        Unverified fu = parse(flat_bytes);
        REQUIRE(fu.well_formed());
        Admission fa = admit(fu, manifest_schema());
        REQUIRE(fa.ok());
        CHECK(fa.value().get("referenced") == nullptr);
    }
}

TEST_CASE("decode_schema refuses a pathologically deep type-token stream instead of "
          "overflowing the host stack") {
    // Audit F-19 (sign-off blocker). The type-token stream is FLAT, so its length is
    // capped by kMaxListCount (~1M), NOT by the value-depth cap — a field typed
    // List<List<…Int>> nested tens of thousands deep encodes to a small descriptor that
    // PASSES the meta-schema gate, then, before the cap, drove decode_type to recurse
    // once per List token and SIGSEGV'd the trusted host at mount time, uncatchable.
    //
    // This exercises the REAL decode path — hand-built descriptor -> gate -> decode_schema,
    // the exact bytes host.cpp/kernel.cpp/remote_console.cpp feed. (The in-process
    // make_schema path never calls decode_type; a green there proved nothing about this.)

    SUBCASE("exactly at the cap still decodes — the bound does not reject legitimate nesting") {
        std::string bytes = deep_list_descriptor_bytes(kMaxTypeDepth); // 64 nested lists
        Unverified u = parse(bytes);
        Admission a = admit(u, schema_desc_schema());
        REQUIRE(a.ok());
        Registry deps;
        CHECK_NOTHROW(decode_schema(a.value(), deps));
    }

    SUBCASE("one past the cap is refused cleanly, not crashed") {
        std::string bytes = deep_list_descriptor_bytes(kMaxTypeDepth + 1); // 65 nested lists
        Unverified u = parse(bytes);
        Admission a = admit(u, schema_desc_schema());
        REQUIRE(a.ok()); // the gate admits it: the depth is invisible to the meta-schema
        Registry deps;
        CHECK_THROWS_AS(decode_schema(a.value(), deps), std::runtime_error);
    }

    // THE DECODE-MATERIALIZATION BUDGET IS A SECOND BOUND IN FRONT OF THIS ONE, and
    // the two must be shown to meet with no gap between them.
    // docs/reference/bounds.md#the-decode-materialization-bound A descriptor of n nested List tokens materialises
    //     3 (SchemaDesc slots) + 1 (fields element) + 3 (Field slots)
    //       + (n+1) (type-token elements) + 3(n+1) (each TypeToken's slots)  =  11 + 4n
    // decoded cells, so the gate now admits a stream only while 11 + 4n fits the
    // materialization budget. Above that the descriptor never becomes a Value at all.
    const int deepest_admissible = static_cast<int>((detail::kMaxDecodedCells - 11) / 4);

    SUBCASE("the deepest descriptor the budget still admits is still refused by kMaxTypeDepth") {
        // The F-19 spine, unchanged, at the largest size the budget leaves reachable: the gate
        // admits (the depth stays invisible to the meta-schema) and decode_type refuses.
        std::string bytes = deep_list_descriptor_bytes(deepest_admissible);
        Unverified u = parse(bytes);
        Admission a = admit(u, schema_desc_schema());
        REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
        Registry deps;
        CHECK_THROWS_AS(decode_schema(a.value(), deps), std::runtime_error);
    }

    SUBCASE("one token deeper, and the gate itself refuses — the two bounds are adjacent") {
        std::string bytes = deep_list_descriptor_bytes(deepest_admissible + 1);
        Unverified u = parse(bytes);
        Admission a = admit(u, schema_desc_schema());
        REQUIRE_FALSE(a.ok());
        CHECK(a.first_error().kind == ErrorKind::MalformedBytes);
        CHECK(a.first_error().detail.find("materialization budget") != std::string::npos);
    }

    SUBCASE("the auditor's ceiling case N=100000 no longer even reaches decode_schema") {
        // The F-19 era's record: ~200 KB of descriptor, well under the frame cap, which the
        // gate ADMITTED — leaving kMaxTypeDepth as the only thing between it and the host's
        // stack. The budget refuses it a layer earlier (400,011 cells, far past it), so a
        // descriptor big enough to have threatened the stack is no longer an admitted value.
        // kMaxTypeDepth is not thereby redundant: it still owns every in-budget case above.
        std::string bytes = deep_list_descriptor_bytes(100000);
        Unverified u = parse(bytes);
        Admission a = admit(u, schema_desc_schema());
        REQUIRE_FALSE(a.ok());
        CHECK(a.first_error().kind == ErrorKind::MalformedBytes);
        CHECK(a.first_error().detail.find("materialization budget") != std::string::npos);
    }
}

TEST_CASE("a descriptor that lies about its shape is refused by the meta-schema gate") {
    // A manifest payload claiming zen.Manifest but missing the required 'state'.
    Value broken(manifest_schema());
    broken.set("accepted", Cell::list({}));
    // 'state' deliberately unset
    Unverified u = parse(serialize(broken));
    Admission a = admit(u, manifest_schema());
    CHECK_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MissingField);
}

// ---- zen.Manifest v5: the emit-set crosses, and a contradiction cannot ----------------
// docs/decisions/declared-vocabulary-is-agreed-at-admission.md

TEST_CASE("a manifest (v5) carries the declared emit-set and the components it nests, and "
          "a weave declaring no emits sends no section") {
    auto ping = SchemaBuilder("Ping", 1).field("seq", Kind::Int).build();
    auto counter = SchemaBuilder("Counter", 1).field("count", Kind::Int).build();
    auto greet = SchemaBuilder("Greet", 1).field("msg", Kind::Text).build();
    auto part = SchemaBuilder("Part", 1).field("a", Kind::Int).build();
    auto whole = SchemaBuilder("Whole", 1).list("parts", type_message(part)).build();

    const std::vector<std::shared_ptr<const Schema>> accepted{ping};
    const std::vector<std::shared_ptr<const Schema>> emits{greet, whole};
    const std::string bytes =
        serialize(encode_manifest(accepted, *counter, nullptr, nullptr, &emits));
    Unverified u = parse(bytes);
    REQUIRE(u.well_formed());
    Admission a = admit(u, manifest_schema());
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    const Value& manifest = a.value();
    REQUIRE(manifest.get("emits") != nullptr);
    CHECK(manifest.get("emits")->as_list().size() == 2);
    // The emitted shape's component travels in `referenced` like a door's would.
    REQUIRE(manifest.get("referenced") != nullptr);
    REQUIRE(manifest.get("referenced")->as_list().size() == 1);

    // The reconstruct sequence, against an EMPTY registry: every emitted shape
    // arrives with the identity it was declared with.
    Registry deps;
    SchemaClaimScope scope;
    decode_referenced(manifest, deps, scope);
    std::vector<std::shared_ptr<const Schema>> rebuilt;
    for (const Cell& c : manifest.get("emits")->as_list()) {
        rebuilt.push_back(decode_schema(*c.as_message(), deps));
    }
    REQUIRE(rebuilt.size() == 2);
    CHECK(rebuilt[0]->content_id() == greet->content_id());
    CHECK(rebuilt[1]->content_id() == whole->content_id());

    // No emit-set, no section: the lean manifest of a weave that declares none.
    const std::vector<std::shared_ptr<const Schema>> none;
    Unverified lean = parse(serialize(encode_manifest(accepted, *counter, nullptr, nullptr, &none)));
    Admission la = admit(lean, manifest_schema());
    REQUIRE(la.ok());
    CHECK(la.value().get("emits") == nullptr);
    CHECK(la.value().get("referenced") == nullptr);
}

TEST_CASE("a manifest whose declaration carries two definitions of one component travels "
          "with both, is refused at the second, and leaves nothing claimed") {
    // The independent review's Box/Box2 finding. The encoder once deduplicated
    // `referenced` by name: the second Part was dropped, Box2 was reconstructed over
    // the first Part, and the artifact loaded advertising a Box2 it never declared.
    auto part_a = SchemaBuilder("Part", 1).field("a", Kind::Int).build();
    auto part_b = SchemaBuilder("Part", 1).field("a", Kind::Int).field("b", Kind::Bool).build();
    auto box = SchemaBuilder("Box", 1).message("part", part_a).build();
    auto box2 = SchemaBuilder("Box2", 1).message("part", part_b).build();
    auto counter = SchemaBuilder("Counter", 1).field("count", Kind::Int).build();

    for (const auto& accepted : {std::vector<std::shared_ptr<const Schema>>{box, box2},
                                 std::vector<std::shared_ptr<const Schema>>{box2, box}}) {
        Unverified u = parse(serialize(encode_manifest(accepted, *counter)));
        Admission a = admit(u, manifest_schema());
        REQUIRE(a.ok()); // the meta-schema cannot know; the registry can
        const Value& manifest = a.value();
        // BOTH definitions crossed: the manifest says what the weave said.
        REQUIRE(manifest.get("referenced") != nullptr);
        REQUIRE(manifest.get("referenced")->as_list().size() == 2);

        Registry deps;
        {
            SchemaClaimScope scope;
            CHECK_THROWS_AS(decode_referenced(manifest, deps, scope), SchemaConflict);
            CHECK(deps.size() == 1); // the first Part, held by the scope until it goes
        }
        CHECK(deps.size() == 0); // ...and it went: a refused manifest leaves nothing
        Registry forever;
        CHECK_THROWS_AS(decode_referenced(manifest, forever), SchemaConflict);
    }

    // The agreeing shape of the same declaration: two doors sharing ONE Part.
    auto whole = SchemaBuilder("Whole", 1).list("parts", type_message(part_a)).build();
    const std::vector<std::shared_ptr<const Schema>> agree{box, whole};
    Unverified u = parse(serialize(encode_manifest(agree, *counter)));
    Admission a = admit(u, manifest_schema());
    REQUIRE(a.ok());
    REQUIRE(a.value().get("referenced")->as_list().size() == 1);
    Registry deps;
    SchemaClaimScope scope;
    decode_referenced(a.value(), deps, scope);
    CHECK(deps.lookup("Part", 1)->content_id() == part_a->content_id());
    const Cell::Array& doors = a.value().get("accepted")->as_list();
    CHECK(decode_schema(*doors[0].as_message(), deps)->content_id() == box->content_id());
    CHECK(decode_schema(*doors[1].as_message(), deps)->content_id() == whole->content_id());
}

TEST_CASE("an externally supplied manifest is held to the same rules: an emitted shape "
          "whose component never travelled is refused cleanly, and a v4 manifest does not "
          "pass the v5 door") {
    SUBCASE("an `emits` entry over an unresolved component") {
        // Hand-built, not SDK output: `emits` names a shape nesting `Missing v1`, and no
        // `referenced` section brings it. Reconstruction throws exactly as an accepted
        // door's would, and the load refuses cleanly instead of guessing.
        auto missing = SchemaBuilder("Missing", 1).field("x", Kind::Int).build();
        auto carrier = SchemaBuilder("Carrier", 1).message("m", missing).build();
        auto counter = SchemaBuilder("Counter", 1).field("count", Kind::Int).build();
        Value m(manifest_schema());
        m.set("accepted", Cell::list({}));
        m.set("state", Cell::message(encode_schema(*counter)));
        m.set("emits", Cell::list({Cell::message(encode_schema(*carrier))}));
        Unverified u = parse(serialize(m));
        Admission a = admit(u, manifest_schema());
        REQUIRE(a.ok());
        Registry deps;
        CHECK_THROWS_AS(decode_schema(*a.value().get("emits")->as_list()[0].as_message(), deps),
                        std::runtime_error);
        CHECK(deps.size() == 0);
    }
    SUBCASE("the previous manifest version is a different shape at this door") {
        // zen.Manifest v4, spelled as it was: the same sections minus `emits`. A v8
        // image's descriptor produces exactly this; the ABI version gate refuses that
        // image before its manifest is read, and this pins that the manifest door
        // would not silently pass it either.
        auto v4 = SchemaBuilder("zen.Manifest", 4)
                      .list("referenced", type_message(schema_desc_schema()), /*required=*/false)
                      .list("accepted", type_message(schema_desc_schema()))
                      .message("state", schema_desc_schema())
                      .message("requests", capability_ask_schema(), /*required=*/false)
                      .list("claims", type_message(schema_desc_schema()), /*required=*/false)
                      .build();
        CHECK(v4->content_id() != manifest_schema()->content_id());
        auto counter = SchemaBuilder("Counter", 1).field("count", Kind::Int).build();
        Value old(v4);
        old.set("accepted", Cell::list({}));
        old.set("state", Cell::message(encode_schema(*counter)));
        Unverified u = parse(serialize(old));
        REQUIRE(u.well_formed());
        Admission a = admit(u, manifest_schema());
        CHECK_FALSE(a.ok());
        CHECK(a.first_error().kind == ErrorKind::SchemaMismatch);
    }
}

} // TEST_SUITE
