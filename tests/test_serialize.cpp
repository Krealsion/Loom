// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include "fixtures.hpp"

#include <zen/serialize.hpp>

// The decoder's caps live with the wire primitives, internal to loom (the same
// reach test_sdl.cpp already takes into src/). The R2F-A cases pin the exact
// boundary, so they must read the real constant rather than a copy of it.
#include "../src/detail/binary.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

using namespace loom;

namespace {

// A UTF-8 string with non-ASCII content, spelled in explicit bytes so the test
// does not depend on the source file's encoding: "héllo 世界 😀".
std::string utf8_sample() {
    return "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80";
}

// serialize (native binary) -> parse -> admit against the same schema. Requires
// success and returns the trusted, re-admitted value.
Value round_trip(const Value& v, std::shared_ptr<const Schema> door) {
    std::string bytes = serialize(v);
    Unverified u = parse(bytes);
    REQUIRE(u.well_formed());
    Admission a = admit(u, door);
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    return std::move(a).value();
}

// Canonical value-equality: two values are equal iff their native encodings are
// byte-identical (the format is canonical, so this is exact — and it treats all
// NaNs as equal, since encode normalizes them).
bool same_value(const Value& a, const Value& b) { return serialize(a) == serialize(b); }

// ---- R2F-A wire forgery helpers -------------------------------------------
//
// The honest API cannot express the attack these cases pin: `serialize()` writes
// a count that matches an array it actually holds, so a value commanding a
// million elements would first have to BE a million elements. The hostile frame
// therefore has to be forged byte by byte (the unsayable-attack rule).

// The self-describing native envelope: magic, format version, schema name,
// schema version, mandatory content id. Everything after it is the body.
std::string native_header(const std::shared_ptr<const Schema>& s) {
    std::string h;
    h.push_back('\x5A'); // 'Z'
    h.push_back('\x4E'); // 'N'
    h.push_back('\x01'); // format version
    const auto nlen = static_cast<std::uint16_t>(s->name().size());
    h.push_back(static_cast<char>(nlen & 0xFF));
    h.push_back(static_cast<char>((nlen >> 8) & 0xFF));
    h += s->name();
    const std::uint32_t v = s->version();
    for (int i = 0; i < 4; ++i) {
        h.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
    const std::uint64_t cid = s->content_id();
    for (int i = 0; i < 8; ++i) {
        h.push_back(static_cast<char>((cid >> (8 * i)) & 0xFF));
    }
    return h;
}

// Minimal (canonical) unsigned LEB128, matching detail::put_uvarint.
void put_varint(std::string& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

// A message with NO fields. Its presence bitmask is (0 + 7) / 8 = ZERO bytes
// wide, so an element of this type consumes no body bytes at all — the encoding
// whose decoded population is entirely unrelated to its serialized size. It is a
// legitimate shape, not a malformation, which is exactly why the repair may not
// simply outlaw it.
std::shared_ptr<const Schema> nothing_schema() {
    static const auto s = SchemaBuilder("R2FA.Nothing", 1).build();
    return s;
}

// { items: List<Nothing> } — the amplification carrier.
std::shared_ptr<const Schema> nothing_list_schema() {
    static const auto s =
        SchemaBuilder("R2FA.NothingList", 1).list("items", type_message(nothing_schema())).build();
    return s;
}

// A forged NothingList whose one list field claims `count` zero-body elements.
std::string nothing_list_bytes(std::uint64_t count) {
    std::string b = native_header(nothing_list_schema());
    b.push_back('\x01'); // presence bitmask: field 0 present
    put_varint(b, count);
    return b;
}

// { a: List<Nothing>?, b: List<Nothing>? } — two independently modest lists, for
// proving the budget is one shared allowance and not one allowance per container.
std::shared_ptr<const Schema> two_lists_schema() {
    static const auto s = SchemaBuilder("R2FA.TwoLists", 1)
                              .list("a", type_message(nothing_schema()), /*required=*/false)
                              .list("b", type_message(nothing_schema()), /*required=*/false)
                              .build();
    return s;
}

// Present `a` and/or `b` with the given counts (a nullopt field stays absent).
std::string two_lists_bytes(std::optional<std::uint64_t> a, std::optional<std::uint64_t> b) {
    std::string out = native_header(two_lists_schema());
    unsigned char mask = 0;
    if (a) {
        mask |= 0x01u;
    }
    if (b) {
        mask |= 0x02u;
    }
    out.push_back(static_cast<char>(mask));
    if (a) {
        put_varint(out, *a);
    }
    if (b) {
        put_varint(out, *b);
    }
    return out;
}

// A two-field message, both optional, so an element costs exactly one wire byte
// (its presence bitmask) and exactly two decoded cells (its slot vector).
std::shared_ptr<const Schema> pair_schema() {
    static const auto s = SchemaBuilder("R2FA.Pair", 1)
                              .field("x", Kind::Int, /*required=*/false)
                              .field("y", Kind::Int, /*required=*/false)
                              .build();
    return s;
}

std::shared_ptr<const Schema> pair_list_schema() {
    static const auto s =
        SchemaBuilder("R2FA.PairList", 1).list("items", type_message(pair_schema())).build();
    return s;
}

// `count` Pairs, each with no field present: 1 body byte per element.
std::string pair_list_bytes(std::uint64_t count) {
    std::string b = native_header(pair_list_schema());
    b.push_back('\x01'); // presence bitmask: field 0 present
    put_varint(b, count);
    b.append(static_cast<std::size_t>(count), '\0'); // each Pair's empty presence mask
    return b;
}

} // namespace

TEST_SUITE("serialize") {

TEST_CASE("native output is binary with the ZN magic, not JSON") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    std::string bytes = serialize(v);
    REQUIRE(bytes.size() >= 3);
    CHECK(static_cast<unsigned char>(bytes[0]) == 0x5A); // 'Z'
    CHECK(static_cast<unsigned char>(bytes[1]) == 0x4E); // 'N'
    CHECK(static_cast<unsigned char>(bytes[2]) == 0x01); // format version
}

TEST_CASE("primitives round-trip losslessly, including large ints and special floats") {
    auto schema = SchemaBuilder("Prims", 1)
                      .field("i", Kind::Int)
                      .field("f", Kind::Float)
                      .field("t", Kind::Text)
                      .field("b", Kind::Bool)
                      .build();
    Value v(schema);
    const std::int64_t big = 9007199254740993LL; // 2^53 + 1
    v.set("i", Cell::integer(big));
    v.set("f", Cell::real(0.1));
    v.set("t", Cell::text(utf8_sample()));
    v.set("b", Cell::boolean(true));

    Value back = round_trip(v, schema);
    CHECK(back.get("i")->as_int() == big);
    CHECK(back.get("f")->as_float() == 0.1);
    CHECK(back.get("t")->as_text() == utf8_sample());
    CHECK(back.get("b")->as_bool() == true);
}

TEST_CASE("extreme ints round-trip") {
    auto schema = SchemaBuilder("I", 1).field("x", Kind::Int).build();
    for (std::int64_t n : {std::numeric_limits<std::int64_t>::min(),
                           std::numeric_limits<std::int64_t>::max(), std::int64_t{-1},
                           std::int64_t{0}, std::int64_t{1}}) {
        Value v(schema);
        v.set("x", Cell::integer(n));
        CHECK(round_trip(v, schema).get("x")->as_int() == n);
    }
}

TEST_CASE("NaN and infinities survive; signed zero is preserved and distinct") {
    auto schema = SchemaBuilder("F", 1).field("x", Kind::Float).build();
    auto rt = [&](double d) {
        Value v(schema);
        v.set("x", Cell::real(d));
        return round_trip(v, schema).get("x")->as_float();
    };
    CHECK(std::isnan(rt(std::numeric_limits<double>::quiet_NaN())));
    CHECK(rt(std::numeric_limits<double>::infinity()) == std::numeric_limits<double>::infinity());
    CHECK(rt(-std::numeric_limits<double>::infinity()) == -std::numeric_limits<double>::infinity());

    const double neg_zero = rt(-0.0);
    CHECK(neg_zero == 0.0);                      // compares equal to +0.0 ...
    CHECK(std::signbit(neg_zero));               // ... but the sign bit survived
    CHECK_FALSE(std::signbit(rt(0.0)));          // +0.0 stays +0.0
}

TEST_CASE("bytes round-trip raw (no base64 in native)") {
    Value v(fx::Blob());
    Bytes data;
    for (int i = 0; i < 256; ++i) {
        data.push_back(static_cast<std::uint8_t>(i));
    }
    v.set("data", Cell::bytes(data));
    Value back = round_trip(v, fx::Blob());
    CHECK(back.get("data")->as_bytes() == data);
}

TEST_CASE("lists, nested messages, and lists of messages round-trip") {
    Value v(fx::Inventory());
    v.set("owner", Cell::text("me"));
    v.set("items", Cell::list({Cell::text("sword"), Cell::text("shield")}));
    v.set("counts", Cell::list({Cell::integer(1), Cell::integer(2), Cell::integer(3)}));
    Value back = round_trip(v, fx::Inventory());
    CHECK(back.get("items")->as_list().size() == 2);
    CHECK(back.get("counts")->as_list()[2].as_int() == 3);

    Value leader(fx::PlayerState());
    leader.set("hp", Cell::integer(50)).set("name", Cell::text("Cap"));
    Value m1(fx::PlayerState());
    m1.set("hp", Cell::integer(10)).set("name", Cell::text("A"));
    Value squad(fx::Squad());
    squad.set("name", Cell::text("Alpha"));
    squad.set("leader", Cell::message(std::move(leader)));
    squad.set("members", Cell::list({Cell::message(std::move(m1))}));

    Value back2 = round_trip(squad, fx::Squad());
    CHECK(back2.get("leader")->as_message()->get("name")->as_text() == "Cap");
    CHECK(back2.get("members")->as_list()[0].as_message()->get("hp")->as_int() == 10);
}

TEST_CASE("the remaining fixtures round-trip too") {
    Value mv(fx::Move());
    mv.set("dx", Cell::real(1.5)).set("dy", Cell::real(-2.5));
    Value mv_back = round_trip(mv, fx::Move());
    CHECK(mv_back.get("dx")->as_float() == 1.5);
    CHECK(mv_back.get("dy")->as_float() == -2.5);

    Value sc(fx::SetColor());
    sc.set("r", Cell::integer(10)).set("g", Cell::integer(20)).set("b", Cell::integer(30));
    sc.set("named", Cell::boolean(false));
    Value sc_back = round_trip(sc, fx::SetColor());
    CHECK(sc_back.get("g")->as_int() == 20);
    CHECK(sc_back.get("named")->as_bool() == false);
}

TEST_CASE("optional-absent fields round-trip as absent") {
    Value g(fx::Greeting());
    g.set("to", Cell::text("world")); // 'note' optional, omitted
    Value back = round_trip(g, fx::Greeting());
    CHECK(back.has("to"));
    CHECK_FALSE(back.has("note"));
}

TEST_CASE("the native header carries a challengeable schema identity") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    Unverified u = parse(serialize(v));
    REQUIRE(u.well_formed());
    CHECK(u.claimed_name() == "PlayerState");
    CHECK(u.claimed_version() == 1);
}

// ---- Canonicality ---------------------------------------------------------

TEST_CASE("encoding is canonical: stable, twin-identical, and round-trip-stable") {
    Value a(fx::PlayerState());
    a.set("hp", Cell::integer(30)).set("name", Cell::text("Ami"));

    // Stable across calls.
    CHECK(serialize(a) == serialize(a));

    // Identical for a separately-built, structurally-equal value (and a twin schema).
    auto twin_schema =
        SchemaBuilder("PlayerState", 1).field("hp", Kind::Int).field("name", Kind::Text).build();
    Value b(twin_schema);
    b.set("hp", Cell::integer(30)).set("name", Cell::text("Ami"));
    CHECK(serialize(a) == serialize(b));

    // A decoded value re-serializes to the very same bytes (content-addressable).
    std::string bytes = serialize(a);
    Value back = round_trip(a, fx::PlayerState());
    CHECK(serialize(back) == bytes);
}

TEST_CASE("canonicality holds through nesting and lists") {
    auto build = [] {
        Value leader(fx::PlayerState());
        leader.set("hp", Cell::integer(7)).set("name", Cell::text("Cap"));
        Value squad(fx::Squad());
        squad.set("name", Cell::text("Alpha"));
        squad.set("leader", Cell::message(std::move(leader)));
        squad.set("members", Cell::list({}));
        return squad;
    };
    CHECK(serialize(build()) == serialize(build()));
    CHECK(serialize(round_trip(build(), fx::Squad())) == serialize(build()));
}

// ---- Cross-format equivalence ---------------------------------------------

TEST_CASE("native and compat decode to the same value") {
    Value v(fx::Squad());
    {
        Value leader(fx::PlayerState());
        leader.set("hp", Cell::integer(50)).set("name", Cell::text(utf8_sample()));
        Value m(fx::PlayerState());
        m.set("hp", Cell::integer(-3)).set("name", Cell::text("B"));
        v.set("name", Cell::text("Alpha"));
        v.set("leader", Cell::message(std::move(leader)));
        v.set("members", Cell::list({Cell::message(std::move(m))}));
    }

    Value via_binary = admit(parse(serialize(v)), fx::Squad()).value();
    Value via_json = admit(compat::parse(compat::serialize(v)), fx::Squad()).value();

    CHECK(same_value(via_binary, v));
    CHECK(same_value(via_json, v));
    CHECK(same_value(via_binary, via_json));
}

// ---- Strictness (native) --------------------------------------------------

TEST_CASE("content_id is mandatory in native: a header missing it is rejected") {
    auto empty = SchemaBuilder("Empty", 1).build(); // zero fields => empty body
    std::string bytes = serialize(Value(empty));
    // Drop the final byte so the 8-byte content id can no longer be read in full.
    bytes.pop_back();
    Unverified u = parse(bytes);
    CHECK_FALSE(u.well_formed());
    CHECK(admit(u, empty).first_error().kind == ErrorKind::MalformedBytes);
}

TEST_CASE("a mismatched content_id is SchemaMismatch") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    std::string bytes = serialize(v);
    // content_id sits after magic(3) + nameLen(2) + name(11) + schemaVersion(4) = offset 20.
    const std::size_t cid_off = 3 + 2 + std::string("PlayerState").size() + 4;
    REQUIRE(bytes.size() > cid_off);
    bytes[cid_off] = static_cast<char>(bytes[cid_off] ^ 0xFF);
    Admission a = admit(parse(bytes), fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::SchemaMismatch);
}

