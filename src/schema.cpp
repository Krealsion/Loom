// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/schema.hpp>

#include "detail/hash.hpp"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace loom {
namespace {

void validate_typeref(const TypeRef& t) {
    switch (t.kind) {
    case Kind::Message:
        if (!t.message) {
            throw std::invalid_argument("Message type requires a nested schema");
        }
        if (t.element) {
            throw std::invalid_argument("Message type must not carry a list element");
        }
        break;
    case Kind::List:
        if (!t.element) {
            throw std::invalid_argument("List type requires an element type");
        }
        if (t.message) {
            throw std::invalid_argument("List type must not carry a message schema");
        }
        validate_typeref(*t.element);
        break;
    default:
        if (t.message || t.element) {
            throw std::invalid_argument("primitive type must not carry a schema or element");
        }
        break;
    }
}

// Fold a type's normalized structure into the running hash. Message types fold
// in the *precomputed* content id of their nested schema, so identity is a
// shallow, cheap recursion even for deep trees.
void hash_typeref(detail::Fnv1a& h, const TypeRef& t) {
    h.byte(static_cast<std::uint8_t>(t.kind));
    switch (t.kind) {
    case Kind::Message:
        h.u64(t.message->content_id());
        break;
    case Kind::List:
        hash_typeref(h, *t.element);
        break;
    default:
        break;
    }
}

ContentId compute_content_id(const std::string& name, std::uint32_t version,
                             const std::vector<Field>& fields) {
    detail::Fnv1a h;
    h.field(name);
    h.u64(version);
    h.u64(fields.size());
    for (const Field& f : fields) {
        h.field(f.name);
        h.byte(f.required ? 1U : 0U);
        hash_typeref(h, f.type);
    }
    return h.value();
}

} // namespace

TypeRef type_of(Kind k) {
    if (k == Kind::Message || k == Kind::List) {
        throw std::invalid_argument("type_of is for primitive kinds; use type_message/type_list");
    }
    return TypeRef{k, nullptr, nullptr};
}

TypeRef type_message(std::shared_ptr<const Schema> schema) {
    if (!schema) {
        throw std::invalid_argument("type_message requires a non-null schema");
    }
    return TypeRef{Kind::Message, std::move(schema), nullptr};
}

TypeRef type_list(TypeRef element) {
    return TypeRef{Kind::List, nullptr, std::make_shared<const TypeRef>(std::move(element))};
}

Schema::Schema(std::string name, std::uint32_t version, std::vector<Field> fields)
    : name_(std::move(name)), version_(version), fields_(std::move(fields)), content_id_(0) {
    std::unordered_set<std::string_view> seen;
    seen.reserve(fields_.size());
    for (const Field& f : fields_) {
        if (f.name.empty()) {
            throw std::invalid_argument("field name must not be empty");
        }
        if (!seen.insert(f.name).second) {
            throw std::invalid_argument("duplicate field name '" + f.name + "'");
        }
        validate_typeref(f.type);
    }
    content_id_ = compute_content_id(name_, version_, fields_);
}

const Field* Schema::find(std::string_view field_name) const noexcept {
    for (const Field& f : fields_) {
        if (f.name == field_name) {
            return &f;
        }
    }
    return nullptr;
}

SchemaBuilder::SchemaBuilder(std::string name, std::uint32_t version)
    : name_(std::move(name)), version_(version) {}

SchemaBuilder& SchemaBuilder::field(std::string name, Kind kind, bool required) {
    return add(Field{std::move(name), type_of(kind), required});
}

SchemaBuilder& SchemaBuilder::message(std::string name, std::shared_ptr<const Schema> schema,
                                      bool required) {
    return add(Field{std::move(name), type_message(std::move(schema)), required});
}

SchemaBuilder& SchemaBuilder::list(std::string name, TypeRef element, bool required) {
    return add(Field{std::move(name), type_list(std::move(element)), required});
}

SchemaBuilder& SchemaBuilder::add(Field f) {
    fields_.push_back(std::move(f));
    return *this;
}

std::shared_ptr<const Schema> SchemaBuilder::build() const {
    return std::make_shared<const Schema>(name_, version_, fields_);
}

std::shared_ptr<const Schema> make_schema(std::string name, std::uint32_t version,
                                          std::vector<Field> fields) {
    return std::make_shared<const Schema>(std::move(name), version, std::move(fields));
}

// ---- the component closure ---------------------------------------------------

void collect_referenced(const Schema& root, std::vector<std::shared_ptr<const Schema>>& out) {
    for (const Field& f : root.fields()) {
        collect_referenced(f.type, out);
    }
}

void collect_referenced(const TypeRef& type, std::vector<std::shared_ptr<const Schema>>& out) {
    if (type.kind == Kind::List && type.element) {
        collect_referenced(*type.element, out);
        return;
    }
    if (type.kind != Kind::Message || !type.message) {
        return;
    }
    // ALREADY CARRIED MEANS ALREADY EXPANDED, so the identity scan comes BEFORE the
    // descent. A schema is appended only after its components were (post-order), so
    // finding it in `out` means its whole closure is there too and there is nothing
    // left to walk under it. Descending first and scanning afterwards deduplicated the
    // OUTPUT but not the WORK: a chain of schemas each holding two fields of the
    // previous one was expanded as a binary tree — 2^depth visits for depth+1 distinct
    // schemas, which stalled a host's registration on a 27-schema declaration. `out`
    // is the visited set, across the components of one root and across repeated calls.
    //
    // Identity, not name: the same definition reached twice is carried once, and a
    // DIFFERENT definition under a name already carried is kept beside it for the
    // agreement check downstream to refuse — never dropped on the strength of its name.
    for (const auto& seen : out) {
        if (same_identity(*seen, *type.message)) {
            return;
        }
    }
    collect_referenced(*type.message, out); // components first (post-order)
    out.push_back(type.message);
}

} // namespace loom
