#include <zen/serialize.hpp>

#include <zen/registry.hpp>

#include "detail/base64.hpp"
#include "detail/gate_internal.hpp"
#include "detail/json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>

namespace zen {

// Envelope format version. Bumped only if the envelope shape (not a payload
// schema) ever changes; readers reject envelopes they do not understand.
inline constexpr int kEnvelopeVersion = 1;

// ===========================================================================
//  Serialization (in-memory Value -> text)
// ===========================================================================

namespace {

std::string format_content_id(ContentId id) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s = "0x";
    for (int shift = 60; shift >= 0; shift -= 4) {
        s.push_back(kHex[(id >> shift) & 0xF]);
    }
    return s;
}

void encode_int(std::int64_t v, std::string& out) {
    std::array<char, 24> buf{};
    auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    out.push_back('"');
    out.append(buf.data(), r.ptr);
    out.push_back('"');
}

void encode_float(double v, std::string& out) {
    if (std::isnan(v)) {
        out += "\"NaN\"";
        return;
    }
    if (std::isinf(v)) {
        out += v < 0 ? "\"-Infinity\"" : "\"Infinity\"";
        return;
    }
    std::array<char, 32> buf{};
    auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    out.append(buf.data(), r.ptr);
}

void encode_cell(const Cell& c, std::string& out);

// Encode a value's present fields as a JSON object (no header). Used for the
// top-level "fields" and for every nested message.
void encode_fields(const Value& v, std::string& out) {
    out.push_back('{');
    bool first = true;
    const auto& fields = v.schema().fields();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const Cell* c = v.at(i);
        if (c == nullptr) {
            continue; // absent fields are simply omitted
        }
        if (!first) {
            out.push_back(',');
        }
        first = false;
        detail::json_quote(fields[i].name, out);
        out.push_back(':');
        encode_cell(*c, out);
    }
    out.push_back('}');
}

void encode_cell(const Cell& c, std::string& out) {
    switch (c.kind()) {
    case Kind::Int:
        encode_int(c.as_int(), out);
        break;
    case Kind::Float:
        encode_float(c.as_float(), out);
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
    case Kind::Message: {
        const std::shared_ptr<Value>& nested = c.as_message();
        if (nested) {
            encode_fields(*nested, out);
        } else {
            out += "null";
        }
        break;
    }
    case Kind::List: {
        out.push_back('[');
        bool first = true;
        for (const Cell& e : c.as_list()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            encode_cell(e, out);
        }
        out.push_back(']');
        break;
    }
    }
}

} // namespace

std::string serialize(const Value& value) {
    const Schema& s = value.schema();
    std::string out;
    out += "{\"zen\":";
    out += std::to_string(kEnvelopeVersion);
    out += ",\"schema\":";
    detail::json_quote(s.name(), out);
    out += ",\"version\":";
    out += std::to_string(s.version());
    out += ",\"content_id\":";
    detail::json_quote(format_content_id(s.content_id()), out);
    out += ",\"fields\":";
    encode_fields(value, out);
    out.push_back('}');
    return out;
}

// ===========================================================================
//  Parsing (text -> Unverified) and the persistence gate
// ===========================================================================

struct Unverified::Impl {
    bool ok = false;
    Error envelope_error{ErrorKind::MalformedBytes, "", "", "", "not parsed"};
    std::string name;
    std::uint32_t version = 0;
    bool has_content_id = false;
    ContentId content_id = 0;
    detail::JsonValue fields; // the "fields" object
};

