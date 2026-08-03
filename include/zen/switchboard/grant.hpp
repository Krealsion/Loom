// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_GRANT_HPP
#define ZEN_SWITCHBOARD_GRANT_HPP

// The capability grant: what a Weave may do. It is the authority the bus checks
// every Weave-originated send against, default nearly empty. Grants flow from the
// host (the root of trust) at mount time, out-of-band — there is no in-band path
// by which a Weave widens its own grant.
//
// One grant is the single source of truth, projected onto whatever boundary the
// hosting mode provides: in B1 the *message* boundary (send-permissions, enforced
// here); in B2 the *process* boundary (crash containment); in B3 the *syscall*
// boundary (the OS-capability flags, enforced by an out-of-process sandbox).

#include <zen/switchboard/message.hpp> // WeaveId

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loom {

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
    WeaveId target{};          ///< used iff !any_target and not a role rule
    std::string target_role{}; ///< non-empty iff a role rule (see Grant::allow_to_role)
};

/// One observe rule: may read the latest claim of shapes matching the selector
/// (R2E-0). Deliberately NOT a `SendRule`: a send rule answers "may you emit this
/// shape *there*", an observe rule answers "may you pull this shape". Reusing one
/// for the other would be convenient and misleading, and an operator reading a
/// refusal would go edit the wrong thing.
///
/// There is no author/office selector, on purpose: the SHAPE is what exposure is
/// about, and shape-scoped authority is exactly the granularity `allow_to_any`
/// already has for sending. Narrowing by claimant is a real future rule; it waits
/// for a consumer that needs it rather than being guessed at now.
struct ObserveRule {
    bool any_shape = false;
    std::string shape_name; ///< used iff !any_shape
    std::uint32_t shape_version = 0;
};

/// Resource limits — a *quantitative* capability (B5, enforced via cgroup-v2). `0`
/// means "use the host-computed conservative default" (bounded so one Weave can't
/// starve the host); a positive value is an explicit raise. `unlimited_memory` is the
/// only opt-out (a trusted compute Weave may use all RAM) and it removes the **memory**
/// cap ONLY: **pids stays bounded** (no grant can license a fork bomb) and cpu stays a
/// fair-share weight. There is no wholesale "no limits" opt-out.
struct ResourceLimits {
    std::int64_t memory_bytes = 0;  ///< 0 = conservative default; >0 = explicit cap
    std::int64_t pids = 0;          ///< 0 = conservative default; >0 = explicit max (fork-bomb stop)
    std::int64_t cpu_weight = 0;    ///< 0 = default share (100); else 1..10000 (cgroup cpu.weight)
    bool unlimited_memory = false;  ///< opt out of the MEMORY cap only; pids stays bounded
};

/// What a Weave may do. Default-constructed = empty: may send nothing, holds no
/// OS-capabilities. Minimal authority by default.
class Grant {
public:
    Grant() = default;

    static Grant nothing() { return Grant{}; }

    /// May send shape (name, version) to a specific target.
    Grant& allow(std::string shape_name, std::uint32_t shape_version, WeaveId target) {
        rules_.push_back(SendRule{false, std::move(shape_name), shape_version, false, target});
        return *this;
    }
    /// May send shape (name, version) to any accepter.
    Grant& allow_to_any(std::string shape_name, std::uint32_t shape_version) {
        rules_.push_back(SendRule{false, std::move(shape_name), shape_version, true, WeaveId{}});
        return *this;
    }
    /// May send any shape to a specific target.
    Grant& allow_any_to(WeaveId target) {
        rules_.push_back(SendRule{true, std::string{}, 0, false, target});
        return *this;
    }
    /// May send any shape to any target (permissive).
    Grant& allow_any() {
        rules_.push_back(SendRule{true, std::string{}, 0, true, WeaveId{}});
        return *this;
    }
    /// May send shape (name, version) to whichever Weave currently holds `role`.
    /// The send names a role, not a WeaveId; the role is resolved to its holder at
    /// delivery, so this rule survives the holder reloading (its WeaveId is stable).
    /// A role rule authorizes only role-targeted sends (see permits_role); it never
    /// authorizes a direct WeaveId send.
    Grant& allow_to_role(std::string shape_name, std::uint32_t shape_version, std::string role) {
        rules_.push_back(SendRule{false, std::move(shape_name), shape_version, false, WeaveId{},
                                  std::move(role)});
        return *this;
    }
    /// MAY READ THE LATEST CLAIM of shape (name, version), from any claimant and
    /// from any office (R2E-0). Default-absent, like every other authority here:
    /// no existing weave gains any reach from Senses existing, and a Sense
    /// repository is therefore not a universal data-exfiltration rail — reading
    /// it takes a deliberate host decision, out of band, exactly as sending does.
    Grant& allow_observe(std::string shape_name, std::uint32_t shape_version) {
        observe_.push_back(ObserveRule{false, std::move(shape_name), shape_version});
        return *this;
    }
    /// May read the latest claim of ANY shape (permissive — an inspector, a
    /// renderer, the host's own console).
    Grant& allow_observe_any() {
        observe_.push_back(ObserveRule{true, std::string{}, 0});
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
    /// Raise the Weave's resource limits (B5). Any `0` field keeps the host-computed
    /// conservative default; positive fields are explicit raises.
    Grant& with_resources(ResourceLimits limits) {
        res_ = limits;
        return *this;
    }
    /// Opt out of the **memory** cap only (a trusted compute Weave may use all RAM).
    /// pids stays bounded (no grant can license a fork bomb) and cpu stays a fair-share
    /// weight — there is no wholesale "no limits" opt-out.
    Grant& with_unlimited_memory() {
        res_.unlimited_memory = true;
        return *this;
    }

    /// True iff some rule permits sending shape (name, version) to `target`.
    bool permits(std::string_view shape_name, std::uint32_t shape_version, WeaveId target) const {
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

    /// True iff some rule permits sending shape (name, version) to a Weave holding
    /// `role`. Authorized only by a role rule for the same role (or an any-target
    /// rule); a plain WeaveId rule never authorizes a role-targeted send, just as a
    /// role rule never authorizes a direct WeaveId send (see permits).
    bool permits_role(std::string_view shape_name, std::uint32_t shape_version,
                      std::string_view role) const {
        for (const SendRule& r : rules_) {
            const bool shape_ok =
                r.any_shape || (r.shape_name == shape_name && r.shape_version == shape_version);
            const bool target_ok =
                r.any_target || (!r.target_role.empty() && r.target_role == role);
            if (shape_ok && target_ok) {
                return true;
            }
        }
        return false;
    }

    /// True iff some rule permits observing the latest claim of this shape
    /// (R2E-0). Send rules are never consulted: they answer a different question.
    bool permits_observe(std::string_view shape_name, std::uint32_t shape_version) const {
        for (const ObserveRule& r : observe_) {
            if (r.any_shape || (r.shape_name == shape_name && r.shape_version == shape_version)) {
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
    const std::vector<ObserveRule>& observe_rules() const noexcept { return observe_; }

private:
    std::vector<SendRule> rules_;
    std::vector<ObserveRule> observe_; ///< read authority for latest claims (R2E-0)
    std::uint32_t os_ = 0;
    FsAccess fs_ = FsAccess::None; // safe default
    std::string fs_path_;          // the tree ReadOnly exposes (empty otherwise)
    ResourceLimits res_;           // bounded-by-default resource limits
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_GRANT_HPP
