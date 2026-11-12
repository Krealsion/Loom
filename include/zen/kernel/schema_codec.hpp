// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_SCHEMA_CODEC_HPP
#define ZEN_KERNEL_SCHEMA_CODEC_HPP

// Crossing a Schema over the C ABI without C++ types: a Schema is encoded as a Value of a fixed
// meta-schema, so it travels as bytes and is re-admitted through the gate like any value
// before the host rebuilds it. The library encodes and the host decodes, each with its own
// copy of this header; they agree only on the bytes.
//
// A type reference is encoded as a flat, prefix-order list of tokens, so nested
// Lists and Messages need no recursive meta-schema:
//   Int/Float/Text/Bool/Bytes/Any -> one token {kind}
//   Message/Union                 -> one token {kind, ref_name, ref_version}
//   List                          -> one token {kind} then the element's tokens
// Message and Union fields and List elements reference their nested schema by (name, version),
// resolved against a dependency Registry — so a manifest lists referenced
// schemas before the schemas that reference them. A record is described by `zen.SchemaDesc`, a
// union by `zen.UnionDesc` (its cases, each carrying a type or nothing), and a list that may hold
// either holds `zen.ShapeDesc`, the union of the two.

#include <zen/kernel/descriptor.hpp>
#include <zen/registry.hpp>
#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace loom {

/// A Weave's ask: the capabilities it requests of the host. Advice, not authority: carried in
/// the manifest and gated like the accept-set, so the host can show it; the host's admission
/// policy decides alone. A default one is no ask, and writes no manifest section.
struct CapabilityAsk {
    bool network = false;           ///< would like os_cap::Network
    std::string filesystem;         ///< an FsAccess level name (e.g. "write-scoped"); "" = none
    std::vector<std::string> roles; ///< role names it would like a send-rule to
};

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

/// A record's description.
inline std::shared_ptr<const Schema> schema_desc_schema() {
    static const auto s = SchemaBuilder("zen.SchemaDesc", 1)
                              .field("name", Kind::Text)
                              .field("version", Kind::Int)
                              .list("fields", type_message(field_desc_schema()))
                              .build();
    return s;
}

/// A union's case: its name, whether it carries a value, and the type it carries (its tokens,
/// empty when it carries nothing).
inline std::shared_ptr<const Schema> case_desc_schema() {
    static const auto s = SchemaBuilder("zen.Case", 1)
                              .field("name", Kind::Text)
                              .field("carries", Kind::Bool)
                              .list("type", type_message(type_token_schema()))
                              .build();
    return s;
}

/// A union's description: its cases in declared order.
inline std::shared_ptr<const Schema> union_desc_schema() {
    static const auto s = SchemaBuilder("zen.UnionDesc", 1)
                              .field("name", Kind::Text)
                              .field("version", Kind::Int)
                              .list("cases", type_message(case_desc_schema()))
                              .build();
    return s;
}

/// A description of either form: the union of a record's and a union's.
inline std::shared_ptr<const Schema> shape_desc_schema() {
    static const auto s = make_union("zen.ShapeDesc", 1,
                                     {case_of("record", type_message(schema_desc_schema())),
                                      case_of("union", type_message(union_desc_schema()))});
    return s;
}

inline std::shared_ptr<const Schema> capability_ask_schema() {
    static const auto s = SchemaBuilder("zen.CapabilityAsk", 1)
                              .field("network", Kind::Bool)
                              .field("filesystem", Kind::Text)
                              .list("roles", type_of(Kind::Text))
                              .build();
    return s;
}

