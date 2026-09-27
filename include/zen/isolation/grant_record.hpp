// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_ISOLATION_GRANT_RECORD_HPP
#define ZEN_ISOLATION_GRANT_RECORD_HPP

// Where a mod's authority above the floor comes from. A mod lands on the floor; it may ask for
// more (its manifest's CapabilityAsk: advice, untrusted), and the host alone decides, recording
// a grant delta here. Keyed by the .so content hash, so a rebuild is a new identity that starts
// on the floor. Only the host writes it; it persists as a gated Value in Zen's JSON, editable
// per install. docs/reference/capabilities.md#where-a-grant-comes-from-the-powerbox

#include <zen/switchboard/grant.hpp> // (re-exported vocabulary; FsAccess names)

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace loom {

// The storage protocol the floor pre-wires every mod to reach: the StorageBroker registers
// under kStorageRole and accepts these exact shapes, so the floor grants storage shapes to the
// storage role and not any shape to anyone.
inline constexpr const char* kStorageRole = "storage";
inline constexpr const char* kStoragePut = "StoragePut";
inline constexpr const char* kStorageGet = "StorageGet";
inline constexpr const char* kStorageValue = "StorageValue"; ///< the broker's reply shape
inline constexpr std::uint32_t kStorageProtocolVersion = 1;

// The network protocol the NetworkBroker, registered under role "net", accepts. The floor
// grants the net role to no mod; only a recorded grant delta does. A mod that reaches "net"
// holds the role send rule only, not os_cap::Network: it stays network-denied and reaches the
// network solely through the broker. The same request-response shape as storage.
inline constexpr const char* kNetRole = "net";
inline constexpr const char* kNetRequest = "NetRequest";
inline constexpr const char* kNetResponse = "NetResponse"; ///< the broker's reply shape
inline constexpr std::uint32_t kNetProtocolVersion = 1;

/// A capability delta the host has granted a specific weave above the floor, in the dimensions
/// a delta can raise; an empty delta is the floor (no change).
struct GrantDelta {
    bool network = false;             ///< grant os_cap::Network (the OS capability itself — rare)
    std::string filesystem{};         ///< an FsAccess level name (e.g. "read-only"); "" = none
    std::vector<std::string> roles{}; ///< broker roles a mod may reach beyond the floor's storage
};

/// The persisted, per-install grant ledger: content-hash -> delta. A missing file
/// (or no path) is an empty record — every Weave floors. Only the host mutates it.
class GrantRecord {
public:
    GrantRecord() = default;

    /// Point the record at a JSON file and load it (a missing file is an empty
    /// record). Subsequent record() calls persist back to this path. Throws if the
    /// file exists but is not a well-formed, conforming grant record.
    void load(std::string path);

    /// The delta recorded for `content_hash`, or an empty delta (the floor) if none.
    GrantDelta lookup(const std::string& content_hash) const;

    /// Record (replace) a delta for `content_hash` and persist it at once: the host's pen.
    void record(const std::string& content_hash, GrantDelta delta);

private:
    std::string path_; ///< empty = in-memory only (persist is a no-op)
    std::map<std::string, GrantDelta> deltas_;
    void persist() const;
};

/// A deterministic content hash of the file at `so_path`: SHA-256 truncated to 128 bits,
/// lowercase hex, stable across runs and machines, so a persisted key survives a restart. Throws
/// std::runtime_error if the file cannot be read. Cryptographic, because this key alone decides
/// a mod's authority above the floor; it names a build by its bytes and vouches for no author.
/// The same identity as `file_content_id` (zen/content_id.hpp).
std::string so_content_hash(const std::string& so_path);

} // namespace loom

#endif // ZEN_ISOLATION_GRANT_RECORD_HPP
