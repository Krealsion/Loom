#ifndef ZEN_SWITCHBOARD_GRANT_HPP
#define ZEN_SWITCHBOARD_GRANT_HPP

// The capability grant: what a Shard may do. It is the authority the bus checks
// every Shard-originated send against, default nearly empty. Grants flow from the
// host (the root of trust) at mount time, out-of-band — there is no in-band path
// by which a Shard widens its own grant.
//
// One grant is the single source of truth, projected onto whatever boundary the
// hosting mode provides: in B1 the *message* boundary (send-permissions, enforced
// here); in B2 the *syscall* boundary (the OS-capability flags, enforced by an
// out-of-process sandbox).

#include <zen/switchboard/message.hpp> // ShardId

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zen::sb {

/// OS-capability flags: DECLARED in B1, ENFORCED in B2. They govern
/// instruction-level behaviour a loaded .so can reach directly (open a socket, a
/// file, spawn a process), which only process isolation can stop — so B1 records
/// them on the grant and B2 projects them onto a sandbox profile with no rework.
/// Nothing in B1 consults them.
namespace os_cap {
inline constexpr std::uint32_t None = 0;
inline constexpr std::uint32_t Network = 1u << 0;
inline constexpr std::uint32_t FilesystemRead = 1u << 1;
inline constexpr std::uint32_t FilesystemWrite = 1u << 2;
inline constexpr std::uint32_t SpawnProcess = 1u << 3;
} // namespace os_cap

/// One send rule: may send shapes matching the shape-selector to targets matching
/// the target-selector. Each selector is a specific value or "any".
struct SendRule {
    bool any_shape = false;
    std::string shape_name; ///< used iff !any_shape
    std::uint32_t shape_version = 0;
    bool any_target = false;
    ShardId target{}; ///< used iff !any_target
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
    /// Record OS-capability flags (reserved for B2; not enforced in B1).
    Grant& with_os_capabilities(std::uint32_t caps) {
        os_ |= caps;
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
    const std::vector<SendRule>& rules() const noexcept { return rules_; }

private:
    std::vector<SendRule> rules_;
    std::uint32_t os_ = 0;
};

} // namespace zen::sb

#endif // ZEN_SWITCHBOARD_GRANT_HPP