TEST_CASE("a Bool byte outside {0,1} is rejected") {
    auto schema = SchemaBuilder("B", 1).field("b", Kind::Bool).build();
    Value v(schema);
    v.set("b", Cell::boolean(true));
    std::string bytes = serialize(v); // the bool byte is the last byte
    bytes.back() = static_cast<char>(0x02);
    Admission a = admit(parse(bytes), schema);
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedField);
    CHECK(a.first_error().path == "b");
}

TEST_CASE("a non-minimal varint is rejected") {
    auto schema = SchemaBuilder("I", 1).field("n", Kind::Int).build();
    Value v(schema);
    v.set("n", Cell::integer(1)); // zigzag(1)=2 => single byte 0x02 at the end
    std::string bytes = serialize(v);
    bytes.pop_back();          // remove the 0x02
    bytes.push_back('\x82');   // 0x82: continuation, low bits = 2
    bytes.push_back('\x00');   // 0x00: terminator -> non-minimal encoding of 2
    Admission a = admit(parse(bytes), schema);
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedField);
    CHECK(a.first_error().path == "n");
}

TEST_CASE("trailing bytes after the value are rejected") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    std::string bytes = serialize(v);
    bytes.push_back('\x00'); // one junk byte past the value
    Admission a = admit(parse(bytes), fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedBytes);
}

