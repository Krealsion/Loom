#ifndef ZEN_ISOLATION_GRANT_RECORD_HPP
#define ZEN_ISOLATION_GRANT_RECORD_HPP

// The first *policy* surface: where a real mod's authority above the floor comes
// from. A mod lands on the floor with no ceremony; it may *ask* for more (the
// manifest's CapabilityAsk — advice, gated, untrusted); and the host alone decides,
// recording any grant *delta* here. This is the powerbox's ledger: the host holds
// the pen. A declaration is never a grant — only a delta recorded here raises a
// Shard above the floor.
//
// The record is keyed by the .so *content-hash* (stable across path moves; a rebuild
// is a new identity, so a recompiled mod re-floors — the honest default). It is TCB
// data: only the host writes it, never a Shard. It persists as a gated Value in
// Zen's JSON (inspectable, editable per-install), funnelled through the same gate as
// any other value.

#include <zen/switchboard/grant.hpp> // (re-exported vocabulary; FsAccess names)

#include <cstdint>
#include <map>
#include <string>

namespace zen::isolation {

// The storage protocol the floor pre-wires every mod to reach. The StorageBroker
// (Part B) registers under kStorageRole and accepts these exact shapes; pinning the
// names here keeps the floor least-privilege — storage shapes to the storage role,
// not "any shape to anyone".
inline constexpr const char* kStorageRole = "storage";
inline constexpr const char* kStoragePut = "StoragePut";
inline constexpr const char* kStorageGet = "StorageGet";
inline constexpr std::uint32_t kStorageProtocolVersion = 1;

/// A capability *delta* the host has granted a specific Shard above the floor. Only
/// the dimensions a delta can raise today are represented; an empty delta is the
/// floor (no change).
struct GrantDelta {
    bool network = false;   ///< grant os_cap::Network
    std::string filesystem; ///< an FsAccess level name to grant (e.g. "read-only"); "" = no change
};

/// The persisted, per-install grant ledger: content-hash -> delta. A missing file
/// (or no path) is an empty record — every Shard floors. Only the host mutates it.
class GrantRecord {
public:
    GrantRecord() = default;

    /// Point the record at a JSON file and load it (a missing file is an empty
    /// record). Subsequent record() calls persist back to this path. Throws if the
    /// file exists but is not a well-formed, conforming grant record.
    void load(std::string path);

    /// The delta recorded for `content_hash`, or an empty delta (the floor) if none.
    GrantDelta lookup(const std::string& content_hash) const;

    /// Record (replace) a delta for `content_hash` and persist immediately. This is
    /// the host's pen — the stand-in for the consent UX (deferred to a later phase).
    void record(const std::string& content_hash, GrantDelta delta);

private:
    std::string path_; ///< empty = in-memory only (persist is a no-op)
    std::map<std::string, GrantDelta> deltas_;
    void persist() const;
};

/// A deterministic content-hash (FNV-1a, 64-bit, lowercase hex) of the file at
/// `so_path`. Stable across runs and machines, so a persisted key survives a host
/// restart (unlike std::hash). Throws std::runtime_error if the file cannot be read.
/// This is identity, not authentication: it names a build, it does not vouch for it.
std::string so_content_hash(const std::string& so_path);

} // namespace zen::isolation

#endif // ZEN_ISOLATION_GRANT_RECORD_HPP
