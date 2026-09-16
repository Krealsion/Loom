// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include "config_file.hpp"

#include <zen/admission.hpp>
#include <zen/serialize.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

#ifdef _WIN32
// For MoveFileExA. The whole reason this file needs a platform branch at all is that
// `std::rename` REFUSES to replace an existing file on Windows, and the workaround the
// host shipped with — delete, then rename — is not a replacement: it has a window in
// which neither the old record nor the new one is on disk.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

namespace {

/// REPLACE `from` WITH `to` IN ONE OPERATION — the only kind of replacement that can
/// honestly be called atomic.
///
/// POSIX `rename(2)` replaces an existing destination as one step: a reader sees either
/// the old file or the new one, never neither. Windows `std::rename` refuses when the
/// destination exists, and the obvious workaround — `remove` then `rename` — is exactly
/// the thing this function exists NOT to do: between the two calls the last good record
/// is gone, and a process that dies there (or a rename that then fails) leaves a person
/// with no decisions at all. `MoveFileExA` with `MOVEFILE_REPLACE_EXISTING` is Windows'
/// own single-operation replace, so both platforms make the same promise by the same
/// shape rather than one of them faking it.
bool replace_file(const std::string& from, const std::string& to, std::string* error) {
#ifdef _WIN32
    if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0) {
        return true;
    }
    *error = "cannot replace '" + to + "' (Windows error " +
             std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
    return false;
#else
    if (std::rename(from.c_str(), to.c_str()) == 0) {
        return true;
    }
    *error = "cannot replace '" + to + "'";
    return false;
#endif
}

} // namespace

bool write_gated_file(const std::string& path, const loom::Value& value, std::string* error) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            *error = "cannot write '" + tmp + "'";
            return false;
        }
        out << loom::compat::serialize(value) << '\n';
        out.flush();
        if (!out) {
            *error = "failed writing '" + tmp + "'";
            return false;
        }
    }
    if (!replace_file(tmp, path, error)) {
        // The replacement did not happen, so the PREVIOUS record is still the record —
        // which is the promise. Take the half-written candidate away so a later reader
        // cannot mistake it for one, and leave the failure to the caller to report.
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace loom::host
