#include <zen/isolation/grant_record.hpp>

#include <zen/admission.hpp>
#include <zen/schema.hpp>
#include <zen/serialize.hpp>
#include <zen/value.hpp>

#include "detail/sha256.hpp" // the grant-key digest (audit F-1); no external crypto dep

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
#include <unistd.h>

namespace loom {

namespace {

// The persisted shape: a list of {content_hash, network, filesystem, roles} entries. A
// gated Value like everything else — the record funnels through the same admit(). v2
// adds `roles` (the broker roles a mod may reach beyond the floor's storage); bumping
// the version rather than mutating v1 keeps the project's frozen-(name,version) invariant.
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

// Write `data` to `path` and fsync it: the bytes are on stable storage before this
// returns, so a later rename of `path` cannot become durable ahead of its contents.
// Throws (and removes the temp file) on any failure. POSIX only — std::ofstream gives
// no fd to fsync, which is precisely why the durability claim went unbacked before.
void write_file_synced(const std::string& path, const std::string& data) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        throw std::runtime_error("grant record: cannot write '" + path + "'");
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
    std::string bytes;
    try {
        bytes = read_file(so_path);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("so_content_hash: ") + e.what());
    }
    // SHA-256 truncated to 128 bits (32 lowercase-hex chars) — deterministic across
    // runs and machines. This keys a security-relevant identity (an above-floor grant),
    // so it must be COLLISION-RESISTANT, not merely a fast name: FNV-1a (the prior key)
    // offered only ~2^32 birthday resistance, cheap for a determined attacker to forge a
    // second .so onto an existing grant (audit F-1). Truncated SHA-256 raises that to
    // ~2^128 second-preimage / ~2^64 collision. Still content-addressing, not
    // authentication — a *signed* author identity remains the identity phase's job.
    return loom::detail::sha256_hex_prefix(bytes, 16);
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

    // Write to a temp file, fsync its CONTENTS, atomically rename into place, then fsync
    // the directory so the rename itself is durable. The record is TCB data the host's
    // startup depends on: a partial, torn, or unsynced write (ENOSPC, power loss) must
    // never corrupt the live ledger — a corrupt ledger throws on the next load() and
    // bricks the host. rename(2) is atomic within a filesystem, but without the
    // content-fsync a crash could make the rename durable while the data blocks are not,
    // leaving the ledger's name pointing at empty/torn bytes. The two fsyncs close that
    // window — this is the durability the temp-file-then-rename dance exists to deliver.
    const std::string tmp = path_ + ".tmp";
    write_file_synced(tmp, json);
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        (void)std::remove(tmp.c_str());
        throw std::runtime_error("grant record: cannot replace '" + path_ + "'");
    }
    fsync_parent_dir(path_);
}

} // namespace loom
