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
#include <zen/switchboard/grant.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/switchboard/sense.hpp>
#include <zen/switchboard/weave_contract.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace loom {

class Switchboard;

/// What a derived claim is made of at each read: the bus's live tables, read and not changed. A
/// function and nothing it could capture, so the value has no source but the bus.
using ClaimDerivation = Value (*)(const Switchboard&);

/// The role Loom's participants claim is made as (zen/host/participants.hpp), which a reader
/// trusts as Loom's: a name a reader knows without being told an id. Loom's own: no registration,
/// load, commit, admission or prepared replacement naming it binds it or moves it, on any bus,
/// whether or not a claimant is mounted. `DerivedClaims::mount_claimant` alone binds it.
inline constexpr const char* kParticipantsRole = "loom.participants";

/// The derived-claim door and what the bus asks of it at a read.
struct DerivedClaims {
    /// Whether `role` is one only `mount_claimant` binds: `kParticipantsRole`, and no other. The
    /// bus asks it wherever a role is bound or moved, and refuses there.
    static bool reserved_role(std::string_view role) noexcept;

    /// Registration's refusal of a reserved role in its set: throws `std::invalid_argument`
    /// naming it, before anything is recorded, so no role of the set is bound (MSG-13).
    static void refuse_reserved_role(const std::string& role);

    /// Register `claimant` holding the reserved role `as_role`, and claim as it the value `derive`
    /// makes at each read (`derive_role_claim_as`): the one door that binds a reserved role, and
    /// a host's, as registration is. All or nothing: throws `std::invalid_argument` for a role
    /// that is not reserved or is held already, and `std::logic_error` when the claim is refused,
    /// leaving nothing registered. `mount_participants` mounts Loom's claimant through it.
    static WeaveId mount_claimant(Switchboard& bus, std::unique_ptr<Weave> claimant, Grant grant,
                                  std::string_view as_role, std::string_view shape_name,
                                  std::uint32_t shape_version, ClaimDerivation derive);

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
