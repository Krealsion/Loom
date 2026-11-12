// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#include <zen/serialize.hpp>

#include <zen/registry.hpp>

#include "detail/base64.hpp"
#include "detail/binary.hpp"
#include "detail/gate_internal.hpp"
#include "detail/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace loom {

// Native binary envelope constants. These bytes are a frozen, on-the-wire
// commitment: the magic and format version identify the format to a reader that
// has no schema yet.
inline constexpr std::uint8_t kBinMagic0 = 0x5A; // 'Z'
inline constexpr std::uint8_t kBinMagic1 = 0x4E; // 'N'
inline constexpr std::uint8_t kBinFormatVersion = 1;

// Compat (JSON) envelope format version.
inline constexpr int kJsonEnvelopeVersion = 1;

// The parsed-but-unverified state. It carries the claim (resolvable without a
// schema) and the still-opaque body — native bytes, or a JSON tree — decoded
// only inside admit(), under the resolved door.
struct Unverified::Impl {
    enum class Format : std::uint8_t { Native, Json };

    bool ok = false;
    Error envelope_error{ErrorKind::MalformedBytes, "", "", "", "not parsed"};
    Format format = Format::Native;
    std::string name;
    std::uint32_t version = 0;
    bool has_content_id = false;
    ContentId content_id = 0;

    std::string body;              // native: raw body bytes
    detail::JsonValue json_fields; // compat: the "fields" object
};

namespace {

// ---- Shared helpers -------------------------------------------------------

std::string format_content_id(ContentId id) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s = "0x";
    for (int shift = 60; shift >= 0; shift -= 4) {
        s.push_back(kHex[(id >> shift) & 0xF]);
    }
    return s;
}

std::string version_label(const Schema& s) {
    return s.name() + " v" + std::to_string(s.version());
}

std::string type_label(const TypeRef& t) {
    switch (t.kind) {
    case Kind::Message:
        return "Message(" + version_label(*t.message) + ")";
    case Kind::Union:
        return "Union(" + version_label(*t.message) + ")";
    case Kind::List:
        return "List<" + type_label(*t.element) + ">";
    default:
        return name_of(t.kind);
    }
}

// ===========================================================================
//  The decode budget: bounded host-side materialization
//  docs/reference/bounds.md#the-decode-materialization-bound
// ===========================================================================

// One budget per top-level decode, shared by everything nested in it and never reset per
// container, and spent before the cells it pays for exist, so exhaustion is a refusal, never an
// allocation regretted afterwards. Host-owned and automatic: no caller and nothing on the wire
// can widen it.
struct DecodeBudget {
    std::uint64_t remaining = kMaxDecodedCells;

    /// Charge `n` cells. On refusal nothing is charged and nothing is built —
    /// the caller turns it into a fatal parse failure.
    bool take(std::uint64_t n) noexcept {
        if (n > remaining) {
            return false;
        }
        remaining -= n;
        return true;
    }
};

// The cells entering a value of `s` costs: a record, a slot per declared field; a union, one.
std::uint64_t entry_cells(const Schema& s) { return s.is_union() ? 1 : s.fields().size(); }

std::string budget_exhausted_detail() {
    return "decoded structure exceeds the materialization budget of " +
           std::to_string(kMaxDecodedCells) + " cells (shared by the whole value)";
}

// ===========================================================================
//  Native binary: encode (Value -> bytes)
// ===========================================================================

void bin_require_kind(const Cell& c, Kind expected) {
    if (c.kind() != expected) {
        throw std::invalid_argument(std::string("serialize: a field holds ") + name_of(c.kind()) +
                                    " but its schema declares " + name_of(expected) +
                                    " (validate the value before serializing)");
    }
}

void bin_encode_value(const Cell& c, const TypeRef& t, std::string& out);

void bin_encode_body(const Value& v, std::string& out) {
    const auto& fields = v.schema().fields();
    if (v.schema().is_union()) {
        // A union's body is its case's index, then that case's value; a case that carries
        // nothing has no value.
        const std::optional<std::size_t> chosen = v.chosen();
        if (!chosen) {
            throw std::invalid_argument("serialize: union '" + v.schema().name() +
                                        "' holds no case (validate before serializing)");
        }
        detail::put_uvarint(out, *chosen);
        if (fields[*chosen].carries) {
            const Cell* c = v.at(*chosen);
            if (c == nullptr) {
                throw std::invalid_argument("serialize: a union's case carries no value");
            }
            bin_encode_value(*c, fields[*chosen].type, out);
        }
        return;
    }
    const std::size_t n = fields.size();
    const std::size_t mask_bytes = (n + 7) / 8;
    const std::size_t mask_pos = out.size();
    out.append(mask_bytes, '\0'); // presence bitmask, filled as we go

    for (std::size_t i = 0; i < n; ++i) {
        const Cell* c = v.at(i);
        if (c == nullptr) {
            continue;
        }
        auto& mask_byte = out[mask_pos + i / 8];
        mask_byte = static_cast<char>(static_cast<unsigned char>(mask_byte) |
                                      static_cast<unsigned char>(1u << (i % 8)));
        bin_encode_value(*c, fields[i].type, out);
    }
}