TEST_CASE("a non-canonical NaN double is rejected") {
    // The encoder normalizes every NaN to the one canonical bit pattern (0x7FF8000000000000); the
    // decoder rejects any OTHER NaN payload. Inject a non-canonical NaN (exponent all ones, mantissa
    // nonzero, but not the canonical pattern) over the float's 8 trailing LE bytes.
    auto schema = SchemaBuilder("F", 1).field("x", Kind::Float).build();
    Value v(schema);
    v.set("x", Cell::real(1.0));
    std::string bytes = serialize(v); // body tail = the 8 little-endian bytes of the f64
    const std::uint64_t bad = 0x7FF0000000000001ULL;
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[bytes.size() - 8 + i] = static_cast<char>((bad >> (8 * i)) & 0xFFu);
    }
    Admission a = admit(parse(bytes), schema);
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedField);
    CHECK(a.first_error().path == "x");
}

TEST_CASE("a non-zero padding bit in the presence bitmask is rejected") {
    // PlayerState has 2 fields, so the 1-byte presence mask uses bits 0..1; bits 2..7 are reserved
    // and must be zero. The mask is the first body byte (right after the 28-byte header).
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    std::string bytes = serialize(v);
    const std::size_t mask_off = 3 + 2 + std::string("PlayerState").size() + 4 + 8; // = 28
    REQUIRE(bytes.size() > mask_off);
    // Set reserved bit 2, leaving the two real presence bits intact (so the body still parses and
    // the rejection is specifically the padding-bit check, not a downstream desync).
    bytes[mask_off] = static_cast<char>(static_cast<unsigned char>(bytes[mask_off]) | 0x04u);
    Admission a = admit(parse(bytes), fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedBytes);
}

