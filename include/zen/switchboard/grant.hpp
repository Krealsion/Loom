// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_GRANT_HPP
#define ZEN_SWITCHBOARD_GRANT_HPP

// The capability grant: what a Weave may do. The bus checks every Weave-originated send against
// it; it is nearly empty by default, attached by the host at admission, and no Weave can widen
// its own. One grant is projected onto whatever boundaries the hosting mode provides, tiers B1
// to B5: messages, process, OS capability, filesystem view and resources
// (docs/reference/capabilities.md#hosting-and-enforcement-tiers).
//
// The projections answer at different moments (GATE-05):
//
//   SendRule       read at every delivery
//   ObserveRule    read at every observation
//   os_cap         read once, at IsolationHost::mount, to choose the child's network namespace
//   FsAccess       read once, to build the child's mount-namespace view
//   ResourceLimits read once, to write the child's cgroup leaf
//
// The first two are the stored value, so changing them changes enforcement. The last three are
// consumed into kernel state no later write can move, so only the first two form a
// `LiveAuthority`, the half the delegation door can change.

#include <zen/switchboard/message.hpp> // WeaveId

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loom {

/// Hard, binary OS-capability flags: enforced or refused. `Network` is enforced out of process
/// (tier B3, a network namespace with no interface); `SpawnProcess` is declared and nothing
/// enforces it. Filesystem reach is `FsAccess`, below.
namespace os_cap {
inline constexpr std::uint32_t None = 0;
inline constexpr std::uint32_t Network = 1u << 0;
inline constexpr std::uint32_t SpawnProcess = 1u << 1;
} // namespace os_cap

/// A graduated capability: a level on a safe-to-dangerous axis whose default is the safe end,
/// so a forgotten grant fails to the floor. Filesystem reach is graduated: none, read-only,
/// write to a scoped directory, write without the exec bit, write anywhere. All five are
/// enforced out of process (tier B4) by a private mount namespace built from them
/// (`build_view_plan`, `src/isolation/host.cpp`).
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

/// One observe rule: may read the latest claim of matching shapes (SENSE-05). Not a `SendRule`:
/// one says "may you send this there", the other "may you pull this". It selects by shape only,
/// with no author or office selector.
struct ObserveRule {
    bool any_shape = false;
    std::string shape_name; ///< used iff !any_shape
    std::uint32_t shape_version = 0;
};

/// Resource limits, a quantitative capability (tier B5, a cgroup-v2 leaf). `0` means the host's
/// conservative default; a positive value is an explicit raise. `unlimited_memory` lifts only
/// the memory cap: pids stay bounded and cpu stays a fair-share weight.
struct ResourceLimits {
    std::int64_t memory_bytes = 0;  ///< 0 = conservative default; >0 = explicit cap
    std::int64_t pids = 0;          ///< 0 = conservative default; >0 = explicit max (fork-bomb stop)
    std::int64_t cpu_weight = 0;    ///< 0 = default share (100); else 1..10000 (cgroup cpu.weight)
    bool unlimited_memory = false;  ///< opt out of the MEMORY cap only; pids stays bounded
};

/// The half of an authority that answers at the moment of use (GATE-05): send and observe rules,
/// and nothing else. Each is read off the live record when needed, so replacing this value
/// changes enforcement at once. The containment fields of a `Grant` are not here: they were
/// consumed into kernel state when the child was spawned, and this type has no word for them.
/// A value, owning nothing a subject's lifetime affects.
/// docs/reference/capabilities.md#live-delegation
class LiveAuthority {
public:
    LiveAuthority() = default;

    /// The empty authority, which permits nothing: what a subject holds until an administrator
    /// installs something, and what installing it again means (revoke).
    static LiveAuthority nothing() { return LiveAuthority{}; }

    /// May send shape (name, version) to a specific target.
    LiveAuthority& allow(std::string shape_name, std::uint32_t shape_version, WeaveId target) {
        rules_.push_back(SendRule{false, std::move(shape_name), shape_version, false, target});
        return *this;
    }
    /// May send shape (name, version) to any accepter.
    LiveAuthority& allow_to_any(std::string shape_name, std::uint32_t shape_version) {
        rules_.push_back(SendRule{false, std::move(shape_name), shape_version, true, WeaveId{}});
        return *this;
    }
    /// May send any shape to a specific target.
    LiveAuthority& allow_any_to(WeaveId target) {
        rules_.push_back(SendRule{true, std::string{}, 0, false, target});
        return *this;
    }
    /// May send any shape to any target (permissive).
    LiveAuthority& allow_any() {
        rules_.push_back(SendRule{true, std::string{}, 0, true, WeaveId{}});
        return *this;
    }
    /// May send shape (name, version) to whichever Weave currently holds `role`.
    LiveAuthority& allow_to_role(std::string shape_name, std::uint32_t shape_version,
                                 std::string role) {
        rules_.push_back(SendRule{false, std::move(shape_name), shape_version, false, WeaveId{},
                                  std::move(role)});
        return *this;
    }
    /// May read the latest claim of shape (name, version).
    LiveAuthority& allow_observe(std::string shape_name, std::uint32_t shape_version) {
        observe_.push_back(ObserveRule{false, std::move(shape_name), shape_version});
        return *this;
    }
    /// May read the latest claim of ANY shape.
    LiveAuthority& allow_observe_any() {
        observe_.push_back(ObserveRule{true, std::string{}, 0});
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

    /// True iff some rule permits sending the shape to the holder of `role`: only a role rule
    /// for that role, or an any-target rule. A WeaveId rule never authorizes a role-addressed
    /// send, and a role rule never a direct one.
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

    /// True iff some rule permits observing the latest claim of this shape.
    bool permits_observe(std::string_view shape_name, std::uint32_t shape_version) const {
        for (const ObserveRule& r : observe_) {
            if (r.any_shape || (r.shape_name == shape_name && r.shape_version == shape_version)) {
                return true;
            }
        }
        return false;
    }

    /// Attenuation: is `inner` entirely within this authority? Semantic, not textual. A rule is
    /// a rectangle of shape selector by target selector, each "any" or one exact atom:
    ///
    ///   shape   Any contains Exact(name, version)
    ///   target  Any contains ExactId(T), and Any contains Role(R)
    ///           ExactId(T) and Role(R) contain one another in neither direction
    ///
    /// The last line matters: a role rule follows whoever holds the office at delivery and an id
    /// rule follows one weave, so relating them would turn today's routing into permanent
    /// authority. Send and observe rules are checked in their own dimensions and never cross.
    /// A rule is covered when one rule here contains it; where that is ever conservative it
    /// refuses a legal delegation, never admits an illegal one. The empty authority is contained
    /// by everything, so revocation is always possible.
    bool contains(const LiveAuthority& inner) const {
        for (const SendRule& want : inner.rules_) {
            bool covered = false;
            for (const SendRule& have : rules_) {
                if (shape_contains(have, want) && target_contains(have, want)) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                return false;
            }
        }
        for (const ObserveRule& want : inner.observe_) {
            bool covered = false;
            for (const ObserveRule& have : observe_) {
                if (observe_contains(have, want)) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                return false;
            }
        }
        return true;
    }

    /// Does this authority say anything? An empty one permits nothing.
    bool empty() const noexcept { return rules_.empty() && observe_.empty(); }

    const std::vector<SendRule>& rules() const noexcept { return rules_; }
    const std::vector<ObserveRule>& observe_rules() const noexcept { return observe_; }

private:
    static bool shape_contains(const SendRule& outer, const SendRule& inner) {
        if (outer.any_shape) {
            return true;
        }
        if (inner.any_shape) {
            return false;
        }
        return outer.shape_name == inner.shape_name && outer.shape_version == inner.shape_version;
    }
    static bool target_contains(const SendRule& outer, const SendRule& inner) {
        if (outer.any_target) {
            return true;
        }
        if (inner.any_target) {
            return false;
        }
        const bool outer_is_role = !outer.target_role.empty();
        const bool inner_is_role = !inner.target_role.empty();
        if (outer_is_role != inner_is_role) {
            return false; // an office and a weave are different kinds of destination
        }
        return outer_is_role ? outer.target_role == inner.target_role
                             : outer.target == inner.target;
    }
    static bool observe_contains(const ObserveRule& outer, const ObserveRule& inner) {
        if (outer.any_shape) {
            return true;
        }
        if (inner.any_shape) {
            return false;
        }
        return outer.shape_name == inner.shape_name && outer.shape_version == inner.shape_version;
    }

    std::vector<SendRule> rules_;
    std::vector<ObserveRule> observe_;
};

/// What a Weave may do: the admission envelope a host names when it admits a subject,
/// containment included. Default-constructed it is empty, sending nothing and holding no OS
/// capability. Its live half is a `LiveAuthority`.
class Grant {
public:
    Grant() = default;

    static Grant nothing() { return Grant{}; }

    /// May send shape (name, version) to a specific target.
    Grant& allow(std::string shape_name, std::uint32_t shape_version, WeaveId target) {
        live_.allow(std::move(shape_name), shape_version, target);
        return *this;
    }
    /// May send shape (name, version) to any accepter.
    Grant& allow_to_any(std::string shape_name, std::uint32_t shape_version) {
        live_.allow_to_any(std::move(shape_name), shape_version);
        return *this;
    }
    /// May send any shape to a specific target.
    Grant& allow_any_to(WeaveId target) {
        live_.allow_any_to(target);
        return *this;
    }
    /// May send any shape to any target (permissive).
    Grant& allow_any() {
        live_.allow_any();
        return *this;
    }
    /// May send shape (name, version) to whichever Weave holds `role`, resolved at delivery, so
    /// the rule survives the holder reloading. It authorizes only role-addressed sends.
    Grant& allow_to_role(std::string shape_name, std::uint32_t shape_version, std::string role) {
        live_.allow_to_role(std::move(shape_name), shape_version, std::move(role));
        return *this;
    }
    /// May read the latest claim of shape (name, version), from any claimant or office
    /// (SENSE-05). Absent by default like every authority here, so reading Senses takes a
    /// deliberate host decision, as sending does.
    Grant& allow_observe(std::string shape_name, std::uint32_t shape_version) {
        live_.allow_observe(std::move(shape_name), shape_version);
        return *this;
    }
    /// May read the latest claim of any shape: for an inspector, a renderer, a console.
    Grant& allow_observe_any() {
        live_.allow_observe_any();
        return *this;
    }
    /// Record OS-capability flags, enforced out of process (tier B3); the in-process Switchboard
    /// does not consult them.
    Grant& with_os_capabilities(std::uint32_t caps) {
        os_ |= caps;
        return *this;
    }
    /// Set the graduated filesystem-access level, enforced out of process (tier B4); the default
    /// is `FsAccess::None`. `scoped_path` is the host tree `ReadOnly` exposes; other levels
    /// ignore it.
    Grant& with_filesystem(FsAccess level, std::string scoped_path = "") {
        fs_ = level;
        fs_path_ = std::move(scoped_path);
        return *this;
    }
    /// Raise the Weave's resource limits (tier B5). A `0` field keeps the host's conservative
    /// default.
    Grant& with_resources(ResourceLimits limits) {
        res_ = limits;
        return *this;
    }
    /// Opt out of the memory cap only. pids stays bounded and cpu stays a fair-share weight.
    Grant& with_unlimited_memory() {
        res_.unlimited_memory = true;
        return *this;
    }

    /// True iff some rule permits sending shape (name, version) to `target`.
    bool permits(std::string_view shape_name, std::uint32_t shape_version, WeaveId target) const {
        return live_.permits(shape_name, shape_version, target);
    }

    /// True iff some rule permits sending the shape to the holder of `role`: only a role rule
    /// for that role, or an any-target rule (see LiveAuthority::permits_role).
    bool permits_role(std::string_view shape_name, std::uint32_t shape_version,
                      std::string_view role) const {
        return live_.permits_role(shape_name, shape_version, role);
    }

    /// True iff some rule permits observing the latest claim of this shape (SENSE-05); send
    /// rules are never consulted.
    bool permits_observe(std::string_view shape_name, std::uint32_t shape_version) const {
        return live_.permits_observe(shape_name, shape_version);
    }

    /// The baseline live authority this admission established (GATE-05): what a delegated overlay
    /// is measured against and added to. Read-only: it stays what the host said at
    /// `register_weave` for the subject's life.
    const LiveAuthority& live() const noexcept { return live_; }

    std::uint32_t os_capabilities() const noexcept { return os_; }
    bool has_os_capability(std::uint32_t cap) const noexcept {
        return cap != 0 && (os_ & cap) == cap;
    }
    FsAccess filesystem() const noexcept { return fs_; }
    const std::string& filesystem_path() const noexcept { return fs_path_; }
    const ResourceLimits& resources() const noexcept { return res_; }
    const std::vector<SendRule>& rules() const noexcept { return live_.rules(); }
    const std::vector<ObserveRule>& observe_rules() const noexcept {
        return live_.observe_rules();
    }

private:
    /// Speech and observation: the rules read at every delivery and observation.
    LiveAuthority live_;
    /// The containment policy, consumed once, out of process, into namespace, mount view and
    /// cgroup state this process cannot revisit, so the delegation door cannot reach it.
    std::uint32_t os_ = 0;
    FsAccess fs_ = FsAccess::None; // safe default
    std::string fs_path_;          // the tree ReadOnly exposes (empty otherwise)
    ResourceLimits res_;           // bounded-by-default resource limits
};

/// Effective authority, the one answer to "may this be said?": baseline union delegated. Free
/// functions called by delivery and by capability-scoped inspection alike. A union can only add,
/// so no administrator can take a subject's baseline away.
inline bool effective_permits(const LiveAuthority& base, const LiveAuthority& delegated,
                              std::string_view shape_name, std::uint32_t shape_version,
                              WeaveId target) {
    return base.permits(shape_name, shape_version, target) ||
           delegated.permits(shape_name, shape_version, target);
}
inline bool effective_permits_role(const LiveAuthority& base, const LiveAuthority& delegated,
                                   std::string_view shape_name, std::uint32_t shape_version,
                                   std::string_view role) {
    return base.permits_role(shape_name, shape_version, role) ||
           delegated.permits_role(shape_name, shape_version, role);
}
inline bool effective_permits_observe(const LiveAuthority& base, const LiveAuthority& delegated,
                                      std::string_view shape_name, std::uint32_t shape_version) {
    return base.permits_observe(shape_name, shape_version) ||
           delegated.permits_observe(shape_name, shape_version);
}

/// The right to administer one subject's delegated live authority (GATE-05), so an
/// administrator weave can exist without being a host. It carries three facts together:
///
///   board    the Loom that minted it, weakly, so it expires with that Loom
///   subject  the one WeaveId it may administer: in the capability, not the call, so there is
///            no argument to point at another weave; WeaveIds are never reused
///   ceiling  the most it may ever install, named by the host; not the holder's own grant,
///            since what a Weaver may say and what it may hand out are different questions
///
/// A default-constructed one is inert, so an administrator can hold one as a member before the
/// host hands it anything; using it fails visibly.
/// docs/reference/capabilities.md#live-delegation
class GrantAuthority {
public:
    GrantAuthority() = default;
    GrantAuthority(const GrantAuthority&) = default;
    GrantAuthority& operator=(const GrantAuthority&) = default;
    GrantAuthority(GrantAuthority&&) = default;
    GrantAuthority& operator=(GrantAuthority&&) = default;

    /// Does this name a board and a subject? False for a default one. It does not promise the
    /// board is alive or the subject still mounted; only the issuing Switchboard says that, at
    /// the moment of use.
    bool valid() const noexcept { return subject_.valid(); }

    /// The one governed subject.
    WeaveId subject() const noexcept { return subject_; }

    /// The most this capability may ever install, readable so a Weaver can see the boundary
    /// before a request meets it.
    const LiveAuthority& ceiling() const noexcept { return ceiling_; }

private:
    friend class Switchboard;
    GrantAuthority(std::weak_ptr<const LoomIdentity> issuer, WeaveId subject, LiveAuthority ceiling)
        : issuer_(std::move(issuer)), subject_(subject), ceiling_(std::move(ceiling)) {}

    /// Weak, as LifecycleAuthority's is: it does not keep its board alive, and one from a dead
    /// board never validates against a later board at the same address.
    std::weak_ptr<const LoomIdentity> issuer_;
    WeaveId subject_{};
    LiveAuthority ceiling_;
};

/// Why an administration attempt did or did not take effect (GATE-05): each outcome sends an
/// operator somewhere different.
enum class GrantOutcome : std::uint8_t {
    Installed,      ///< the delegated authority is now exactly what was requested
    NoAuthority,    ///< a default-constructed / inert capability: it names no subject
    ForeignBoard,   ///< minted by another Loom, or by one that has since died
    NoSuchSubject,  ///< the governed subject is not mounted here (any more)
    ExceedsCeiling, ///< the request is not a semantic subset of the ceiling
    NoLiveDelivery, ///< this Bus is not a live participating context and never had standing
};

const char* name_of(GrantOutcome outcome) noexcept;

/// The result of one administration act. On any outcome but `Installed` nothing changed and
/// `installed == previous`.
struct GrantChange {
    GrantOutcome outcome = GrantOutcome::NoLiveDelivery;
    WeaveId subject{};          ///< which governed subject; from the capability, never a parameter
    LiveAuthority previous;     ///< what the delegated overlay was before this call
    LiveAuthority installed;    ///< what it is now
    explicit operator bool() const noexcept { return outcome == GrantOutcome::Installed; }
};

/// What a subject may do, as the bus will decide it (GATE-05): only the one subject the
/// capability governs, and only its message authority, never the registry, another subject or
/// the containment policy. `permits*` call the same predicates delivery does, over copies of the
/// same two values. Held by value, so it stays readable across the change it is used around.
struct AuthorityView {
    /// False when the capability was inert, foreign, or names a subject that is
    /// gone — in which case every field below is empty rather than misleading.
    bool available = false;
    WeaveId subject{};
    /// The live half of what the host attached at admission. Immutable.
    LiveAuthority base;
    /// What an administrator has installed since. Replaceable, within the ceiling.
    LiveAuthority delegated;

    bool permits(std::string_view shape_name, std::uint32_t shape_version, WeaveId target) const {
        return effective_permits(base, delegated, shape_name, shape_version, target);
    }
    bool permits_role(std::string_view shape_name, std::uint32_t shape_version,
                      std::string_view role) const {
        return effective_permits_role(base, delegated, shape_name, shape_version, role);
    }
    bool permits_observe(std::string_view shape_name, std::uint32_t shape_version) const {
        return effective_permits_observe(base, delegated, shape_name, shape_version);
    }
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_GRANT_HPP