void bin_encode_value(const Cell& c, const TypeRef& t, std::string& out) {
    switch (t.kind) {
    case Kind::Int:
        bin_require_kind(c, Kind::Int);
        detail::put_zigzag(out, c.as_int());
        break;
    case Kind::Float:
        bin_require_kind(c, Kind::Float);
        detail::put_f64le(out, c.as_float());
        break;
    case Kind::Text: {
        bin_require_kind(c, Kind::Text);
        const std::string& s = c.as_text();
        detail::put_uvarint(out, s.size());
        out.append(s);
        break;
    }
    case Kind::Bool:
        bin_require_kind(c, Kind::Bool);
        detail::put_u8(out, c.as_bool() ? 1 : 0);
        break;
    case Kind::Bytes: {
        bin_require_kind(c, Kind::Bytes);
        const Bytes& b = c.as_bytes();
        detail::put_uvarint(out, b.size());
        out.append(reinterpret_cast<const char*>(b.data()), b.size());
        break;
    }
    case Kind::Message:
    case Kind::Union: {
        bin_require_kind(c, t.kind);
        const std::shared_ptr<const Value>& nested = c.as_message();
        if (!nested) {
            throw std::invalid_argument(
                "serialize: a present Message field is null (validate before serializing)");
        }
        bin_encode_body(*nested, out); // inline, no per-nested header
        break;
    }
    case Kind::Any: {
        // The value names its own shape: the name, the version and the content id, then its body.
        if (c.kind() != Kind::Message && c.kind() != Kind::Union) {
            throw std::invalid_argument(std::string("serialize: an Any field holds ") +
                                        name_of(c.kind()) + ", not a value of a shape");
        }
        const Schema& s = c.as_message()->schema();
        if (s.name().size() > 0xFFFF) {
            throw std::invalid_argument("serialize: schema name exceeds 65535 bytes");
        }
        detail::put_uvarint(out, s.name().size());
        out.append(s.name());
        detail::put_u32le(out, s.version());
        detail::put_u64le(out, s.content_id());
        bin_encode_body(*c.as_message(), out);
        break;
    }
    case Kind::List: {
        bin_require_kind(c, Kind::List);
        const Cell::Array& arr = c.as_list();
        detail::put_uvarint(out, arr.size());
        for (const Cell& e : arr) {
            bin_encode_value(e, *t.element, out);
        }
        break;
    }
    }
}

// ===========================================================================
//  Native binary: decode (bytes -> candidate Value), schema-guided, positional
// ===========================================================================

// A positional decoder. A low-level read failure desyncs the stream — there is
// no field-name to resync on — so it is fatal: one precise error, then stop. The
// gate (validate_into) still judges a fully-decoded candidate; required-but-
// absent fields are reported there, not here.
struct BinaryDecoder {
    detail::BinReader& r;
    DecodeBudget& budget; ///< shared by the whole top-level decode
    const Registry* open; ///< resolves an Any field's shape; null: no registry was given
    std::vector<Error> errs;
    bool fatal = false;

    BinaryDecoder(detail::BinReader& reader, DecodeBudget& b, const Registry* o)
        : r(reader), budget(b), open(o) {}

    void fail(ErrorKind kind, std::string path, std::string expected, std::string detail) {
        errs.push_back(Error{kind, std::move(path), std::move(expected), "binary", std::move(detail)});
        fatal = true;
    }

    /// Charge `cells` against the shared budget, failing the whole decode if the
    /// value's materialization would exceed the bound. Called before the cells
    /// are created, never after.
    bool afford(std::uint64_t cells, const std::string& path, std::string expected) {
        if (budget.take(cells)) {
            return true;
        }
        fail(ErrorKind::MalformedBytes, path, std::move(expected), budget_exhausted_detail());
        return false;
    }

    void body(const Schema& door, Value& out, const std::string& base, int depth) {
        if (fatal) {
            return;
        }
        if (depth > detail::kMaxBinaryDepth) {
            fail(ErrorKind::MalformedBytes, base, "", "maximum nesting depth exceeded");
            return;
        }
        const auto& fields = door.fields();
        const std::size_t n = fields.size();
        if (door.is_union()) {
            union_body(door, out, base, depth);
            return;
        }
        const std::size_t mask_bytes = (n + 7) / 8;

        std::string_view mask;
        if (!r.take(mask_bytes, mask)) {
            fail(ErrorKind::MalformedBytes, base, "", "truncated presence bitmask");
            return;
        }
        if (n % 8 != 0) {
            const auto last = static_cast<unsigned char>(mask[mask_bytes - 1]);
            const auto valid = static_cast<unsigned char>((1u << (n % 8)) - 1u);
            if ((last & ~valid) != 0) {
                fail(ErrorKind::MalformedBytes, base, "", "non-zero padding bit in presence bitmask");
                return;
            }
        }

        for (std::size_t i = 0; i < n; ++i) {
            if (fatal) {
                return;
            }
            const auto byte = static_cast<unsigned char>(mask[i / 8]);
            const bool present = ((byte >> (i % 8)) & 1u) != 0;
            if (!present) {
                continue; // absent — the gate decides whether that is fatal
            }
            const std::string path = base.empty() ? fields[i].name : base + "." + fields[i].name;
            Cell c = Cell::boolean(false);
            if (value(fields[i].type, c, path, depth)) {
                out.set(fields[i].name, std::move(c));
            }
        }
    }

    void union_body(const Schema& door, Value& out, const std::string& base, int depth) {
        const auto& cases = door.fields();
        std::uint64_t index = 0;
        if (!r.uvarint(index)) {
            fail(ErrorKind::MalformedField, base, version_label(door),
                 "truncated or non-minimal case index");
            return;
        }
        if (index >= cases.size()) {
            fail(ErrorKind::MalformedField, base, version_label(door),
                 "case index " + std::to_string(index) + " is past the last of " +
                     std::to_string(cases.size()) + " cases");
            return;
        }
        const Field& f = cases[static_cast<std::size_t>(index)];
        if (!f.carries) {
            out.choose(f.name);
            return;
        }
        const std::string path = base.empty() ? f.name : base + "." + f.name;
        Cell c = Cell::boolean(false);
        if (value(f.type, c, path, depth)) {
            out.set(f.name, std::move(c));
        }
    }