TEST_CASE("a non-Zen byte string is refused, not crashed") {
    for (const char* junk : {"", "hello", "{\"zen\":1}", "ZN"}) {
        Unverified u = parse(junk);
        CHECK_FALSE(u.well_formed());
        CHECK_FALSE(admit(u, fx::PlayerState()).ok());
    }
}

TEST_CASE("the wrong door is refused even with valid bytes") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)).set("name", Cell::text("Ami"));
    Admission a = admit(parse(serialize(v)), fx::Move());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::SchemaMismatch);
}

TEST_CASE("a payload missing a required field is refused via the gate") {
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(1)); // no name
    Admission a = admit(parse(serialize(v)), fx::PlayerState());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MissingField);
    CHECK(a.first_error().path == "name");
}

// ---- R2F-A: bounded decode materialization ---------------------------------

TEST_CASE("R2F-A: a compact value cannot command an unbounded decoded population") {
    // The COLD-1 amplification, verbatim in shape: a few dozen wire bytes claim
    // 1,048,576 zero-body elements — exactly kMaxListCount, so the per-list cap
    // has nothing to say — and the pre-R2F-A decoder MATERIALISED all of them and
    // ADMITTED the value (measured: 37 B -> 1,048,576 cells -> +102,336 kB RSS).
    const std::string bytes = nothing_list_bytes(detail::kMaxListCount);
    CHECK(bytes.size() < 64); // a compact value by any measure

    Unverified u = parse(bytes);
    REQUIRE(u.well_formed()); // the ENVELOPE is fine; the amplification is in the body

    Admission a = admit(u, nothing_list_schema());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedBytes);
    // The refusal names the LIST, never an element index: the budget is spent for
    // the whole population before the first element is built, so there is no
    // "items[65537]" to point at. This is the check-before-materialisation witness.
    CHECK(a.first_error().path == "items");
    CHECK(a.first_error().detail.find("materialization budget") != std::string::npos);
}

