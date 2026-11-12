// SPDX-License-Identifier: AGPL-3.0-only
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
    case Kind::Union:
        if (!t.message) {
            throw std::invalid_argument("a nested shape's type requires its schema");
        }
        if (t.element) {
            throw std::invalid_argument("a nested shape's type must not carry a list element");
        }
        if (t.message->is_union() != (t.kind == Kind::Union)) {
            throw std::invalid_argument(t.kind == Kind::Union
                                            ? "a Union type names a union shape"
                                            : "a Message type names a record shape");
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
    case Kind::Union:
        h.u64(t.message->content_id());
        break;
    case Kind::List:
        hash_typeref(h, *t.element);
        break;
    default:
        break;
    }
}

// A record's id folds what it folded before unions existed, so every record keeps its id. A
// union folds its form first, and for each case whether it carries, then the type it carries.
ContentId compute_content_id(Form form, const std::string& name, std::uint32_t version,
                             const std::vector<Field>& fields) {
    detail::Fnv1a h;
    if (form == Form::Union) {
        h.field("union");
    }
    h.field(name);
    h.u64(version);
    h.u64(fields.size());
    for (const Field& f : fields) {
        h.field(f.name);
        h.byte(f.required ? 1U : 0U);
        if (form == Form::Union) {
            h.byte(f.carries ? 1U : 0U);
            if (!f.carries) {
                continue;
            }
        }
        hash_typeref(h, f.type);
    }
    return h.value();
}

} // namespace

TypeRef type_of(Kind k) {
    if (k == Kind::Message || k == Kind::Union || k == Kind::List) {
        throw std::invalid_argument("type_of is for primitive kinds; use type_message/type_list");
    }
    return TypeRef{k, nullptr, nullptr};
}

TypeRef type_message(std::shared_ptr<const Schema> schema) {
    if (!schema) {
        throw std::invalid_argument("type_message requires a non-null schema");
    }
    const Kind kind = schema->is_union() ? Kind::Union : Kind::Message;
    return TypeRef{kind, std::move(schema), nullptr};
}

TypeRef type_any() { return TypeRef{Kind::Any, nullptr, nullptr}; }

Field case_of(std::string name, TypeRef type) {
    return Field{std::move(name), std::move(type), /*required=*/true, /*carries=*/true};
}

Field case_bare(std::string name) {
    return Field{std::move(name), type_of(Kind::Bool), /*required=*/true, /*carries=*/false};
}

TypeRef type_list(TypeRef element) {
    return TypeRef{Kind::List, nullptr, std::make_shared<const TypeRef>(std::move(element))};
}

Schema::Schema(std::string name, std::uint32_t version, std::vector<Field> fields)
    : Schema(Form::Record, std::move(name), version, std::move(fields)) {}

Schema::Schema(Form form, std::string name, std::uint32_t version, std::vector<Field> fields)
    : name_(std::move(name)), version_(version), form_(form), fields_(std::move(fields)),
      content_id_(0) {
    if (form_ == Form::Union && fields_.empty()) {
        throw std::invalid_argument("union '" + name_ + "' declares no case");
    }
    std::unordered_set<std::string_view> seen;
    seen.reserve(fields_.size());
    for (const Field& f : fields_) {
        if (f.name.empty()) {
            throw std::invalid_argument("field name must not be empty");
        }
        if (!seen.insert(f.name).second) {
            throw std::invalid_argument("duplicate field name '" + f.name + "'");
        }
        if (form_ == Form::Record && !f.carries) {
            throw std::invalid_argument("field '" + f.name +
                                        "': only a union's case may carry nothing");
        }
        if (form_ == Form::Union && !f.required) {
            throw std::invalid_argument("case '" + f.name + "': a union's case is never optional");
        }
        validate_typeref(f.type);
    }
    content_id_ = compute_content_id(form_, name_, version_, fields_);
}

Schema::Schema(GivenContentId, std::string name, std::uint32_t version, std::vector<Field> fields,
               ContentId content_id)
    : Schema(std::move(name), version, std::move(fields)) {
    content_id_ = content_id;
}

namespace {

bool same_type(const TypeRef& a, const TypeRef& b) noexcept {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case Kind::Message:
    case Kind::Union:
        return same_identity(*a.message, *b.message);
    case Kind::List:
        return same_type(*a.element, *b.element);
    default:
        return true;
    }
}

} // namespace

bool same_structure(const Schema& a, const Schema& b) noexcept {
    if (a.form() != b.form()) {
        return false;
    }
    const std::vector<Field>& fa = a.fields();
    const std::vector<Field>& fb = b.fields();
    if (fa.size() != fb.size()) {
        return false;
    }
    for (std::size_t i = 0; i < fa.size(); ++i) {
        if (fa[i].name != fb[i].name || fa[i].required != fb[i].required ||
            fa[i].carries != fb[i].carries) {
            return false;
        }
        if (fa[i].carries && !same_type(fa[i].type, fb[i].type)) {
            return false;
        }
    }
    return true;
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
    return add(Field{std::move(name), type_of(kind), required, /*carries=*/true});
}

SchemaBuilder& SchemaBuilder::message(std::string name, std::shared_ptr<const Schema> schema,
                                      bool required) {
    return add(Field{std::move(name), type_message(std::move(schema)), required, /*carries=*/true});
}

SchemaBuilder& SchemaBuilder::list(std::string name, TypeRef element, bool required) {
    return add(Field{std::move(name), type_list(std::move(element)), required, /*carries=*/true});
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

std::shared_ptr<const Schema> make_union(std::string name, std::uint32_t version,
                                         std::vector<Field> cases) {
    return std::make_shared<const Schema>(Form::Union, std::move(name), version,
                                          std::move(cases));
}

// ---- the component closure ---------------------------------------------------

void collect_referenced(const Schema& root, std::vector<std::shared_ptr<const Schema>>& out) {
    for (const Field& f : root.fields()) {
        if (f.carries) {
            collect_referenced(f.type, out);
        }
    }
}

void collect_referenced(const TypeRef& type, std::vector<std::shared_ptr<const Schema>>& out) {
    if (type.kind == Kind::List && type.element) {
        collect_referenced(*type.element, out);
        return;
    }
    if ((type.kind != Kind::Message && type.kind != Kind::Union) || !type.message) {
        return; // an Any field names no shape: what it holds is the registry's
    }
    // Already carried means already expanded, so the identity scan comes before the descent:
    // a schema is appended only after its components (post-order), so finding it means its
    // closure is there. `out` is the visited set, so a chain of schemas each nesting two of the
    // previous is walked once per schema, not 2^depth times. Identity, not name: a different
    // definition under a carried name is kept beside it for the agreement check to refuse.
    for (const auto& seen : out) {
        if (same_identity(*seen, *type.message)) {
            return;
        }
    }
    collect_referenced(*type.message, out); // components first (post-order)
    out.push_back(type.message);
}

} // namespace loom
