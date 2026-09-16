// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include "config_file.hpp"

#include <zen/admission.hpp>
#include <zen/serialize.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace loom::host {

namespace {

std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
        return {};
    }
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/// Does this text look like it already carries an envelope? Used only to decide WHICH
/// error to report when both readings fail, never to decide which reading to trust —
/// both are tried either way, so a misjudgement here costs a person a clearer sentence
/// and never a wrong answer.
bool looks_enveloped(const std::string& text) {
    const std::size_t head = text.find("\"zen\"");
    return head != std::string::npos && head < 64;
}

std::optional<loom::Value> admit_text(const std::string& text,
                                      const std::shared_ptr<const loom::Schema>& schema,
                                      std::string* why) {
    loom::Unverified u = loom::compat::parse(text);
    loom::Admission a = loom::admit(u, schema);
    if (!a.ok()) {
        *why = a.first_error().message();
        return std::nullopt;
    }
    return a.value();
}

} // namespace

std::optional<loom::Value> read_gated_file(const std::string& path,
                                           const std::shared_ptr<const loom::Schema>& schema,
                                           bool* missing, std::string* error) {
    *missing = false;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *missing = true;
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = trim(ss.str());
    if (text.empty()) {
        *missing = true; // an empty file says exactly as much as an absent one
        return std::nullopt;
    }

    // The bare form first, because it is the one a person writes and therefore the one
    // whose error is worth reporting by default.
    std::string bare_why;
    const std::string wrapped = "{\"zen\":1,\"schema\":\"" + schema->name() +
                                "\",\"version\":" + std::to_string(schema->version()) +
                                ",\"fields\":" + text + "}";
    if (auto v = admit_text(wrapped, schema, &bare_why)) {
        return v;
    }
    std::string env_why;
    if (auto v = admit_text(text, schema, &env_why)) {
        return v;
    }
    *error = "'" + path + "' refused: " + (looks_enveloped(text) ? env_why : bare_why);
    return std::nullopt;
}

bool write_gated_file(const std::string& path, const loom::Value& value, std::string* error) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            *error = "cannot write '" + tmp + "'";
            return false;
        }
        out << loom::compat::serialize(value) << '\n';
        if (!out) {
            *error = "failed writing '" + tmp + "'";
            return false;
        }
    }
    std::remove(path.c_str()); // Windows rename does not replace an existing file
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        *error = "cannot replace '" + path + "'";
        return false;
    }
    return true;
}

} // namespace loom::host
