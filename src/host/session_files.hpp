// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_SESSION_FILES_HPP
#define ZEN_HOST_SESSION_FILES_HPP

// Where a client finds a session. A host writes `session.key` (the owner's client key: whoever
// reads it acts as the owner) before it listens and reads it back; a key not its user's alone (on
// FAT or exFAT, never) is removed and nothing is served. Listening, it writes `session.json`
// (endpoint, lifetime id, pid, version, start time; not a secret, not authority), and removes both
// on a clean end. A host that dies leaves them describing a lifetime that is over.

#include "../detail/json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace loom::host {

inline constexpr const char* kSessionFile = "session.json";
inline constexpr const char* kSessionKeyFile = "session.key";

struct SessionFileFacts {
    std::string lifetime;
    std::string endpoint;
    std::int64_t pid = 0;
    std::string host;
    std::int64_t abi = 0;
    std::int64_t started_ms = 0;
    std::string directory;
};

/// Replace `path` with `text`, by way of a sibling temp file, so a reader sees the old file or
/// the new one. `owner_only` creates the temp file readable and writable by this process's user
/// alone from its first byte: mode 0600 on POSIX, a protected one-entry DACL on Windows. Then it
/// reads the file back where it stands, and one that does not read as this user's alone is
/// removed and refused, `why` naming the directory, the reason and where a session belongs.
bool write_session_text(const std::filesystem::path& path, const std::string& text,
                        bool owner_only, std::string* why);

/// Whether `file` reads back as this process's user's alone. POSIX: a plain file owned by the
/// effective user, mode 0600. Windows: on a volume that keeps permissions, owned by the process
/// user, with a protected DACL of one entry allowing that user. Otherwise false, `why` the
/// reason in a clause.
bool reads_as_owner_only(const std::filesystem::path& file, std::string* why);

inline std::string session_json(const SessionFileFacts& f) {
    std::string out = "{\n  \"lifetime\": ";
    loom::detail::json_quote(f.lifetime, out);
    out += ",\n  \"endpoint\": ";
    loom::detail::json_quote(f.endpoint, out);
    out += ",\n  \"pid\": " + std::to_string(f.pid);
    out += ",\n  \"host\": ";
    loom::detail::json_quote(f.host, out);
    out += ",\n  \"abi\": " + std::to_string(f.abi);
    out += ",\n  \"started_ms\": " + std::to_string(f.started_ms);
    out += ",\n  \"directory\": ";
    loom::detail::json_quote(f.directory, out);
    out += ",\n  \"key\": ";
    loom::detail::json_quote(kSessionKeyFile, out);
    out += "\n}\n";
    return out;
}

/// Remove both files -- a clean end. A missing file is not an error.
inline void remove_session_files(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove(dir / kSessionFile, ec);
    std::filesystem::remove(dir / kSessionKeyFile, ec);
}

} // namespace loom::host

#endif // ZEN_HOST_SESSION_FILES_HPP