    // An Any field: the shape's name, version and content id, then its body, decoded under the
    // registry's definition of that name and version.
    bool open_value(Cell& out, const std::string& path, int depth) {
        std::uint64_t name_len = 0;
        if (!r.uvarint(name_len) || name_len > 0xFFFF || name_len > r.remaining()) {
            fail(ErrorKind::MalformedField, path, "Any", "truncated or oversized shape name");
            return false;
        }
        std::string_view name;
        r.take(static_cast<std::size_t>(name_len), name);
        std::uint32_t version = 0;
        std::uint64_t content_id = 0;
        if (!detail::valid_utf8(name) || !r.u32le(version) || !r.u64le(content_id)) {
            fail(ErrorKind::MalformedField, path, "Any", "malformed shape identity");
            return false;
        }
        const std::string label = std::string(name) + " v" + std::to_string(version);
        if (open == nullptr) {
            fail(ErrorKind::UnknownSchema, path, "Any",
                 label + ": an open field is admitted against a registry, and none was given");
            return false;
        }
        std::shared_ptr<const Schema> shape = open->lookup(std::string(name), version);
        if (!shape) {
            fail(ErrorKind::UnknownSchema, path, "a registered shape",
                 label + ": no such schema is registered");
            return false;
        }
        if (shape->content_id() != content_id) {
            fail(ErrorKind::SchemaMismatch, path, format_content_id(shape->content_id()),
                 label + " was written against another shape of this name and version (" +
                     format_content_id(content_id) + ")");
            return false;
        }
        if (!afford(entry_cells(*shape), path, "Any")) {
            return false;
        }
        Value nested(shape);
        body(*shape, nested, path, depth + 1);
        if (fatal) {
            return false;
        }
        out = Cell::message(std::move(nested));
        return true;
    }

    bool value(const TypeRef& t, Cell& out, const std::string& path, int depth) {
        if (fatal) {
            return false;
        }
        if (depth > detail::kMaxBinaryDepth) {
            fail(ErrorKind::MalformedBytes, path, "", "maximum nesting depth exceeded");
            return false;
        }
        switch (t.kind) {
        case Kind::Int: {
            std::int64_t v = 0;
            if (!r.zigzag(v)) {
                fail(ErrorKind::MalformedField, path, "Int", "truncated or non-minimal varint");
                return false;
            }
            out = Cell::integer(v);
            return true;
        }
        case Kind::Float: {
            double d = 0;
            if (!r.f64le(d)) {
                fail(ErrorKind::MalformedField, path, "Float", "truncated or non-canonical float");
                return false;
            }
            out = Cell::real(d);
            return true;
        }
        case Kind::Text: {
            std::uint64_t len = 0;
            if (!r.uvarint(len)) {
                fail(ErrorKind::MalformedField, path, "Text", "truncated or non-minimal length");
                return false;
            }
            if (len > detail::kMaxFieldBytes || len > r.remaining()) {
                fail(ErrorKind::MalformedField, path, "Text", "length exceeds remaining input or cap");
                return false;
            }
            std::string_view sv;
            r.take(static_cast<std::size_t>(len), sv);
            if (!detail::valid_utf8(sv)) {
                fail(ErrorKind::MalformedField, path, "Text", "invalid UTF-8");
                return false;
            }
            out = Cell::text(std::string(sv));
            return true;
        }
        case Kind::Bool: {
            std::uint8_t b = 0;
            if (!r.u8(b)) {
                fail(ErrorKind::MalformedField, path, "Bool", "truncated");
                return false;
            }
            if (b > 1) {
                fail(ErrorKind::MalformedField, path, "Bool", "byte is not 0x00 or 0x01");
                return false;
            }
            out = Cell::boolean(b != 0);
            return true;
        }
        case Kind::Bytes: {
            std::uint64_t len = 0;
            if (!r.uvarint(len)) {
                fail(ErrorKind::MalformedField, path, "Bytes", "truncated or non-minimal length");
                return false;
            }
            if (len > detail::kMaxFieldBytes || len > r.remaining()) {
                fail(ErrorKind::MalformedField, path, "Bytes",
                     "length exceeds remaining input or cap");
                return false;
            }
            std::string_view sv;
            r.take(static_cast<std::size_t>(len), sv);
            const auto* begin = reinterpret_cast<const unsigned char*>(sv.data());
            out = Cell::bytes(Bytes(begin, begin + sv.size()));
            return true;
        }
        case Kind::Message:
        case Kind::Union: {
            // A record's Value allocates one slot per DECLARED field, present or not, so the
            // slot vector is charged before it exists; a union's holds one.
            if (!afford(entry_cells(*t.message), path, type_label(t))) {
                return false;
            }
            Value nested(t.message);
            body(*t.message, nested, path, depth + 1);
            if (fatal) {
                return false;
            }
            out = Cell::message(std::move(nested));
            return true;
        }
        case Kind::Any:
            return open_value(out, path, depth);
        case Kind::List: {
            std::uint64_t count = 0;
            if (!r.uvarint(count)) {
                fail(ErrorKind::MalformedField, path, type_label(t), "truncated or non-minimal count");
                return false;
            }
            if (count > detail::kMaxListCount) {
                fail(ErrorKind::MalformedField, path, type_label(t), "list count exceeds cap");
                return false;
            }
            // The WHOLE commanded population is charged here, against the value's
            // one shared budget, before a single element is built — an element
            // may cost zero wire bytes, so nothing downstream would stop it.
            if (!afford(count, path, type_label(t))) {
                return false;
            }
            Cell::Array arr;
            arr.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, 4096)));
            for (std::uint64_t k = 0; k < count; ++k) {
                if (fatal) {
                    return false;
                }
                Cell ec = Cell::boolean(false);
                if (!value(*t.element, ec, path + "[" + std::to_string(k) + "]", depth + 1)) {
                    return false;
                }
                arr.push_back(std::move(ec));
            }
            out = Cell::list(std::move(arr));
            return true;
        }
        }
        return false; // unreachable
    }
};

