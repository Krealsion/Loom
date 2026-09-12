// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
#ifndef ZEN_WEAVE_DISPATCH_REFUSAL_HPP
#define ZEN_WEAVE_DISPATCH_REFUSAL_HPP

#include <zen/switchboard/bus.hpp>
#include <zen/weave/shape.hpp>

#include <charconv>

namespace loom {
/// Explicitly accept this shape to request later dispatch-refusal notices for
/// ordinary directed/role sends authored by this incarnation. The shape alone
/// is ordinary speech: check Mail::dispatch_refused() / Message provenance.
/// Absence proves nothing. See docs/reference/messaging.md.
struct DispatchRefused {
    // Canonical unsigned decimal: Loom's wire Int is signed. These fields retain
    // all 64 bits without changing the value grammar. Use the typed accessors.
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

/// A BOUND PARTICIPANT OF A JOINT OPERATION THIS WEAVE OPERATES WAS REPLACED OR REMOVED
/// (docs/reference/joint-publication.md#the-operators-notices). Sent to the operator, and only to
/// an operator that explicitly accepts it, once per operation, when a lifecycle change of a
/// bound participant ends the operation (`invalidate_joint_for`) or ends the possibility of
/// an answer to an operation the bus had already aborted -- so an operator waiting on a
/// reply that died with its author is not waiting on silence the bus already knows the end
/// of. Deliberately NOT sent when a bound claim merely moves (`abort_joint_on_claim`): that
/// owner is alive and answers the operator in its own, more specific words, and the operator
/// meets the abort at its next verb. THE SHAPE ALONE IS ORDINARY SPEECH: an operator
/// re-reads `joint_status` for the bus's own record, which is the fact; a forged notice makes
/// it consult a record that says Preparing, and it does nothing. Absence proves nothing.
/// A QUEUED NOTICE IS NOT CONSUMPTION: the record it names is kept until the operator
/// releases it (SENSE-07), so the re-read always finds it.
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

/// THE APPLICATION OF A JOINT PUBLICATION THIS WEAVE OPERATES SETTLED
/// (docs/reference/joint-publication.md#the-operators-notices). Sent to the operator, and only to an operator
/// that explicitly accepts it, once per settlement: every bound claimant was shown its
/// published value and completed (`applied`), or one could not complete its showing
/// (`reason` Failed: it is HELD, deliveries to it are refused `ApplicationFailed`, and
/// only a reload or a removal ends that), or one answered that it keeps state of its own
/// (`reason` Declined: functioning, not held, re-claiming its truth at its next
/// delivery), or one was removed before it was shown
/// (`reason` Lost). A repair that re-settles -- a reloaded successor shown again -- is
/// said again, with the successor's own answer. `claimant` and `role` name the
/// participant a non-application is about, so the operator can tell a requester WHICH
/// owner and not merely that something did. THE SHAPE ALONE IS ORDINARY SPEECH: the
/// operator re-reads `joint_status(op).application` for the bus's own record, which is
/// the fact, and that record is KEPT until the operator releases it -- a notice is never
/// the last copy of an outcome; a forged notice makes it consult a record that says
/// otherwise; a stale one, a record that says Missing (SENSE-07). Its ordinary payload is
/// not an authenticated outcome: the record is.
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
