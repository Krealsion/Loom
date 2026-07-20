#include <doctest.h>

#include <zen/kernel/schema_codec.hpp>
#include <zen/serialize.hpp>
#include <zen/zen.hpp>

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

    SUBCASE("the auditor's ceiling case N=100000, formerly a SIGSEGV, now refuses cleanly") {
        std::string bytes = deep_list_descriptor_bytes(100000);
        Unverified u = parse(bytes);
        Admission a = admit(u, schema_desc_schema());
        REQUIRE(a.ok()); // ~200 KB, well under the frame cap — admitted, as the auditor saw
        Registry deps;
        CHECK_THROWS_AS(decode_schema(a.value(), deps), std::runtime_error);
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

} // TEST_SUITE