inline std::shared_ptr<const Schema> manifest_schema() {
    // The manifest: `referenced` (the nested schemas, listed before whatever references
    // them, so the manifest is self-contained), `accepted`, `state`, and the optional
    // `requests` (the ask), `claims` (the claim-set) and `emits` (the emit-set: vocabulary the
    // host claims into its agreement wall, never authority). An image built for an older
    // manifest is refused by the manifest version its descriptor names, before its manifest is
    // read. docs/decisions/declared-vocabulary-is-agreed-at-admission.md
    static const auto s = SchemaBuilder("zen.Manifest", kManifestVersion)
                              .list("referenced", type_message(shape_desc_schema()),
                                    /*required=*/false)
                              .list("accepted", type_message(shape_desc_schema()))
                              .message("state", shape_desc_schema())
                              .message("requests", capability_ask_schema(), /*required=*/false)
                              .list("claims", type_message(shape_desc_schema()),
                                    /*required=*/false)
                              .list("emits", type_message(shape_desc_schema()),
                                    /*required=*/false)
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
    case Kind::Union:
        tokens.push_back(Cell::message(make_type_token(t.kind, t.message)));
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

/// A record's description (`zen.SchemaDesc`). Throws std::invalid_argument for a union, whose
/// description is `encode_union`'s; `encode_shape` describes either.
inline Value encode_schema(const Schema& s) {
    if (s.is_union()) {
        throw std::invalid_argument("encode_schema: '" + s.name() +
                                    "' is a union, described by encode_union or encode_shape");
    }
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

/// A union's description (`zen.UnionDesc`). Throws std::invalid_argument for a record.
inline Value encode_union(const Schema& s) {
    if (!s.is_union()) {
        throw std::invalid_argument("encode_union: '" + s.name() + "' is a record");
    }
    Value desc(union_desc_schema());
    desc.set("name", Cell::text(s.name()));
    desc.set("version", Cell::integer(static_cast<std::int64_t>(s.version())));
    std::vector<Cell> cases;
    cases.reserve(s.fields().size());
    for (const Field& f : s.fields()) {
        Value cd(case_desc_schema());
        cd.set("name", Cell::text(f.name));
        cd.set("carries", Cell::boolean(f.carries));
        std::vector<Cell> tokens;
        if (f.carries) {
            encode_type(f.type, tokens);
        }
        cd.set("type", Cell::list(std::move(tokens)));
        cases.push_back(Cell::message(std::move(cd)));
    }
    desc.set("cases", Cell::list(std::move(cases)));
    return desc;
}

/// A description of either form (`zen.ShapeDesc`).
inline Value encode_shape(const Schema& s) {
    Value v(shape_desc_schema());
    if (s.is_union()) {
        v.set("union", Cell::message(encode_union(s)));
    } else {
        v.set("record", Cell::message(encode_schema(s)));
    }
    return v;
}

inline Value encode_capability_ask(const CapabilityAsk& ask) {
    Value v(capability_ask_schema());
    v.set("network", Cell::boolean(ask.network));
    v.set("filesystem", Cell::text(ask.filesystem));
    std::vector<Cell> roles;
    roles.reserve(ask.roles.size());
    for (const auto& r : ask.roles) {
        roles.push_back(Cell::text(r));
    }
    v.set("roles", Cell::list(std::move(roles)));
    return v;
}

// The `referenced` section is the component closure, walked by `loom::collect_referenced`
// (zen/schema.hpp), post-order so the decoder resolves it front to back, and deduplicated by
// identity, so two definitions of one (name, version) both travel and the loader refuses the
// manifest. `ask` may be null: then no requests section is written. `referenced` is written
// only when a declared shape nests something, and `emits` only when an emit-set is declared.
inline Value encode_manifest(const std::vector<std::shared_ptr<const Schema>>& accepted,
                             const Schema& state, const CapabilityAsk* ask = nullptr,
                             const std::vector<std::shared_ptr<const Schema>>* claims = nullptr,
                             const std::vector<std::shared_ptr<const Schema>>* emits = nullptr) {
    Value m(manifest_schema());
    std::vector<std::shared_ptr<const Schema>> referenced;
    for (const auto& s : accepted) {
        collect_referenced(*s, referenced);
    }
    collect_referenced(state, referenced);
    // Claimed and emitted shapes may nest others too; their dependencies join the same section.
    if (claims != nullptr) {
        for (const auto& s : *claims) {
            collect_referenced(*s, referenced);
        }
    }
    if (emits != nullptr) {
        for (const auto& s : *emits) {
            collect_referenced(*s, referenced);
        }
    }
    if (!referenced.empty()) {
        std::vector<Cell> refs;
        refs.reserve(referenced.size());
        for (const auto& s : referenced) {
            refs.push_back(Cell::message(encode_shape(*s)));
        }
        m.set("referenced", Cell::list(std::move(refs)));
    }
    std::vector<Cell> acc;
    acc.reserve(accepted.size());
    for (const auto& s : accepted) {
        acc.push_back(Cell::message(encode_shape(*s)));
    }
    m.set("accepted", Cell::list(std::move(acc)));
    m.set("state", Cell::message(encode_shape(state)));
    if (ask != nullptr) {
        m.set("requests", Cell::message(encode_capability_ask(*ask)));
    }
    if (claims != nullptr && !claims->empty()) {
        std::vector<Cell> cl;
        cl.reserve(claims->size());
        for (const auto& s : *claims) {
            cl.push_back(Cell::message(encode_shape(*s)));
        }
        m.set("claims", Cell::list(std::move(cl)));
    }
    if (emits != nullptr && !emits->empty()) {
        std::vector<Cell> em;
        em.reserve(emits->size());
        for (const auto& s : *emits) {
            em.push_back(Cell::message(encode_shape(*s)));
        }
        m.set("emits", Cell::list(std::move(em)));
    }
    return m;
}

// ---- decode (admitted descriptor Value -> Schema) ----------------------------
// Precondition: `desc` has passed the gate against the meta-schema. Reconstruction can still
// fail (an unresolved nested schema, a malformed type); that throws, and the host refuses the
// load cleanly.

// Refuses a type nested deeper than this on the way down. decode_type recurses once per List
// token and the token stream is bounded only by the list cap, so a deeply nested List<List<...>>
// would otherwise overflow the host's stack at load. It equals detail::kMaxBinaryDepth: a
// value nested deeper can never be serialized or admitted, so no legitimate schema is refused.
// Kept equal by hand, since this public, portable header cannot include src/detail.
inline constexpr int kMaxTypeDepth = 64;

inline TypeRef decode_type(const Cell::Array& tokens, std::size_t& i, const Registry& deps,
                           int depth = 0) {
    if (depth > kMaxTypeDepth) {
        throw std::runtime_error("schema descriptor: type nesting exceeds depth cap");
    }
    if (i >= tokens.size()) {
        throw std::runtime_error("schema descriptor: truncated type-token stream");
    }
    const Value& tok = *tokens[i].as_message();
    ++i;
    const std::int64_t raw = tok.get("kind")->as_int();
    if (raw < 0 || raw > static_cast<std::int64_t>(Kind::Any)) {
        throw std::runtime_error("schema descriptor: kind out of range");
    }
    const auto kind = static_cast<Kind>(static_cast<std::uint8_t>(raw));
    switch (kind) {
    case Kind::Message:
    case Kind::Union: {
        const Cell* ref_name = tok.get("ref_name");
        const Cell* ref_version = tok.get("ref_version");
        if (ref_name == nullptr || ref_version == nullptr) {
            throw std::runtime_error("schema descriptor: a nested shape's token is missing its "
                                     "reference");
        }
        auto ref =
            deps.lookup(ref_name->as_text(), static_cast<std::uint32_t>(ref_version->as_int()));
        if (!ref) {
            throw std::runtime_error("schema descriptor: unresolved nested schema '" +
                                     ref_name->as_text() + "'");
        }
        if (ref->is_union() != (kind == Kind::Union)) {
            throw std::runtime_error("schema descriptor: '" + ref_name->as_text() + "' is " +
                                     (ref->is_union() ? "a union" : "a record") +
                                     ", and its token says otherwise");
        }
        return type_message(ref);
    }
    case Kind::List:
        return type_list(decode_type(tokens, i, deps, depth + 1));
    default:
        return type_of(kind);
    }
}

/// A record from its admitted description (`zen.SchemaDesc`).
inline std::shared_ptr<const Schema> decode_record(const Value& desc, const Registry& deps) {
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
        rebuilt.push_back(Field{std::move(fname), std::move(type), required, /*carries=*/true});
    }
    return make_schema(std::move(name), version, std::move(rebuilt));
}

/// A union from its admitted description (`zen.UnionDesc`). A case that carries nothing has no
/// type tokens; one that carries a value has exactly one type's.
inline std::shared_ptr<const Schema> decode_union(const Value& desc, const Registry& deps) {
    std::string name = desc.get("name")->as_text();
    const auto version = static_cast<std::uint32_t>(desc.get("version")->as_int());
    const Cell::Array& cases = desc.get("cases")->as_list();
    std::vector<Field> rebuilt;
    rebuilt.reserve(cases.size());
    for (const Cell& case_cell : cases) {
        const Value& cd = *case_cell.as_message();
        std::string cname = cd.get("name")->as_text();
        const Cell::Array& tokens = cd.get("type")->as_list();
        if (!cd.get("carries")->as_bool()) {
            if (!tokens.empty()) {
                throw std::runtime_error("schema descriptor: a case that carries nothing has a type");
            }
            rebuilt.push_back(case_bare(std::move(cname)));
            continue;
        }
        std::size_t i = 0;
        TypeRef type = decode_type(tokens, i, deps);
        if (i != tokens.size()) {
            throw std::runtime_error("schema descriptor: trailing type tokens");
        }
        rebuilt.push_back(case_of(std::move(cname), std::move(type)));
    }
    // A union with no case is refused by the Schema itself (std::invalid_argument).
    return make_union(std::move(name), version, std::move(rebuilt));
}

/// A shape from any admitted description, read by the description it claims: a record's
/// `zen.SchemaDesc`, a union's `zen.UnionDesc`, or a `zen.ShapeDesc` holding either. So a list of
/// any of them -- a manifest's, a describe answer's, a list a consumer's own shape keeps -- reads
/// alike.
inline std::shared_ptr<const Schema> decode_schema(const Value& desc, const Registry& deps) {
    const Schema& claimed = desc.schema();
    if (same_identity(claimed, *schema_desc_schema())) {
        return decode_record(desc, deps);
    }
    if (same_identity(claimed, *union_desc_schema())) {
        return decode_union(desc, deps);
    }
    if (!same_identity(claimed, *shape_desc_schema())) {
        throw std::runtime_error("schema descriptor: '" + claimed.name() +
                                 "' is not a description of a shape");
    }
    if (const Cell* u = desc.get("union")) {
        return decode_union(*u->as_message(), deps);
    }
    return decode_record(*desc.get("record")->as_message(), deps);
}

/// Decode the optional `referenced` section of an admitted manifest, or of any value keeping one
/// (a list of descriptions, `decode_schema`), into `deps`, in order; the
/// encoder's post-order makes one forward pass enough. Identical re-registration is a no-op, a
/// conflicting one throws SchemaConflict, and a mis-ordered hand-built list fails its own
/// resolution. This overload publishes for the Registry's whole lifetime; prefer the
/// claim-taking one wherever the artifact that brought the components can go away.
inline void decode_referenced(const Value& manifest, Registry& deps) {
    const Cell* refs = manifest.get("referenced");
    if (refs == nullptr) {
        return; // flat manifest: nothing nested, nothing to do
    }
    for (const Cell& c : refs->as_list()) {
        deps.register_schema(decode_schema(*c.as_message(), deps));
    }
}

/// The same decode, taking a live claim instead of publishing for good (LIFE-08): the
/// components resolve while `scope` lives. One claim per entry, since each entry resolves
/// against the ones before it. Two entries naming one (name, version) with different content
/// meet the registry's wall at the second and throw `SchemaConflict`; the entries already
/// claimed are in `scope`, so the caller's scope releases them and a refused manifest leaves
/// nothing.
inline void decode_referenced(const Value& manifest, Registry& deps, SchemaClaimScope& scope) {
    const Cell* refs = manifest.get("referenced");
    if (refs == nullptr) {
        return;
    }
    for (const Cell& c : refs->as_list()) {
        deps.claim(scope, {decode_schema(*c.as_message(), deps)});
    }
}

// Precondition: `v` passed the gate against capability_ask_schema() (as a message nested in an
// admitted manifest), so every field is present and well-typed.
inline CapabilityAsk decode_capability_ask(const Value& v) {
    CapabilityAsk ask;
    ask.network = v.get("network")->as_bool();
    ask.filesystem = v.get("filesystem")->as_text();
    for (const Cell& c : v.get("roles")->as_list()) {
        ask.roles.push_back(c.as_text());
    }
    return ask;
}

} // namespace loom

#endif // ZEN_KERNEL_SCHEMA_CODEC_HPP