TEST_CASE("R2F-A: a valid compact list of zero-field messages still decodes") {
    // The repair must not be `count <= remaining_wire_bytes`. A zero-field Message
    // legitimately consumes zero body bytes, so 1,000 of them ride 4 body bytes —
    // and that is a VALID value, not an attack.
    const std::string bytes = nothing_list_bytes(1000);
    CHECK(bytes.size() < 64);

    Admission a = admit(parse(bytes), nothing_list_schema());
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    CHECK(a.value().get("items")->as_list().size() == 1000);
    for (const Cell& e : a.value().get("items")->as_list()) {
        CHECK(e.kind() == Kind::Message);
        CHECK(e.as_message() != nullptr);
        CHECK(e.as_message()->field_count() == 0);
    }
}

TEST_CASE("R2F-A: the materialization boundary is exact and inclusive") {
    // The bound is INCLUSIVE: a decode whose total materialization is exactly
    // kMaxDecodedCells is accepted; the first cell beyond it is refused.
    // NothingList costs 1 (the door's own slot vector) + one cell per element.
    const std::uint64_t at_the_bound = detail::kMaxDecodedCells - 1;

    Admission exact = admit(parse(nothing_list_bytes(at_the_bound)), nothing_list_schema());
    REQUIRE_MESSAGE(exact.ok(), (exact.ok() ? "" : exact.first_error().message()));
    CHECK(exact.value().get("items")->as_list().size() == at_the_bound);

    Admission over = admit(parse(nothing_list_bytes(at_the_bound + 1)), nothing_list_schema());
    REQUIRE_FALSE(over.ok());
    CHECK(over.first_error().kind == ErrorKind::MalformedBytes);
    CHECK(over.first_error().path == "items");
}

