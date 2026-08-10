// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_GRANT_HPP
#define ZEN_SWITCHBOARD_GRANT_HPP

// The capability grant: what a Weave may do. It is the authority the bus checks
// every Weave-originated send against, default nearly empty. A grant is attached
// by the host (the root of trust) at admission, and no Weave can widen its own.
//
// One grant is the single source of truth, projected onto whatever boundary the
// hosting mode provides: in B1 the *message* boundary (send-permissions, enforced
// here); in B2 the *process* boundary (crash containment); in B3 the *syscall*
// boundary (the OS-capability flags, enforced by an out-of-process sandbox).
//
// AND THE PROJECTIONS DO NOT ALL ANSWER AT THE SAME MOMENT (GRANT-0). That was
// always true and used not to matter, because nothing could change a grant after
// admission. It matters now:
//
//   SendRule       read at EVERY delivery, off the record the router just found
//   ObserveRule    read at EVERY observation, off the record the reader named
//   os_cap         read ONCE, at IsolationHost::mount, to choose the child's
//                  network namespace before it is spawned
//   FsAccess       read ONCE, to build the child's mount-namespace view, which
//                  is pivot_root'ed into and then detached
//   ResourceLimits read ONCE, to write the child's cgroup leaf
//
// The first two ARE the stored value: change it and enforcement changes with it.
// The last three are consumed into kernel state the host cannot revisit — a
// namespace already entered, a root already detached, a leaf already written. A
// field rewritten in this process's memory afterwards would move no kernel at
// all, so an API that let one be "changed live" would be describing a power that
// does not exist. Hence `LiveAuthority`, below: the half that answers at the
// moment of use, split out so the other half is not merely undocumented but
// UNSAYABLE through the delegation door.

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

/// THE HALF OF AN AUTHORITY THAT ANSWERS AT THE MOMENT OF USE (GRANT-0).
///
/// Send rules and observe rules, and deliberately NOTHING ELSE. Every rule here
/// is consulted off the live record each time it is needed — a send rule in
/// `deliver_one`, an observe rule in `observe_as` — so replacing this value
/// replaces real, immediately-effective enforcement. That is what makes it the
/// one authority a running subject's may be changed, and why the containment
/// fields of a `Grant` are not in it: they were consumed into kernel state at
/// spawn and a later write here would move nothing.
///
/// SO THE TYPE IS THE PROMISE. A delegation door that accepted a whole `Grant`
/// would have to *refuse* requests naming `os_cap::Network` — and a refusal is a
/// runtime check somebody can forget to write, or write and then delete. There
/// is no `with_os_capabilities` on this class, so the escalation is not refused;
/// it cannot be spelled. The strongest form of "you may not ask for that" is
/// having no word for it.
///
/// It is a value: copyable, movable, comparable-by-containment, and owning
/// nothing that a subject's lifetime affects.
class LiveAuthority {
public:
    LiveAuthority() = default;

    /// The empty authority — permits nothing. What a subject holds as delegated
    /// authority until an administrator installs something, and what installing
    /// it again means: revoke.
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

    /// True iff some rule permits observing the latest claim of this shape.
    bool permits_observe(std::string_view shape_name, std::uint32_t shape_version) const {
        for (const ObserveRule& r : observe_) {
            if (r.any_shape || (r.shape_name == shape_name && r.shape_version == shape_version)) {
                return true;
            }
        }
        return false;
    }

    /// ATTENUATION — is `inner` entirely within what this authority already says?
    ///
    /// The security-critical relation, and it is SEMANTIC rather than textual or
    /// countable. A rule denotes a rectangle (shape-selector × target-selector),
    /// and each selector is either "any" or one exact atom:
    ///
    ///   shape   Any  ⊒  Exact(name, version)
    ///   target  Any  ⊒  ExactId(T)      and      Any  ⊒  Role(R)
    ///           ExactId(T) and Role(R) are INCOMPARABLE — neither contains the
    ///           other, whichever weave happens to hold R right now.
    ///
    /// THAT LAST LINE IS THE ONE WITH TEETH. A role rule follows whoever holds the
    /// office at delivery; an id rule follows one weave forever. Reading "R is
    /// held by T today" as "so an R rule contains a T rule" would freeze a routing
    /// decision into permanent authority, and the reverse would hand a ceiling
    /// scoped to one weave the standing to speak to every future occupant of an
    /// office. Both are escalations, so the relation refuses to relate them at all
    /// (`permits`/`permits_role` already keep the two apart at delivery; this is
    /// the same wall, one level up).
    ///
    /// SEND AND OBSERVE ARE CHECKED IN THEIR OWN DIMENSIONS and never cross: an
    /// `allow_any()` ceiling licenses no observation, and `allow_observe_any()`
    /// licenses no speech. They answer different questions (see ObserveRule), and
    /// one accidental wildcard relation between them would be a silent widening.
    ///
    /// A rule is covered when some SINGLE rule here contains it. Over the infinite
    /// shape and target domains that is also complete — no finite set of exact
    /// rules can cover an "any" rectangle, since a shape or a target outside the
    /// set always remains — and where it is ever conservative it errs by refusing
    /// a legal delegation, never by admitting an illegal one.
    ///
    /// The empty authority is contained by everything, which is what makes
    /// revocation always representable regardless of ceiling.
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