// ===========================================================================
//  Compat JSON: helpers, encode, and the strict decoder
// ===========================================================================

bool json_parse_int64(std::string_view text, std::int64_t& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto res = std::from_chars(begin, end, out, 10);
    return res.ec == std::errc{} && res.ptr == end;
}

bool json_parse_double(std::string_view text, double& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto res = std::from_chars(begin, end, out);
    return res.ec == std::errc{} && res.ptr == end;
}

bool json_parse_u32(std::string_view text, std::uint32_t& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto res = std::from_chars(begin, end, out, 10);
    return res.ec == std::errc{} && res.ptr == end;
}

bool json_parse_content_id(std::string_view text, ContentId& out) {
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 16) {
        return false;
    }
    ContentId v = 0;
    for (char ch : text) {
        v <<= 4;
        if (ch >= '0' && ch <= '9') {
            v |= static_cast<ContentId>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            v |= static_cast<ContentId>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            v |= static_cast<ContentId>(ch - 'A' + 10);
        } else {
            return false;
        }
    }
    out = v;
    return true;
}

const char* json_type_name(const detail::JsonValue& v) {
    switch (v.type) {
    case detail::JsonValue::Type::Null:
        return "json:null";
    case detail::JsonValue::Type::Bool:
        return "json:bool";
    case detail::JsonValue::Type::Number:
        return "json:number";
    case detail::JsonValue::Type::String:
        return "json:string";
    case detail::JsonValue::Type::Array:
        return "json:array";
    case detail::JsonValue::Type::Object:
        return "json:object";
    }
    return "json:?";
}

void json_encode_int(std::int64_t v, std::string& out) {
    std::array<char, 24> buf{};
    auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    out.push_back('"');
    out.append(buf.data(), res.ptr);
    out.push_back('"');
}

void json_encode_float(double v, std::string& out) {
    if (std::isnan(v)) {
        out += "\"NaN\"";
        return;
    }
    if (std::isinf(v)) {
        out += v < 0 ? "\"-Infinity\"" : "\"Infinity\"";
        return;
    }
    std::array<char, 32> buf{};
    auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    out.append(buf.data(), res.ptr);
}

// `t` is the type the cell's field declares, or null where the cell does not hold it: an Any
// field's value is written with the identity of its shape, and every other cell by its kind.
void json_encode_cell(const Cell& c, const TypeRef* t, std::string& out);

void json_encode_fields(const Value& v, std::string& out) {
    const auto& fields = v.schema().fields();
    if (v.schema().is_union()) {
        // A union is an object with one member, its case's name and value; a case that carries
        // nothing is its name alone. One holding no case is written as an empty object, which
        // the decoder refuses as NoCase.
        const std::optional<std::size_t> chosen = v.chosen();
        if (chosen && !fields[*chosen].carries) {
            detail::json_quote(fields[*chosen].name, out);
            return;
        }
        out.push_back('{');
        if (chosen && v.at(*chosen) != nullptr) {
            detail::json_quote(fields[*chosen].name, out);
            out.push_back(':');
            json_encode_cell(*v.at(*chosen), &fields[*chosen].type, out);
        }
        out.push_back('}');
        return;
    }
    out.push_back('{');
    bool first = true;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const Cell* c = v.at(i);
        if (c == nullptr) {
            continue;
        }
        if (!first) {
            out.push_back(',');
        }
        first = false;
        detail::json_quote(fields[i].name, out);
        out.push_back(':');
        json_encode_cell(*c, &fields[i].type, out);
    }
    out.push_back('}');
}

// An Any field's value: the envelope's identity members, and the value's own fields.
void json_encode_open(const Value& v, std::string& out) {
    const Schema& s = v.schema();
    out += "{\"schema\":";
    detail::json_quote(s.name(), out);
    out += ",\"version\":";
    out += std::to_string(s.version());
    out += ",\"content_id\":";
    detail::json_quote(format_content_id(s.content_id()), out);
    out += ",\"fields\":";
    json_encode_fields(v, out);
    out.push_back('}');
}

void json_encode_cell(const Cell& c, const TypeRef* t, std::string& out) {
    switch (c.kind()) {
    case Kind::Int:
        json_encode_int(c.as_int(), out);
        break;
    case Kind::Float:
        json_encode_float(c.as_float(), out);
        break;
    case Kind::Text:
        detail::json_quote(c.as_text(), out);
        break;
    case Kind::Bool:
        out += c.as_bool() ? "true" : "false";
        break;
    case Kind::Bytes:
        detail::json_quote(detail::base64_encode(c.as_bytes()), out);
        break;
    case Kind::Message:
    case Kind::Union: {
        const std::shared_ptr<const Value>& nested = c.as_message();
        if (!nested) {
            out += "null";
        } else if (t != nullptr && t->kind == Kind::Any) {
            json_encode_open(*nested, out);
        } else {
            json_encode_fields(*nested, out);
        }
        break;
    }
    case Kind::List: {
        const TypeRef* element = t != nullptr && t->kind == Kind::List ? t->element.get() : nullptr;
        out.push_back('[');
        bool first = true;
        for (const Cell& e : c.as_list()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            json_encode_cell(e, element, out);
        }
        out.push_back(']');
        break;
    }
    case Kind::Any:
        break; // a cell's own kind is never Any: an Any field holds a Message or a Union
    }
}

