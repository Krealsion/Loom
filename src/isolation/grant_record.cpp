// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/isolation/grant_record.hpp>

#include <zen/admission.hpp>
#include <zen/content_id.hpp>
#include <zen/schema.hpp>
#include <zen/serialize.hpp>
#include <zen/value.hpp>


#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

// Durable-write path (host-side, POSIX; this library is never built on Windows).
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h> // fchmod — enforce 0600 on the temp regardless of a pre-existing file
#include <unistd.h>

namespace loom {

namespace {

// The persisted shape: a list of {content_hash, network, filesystem, roles} entries, a gated
// Value admitted like everything else; `roles` are the broker roles a mod may reach beyond the
// floor's storage.
std::shared_ptr<const loom::Schema> grant_entry_schema() {
    static const auto s = loom::SchemaBuilder("zen.GrantEntry", 2)
                              .field("content_hash", loom::Kind::Text)
                              .field("network", loom::Kind::Bool)
                              .field("filesystem", loom::Kind::Text)
                              .list("roles", loom::type_of(loom::Kind::Text))
                              .build();
    return s;
}

std::shared_ptr<const loom::Schema> grant_record_schema() {
    static const auto s = loom::SchemaBuilder("zen.GrantRecord", 1)
                              .list("entries", loom::type_message(grant_entry_schema()))
                              .build();
    return s;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Write `data` to `path` and fsync it, so a later rename of `path` cannot become durable ahead
// of its contents. Throws (and removes the temp file) on any failure. POSIX only: std::ofstream
// gives no fd to fsync.
void write_file_synced(const std::string& path, const std::string& data) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        throw std::runtime_error("grant record: cannot write '" + path + "'");
    }
    // open(O_CREAT, 0600) ignores the mode when the temp already exists, and rename keeps the
    // source mode, so a pre-planted looser temp (a `0666` `.tmp`) would make this TCB record
    // world-writable. 0600 is enforced on the open fd, whatever file was there.
    if (::fchmod(fd, 0600) != 0) {
        (void)::close(fd);
        (void)std::remove(path.c_str());
        throw std::runtime_error("grant record: cannot set mode on '" + path + "'");
    }
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)::close(fd);
            (void)std::remove(path.c_str());
            throw std::runtime_error("grant record: failed writing '" + path + "'");
        }
        off += static_cast<std::size_t>(n);
    }
    int rc = 0;
    do {
        rc = ::fsync(fd);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        (void)::close(fd);
        (void)std::remove(path.c_str());
        throw std::runtime_error("grant record: cannot fsync '" + path + "'");
    }
    if (::close(fd) != 0) {
        (void)std::remove(path.c_str());
        throw std::runtime_error("grant record: cannot close '" + path + "'");
    }
}

// Best-effort fsync of the directory holding `path`, so the rename INTO it survives a
// crash. Best-effort by design: the file contents are already fsync'd, so a failure
// here cannot lose a recorded delta — it only weakens the rename's durability — so it
// does not throw and abort a grant the host has already decided to make.
void fsync_parent_dir(const std::string& path) {
    const auto slash = path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".")
                            : (slash == 0 ? std::string("/") : path.substr(0, slash));
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        return;
    }
    int rc = 0;
    do {
        rc = ::fsync(dfd);
    } while (rc != 0 && errno == EINTR);
    (void)rc;
    (void)::close(dfd);
}

} // namespace

std::string so_content_hash(const std::string& so_path) {
    // One implementation, in the core (`zen/content_id.hpp`), because the in-process admission
    // policy keys artifacts by exactly this identity too. The digest's reasoning (collision
    // resistance, not speed) is in that header; this name stays because the isolation ledger,
    // its tests and its documentation speak of a .so content hash.
    try {
        return loom::file_content_id(so_path);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("so_content_hash: ") + e.what());
    }
}

void GrantRecord::load(std::string path) {
    path_ = std::move(path);
    deltas_.clear();
    std::string bytes;
    try {
        bytes = read_file(path_);
    } catch (const std::exception&) {
        return; // a missing file is an empty record: every Weave floors
    }
    if (bytes.empty()) {
        return; // an empty file is also an empty record (e.g. a freshly-touched path)
    }
    loom::Unverified u = loom::compat::parse(bytes);
    loom::Admission a = loom::admit(u, grant_record_schema());
    if (!a.ok()) {
        throw std::runtime_error("grant record refused: " + a.first_error().message());
    }
    for (const loom::Cell& c : a.value().get("entries")->as_list()) {
        const loom::Value& e = *c.as_message();
        GrantDelta d;
        d.network = e.get("network")->as_bool();
        d.filesystem = e.get("filesystem")->as_text();
        for (const loom::Cell& role : e.get("roles")->as_list()) {
            d.roles.push_back(role.as_text());
        }
        deltas_[e.get("content_hash")->as_text()] = std::move(d);
    }
}

GrantDelta GrantRecord::lookup(const std::string& content_hash) const {
    auto it = deltas_.find(content_hash);
    return it == deltas_.end() ? GrantDelta{} : it->second;
}

void GrantRecord::record(const std::string& content_hash, GrantDelta delta) {
    deltas_[content_hash] = std::move(delta);
    persist();
}

void GrantRecord::persist() const {
    if (path_.empty()) {
        return; // in-memory only
    }
    loom::Value v(grant_record_schema());
    std::vector<loom::Cell> entries;
    entries.reserve(deltas_.size());
    for (const auto& [hash, d] : deltas_) {
        loom::Value e(grant_entry_schema());
        e.set("content_hash", loom::Cell::text(hash));
        e.set("network", loom::Cell::boolean(d.network));
        e.set("filesystem", loom::Cell::text(d.filesystem));
        std::vector<loom::Cell> roles;
        roles.reserve(d.roles.size());
        for (const std::string& role : d.roles) {
            roles.push_back(loom::Cell::text(role));
        }
        e.set("roles", loom::Cell::list(std::move(roles)));
        entries.push_back(loom::Cell::message(std::move(e)));
    }
    v.set("entries", loom::Cell::list(std::move(entries)));
    const std::string json = loom::compat::serialize(v);

    // Write a temp file, fsync its contents, rename it into place atomically, then fsync the
    // directory so the rename is durable. The host's startup depends on this TCB record, and a
    // torn write would make the next load() throw; without the content fsync a crash could make
    // the rename durable before the data, leaving the name pointing at torn bytes.
    const std::string tmp = path_ + ".tmp";
    write_file_synced(tmp, json);
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        (void)std::remove(tmp.c_str());
        throw std::runtime_error("grant record: cannot replace '" + path_ + "'");
    }
    fsync_parent_dir(path_);
}

} // namespace loom
