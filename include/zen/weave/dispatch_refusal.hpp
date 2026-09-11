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
} // namespace loom
#endif