// The compat decoder. Unlike binary, JSON has field names, so a bad field does
// not desync the rest and several errors can be collected. Strict-core: a member
// the door does not declare is fatal (UnknownField), never silently dropped.
struct JsonDecoder {
    std::vector<Error>& errs;
    bool collect_all;
    DecodeBudget& budget;  ///< the SAME budget domain as native
    const Registry* open;  ///< resolves an Any field's shape; null: no registry was given
    bool exhausted = false; ///< budget exhaustion halts collection: no coherent candidate

    bool stop() const { return exhausted || (!collect_all && !errs.empty()); }

    void push(ErrorKind kind, std::string path, std::string expected, std::string actual,
              std::string detail = "") {
        errs.push_back(Error{kind, std::move(path), std::move(expected), std::move(actual),
                             std::move(detail)});
    }

    /// Charge `cells` before materialising them; on refusal the whole decode ends.
    bool afford(std::uint64_t cells, const std::string& path, std::string expected) {
        if (budget.take(cells)) {
            return true;
        }
        push(ErrorKind::MalformedBytes, path, std::move(expected), "json",
             budget_exhausted_detail());
        exhausted = true;
        return false;
    }

    bool cell(const detail::JsonValue& node, const TypeRef& t, Cell& out, const std::string& path);

    // A union: an object with exactly one member naming its case, or a bare case's name alone.
    void union_case(const detail::JsonValue& node, const Schema& door, Value& out,
                    const std::string& base) {
        using JT = detail::JsonValue::Type;
        auto at = [&](const std::string& name) { return base.empty() ? name : base + "." + name; };
        if (node.type == JT::String) {
            const Field* f = door.find(node.text);
            if (f == nullptr) {
                push(ErrorKind::UnknownField, at(node.text), case_names(door), node.text,
                     "the union declares no such case");
                return;
            }
            if (f->carries) {
                push(ErrorKind::MissingField, at(f->name), type_label(f->type), "absent",
                     "the case carries a value: write {\"" + f->name + "\": <value>}");
                return;
            }
            out.choose(f->name);
            return;
        }
        if (node.type != JT::Object) {
            push(ErrorKind::TypeMismatch, base, case_names(door), json_type_name(node));
            return;
        }
        if (node.members.empty()) {
            push(ErrorKind::NoCase, base, case_names(door), "no case",
                 "a union holds exactly one case");
            return;
        }
        if (node.members.size() > 1) {
            std::string named;
            for (const auto& m : node.members) {
                named += (named.empty() ? "" : " ") + m.first;
            }
            push(ErrorKind::SeveralCases, base, case_names(door), named,
                 "a union holds exactly one case");
            return;
        }
        const auto& member = node.members.front();
        const Field* f = door.find(member.first);
        if (f == nullptr) {
            push(ErrorKind::UnknownField, at(member.first), case_names(door), member.first,
                 "the union declares no such case");
            return;
        }
        if (!f->carries) {
            push(ErrorKind::TypeMismatch, at(f->name), "the case's name alone",
                 json_type_name(member.second), "the case carries nothing");
            return;
        }
        Cell c = Cell::boolean(false);
        if (cell(member.second, f->type, c, at(f->name))) {
            out.set(f->name, std::move(c));
        }
    }

    static std::string case_names(const Schema& door) {
        std::string s = "one of";
        for (const Field& f : door.fields()) {
            s += " " + f.name;
        }
        return s;
    }

    // An Any field: {"schema", "version", "content_id" (optional), "fields"}, decoded under the
    // registry's definition of that name and version.
    bool open_value(const detail::JsonValue& node, Cell& out, const std::string& path) {
        using JT = detail::JsonValue::Type;
        if (node.type != JT::Object) {
            push(ErrorKind::TypeMismatch, path, "Any", json_type_name(node));
            return false;
        }
        for (const auto& member : node.members) {
            const std::string& k = member.first;
            if (k != "schema" && k != "version" && k != "content_id" && k != "fields") {
                push(ErrorKind::UnknownField, path + "." + k, "Any", k,
                     "an open field holds schema, version, content_id and fields");
                return false;
            }
        }
        const detail::JsonValue* name = node.find("schema");
        const detail::JsonValue* version = node.find("version");
        const detail::JsonValue* fields_node = node.find("fields");
        std::uint32_t ver = 0;
        if (name == nullptr || name->type != JT::String || version == nullptr ||
            version->type != JT::Number || !json_parse_u32(version->text, ver) ||
            fields_node == nullptr) {
            push(ErrorKind::MalformedField, path, "Any", json_type_name(node),
                 "an open field names its shape by schema and version, and holds its fields");
            return false;
        }
        const std::string label = name->text + " v" + std::to_string(ver);
        if (open == nullptr) {
            push(ErrorKind::UnknownSchema, path, "Any", label,
                 "an open field is admitted against a registry, and none was given");
            return false;
        }
        std::shared_ptr<const Schema> shape = open->lookup(name->text, ver);
        if (!shape) {
            push(ErrorKind::UnknownSchema, path, "a registered shape", label,
                 "no such schema is registered");
            return false;
        }
        if (const detail::JsonValue* cid = node.find("content_id")) {
            ContentId id = 0;
            if (cid->type != JT::String || !json_parse_content_id(cid->text, id)) {
                push(ErrorKind::MalformedField, path + ".content_id", "a hex string",
                     json_type_name(*cid));
                return false;
            }
            if (id != shape->content_id()) {
                push(ErrorKind::SchemaMismatch, path, format_content_id(shape->content_id()),
                     format_content_id(id),
                     label + " was written against another shape of this name and version");
                return false;
            }
        }
        if (!shape->is_union() && fields_node->type != JT::Object) {
            push(ErrorKind::TypeMismatch, path + ".fields", version_label(*shape),
                 json_type_name(*fields_node));
            return false;
        }
        if (!afford(entry_cells(*shape), path, "Any")) {
            return false;
        }
        Value nested(shape);
        fields(*fields_node, *shape, nested, path);
        out = Cell::message(std::move(nested));
        return true;
    }

