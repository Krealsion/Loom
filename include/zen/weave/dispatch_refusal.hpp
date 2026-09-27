// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
#ifndef ZEN_WEAVE_DISPATCH_REFUSAL_HPP
#define ZEN_WEAVE_DISPATCH_REFUSAL_HPP

#include <zen/switchboard/bus.hpp>
#include <zen/weave/shape.hpp>

#include <charconv>

namespace loom {
/// Explicitly accept this shape to be told of later dispatch refusals of the ordinary directed
/// and role-addressed sends this incarnation authors. The shape alone is ordinary speech: check
/// Mail::dispatch_refused(). Absence proves nothing.
/// docs/reference/messaging.md#sender-visible-dispatch-refusal
struct DispatchRefused {
    // Canonical unsigned decimal, since Loom's wire Int is signed; use the typed accessors.
    std::string attempt;
    std::string target; // directed WeaveId; empty for a role address
    std::string role;   // original authored role address, never its resolved holder
    std::string shape;
    std::int64_t version = 0;
    std::string reason; // CapabilityDenied / NoSuchTarget / TargetUnavailable /
                        // NotAccepted / GateRefused; no host error details
    using ZenSelf = DispatchRefused;
    static constexpr const char* zen_name = "zen.DispatchRefused";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(attempt), ZEN_FIELD(target), ZEN_FIELD(role),
                               ZEN_FIELD(shape), ZEN_FIELD(version), ZEN_FIELD(reason));
    }

    Ticket refused_attempt() const noexcept { return Ticket{unsigned_id(attempt)}; }
    WeaveId addressed_weave() const noexcept { return WeaveId{unsigned_id(target)}; }

private:
    static std::uint64_t unsigned_id(const std::string& s) noexcept {
        if (s.empty() || (s.size() > 1 && s[0] == '0')) {
            return 0;
        }
        std::uint64_t n = 0;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
        return r.ec == std::errc{} && r.ptr == s.data() + s.size() ? n : 0;
    }
};

/// A bound participant of a joint operation this weave operates was replaced or removed
/// (docs/reference/joint-publication.md#the-operators-notices). Sent once per operation, and only
/// to an operator that accepts it, when that ends the operation or ends any answer to one the bus
/// already aborted. Not sent when a bound claim merely moves: that owner is alive and answers
/// in its own words. The shape alone is ordinary speech: the operator re-reads `joint_status`,
/// the bus's record, kept until it releases it (SENSE-07). Absence proves nothing.
struct JointEnded {
    std::string op;     ///< canonical unsigned decimal, as `DispatchRefused` spells its attempt
    std::string reason; ///< `name_of(JointRefusal)`: ParticipantChanged / StaleRevision
    using ZenSelf = JointEnded;
    static constexpr const char* zen_name = "zen.JointEnded";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(op), ZEN_FIELD(reason)); }

    std::uint64_t ended_op() const noexcept {
        if (op.empty() || (op.size() > 1 && op[0] == '0')) {
            return 0;
        }
        std::uint64_t n = 0;
        const auto r = std::from_chars(op.data(), op.data() + op.size(), n);
        return r.ec == std::errc{} && r.ptr == op.data() + op.size() ? n : 0;
    }
};

/// The application of a joint publication this weave operates settled
/// (docs/reference/joint-publication.md#the-operators-notices). Sent once per settlement, and
/// only to an operator that accepts it: every claimant applied (`applied`), or one failed (and
/// is held), declined (and is not), or was removed before it was shown (`reason`). A repair that
/// settles again is said again. `claimant` and `role` name the participant a non-application is
/// about. The shape alone is ordinary speech: the operator re-reads
/// `joint_status(op).application`, the bus's record, kept until it releases it (SENSE-07).
struct JointApplied {
    std::string op;       ///< canonical unsigned decimal
    bool applied = false; ///< every bound claimant applied its published value
    std::string claimant; ///< decimal WeaveId of the claimant a non-application is about; empty when applied
    std::string role;     ///< the office that claimant was bound through, if any
    std::string reason;   ///< `name_of(JointApplication)`: Applied / Declined / Failed / Lost
    using ZenSelf = JointApplied;
    static constexpr const char* zen_name = "zen.JointApplied";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(op), ZEN_FIELD(applied), ZEN_FIELD(claimant),
                               ZEN_FIELD(role), ZEN_FIELD(reason));
    }

    std::uint64_t applied_op() const noexcept { return unsigned_id(op); }
    WeaveId failed_claimant() const noexcept { return WeaveId{unsigned_id(claimant)}; }

private:
    static std::uint64_t unsigned_id(const std::string& s) noexcept {
        if (s.empty() || (s.size() > 1 && s[0] == '0')) {
            return 0;
        }
        std::uint64_t n = 0;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
        return r.ec == std::errc{} && r.ptr == s.data() + s.size() ? n : 0;
    }
};
} // namespace loom
#endif
