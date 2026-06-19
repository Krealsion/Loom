#ifndef ZEN_SWITCHBOARD_GRANT_HPP
#define ZEN_SWITCHBOARD_GRANT_HPP

// The capability grant: what a Shard may do. It is the authority the bus checks
// every Shard-originated send against, default nearly empty. Grants flow from the
// host (the root of trust) at mount time, out-of-band — there is no in-band path
// by which a Shard widens its own grant.
//
// One grant is the single source of truth, projected onto whatever boundary the
// hosting mode provides: in B1 the *message* boundary (send-permissions, enforced
// here); in B2 the *process* boundary (crash containment); in B3 the *syscall*
// boundary (the OS-capability flags, enforced by an out-of-process sandbox).

#include <zen/switchboard/message.hpp> // ShardId

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zen::sb {

/// *Hard* (binary) OS-capability flags — enforce-or-refuse, no middle. They govern
/// instruction-level behaviour a loaded .so can reach directly, which only process
/// isolation can stop: `Network` is enforced out-of-process in B3 (a no-interface
/// netns); `SpawnProcess` is reserved for a later phase. **Filesystem reach is NOT
/// here** — it is a *graduated* capability expressed by `FsAccess` (below), the
/// single source of truth for files; the old binary `FilesystemRead/Write` flags
/// were removed in B4 to avoid two competing representations.
namespace os_cap {
inline constexpr std::uint32_t None = 0;
inline constexpr std::uint32_t Network = 1u << 0;
inline constexpr std::uint32_t SpawnProcess = 1u << 1;
} // namespace os_cap

/// A *graduated* capability carries a level along a safe→dangerous axis, and its
/// default is the safe end — a forgotten grant fails to the floor, never to the
/// dangerous reach. Network and SpawnProcess are *hard* (binary: enforce-or-refuse,
/// above). Filesystem is graduated: enforcement (B4+) is none → read-only →
/// write-to-a-scoped-dir → write-with-no-exec-bit → write-anywhere, each step a
/// more deliberate, louder choice. B3 builds the *vocabulary* only — no filesystem
/// enforcement exists yet; this gives that phase a home rather than a retrofit.
enum class FsAccess : std::uint8_t {
    None = 0,      ///< safe default: no filesystem reach
    ReadOnly,      ///< read within a scoped tree
    WriteScoped,   ///< write within a scoped directory
    WriteNoExec,   ///< write, but nothing written may carry the exec bit
    WriteAnywhere, ///< the dangerous end: unrestricted write
};

inline const char* fs_access_name(FsAccess level) noexcept {
    switch (level) {
        case FsAccess::None:
            return "none";
        case FsAccess::ReadOnly:
            return "read-only";
        case FsAccess::WriteScoped:
            return "write-scoped";
        case FsAccess::WriteNoExec:
            return "write-no-exec";
        case FsAccess::WriteAnywhere:
            return "write-anywhere";
    }
    return "?";
}

/// One send rule: may send shapes matching the shape-selector to targets matching
/// the target-selector. Each selector is a specific value or "any".
struct SendRule {
    bool any_shape = false;
    std::string shape_name; ///< used iff !any_shape
    std::uint32_t shape_version = 0;
    bool any_target = false;
    ShardId target{}; ///< used iff !any_target
};

/// Resource limits — a *quantitative* capability (B5, enforced via cgroup-v2). `0`
/// means "use the host-computed conservative default" (bounded so one Shard can't
/// starve the host); a positive value is an explicit raise. `unlimited` is the
/// opt-out (no limits, honestly reported "not resource-contained") — the analog of
/// `os_cap::Network` granted / `FsAccess::WriteAnywhere`.
struct ResourceLimits {
    std::int64_t memory_bytes = 0; ///< 0 = conservative default; >0 = explicit cap
    std::int64_t pids = 0;         ///< 0 = conservative default; >0 = explicit max (fork-bomb stop)
    std::int64_t cpu_weight = 0;   ///< 0 = default share (100); else 1..10000 (cgroup cpu.weight)
    bool unlimited = false;        ///< explicit opt-out: no resource containment
};

/// What a Shard may do. Default-constructed = empty: may send nothing, holds no
/// OS-capabilities. Minimal authority by default.
class Grant {
public:
    Grant() = default;

    static Grant nothing() { return Grant{}; }

    /// May send shape (name, version) to a specific target.
    Grant& allow(std::string shape_name, std::uint32_t shape_version, ShardId target) {
        rules_.push_back(SendRule{false, std::move(shape_name), shape_version, false, target});
        return *this;
    }
    /// May send shape (name, version) to any accepter.
    Grant& allow_to_any(std::string shape_name, std::uint32_t shape_version) {
        rules_.push_back(SendRule{false, std::move(shape_name), shape_version, true, ShardId{}});
        return *this;
    }
    /// May send any shape to a specific target.
    Grant& allow_any_to(ShardId target) {
        rules_.push_back(SendRule{true, std::string{}, 0, false, target});
        return *this;
    }
    /// May send any shape to any target (permissive).
    Grant& allow_any() {
        rules_.push_back(SendRule{true, std::string{}, 0, true, ShardId{}});
        return *this;
    }
    /// Record OS-capability flags (hard capabilities; Network enforced out-of-process
    /// in B3, the rest reserved for later phases). Not consulted in B1.
    Grant& with_os_capabilities(std::uint32_t caps) {
        os_ |= caps;
        return *this;
    }
    /// Set the graduated filesystem-access level (the single source of truth for
    /// files; enforced out-of-process in B4). Defaults to the safe end
    /// (`FsAccess::None`). `scoped_path` is the host tree that `ReadOnly` exposes
    /// read-only in the restricted view; it is ignored by the other levels.
    Grant& with_filesystem(FsAccess level, std::string scoped_path = "") {
        fs_ = level;
        fs_path_ = std::move(scoped_path);
        return *this;
    }
    /// Raise the Shard's resource limits (B5). Any `0` field keeps the host-computed
    /// conservative default; positive fields are explicit raises.
    Grant& with_resources(ResourceLimits limits) {
        res_ = limits;
        return *this;
    }
    /// The explicit resource opt-out: run with no limits ("not resource-contained").
    Grant& with_unlimited_resources() {
        res_.unlimited = true;
        return *this;
    }

    /// True iff some rule permits sending shape (name, version) to `target`.
    bool permits(std::string_view shape_name, std::uint32_t shape_version, ShardId target) const {
        for (const SendRule& r : rules_) {
            const bool shape_ok =
                r.any_shape || (r.shape_name == shape_name && r.shape_version == shape_version);
            const bool target_ok = r.any_target || r.target == target;
            if (shape_ok && target_ok) {
                return true;
            }
        }
        return false;
    }

    std::uint32_t os_capabilities() const noexcept { return os_; }
    bool has_os_capability(std::uint32_t cap) const noexcept {
        return cap != 0 && (os_ & cap) == cap;
    }
    FsAccess filesystem() const noexcept { return fs_; }
    const std::string& filesystem_path() const noexcept { return fs_path_; }
    const ResourceLimits& resources() const noexcept { return res_; }
    const std::vector<SendRule>& rules() const noexcept { return rules_; }

private:
    std::vector<SendRule> rules_;
    std::uint32_t os_ = 0;
    FsAccess fs_ = FsAccess::None; // safe default
    std::string fs_path_;          // the tree ReadOnly exposes (empty otherwise)
    ResourceLimits res_;           // bounded-by-default resource limits
};

} // namespace zen::sb

#endif // ZEN_SWITCHBOARD_GRANT_HPP