    void fields(const detail::JsonValue& obj, const Schema& door, Value& out,
                const std::string& base) {
        if (door.is_union()) {
            union_case(obj, door, out, base);
            return;
        }
        // Strict: every member must name a declared field.
        for (const auto& member : obj.members) {
            if (stop()) {
                return;
            }
            if (door.find(member.first) == nullptr) {
                const std::string path = base.empty() ? member.first : base + "." + member.first;
                push(ErrorKind::UnknownField, path, "(a declared field)", member.first,
                     "the door does not declare this field");
            }
        }
        for (const Field& f : door.fields()) {
            if (stop()) {
                return;
            }
            const detail::JsonValue* node = obj.find(f.name);
            if (node == nullptr) {
                continue; // absent — the gate decides whether that is fatal
            }
            const std::string path = base.empty() ? f.name : base + "." + f.name;
            Cell c = Cell::boolean(false);
            if (cell(*node, f.type, c, path)) {
                out.set(f.name, std::move(c));
            }
        }
    }
};

bool JsonDecoder::cell(const detail::JsonValue& node, const TypeRef& t, Cell& out,
                       const std::string& path) {
    using JT = detail::JsonValue::Type;
    switch (t.kind) {
    case Kind::Int: {
        if (node.type != JT::String) {
            push(ErrorKind::TypeMismatch, path, "Int", json_type_name(node));
            return false;
        }
        std::int64_t v = 0;
        if (!json_parse_int64(node.text, v)) {
            push(ErrorKind::MalformedField, path, "Int", "json:string",
                 "not a base-10 64-bit integer");
            return false;
        }
        out = Cell::integer(v);
        return true;
    }
    case Kind::Float: {
        if (node.type == JT::Number) {
            double d = 0;
            if (!json_parse_double(node.text, d)) {
                push(ErrorKind::MalformedField, path, "Float", "json:number", "unparseable number");
                return false;
            }
            out = Cell::real(d);
            return true;
        }
        if (node.type == JT::String) {
            if (node.text == "NaN") {
                out = Cell::real(std::numeric_limits<double>::quiet_NaN());
                return true;
            }
            if (node.text == "Infinity") {
                out = Cell::real(std::numeric_limits<double>::infinity());
                return true;
            }
            if (node.text == "-Infinity") {
                out = Cell::real(-std::numeric_limits<double>::infinity());
                return true;
            }
            push(ErrorKind::MalformedField, path, "Float", "json:string",
                 "not a number or NaN/Infinity token");
            return false;
        }
        push(ErrorKind::TypeMismatch, path, "Float", json_type_name(node));
        return false;
    }
    case Kind::Text: {
        if (node.type != JT::String) {
            push(ErrorKind::TypeMismatch, path, "Text", json_type_name(node));
            return false;
        }
        if (!detail::valid_utf8(node.text)) {
            push(ErrorKind::MalformedField, path, "Text", "json:string", "invalid UTF-8");
            return false;
        }
        out = Cell::text(node.text);
        return true;
    }
    case Kind::Bool: {
        if (node.type != JT::Bool) {
            push(ErrorKind::TypeMismatch, path, "Bool", json_type_name(node));
            return false;
        }
        out = Cell::boolean(node.boolean);
        return true;
    }
    case Kind::Bytes: {
        if (node.type != JT::String) {
            push(ErrorKind::TypeMismatch, path, "Bytes", json_type_name(node));
            return false;
        }
        Bytes b;
        if (!detail::base64_decode(node.text, b)) {
            push(ErrorKind::MalformedField, path, "Bytes", "json:string", "invalid base64");
            return false;
        }
        out = Cell::bytes(std::move(b));
        return true;
    }
    case Kind::Message:
    case Kind::Union: {
        const bool fits = node.type == JT::Object || (t.kind == Kind::Union && node.type == JT::String);
        if (!fits) {
            push(ErrorKind::TypeMismatch, path, type_label(t), json_type_name(node));
            return false;
        }
        if (!afford(entry_cells(*t.message), path, type_label(t))) {
            return false;
        }
        Value nested(t.message);
        fields(node, *t.message, nested, path);
        out = Cell::message(std::move(nested));
        return true; // shape errors (if any) were recorded; the gate will also see them
    }
    case Kind::Any:
        return open_value(node, out, path);
    case Kind::List: {
        if (node.type != JT::Array) {
            push(ErrorKind::TypeMismatch, path, type_label(t), json_type_name(node));
            return false;
        }
        if (!afford(node.items.size(), path, type_label(t))) {
            return false;
        }
        Cell::Array arr;
        arr.reserve(node.items.size());
        for (std::size_t i = 0; i < node.items.size(); ++i) {
            if (stop()) {
                break;
            }
            Cell ec = Cell::boolean(false);
            if (cell(node.items[i], *t.element, ec, path + "[" + std::to_string(i) + "]")) {
                arr.push_back(std::move(ec));
            }
        }
        out = Cell::list(std::move(arr));
        return true;
    }
    }
    return false; // unreachable
}

// ===========================================================================
//  The persistence gate: identity -> decode -> validate_into (one gate)
// ===========================================================================

