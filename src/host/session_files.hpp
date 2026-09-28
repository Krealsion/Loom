// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_SESSION_FILES_HPP
#define ZEN_HOST_SESSION_FILES_HPP

// Where a client finds a session, and what it presents: a session host writes `session.json`
// (endpoint, lifetime id, pid, version, start time; not a secret and not authority) and
// `session.key` (the owner's client key; owner-only on POSIX, the directory's ACL on Windows;
// whoever reads it acts as the owner) once listening, and removes both on a clean end. A host
// that dies leaves them describing a lifetime that is over; the host never reads them back.

#include "../detail/json.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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
/// the new one. `owner_only` creates the temp file with mode 0600 on POSIX from its first byte.
inline bool write_session_text(const std::filesystem::path& path, const std::string& text,
                               bool owner_only, std::string* why) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
#ifndef _WIN32
    if (owner_only) {
        const int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd < 0) {
            *why = "cannot create " + tmp.string();
            return false;
        }
        std::size_t off = 0;
        while (off < text.size()) {
            const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
            if (n <= 0) {
                ::close(fd);
                *why = "cannot write " + tmp.string();
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        if (::close(fd) != 0) {
            *why = "cannot finish " + tmp.string();
            return false;
        }
    } else
#endif
    {
        (void)owner_only;
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            *why = "cannot create " + tmp.string();
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (out.fail()) {
            *why = "cannot write " + tmp.string();
            return false;
        }
    }
    // One writer: the host holds the directory's session lock for its whole life.
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        *why = "cannot put " + path.string() + " in place: " + ec.message();
        return false;
    }
    return true;
}

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