    /// Does this authority say anything at all? An empty one permits nothing.
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

/// What a Weave may do. Default-constructed = empty: may send nothing, holds no
/// OS-capabilities. Minimal authority by default.
///
/// It is the ADMISSION ENVELOPE: the whole security posture a host names when it
/// admits a subject, containment included. Its live half is a `LiveAuthority`
/// held by composition rather than two loose vectors, so "the part that can
/// change while the subject runs" is a thing with a name, a type and one
/// definition of what containing another one means — instead of a convention
/// that two files would eventually disagree about.
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
    /// May send shape (name, version) to whichever Weave currently holds `role`.
    /// The send names a role, not a WeaveId; the role is resolved to its holder at
    /// delivery, so this rule survives the holder reloading (its WeaveId is stable).
    /// A role rule authorizes only role-targeted sends (see permits_role); it never
    /// authorizes a direct WeaveId send.
    Grant& allow_to_role(std::string shape_name, std::uint32_t shape_version, std::string role) {
        live_.allow_to_role(std::move(shape_name), shape_version, std::move(role));
        return *this;
    }
    /// MAY READ THE LATEST CLAIM of shape (name, version), from any claimant and
    /// from any office (R2E-0). Default-absent, like every other authority here:
    /// no existing weave gains any reach from Senses existing, and a Sense
    /// repository is therefore not a universal data-exfiltration rail — reading
    /// it takes a deliberate host decision, out of band, exactly as sending does.
    Grant& allow_observe(std::string shape_name, std::uint32_t shape_version) {
        live_.allow_observe(std::move(shape_name), shape_version);
        return *this;
    }
    /// May read the latest claim of ANY shape (permissive — an inspector, a
    /// renderer, the host's own console).
    Grant& allow_observe_any() {
        live_.allow_observe_any();
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
        return live_.permits(shape_name, shape_version, target);
    }

    /// True iff some rule permits sending shape (name, version) to a Weave holding
    /// `role`. Authorized only by a role rule for the same role (or an any-target
    /// rule); a plain WeaveId rule never authorizes a role-targeted send, just as a
    /// role rule never authorizes a direct WeaveId send (see permits).
    bool permits_role(std::string_view shape_name, std::uint32_t shape_version,
                      std::string_view role) const {
        return live_.permits_role(shape_name, shape_version, role);
    }

    /// True iff some rule permits observing the latest claim of this shape
    /// (R2E-0). Send rules are never consulted: they answer a different question.
    bool permits_observe(std::string_view shape_name, std::uint32_t shape_version) const {
        return live_.permits_observe(shape_name, shape_version);
    }

    /// THE BASELINE LIVE AUTHORITY this admission established (GRANT-0).
    ///
    /// The half of this grant that is read at the moment of use, and therefore
    /// the half a delegated overlay is measured against and added to. It is
    /// exposed as a value to READ, never to write: the admission fact stays
    /// exactly what the host said at `register_weave`, for the life of the
    /// subject, and delegation is a second, separate authority beside it.
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
    /// Speech and observation — the rules read at every delivery/observation.
    LiveAuthority live_;
    /// ...and below, the containment policy: consumed ONCE, out-of-process, into
    /// kernel state (namespace, mount view, cgroup leaf) that this process cannot
    /// revisit. Not reachable through the delegation door, because rewriting one
    /// of these words here would change nothing a kernel enforces.
    std::uint32_t os_ = 0;
    FsAccess fs_ = FsAccess::None; // safe default
    std::string fs_path_;          // the tree ReadOnly exposes (empty otherwise)
    ResourceLimits res_;           // bounded-by-default resource limits
};

/// EFFECTIVE AUTHORITY — the one expression that answers "may this be said?".
///
/// Baseline ∪ delegated, and deliberately a free function rather than a method on
/// either: both `deliver_one` and capability-scoped inspection call THESE, so a
/// Weaver reading what a subject may do is reading the same predicate the bus
/// will apply, not a second store that merely believes it agrees. There is no
/// third, materialized "effective authority" object to fall out of date.
///
/// Union, not override: a delegated rule can only ever ADD reach, so revoking
/// delegated authority cannot remove what the host granted at admission, and no
/// administrator can talk a subject out of its baseline.
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