// Combine decode diagnostics with the structural gate. A fatal decode (a binary
// desync) is authoritative on its own and skips the gate — there is no coherent
// candidate to judge. Otherwise validate_into is the conformance authority.
Admission finish(Value candidate, std::vector<Error> decode_errs, bool fatal, const Schema& door,
                 bool collect_all, const Registry* open) {
    if (fatal) {
        return Admission::reject(std::move(decode_errs));
    }

    std::vector<Error> verrs = detail::validate_into(candidate, door, collect_all, open);

    if (!collect_all) {
        if (!decode_errs.empty()) {
            return Admission::reject(std::move(decode_errs.front()));
        }
        if (!verrs.empty()) {
            return Admission::reject(std::move(verrs.front()));
        }
        return Admission::accept(std::move(candidate));
    }

    std::unordered_set<std::string> covered;
    covered.reserve(decode_errs.size());
    for (const Error& e : decode_errs) {
        covered.insert(e.path);
    }
    std::vector<Error> merged = std::move(decode_errs);
    for (Error& e : verrs) {
        if (covered.find(e.path) == covered.end()) {
            merged.push_back(std::move(e));
        }
    }
    if (merged.empty()) {
        return Admission::accept(std::move(candidate));
    }
    return Admission::reject(std::move(merged));
}

Admission admit_against(const Unverified::Impl& impl, const std::shared_ptr<const Schema>& door,
                        bool collect_all, const Registry* open) {
    // Question 1 (identity): the claim must name this exact door.
    if (impl.name != door->name() || impl.version != door->version()) {
        return Admission::reject(Error{ErrorKind::SchemaMismatch, "", version_label(*door),
                                       impl.name + " v" + std::to_string(impl.version),
                                       "the bytes claim a different schema than this door"});
    }
    if (impl.has_content_id && impl.content_id != door->content_id()) {
        return Admission::reject(Error{
            ErrorKind::SchemaMismatch, "", format_content_id(door->content_id()),
            format_content_id(impl.content_id),
            "the bytes were written against a different shape of this name and version"});
    }

    // ONE budget for this whole decode, created here — the single place
    // every untrusted seam funnels through (bus delivery, kernel re-admission,
    // isolation manifests/snapshots/emissions, Bridge frames, persistence loads),
    // in both encodings. No transport carries its own copy of this law.
    DecodeBudget budget;
    if (!budget.take(entry_cells(*door))) {
        return Admission::reject(Error{ErrorKind::MalformedBytes, "", version_label(*door), "",
                                       budget_exhausted_detail()});
    }
    Value candidate(door);

    if (impl.format == Unverified::Impl::Format::Native) {
        detail::BinReader reader(impl.body);
        BinaryDecoder dec(reader, budget, open);
        dec.body(*door, candidate, "", 0);
        if (!dec.fatal && !reader.at_end()) {
            dec.fail(ErrorKind::MalformedBytes, "", "", "trailing bytes after the value");
        }
        return finish(std::move(candidate), std::move(dec.errs), dec.fatal, *door, collect_all,
                      open);
    }

    using JT = detail::JsonValue::Type;
    if (!door->is_union() && impl.json_fields.type != JT::Object) {
        return Admission::reject(Error{ErrorKind::MalformedBytes, "", version_label(*door), "",
                                       "'fields' is not an object"});
    }
    std::vector<Error> decode_errs;
    JsonDecoder dec{decode_errs, collect_all, budget, open};
    dec.fields(impl.json_fields, *door, candidate, "");
    // Exhaustion is fatal in both encodings: there is no coherent candidate to
    // judge, so no partial value reaches validate_into and none can be accepted.
    return finish(std::move(candidate), std::move(decode_errs), dec.exhausted, *door, collect_all,
                  open);
}

Error envelope_error_of(const Unverified& u, const std::shared_ptr<const Unverified::Impl>& impl) {
    (void)u;
    return impl ? impl->envelope_error : Error{ErrorKind::MalformedBytes, "", "", "", "empty"};
}

} // namespace

// ===========================================================================
//  Public API
// ===========================================================================

std::string serialize(const Value& value) {
    const Schema& s = value.schema();
    if (s.name().size() > 0xFFFF) {
        throw std::invalid_argument("serialize: schema name exceeds 65535 bytes");
    }
    std::string out;
    detail::put_u8(out, kBinMagic0);
    detail::put_u8(out, kBinMagic1);
    detail::put_u8(out, kBinFormatVersion);
    detail::put_u16le(out, static_cast<std::uint16_t>(s.name().size()));
    out.append(s.name());
    detail::put_u32le(out, s.version());
    detail::put_u64le(out, s.content_id());
    bin_encode_body(value, out);
    return out;
}

Unverified parse(std::string_view bytes) noexcept {
    Unverified u;
    auto impl = std::make_shared<Unverified::Impl>();
    impl->format = Unverified::Impl::Format::Native;

    auto malformed = [&](std::string why) {
        impl->ok = false;
        impl->envelope_error = Error{ErrorKind::MalformedBytes, "", "", "", std::move(why)};
        u.impl_ = std::move(impl);
        return u;
    };

    detail::BinReader r(bytes);
    std::uint8_t m0 = 0;
    std::uint8_t m1 = 0;
    std::uint8_t ver = 0;
    if (!r.u8(m0) || !r.u8(m1)) {
        return malformed("truncated magic");
    }
    if (m0 != kBinMagic0 || m1 != kBinMagic1) {
        return malformed("bad magic (not a Zen native value)");
    }
    if (!r.u8(ver)) {
        return malformed("truncated format version");
    }
    if (ver != kBinFormatVersion) {
        return malformed("unsupported native format version");
    }

    std::uint16_t name_len = 0;
    if (!r.u16le(name_len)) {
        return malformed("truncated schema-name length");
    }
    std::string_view name_bytes;
    if (!r.take(name_len, name_bytes)) {
        return malformed("truncated schema name");
    }
    if (!detail::valid_utf8(name_bytes)) {
        return malformed("schema name is not valid UTF-8");
    }
    std::uint32_t schema_version = 0;
    if (!r.u32le(schema_version)) {
        return malformed("truncated schema version");
    }
    std::uint64_t content_id = 0;
    if (!r.u64le(content_id)) {
        return malformed("truncated content id (mandatory in the native format)");
    }

    impl->name.assign(name_bytes);
    impl->version = schema_version;
    impl->content_id = content_id;
    impl->has_content_id = true;

    std::string_view body_view;
    r.take(r.remaining(), body_view); // the rest is the still-opaque body
    impl->body.assign(body_view);
    impl->ok = true;
    u.impl_ = std::move(impl);
    return u;
}

