#include <doctest.h>

#include <zen/kernel/schema_codec.hpp>
#include <zen/serialize.hpp>
#include <zen/zen.hpp>

#include <memory>
#include <vector>

using namespace zen;
using namespace zen::kernel;

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
