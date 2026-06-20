#include <zen/isolation/grant_record.hpp>

#include <zen/admission.hpp>
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

namespace zen::isolation {

namespace {

// The persisted shape: a list of {content_hash, network, filesystem} entries. A
// gated Value like everything else — the record funnels through the same admit().
std::shared_ptr<const zen::Schema> grant_entry_schema() {
    static const auto s = zen::SchemaBuilder("zen.GrantEntry", 1)
                              .field("content_hash", zen::Kind::Text)
                              .field("network", zen::Kind::Bool)
                              .field("filesystem", zen::Kind::Text)
                              .build();
    return s;
}

std::shared_ptr<const zen::Schema> grant_record_schema() {
    static const auto s = zen::SchemaBuilder("zen.GrantRecord", 1)
                              .list("entries", zen::type_message(grant_entry_schema()))
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

} // namespace

std::string so_content_hash(const std::string& so_path) {
    std::string bytes;
    try {
        bytes = read_file(so_path);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("so_content_hash: ") + e.what());
    }
    // FNV-1a, 64-bit — deterministic across runs and machines. This is a separate id
    // space from zen-core's schema content-id (no relation intended); it identifies a
    // .so build and nothing more.
    std::uint64_t h = 1469598103934665603ULL;
    for (char ch : bytes) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        h *= 1099511628211ULL;
    }
    static const char* const hex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = hex[h & 0xFU];
        h >>= 4;
    }
    return out;
}

void GrantRecord::load(std::string path) {
    path_ = std::move(path);
    deltas_.clear();
    std::string bytes;
    try {
        bytes = read_file(path_);
    } catch (const std::exception&) {
        return; // a missing file is an empty record: every Shard floors
    }
    if (bytes.empty()) {
        return; // an empty file is also an empty record (e.g. a freshly-touched path)
    }
    zen::Unverified u = zen::compat::parse(bytes);
    zen::Admission a = zen::admit(u, grant_record_schema());
    if (!a.ok()) {
        throw std::runtime_error("grant record refused: " + a.first_error().message());
    }
    for (const zen::Cell& c : a.value().get("entries")->as_list()) {
        const zen::Value& e = *c.as_message();
        GrantDelta d;
        d.network = e.get("network")->as_bool();
        d.filesystem = e.get("filesystem")->as_text();
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
    zen::Value v(grant_record_schema());
    std::vector<zen::Cell> entries;
    entries.reserve(deltas_.size());
    for (const auto& [hash, d] : deltas_) {
        zen::Value e(grant_entry_schema());
        e.set("content_hash", zen::Cell::text(hash));
        e.set("network", zen::Cell::boolean(d.network));
        e.set("filesystem", zen::Cell::text(d.filesystem));
        entries.push_back(zen::Cell::message(std::move(e)));
    }
    v.set("entries", zen::Cell::list(std::move(entries)));
    const std::string json = zen::compat::serialize(v);

    // Write to a temp file then atomically rename into place. The record is TCB data
    // the host's startup depends on: a partial or failed write (ENOSPC, crash) must
    // never corrupt the live ledger — a corrupt ledger would throw on the next load()
    // and brick the host. rename(2) is atomic within a filesystem; the stream is
    // checked so a silent failure surfaces rather than losing a recorded delta.
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("grant record: cannot write '" + tmp + "'");
        }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.flush();
        if (!out.good()) {
            throw std::runtime_error("grant record: failed writing '" + tmp + "'");
        }
    }
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        (void)std::remove(tmp.c_str());
        throw std::runtime_error("grant record: cannot replace '" + path_ + "'");
    }
}

} // namespace zen::isolation