namespace {

bool parse_int64(std::string_view text, std::int64_t& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto r = std::from_chars(begin, end, out, 10);
    return r.ec == std::errc{} && r.ptr == end;
}

bool parse_double(std::string_view text, double& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto r = std::from_chars(begin, end, out);
    return r.ec == std::errc{} && r.ptr == end;
}

bool parse_u32(std::string_view text, std::uint32_t& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto r = std::from_chars(begin, end, out, 10);
    return r.ec == std::errc{} && r.ptr == end;
}

bool parse_content_id(std::string_view text, ContentId& out) {
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

std::string type_label(const TypeRef& t) {
    switch (t.kind) {
    case Kind::Message:
        return "Message(" + t.message->name() + " v" + std::to_string(t.message->version()) + ")";
    case Kind::List:
        return "List<" + type_label(*t.element) + ">";
    default:
        return name_of(t.kind);
    }
}

struct Decoder {
    std::vector<Error>& errs;
    bool collect_all;

    bool stop() const { return !collect_all && !errs.empty(); }

    void push(ErrorKind kind, std::string path, std::string expected, std::string actual,
              std::string detail = "") {
        errs.push_back(Error{kind, std::move(path), std::move(expected), std::move(actual),
                             std::move(detail)});
    }

    // Returns true if a cell was produced (and assigned to `out`).
    bool cell(const detail::JsonValue& node, const TypeRef& t, Cell& out, const std::string& path);

    void fields(const detail::JsonValue& obj, const Schema& door, Value& out,
                const std::string& base) {
        for (const Field& f : door.fields()) {
            if (stop()) {
                return;
            }
            const detail::JsonValue* node = obj.find(f.name);
            if (node == nullptr) {
                continue; // absent — the gate decides whether that is fatal
            }
            std::string path = base.empty() ? f.name : base + "." + f.name;
            Cell c = Cell::boolean(false); // placeholder, overwritten on success
            if (cell(*node, f.type, c, path)) {
                out.set(f.name, std::move(c));
            }
        }
    }
};

bool Decoder::cell(const detail::JsonValue& node, const TypeRef& t, Cell& out,
                   const std::string& path) {
    using JT = detail::JsonValue::Type;
    switch (t.kind) {
    case Kind::Int: {
        if (node.type != JT::String) {
            push(ErrorKind::TypeMismatch, path, "Int", json_type_name(node));
            return false;
        }
        std::int64_t v = 0;
        if (!parse_int64(node.text, v)) {
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
            if (!parse_double(node.text, d)) {
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
    case Kind::Message: {
        if (node.type != JT::Object) {
            push(ErrorKind::TypeMismatch, path, type_label(t), json_type_name(node));
            return false;
        }
        Value nested(t.message);
        fields(node, *t.message, nested, path);
        out = Cell::message(std::move(nested));
        return true; // shape errors (if any) were recorded; the gate will also see them
    }
    case Kind::List: {
        if (node.type != JT::Array) {
            push(ErrorKind::TypeMismatch, path, type_label(t), json_type_name(node));
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

Admission admit_against(const Unverified::Impl& impl, const std::shared_ptr<const Schema>& door,
                        bool collect_all) {
    // Question 1 (identity): the claim must name this exact door.
    if (impl.name != door->name() || impl.version != door->version()) {
        return Admission::reject(Error{
            ErrorKind::SchemaMismatch, "",
            door->name() + " v" + std::to_string(door->version()),
            impl.name + " v" + std::to_string(impl.version),
            "the bytes claim a different schema than this door"});
    }
    if (impl.has_content_id && impl.content_id != door->content_id()) {
        return Admission::reject(Error{
            ErrorKind::SchemaMismatch, "", format_content_id(door->content_id()),
            format_content_id(impl.content_id),
            "the bytes were written against a different shape of this name and version"});
    }

    // Decode the untrusted payload under the door's shape (byte-level faithfulness).
    std::vector<Error> decode_errs;
    Decoder dec{decode_errs, collect_all};
    Value candidate(door);
    dec.fields(impl.fields, *door, candidate, "");

    // Question 2 (structure): the SAME validator the live bus path uses.
    std::vector<Error> verrs = detail::validate_into(candidate, *door, collect_all);

    if (!collect_all) {
        if (!decode_errs.empty()) {
            return Admission::reject(std::move(decode_errs.front()));
        }
        if (!verrs.empty()) {
            return Admission::reject(std::move(verrs.front()));
        }
        return Admission::accept(std::move(candidate));
    }

    // Full report: decode diagnostics first, then any gate errors not already
    // covered at the same path (decode errors are the more specific cause).
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

} // namespace

bool Unverified::well_formed() const noexcept { return impl_ && impl_->ok; }

const std::string& Unverified::claimed_name() const noexcept {
    static const std::string kEmpty;
    return impl_ ? impl_->name : kEmpty;
}

std::uint32_t Unverified::claimed_version() const noexcept { return impl_ ? impl_->version : 0; }

Unverified parse(std::string_view bytes) noexcept {
    Unverified u;
    auto impl = std::make_shared<Unverified::Impl>();

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
    const detail::JsonValue* zen = root.find("zen");
    if (zen == nullptr || zen->type != JT::Number) {
        return malformed("missing or non-numeric 'zen' envelope version");
    }
    {
        std::int64_t ev = 0;
        if (!parse_int64(zen->text, ev) || ev != kEnvelopeVersion) {
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
    if (!parse_u32(version->text, ver)) {
        return malformed("'version' is not a 32-bit unsigned integer");
    }
    const detail::JsonValue* fields = root.find("fields");
    if (fields == nullptr || fields->type != JT::Object) {
        return malformed("missing or non-object 'fields'");
    }

    impl->name = schema->text;
    impl->version = ver;
    if (const detail::JsonValue* cid = root.find("content_id")) {
        if (cid->type != JT::String || !parse_content_id(cid->text, impl->content_id)) {
            return malformed("'content_id' is not a hex string");
        }
        impl->has_content_id = true;
    }
    impl->fields = *fields;
    impl->ok = true;
    u.impl_ = std::move(impl);
    return u;
}

Admission admit(const Unverified& unverified, std::shared_ptr<const Schema> door, Report report) {
    if (!door) {
        throw std::invalid_argument("admit(Unverified, door): door must be non-null");
    }
    if (!unverified.impl_ || !unverified.impl_->ok) {
        Error e = unverified.impl_ ? unverified.impl_->envelope_error
                                   : Error{ErrorKind::MalformedBytes, "", "", "", "empty"};
        return Admission::reject(std::move(e));
    }
    return admit_against(*unverified.impl_, door, report == Report::Full);
}

Admission admit(const Unverified& unverified, const Registry& registry, Report report) {
    if (!unverified.impl_ || !unverified.impl_->ok) {
        Error e = unverified.impl_ ? unverified.impl_->envelope_error
                                   : Error{ErrorKind::MalformedBytes, "", "", "", "empty"};
        return Admission::reject(std::move(e));
    }
    const Unverified::Impl& impl = *unverified.impl_;
    std::shared_ptr<const Schema> door = registry.lookup(impl.name, impl.version);
    if (!door) {
        return Admission::reject(Error{ErrorKind::UnknownSchema, "",
                                       impl.name + " v" + std::to_string(impl.version), "",
                                       "no such schema is registered"});
    }
    return admit_against(impl, door, report == Report::Full);
}

} // namespace zen