TEST_CASE("R2F-A: the budget is ONE allowance shared by the whole value, not one per container") {
    // Two lists, each comfortably inside the bound on its own. Their SUM is not.
    // A per-container budget would admit the pair; one shared budget refuses it.
    const std::uint64_t each = 40000; // 2 door slots + 40,000 << 65,536

    Admission only_a = admit(parse(two_lists_bytes(each, std::nullopt)), two_lists_schema());
    REQUIRE_MESSAGE(only_a.ok(), (only_a.ok() ? "" : only_a.first_error().message()));
    CHECK(only_a.value().get("a")->as_list().size() == each);

    Admission only_b = admit(parse(two_lists_bytes(std::nullopt, each)), two_lists_schema());
    REQUIRE_MESSAGE(only_b.ok(), (only_b.ok() ? "" : only_b.first_error().message()));

    Admission both = admit(parse(two_lists_bytes(each, each)), two_lists_schema());
    REQUIRE_FALSE(both.ok());
    CHECK(both.first_error().kind == ErrorKind::MalformedBytes);
    CHECK(both.first_error().path == "b"); // the allowance ran out in the SECOND list
    CHECK(both.first_error().detail.find("materialization budget") != std::string::npos);
}

TEST_CASE("R2F-A: a nested message charges its whole slot vector, present fields or not") {
    // The unit is a decoded CELL SLOT, so the accounting is exact and checkable:
    //   PairList of N  =  1 (door slot) + N (element cells) + 2N (each Pair's slots)
    //                  =  1 + 3N
    // Not one of the 2N is a *present* field — every Pair below arrives empty — and
    // they still cost, because Value allocates one std::optional<Cell> per DECLARED
    // field. A count-the-decoded-values unit would miss them entirely.
    const std::uint64_t at_the_bound = (detail::kMaxDecodedCells - 1) / 3; // 1 + 3N == 65,536

    Admission exact = admit(parse(pair_list_bytes(at_the_bound)), pair_list_schema());
    REQUIRE_MESSAGE(exact.ok(), (exact.ok() ? "" : exact.first_error().message()));
    CHECK(exact.value().get("items")->as_list().size() == at_the_bound);
    CHECK(1 + 3 * at_the_bound == detail::kMaxDecodedCells);

    Admission over = admit(parse(pair_list_bytes(at_the_bound + 1)), pair_list_schema());
    REQUIRE_FALSE(over.ok());
    CHECK(over.first_error().kind == ErrorKind::MalformedBytes);
    // Here the list's own population fits; the allowance runs out inside an ELEMENT,
    // and the path names the exact element whose slots could not be paid for.
    CHECK(over.first_error().path.rfind("items[", 0) == 0);
    CHECK(over.first_error().detail.find("materialization budget") != std::string::npos);
}

