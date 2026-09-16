// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/content_id.hpp>

#include "detail/sha256.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace loom {

std::string file_content_id(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("file_content_id: cannot open '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    // WHOLE-FILE, not a prefix and not a stat. Two builds of one source differ deep
    // inside; an identity that read only a header would answer "same artifact" for
    // code that is not the code a person approved.
    return loom::detail::sha256_hex_prefix(ss.str(), 16);
}

std::string file_content_id_or_empty(const std::string& path) noexcept {
    try {
        return file_content_id(path);
    } catch (...) {
        return {};
    }
}

} // namespace loom
