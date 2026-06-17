#ifndef ZEN_KERNEL_SCHEMA_CODEC_HPP
#define ZEN_KERNEL_SCHEMA_CODEC_HPP

// Crossing a Schema over the C ABI without C++ types: a Schema is encoded *as a
// Value* of a fixed meta-schema (the minimal schema-as-value precursor), so it
// travels as ordinary bytes and is re-admitted through zen-core's gate just like
// any other value before the host reconstructs it. This header is shared by the
// library (encode) and the host (decode); each side has its own copy, and they
// agree only on the bytes.
//
// A type reference is encoded as a flat, prefix-order list of tokens, so nested
// Lists and Messages need no recursive meta-schema:
//   Int/Float/Text/Bool/Bytes -> one token {kind}
//   Message                    -> one token {kind, ref_name, ref_version}
//   List                       -> one token {kind} then the element's tokens
// Message and List elements reference their nested schema by (name, version),
// resolved against a dependency Registry — so a manifest lists referenced
// schemas before the schemas that reference them.

#include <zen/registry.hpp>
#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace zen::kernel {

// ---- the fixed meta-schemas (the kernel's grammar for describing schemas) ----

inline std::shared_ptr<const Schema> type_token_schema() {
    static const auto s = SchemaBuilder("zen.TypeToken", 1)
                              .field("kind", Kind::Int)
                              .field("ref_name", Kind::Text, /*required=*/false)
                              .field("ref_version", Kind::Int, /*required=*/false)
                              .build();
    return s;
}

inline std::shared_ptr<const Schema> field_desc_schema() {
    static const auto s = SchemaBuilder("zen.Field", 1)
                              .field("name", Kind::Text)
                              .field("required", Kind::Bool)
                              .list("type", type_message(type_token_schema()))
                              .build();
    return s;
}

inline std::shared_ptr<const Schema> schema_desc_schema() {
    static const auto s = SchemaBuilder("zen.SchemaDesc", 1)
                              .field("name", Kind::Text)
                              .field("version", Kind::Int)
                              .list("fields", type_message(field_desc_schema()))
                              .build();
    return s;
}

inline std::shared_ptr<const Schema> manifest_schema() {
    static const auto s = SchemaBuilder("zen.Manifest", 1)
                              .list("accepted", type_message(schema_desc_schema()))
                              .message("state", schema_desc_schema())
                              .build();
    return s;
}

// ---- encode (Schema -> descriptor Value) -------------------------------------

inline Value make_type_token(Kind k, const std::shared_ptr<const Schema>& ref) {
    Value tok(type_token_schema());
    tok.set("kind", Cell::integer(static_cast<std::int64_t>(static_cast<std::uint8_t>(k))));
    if (ref) {
        tok.set("ref_name", Cell::text(ref->name()));
        tok.set("ref_version", Cell::integer(static_cast<std::int64_t>(ref->version())));
    }
    return tok;
}

inline void encode_type(const TypeRef& t, std::vector<Cell>& tokens) {
    switch (t.kind) {
    case Kind::Message:
        tokens.push_back(Cell::message(make_type_token(Kind::Message, t.message)));
        break;
    case Kind::List:
        tokens.push_back(Cell::message(make_type_token(Kind::List, nullptr)));
        encode_type(*t.element, tokens);
        break;
    default:
        tokens.push_back(Cell::message(make_type_token(t.kind, nullptr)));
        break;
    }
}

inline Value encode_schema(const Schema& s) {
    Value desc(schema_desc_schema());
    desc.set("name", Cell::text(s.name()));
    desc.set("version", Cell::integer(static_cast<std::int64_t>(s.version())));

    std::vector<Cell> fields;
    fields.reserve(s.fields().size());
    for (const Field& f : s.fields()) {
        Value fd(field_desc_schema());
        fd.set("name", Cell::text(f.name));
        fd.set("required", Cell::boolean(f.required));
        std::vector<Cell> tokens;
        encode_type(f.type, tokens);
        fd.set("type", Cell::list(std::move(tokens)));
        fields.push_back(Cell::message(std::move(fd)));
    }
    desc.set("fields", Cell::list(std::move(fields)));
    return desc;
}

inline Value encode_manifest(const std::vector<std::shared_ptr<const Schema>>& accepted,
                             const Schema& state) {
    Value m(manifest_schema());
    std::vector<Cell> acc;
    acc.reserve(accepted.size());
    for (const auto& s : accepted) {
        acc.push_back(Cell::message(encode_schema(*s)));
    }
    m.set("accepted", Cell::list(std::move(acc)));
    m.set("state", Cell::message(encode_schema(state)));
    return m;
}

// ---- decode (admitted descriptor Value -> Schema) ----------------------------
//
// Precondition: `desc` has already passed the gate against the meta-schema, so
// its declared shape is sound. Reconstruction can still fail semantically
// (unresolved nested schema, malformed type) — those throw and the host turns
// them into a clean load refusal.

inline TypeRef decode_type(const Cell::Array& tokens, std::size_t& i, const Registry& deps) {
    if (i >= tokens.size()) {
        throw std::runtime_error("schema descriptor: truncated type-token stream");
    }
    const Value& tok = *tokens[i].as_message();
    ++i;
    const std::int64_t raw = tok.get("kind")->as_int();
    if (raw < 0 || raw > 6) {
        throw std::runtime_error("schema descriptor: kind out of range");
    }
    const auto kind = static_cast<Kind>(static_cast<std::uint8_t>(raw));
    switch (kind) {
    case Kind::Message: {
        const Cell* ref_name = tok.get("ref_name");
        const Cell* ref_version = tok.get("ref_version");
        if (ref_name == nullptr || ref_version == nullptr) {
            throw std::runtime_error("schema descriptor: Message token missing its reference");
        }
        auto ref =
            deps.lookup(ref_name->as_text(), static_cast<std::uint32_t>(ref_version->as_int()));
        if (!ref) {
            throw std::runtime_error("schema descriptor: unresolved nested schema '" +
                                     ref_name->as_text() + "'");
        }
        return type_message(ref);
    }
    case Kind::List:
        return type_list(decode_type(tokens, i, deps));
    default:
        return type_of(kind);
    }
}

inline std::shared_ptr<const Schema> decode_schema(const Value& desc, const Registry& deps) {
    std::string name = desc.get("name")->as_text();
    const auto version = static_cast<std::uint32_t>(desc.get("version")->as_int());

    const Cell::Array& fields = desc.get("fields")->as_list();
    std::vector<Field> rebuilt;
    rebuilt.reserve(fields.size());
    for (const Cell& field_cell : fields) {
        const Value& fd = *field_cell.as_message();
        std::string fname = fd.get("name")->as_text();
        const bool required = fd.get("required")->as_bool();
        const Cell::Array& tokens = fd.get("type")->as_list();
        std::size_t i = 0;
        TypeRef type = decode_type(tokens, i, deps);
        if (i != tokens.size()) {
            throw std::runtime_error("schema descriptor: trailing type tokens");
        }
        rebuilt.push_back(Field{std::move(fname), std::move(type), required});
    }
    return make_schema(std::move(name), version, std::move(rebuilt));
}

} // namespace zen::kernel

#endif // ZEN_KERNEL_SCHEMA_CODEC_HPP
