// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The bus's derived claims: zen/switchboard/derived_claims.hpp says what one is.

#include <zen/switchboard/derived_claims.hpp>
#include <zen/switchboard/switchboard.hpp>

#include <string>
#include <utility>

namespace loom {

SenseClaimResult DerivedClaims::derive_role_claim_as(Switchboard& bus, WeaveId claimant,
                                                     std::string_view as_role,
                                                     std::string_view shape_name,
                                                     std::uint32_t shape_version,
                                                     ClaimDerivation derive) {
    auto it = bus.weaves_.find(claimant.value);
    if (it == bus.weaves_.end() || !it->second.alive) {
        return SenseClaimResult{false, SenseRefusal::RoleNotHeld, 0};
    }
    // Switchboard::role_claim_as's rules, in its order: the role held now, then the shape declared.
    if (as_role.empty() || !bus.holds_role_now(claimant, as_role)) {
        return SenseClaimResult{false, SenseRefusal::RoleNotHeld, 0};
    }
    const std::shared_ptr<const Schema>* declared =
        Switchboard::declared_claim(it->second, shape_name, shape_version);
    if (declared == nullptr || derive == nullptr) {
        return SenseClaimResult{false, SenseRefusal::Undeclared, 0};
    }
    const Switchboard::RoleKey key{std::string(as_role), std::string(shape_name), shape_version};
    auto slot = bus.role_claims_.find(key);
    const std::uint64_t revision =
        (slot == bus.role_claims_.end() ? 0 : slot->second.revision) + 1;
    // No value is stored: the record holds the derivation, and `value` is the shape's empty value,
    // which no reading returns.
    Switchboard::ClaimRecord record{Value(*declared), claimant, it->second.life,
                                    it->second.incarnation, revision};
    record.derive = derive;
    record.derived_shape = *declared;
    if (slot == bus.role_claims_.end()) {
        bus.role_claims_.emplace(key, std::move(record));
    } else {
        slot->second = std::move(record);
    }
    return SenseClaimResult{true, SenseRefusal::None, revision};
}

SenseReading DerivedClaims::made(const Switchboard& bus, ClaimDerivation derive,
                                 const Schema& declared, SenseAuthorship by) {
    // MADE NOW, from the tables as they stand at this read, so nothing a read returns was true
    // only before a change.
    Value value = derive(bus);
    if (!same_identity(value.schema(), declared)) {
        return SenseReading{SenseRefusal::GateRefused, {}, std::nullopt};
    }
    return SenseReading{SenseRefusal::None, std::move(by), std::move(value)};
}

bool DerivedClaims::accepts_any_registered(const Switchboard& bus, WeaveId id) {
    const Switchboard::WeaveRecord* rec = bus.find(id);
    return rec != nullptr && rec->accepts_any;
}

} // namespace loom