bool Unverified::well_formed() const noexcept { return impl_ && impl_->ok; }

const std::string& Unverified::claimed_name() const noexcept {
    static const std::string kEmpty;
    return impl_ ? impl_->name : kEmpty;
}

std::uint32_t Unverified::claimed_version() const noexcept { return impl_ ? impl_->version : 0; }

Admission admit(const Unverified& unverified, std::shared_ptr<const Schema> door, Report report) {
    if (!door) {
        throw std::invalid_argument("admit(Unverified, door): door must be non-null");
    }
    if (!unverified.impl_ || !unverified.impl_->ok) {
        return Admission::reject(envelope_error_of(unverified, unverified.impl_));
    }
    return admit_against(*unverified.impl_, door, report == Report::Full, nullptr);
}

Admission admit(const Unverified& unverified, std::shared_ptr<const Schema> door,
                const Registry& open, Report report) {
    if (!door) {
        throw std::invalid_argument("admit(Unverified, door, open): door must be non-null");
    }
    if (!unverified.impl_ || !unverified.impl_->ok) {
        return Admission::reject(envelope_error_of(unverified, unverified.impl_));
    }
    return admit_against(*unverified.impl_, door, report == Report::Full, &open);
}

Admission admit(const Unverified& unverified, const Registry& registry, Report report) {
    if (!unverified.impl_ || !unverified.impl_->ok) {
        return Admission::reject(envelope_error_of(unverified, unverified.impl_));
    }
    const Unverified::Impl& impl = *unverified.impl_;
    std::shared_ptr<const Schema> door = registry.lookup(impl.name, impl.version);
    if (!door) {
        return Admission::reject(Error{ErrorKind::UnknownSchema, "",
                                       impl.name + " v" + std::to_string(impl.version), "",
                                       "no such schema is registered"});
    }
    return admit_against(impl, door, report == Report::Full, &registry);
}

// ---- Compatibility (JSON) -------------------------------------------------

namespace compat {

std::string serialize(const Value& value) {
    const Schema& s = value.schema();
    std::string out;
    out += "{\"zen\":";
    out += std::to_string(kJsonEnvelopeVersion);
    out += ",\"schema\":";
    detail::json_quote(s.name(), out);
    out += ",\"version\":";
    out += std::to_string(s.version());
    out += ",\"content_id\":";
    detail::json_quote(format_content_id(s.content_id()), out);
    out += ",\"fields\":";
    json_encode_fields(value, out);
    out.push_back('}');
    return out;
}

Unverified parse(std::string_view bytes) noexcept {
    Unverified u;
    auto impl = std::make_shared<Unverified::Impl>();
    impl->format = Unverified::Impl::Format::Json;

    detail::JsonParse parsed = detail::parse_json(bytes);
    if (!parsed.ok) {
        impl->envelope_error =
            Error{ErrorKind::MalformedBytes, "", "", "", "not valid JSON: " + parsed.error};
        u.impl_ = std::move(impl);
        return u;
    }

    const detail::JsonValue& root = parsed.value;
    using JT = detail::JsonValue::Type;
    auto malformed = [&](std::string why) {
        impl->ok = false;
        impl->envelope_error = Error{ErrorKind::MalformedBytes, "", "", "", std::move(why)};
        u.impl_ = std::move(impl);
        return u;
    };

    if (root.type != JT::Object) {
        return malformed("top-level value is not an object");
    }
    // Strict envelope: no member outside the known set.
    for (const auto& member : root.members) {
        const std::string& k = member.first;
        if (k != "zen" && k != "schema" && k != "version" && k != "content_id" && k != "fields") {
            return malformed("unknown envelope member '" + k + "'");
        }
    }

    const detail::JsonValue* zen = root.find("zen");
    if (zen == nullptr || zen->type != JT::Number) {
        return malformed("missing or non-numeric 'zen' envelope version");
    }
    {
        std::int64_t ev = 0;
        if (!json_parse_int64(zen->text, ev) || ev != kJsonEnvelopeVersion) {
            return malformed("unsupported envelope version");
        }
    }
    const detail::JsonValue* schema = root.find("schema");
    if (schema == nullptr || schema->type != JT::String) {
        return malformed("missing or non-string 'schema'");
    }
    const detail::JsonValue* version = root.find("version");
    if (version == nullptr || version->type != JT::Number) {
        return malformed("missing or non-numeric 'version'");
    }
    std::uint32_t ver = 0;
    if (!json_parse_u32(version->text, ver)) {
        return malformed("'version' is not a 32-bit unsigned integer");
    }
    // A record's fields are an object; a union's are an object or a bare case's name, which the
    // door decides when the body is decoded.
    const detail::JsonValue* fields = root.find("fields");
    if (fields == nullptr || (fields->type != JT::Object && fields->type != JT::String)) {
        return malformed("missing 'fields', or neither an object nor a case's name");
    }

    impl->name = schema->text;
    impl->version = ver;
    if (const detail::JsonValue* cid = root.find("content_id")) {
        if (cid->type != JT::String || !json_parse_content_id(cid->text, impl->content_id)) {
            return malformed("'content_id' is not a hex string");
        }
        impl->has_content_id = true;
    }
    impl->json_fields = *fields;
    impl->ok = true;
    u.impl_ = std::move(impl);
    return u;
}

} // namespace compat

} // namespace loom
