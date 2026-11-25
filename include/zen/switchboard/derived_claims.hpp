// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss
#ifndef ZEN_SWITCHBOARD_DERIVED_CLAIMS_HPP
#define ZEN_SWITCHBOARD_DERIVED_CLAIMS_HPP

// Derived claims: a role claim whose value Loom makes at each read, from the bus's live tables,
// by a derivation the claim named, so no read is ever behind the tables it is made from: there is
// no stored value to fall behind. Made only through the host's door below, which takes the
// Switchboard no message can hand a weave; `Mail` has no spelling for it, so no weave derives a
// claim (SENSE-08). A unit the bus consults, so the Switchboard's own file and class do not take
// the subject on.

#include <zen/schema.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/switchboard/sense.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <string_view>

namespace loom {

class Switchboard;

/// What a derived claim is made of at each read: the bus's live tables, read and not changed. A
/// function and nothing it could capture, so the value has no source but the bus.
using ClaimDerivation = Value (*)(const Switchboard&);

/// The derived-claim door and what the bus asks of it at a read.
struct DerivedClaims {
    /// Claim as the role `as_role` the value `derive` makes at each read. The host's door, as
    /// `Switchboard::role_claim_as` is, and checked as that is: the claimant is live and holds the
    /// role now, refused `RoleNotHeld`; it declared the shape in `Claims<...>` and names a
    /// derivation, refused `Undeclared`; refused, nothing is stored. Keyed and ended as a role
    /// claim: an observe rule reads it, the role's end drops it, and the claimant's next ordinary
    /// claim of the shape replaces it. Its revision counts its claims, never the tables' changes.
    static SenseClaimResult derive_role_claim_as(Switchboard& bus, WeaveId claimant,
                                                 std::string_view as_role,
                                                 std::string_view shape_name,
                                                 std::uint32_t shape_version,
                                                 ClaimDerivation derive);

    /// A derived claim's reading, made now: `derive`'s value with the claim's authorship, or
    /// refused `GateRefused` when the value made is not of the declared shape, so no value of
    /// another shape reaches a reader. What `Switchboard::observe_role` answers for one.
    static SenseReading made(const Switchboard& bus, ClaimDerivation derive,
                             const Schema& declared, SenseAuthorship by);

    /// Whether `id` accepts any registered shape (`AcceptMode::AnyRegistered`), beside the list
    /// `Switchboard::accepted_schemas` gives: the one table fact a derivation reads that the bus's
    /// own readers do not give. False for an unknown id.
    static bool accepts_any_registered(const Switchboard& bus, WeaveId id);
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_DERIVED_CLAIMS_HPP