TEST_CASE("R2F-A: the budget counts STRUCTURE, not bytes — a big payload is one cell") {
    // Wire-size limits (kMaxFieldBytes, and the remaining-input check) bound how
    // many BYTES a field may carry. The materialization budget bounds how much
    // STRUCTURE the decode may build. A 1 MiB Bytes field is a single cell and must
    // stay perfectly legal.
    Value v(fx::Blob());
    v.set("data", Cell::bytes(Bytes(1u << 20, 0xAB)));
    const std::string bytes = serialize(v);
    CHECK(bytes.size() > (1u << 20));

    Admission a = admit(parse(bytes), fx::Blob());
    REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.first_error().message()));
    CHECK(a.value().get("data")->as_bytes().size() == (1u << 20));
}

TEST_CASE("R2F-A: the budget does not swallow the older, more precise refusals") {
    // A count past the per-list cap keeps its own diagnosis (MalformedField, "list
    // count exceeds cap") — the cheaper container check still runs first, so the
    // new bound weakened no existing malformed-input handling.
    {
        std::string bytes = native_header(fx::Inventory());
        bytes.push_back('\x02'); // presence: only 'items'
        for (int i = 0; i < 9; ++i) {
            bytes.push_back('\xFF'); // a ~64-bit varint count
        }
        bytes.push_back('\x01');
        Admission a = admit(parse(bytes), fx::Inventory());
        REQUIRE_FALSE(a.ok());
        CHECK(a.first_error().kind == ErrorKind::MalformedField);
        CHECK(a.first_error().detail.find("exceeds cap") != std::string::npos);
    }
    // And a list whose elements DO cost bytes still runs out of input first: the
    // truncation is reported as truncation, not as exhaustion.
    {
        std::string bytes = native_header(fx::Inventory());
        bytes.push_back('\x04'); // presence: only 'counts' (List<Int>)
        put_varint(bytes, 1000); // ...but no element bytes follow
        Admission a = admit(parse(bytes), fx::Inventory());
        REQUIRE_FALSE(a.ok());
        CHECK(a.first_error().kind == ErrorKind::MalformedField);
        CHECK(a.first_error().path == "counts[0]");
    }
}

TEST_CASE("R2F-A: the compat (JSON) decoder shares the same budget domain") {
    // The law lives in the decoder, not in a transport, and both encodings reach it
    // through the same admit(). JSON cannot express the COMPACT amplification (an
    // array element costs text), but the structure it builds is bounded identically.
    std::string json = "{\"zen\":1,\"schema\":\"Inventory\",\"version\":1,\"fields\":{\"items\":[";
    const std::uint64_t over = detail::kMaxDecodedCells + 1;
    json.reserve(static_cast<std::size_t>(over) * 3 + 128);
    for (std::uint64_t i = 0; i < over; ++i) {
        if (i != 0) {
            json.push_back(',');
        }
        json += "\"\"";
    }
    json += "]}}";

    Unverified u = compat::parse(json);
    REQUIRE(u.well_formed());
    Admission a = admit(u, fx::Inventory());
    REQUIRE_FALSE(a.ok());
    CHECK(a.first_error().kind == ErrorKind::MalformedBytes);
    CHECK(a.first_error().detail.find("materialization budget") != std::string::npos);

    // ...and a modest JSON list still decodes, in the same breath.
    Admission ok = admit(compat::parse("{\"zen\":1,\"schema\":\"Inventory\",\"version\":1,"
                                       "\"fields\":{\"owner\":\"a\",\"items\":[\"x\",\"y\"],"
                                       "\"counts\":[]}}"),
                         fx::Inventory());
    REQUIRE_MESSAGE(ok.ok(), (ok.ok() ? "" : ok.first_error().message()));
    CHECK(ok.value().get("items")->as_list().size() == 2);
}

TEST_CASE("resolving the claim against a registry (native)") {
    Registry reg;
    reg.register_schema(fx::PlayerState());
    Value v(fx::PlayerState());
    v.set("hp", Cell::integer(42)).set("name", Cell::text("Ami"));
    Unverified u = parse(serialize(v));
    CHECK(admit(u, reg).value().get("hp")->as_int() == 42);

    Registry empty;
    CHECK(admit(u, empty).first_error().kind == ErrorKind::UnknownSchema);
}

} // TEST_SUITE
