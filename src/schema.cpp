#include <zen/schema.hpp>

#include "detail/hash.hpp"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace zen {
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

} // namespace zen