/// THE RIGHT TO ADMINISTER ONE SUBJECT'S DELEGATED LIVE AUTHORITY (GRANT-0).
///
/// The capability that makes an administrator-shaped Weaver possible without
/// making it a host. It carries three facts and no code path can supply them
/// separately:
///
///   BOARD    which Loom minted it — weakly, exactly as LifecycleAuthority does,
///            so it expires with that board and has no standing in another
///   SUBJECT  the ONE governed WeaveId it may administer
///   CEILING  the most it may ever install on that subject
///
/// THE SUBJECT IS IN THE CAPABILITY, NOT IN THE CALL. There is no parameter to
/// point at a different weave, so "administrator for A quietly administers B" is
/// not a refusal that a future edit could drop — it is a sentence with nowhere to
/// put B. WeaveIds are never reused, so a capability outliving its subject can
/// never be inherited by a later one either; it simply stops naming anything.
///
/// THE CEILING IS NOT THE HOLDER'S OWN GRANT, deliberately. "You may only grant
/// what you hold" sounds like the same rule and is not: a Weaver needs authority
/// to ANSWER policy requests and, separately, authority to DELEGATE something it
/// will never say itself. Conflating them would make every send rule a Weaver
/// happens to own into a delegable right. The host names the ceiling here, once,
/// out of band — which is the same shape as naming a grant at admission.
///
/// A default-constructed one is INERT: no board, no subject, nothing delegable.
/// It exists so an administrator can hold one as a member before the host has
/// handed it anything, and using it fails visibly rather than silently widening.
class GrantAuthority {
public:
    GrantAuthority() = default;
    GrantAuthority(const GrantAuthority&) = default;
    GrantAuthority& operator=(const GrantAuthority&) = default;
    GrantAuthority(GrantAuthority&&) = default;
    GrantAuthority& operator=(GrantAuthority&&) = default;

    /// Does this name a board and a subject at all? False for a default one.
    /// It does NOT promise the board is alive or the subject still mounted —
    /// only the issuing Switchboard can say that, and only at the moment of use.
    bool valid() const noexcept { return subject_.valid(); }

    /// The one governed subject. Readable because its holder is entitled to know
    /// what it administers — and because a Weaver's diagnostics are worthless if
    /// it cannot name the subject it just changed.
    WeaveId subject() const noexcept { return subject_; }

    /// The most this capability may ever install. Readable for the same reason:
    /// a Weaver that must refuse a user's request politely needs to be able to
    /// see the boundary rather than discover it only as a refusal.
    const LiveAuthority& ceiling() const noexcept { return ceiling_; }

private:
    friend class Switchboard;
    GrantAuthority(std::weak_ptr<const LoomIdentity> issuer, WeaveId subject, LiveAuthority ceiling)
        : issuer_(std::move(issuer)), subject_(subject), ceiling_(std::move(ceiling)) {}

    /// WEAK, for the reason LifecycleAuthority's is: an authority must not keep
    /// its board alive, and an authority from a dead world must not validate
    /// against a later board that landed on the same address. Expired is refused.
    std::weak_ptr<const LoomIdentity> issuer_;
    WeaveId subject_{};
    LiveAuthority ceiling_;
};

/// Why an administration attempt did or did not take effect (GRANT-0).
///
/// Not a bool, because a Weaver that must tell a user "no" owes them which no it
/// was — and because "your capability is from another Loom" and "the subject you
/// govern has died" send an operator to entirely different places. Not an audit
/// framework either: one enum, one struct, no log.
enum class GrantOutcome : std::uint8_t {
    Installed,      ///< the delegated authority is now exactly what was requested
    NoAuthority,    ///< a default-constructed / inert capability: it names no subject
    ForeignBoard,   ///< minted by another Loom, or by one that has since died
    NoSuchSubject,  ///< the governed subject is not mounted here (any more)
    ExceedsCeiling, ///< the request is not a semantic subset of the ceiling
    NoLiveDelivery, ///< this Bus is not a live participating context and never had standing
};

const char* name_of(GrantOutcome outcome) noexcept;

/// The result of one administration act — enough for a Weaver's diagnostics
/// without becoming a ledger. On any outcome but `Installed` nothing changed and
/// `installed == previous`, so a caller that ignores the outcome still cannot
/// misread the state.
struct GrantChange {
    GrantOutcome outcome = GrantOutcome::NoLiveDelivery;
    WeaveId subject{};          ///< which governed subject; from the capability, never a parameter
    LiveAuthority previous;     ///< what the delegated overlay was before this call
    LiveAuthority installed;    ///< what it is now
    explicit operator bool() const noexcept { return outcome == GrantOutcome::Installed; }
};

/// WHAT A SUBJECT MAY DO, AS THE BUS WILL ACTUALLY DECIDE IT (GRANT-0).
///
/// Capability-scoped inspection: only of the one subject the capability governs,
/// and only of the message authority it can administer. Never the whole registry,
/// never another subject, never the containment policy or anything else about the
/// host — those are not this capability's business and TERM-0's broad observation
/// question is separate work.
///
/// It exists so an administrator never has to keep a second map that merely
/// BELIEVES what the Kernel is enforcing. `permits*` here call the very same
/// `effective_*` predicates `deliver_one` calls, over snapshots of the very same
/// two values, so agreement is structural rather than maintained.
///
/// SNAPSHOTS, BY VALUE, deliberately: an administrator may hold this across the
/// mutation that invalidates it, and a reference into a record whose vectors are
/// about to be replaced would be a dangling read on the far side of exactly the
/// operation this type is used around.
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
