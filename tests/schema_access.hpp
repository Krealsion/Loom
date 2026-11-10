// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_SCHEMA_ACCESS_HPP
#define ZEN_TESTS_SCHEMA_ACCESS_HPP

// Two definitions of one (name, version) that share a content id but not a structure, made
// without a collision: the content id is given, not computed (`Schema`'s private constructor,
// include/zen/schema.hpp). For the cases that show a check does not trust the id.

#include <zen/schema.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace loom::detail {

struct SchemaAccess {
    static std::shared_ptr<const Schema> with_content_id(std::string name, std::uint32_t version,
                                                         std::vector<Field> fields,
                                                         ContentId content_id) {
        return std::shared_ptr<const Schema>(new Schema(Schema::GivenContentId{}, std::move(name),
                                                        version, std::move(fields), content_id));
    }
};

} // namespace loom::detail

namespace zen_test {

/// `name` v`version` with `fields`, carrying `like`'s content id.
inline std::shared_ptr<const loom::Schema> twin_of(const loom::Schema& like,
                                                   std::vector<loom::Field> fields) {
    return loom::detail::SchemaAccess::with_content_id(like.name(), like.version(),
                                                       std::move(fields), like.content_id());
}

} // namespace zen_test

#endif // ZEN_TESTS_SCHEMA_ACCESS_HPP
