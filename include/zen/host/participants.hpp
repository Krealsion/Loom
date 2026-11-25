// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_PARTICIPANTS_HPP
#define ZEN_HOST_PARTICIPANTS_HPP

// Loom's participants claim: who is on the bus, what roles each holds, whether it is alive, at
// which life and incarnation, and what it accepts and declares it emits, claimed as a Sense in
// Loom's role `loom.participants` and read by a weave granted to observe it, inside its handler:
//
//   mail.latest_from_role<loom::host::Participants>(loom::host::kParticipantsRole)
//
// The claim is derived (SENSE-08): Loom makes its value at each read from the Switchboard's live
// tables, the ones routing resolves against, so a read after a role moves, a weave is replaced or
// a participant ends shows it, as the control door's answers do (KERN-03). Nothing is asked and
// nothing waits. A row is the bus's record of the participant, whatever made it: woven, built by
// hand on `loom::Weave`, mounted natively or loaded. A sealed candidate is outside the world and
// has no row.
//
// What it is not: authority. A row names what a participant accepts and declares it emits, never
// what it may send (its grant's) or what an actor may do (the authority reader's,
// zen/host/authority_reader.hpp); and it carries no handle, no grant and no capability. Reading
// takes an observe rule for `zen.Participants`, absent by default (SENSE-05); a reader not granted
// it is refused `NotAuthorized` and learns nothing. Whether a library was loaded by a Kernel is
// the Kernel's book, asked at the control door (`QueryParticipant`), not claimed here.
//
// Host wiring, as the authority reader is: the claim is made through the bus's derived-claim door
// (`DerivedClaims::derive_role_claim_as`), which takes the Switchboard no message can hand a weave.
// While its claimant holds the role, no other weave can: a role has one holder, so a load naming it
// is refused. Another weave's claim of a shape named `zen.Participants`, personal or as a role of
// its own, is under another key, and a read of Loom's role never returns it.
// docs/reference/kernel.md#role-truth

#include <zen/gate.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard/derived_claims.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/declared_shape.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace loom::host {

/// The role Loom's participants claim is made as: the name a reader knows without being told an
/// id.
inline constexpr const char* kParticipantsRole = "loom.participants";

/// One participant on the bus, as the bus holds it now.
struct Participant {
    std::int64_t id = 0;
    std::vector<std::string> roles; ///< every role it holds now, in binding order
    bool alive = false;             ///< false: registered, and receiving nothing until revived
    std::int64_t life = 0;
    std::int64_t incarnation = 0;
    /// It accepts any registered shape (`AcceptMode::AnyRegistered`), beside `accepts`.
    bool accepts_any = false;
    std::vector<DeclaredShape> accepts; ///< the shapes it accepts, in its accept-set's order
    std::vector<DeclaredShape> emits;   ///< what it declared it emits; not what it may send
    using ZenSelf = Participant;
    static constexpr const char* zen_name = "zen.Participant";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(id), ZEN_FIELD(roles), ZEN_FIELD(alive), ZEN_FIELD(life),
                               ZEN_FIELD(incarnation), ZEN_FIELD(accepts_any), ZEN_FIELD(accepts),
                               ZEN_FIELD(emits));
    }
};

/// Every participant on the bus now, by id ascending. `whole` false: the rows would pass
/// `kMaxDecodedCells`, which no reader's gate admits, so none is given and `count` alone says how
/// many there are; a table is never cut short.
struct Participants {
    std::int64_t count = 0;
    bool whole = true;
    std::vector<Participant> rows;
    using ZenSelf = Participants;
    static constexpr const char* zen_name = "zen.Participants";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(count), ZEN_FIELD(whole), ZEN_FIELD(rows));
    }

    /// The row of `id`, or nullptr for one not on the bus.
    const Participant* find(WeaveId id) const {
        for (const Participant& row : rows) {
            if (row.id == static_cast<std::int64_t>(id.value)) {
                return &row;
            }
        }
        return nullptr;
    }

    /// The row of whoever holds `role`, or nullptr when nobody does.
    const Participant* holder_of(std::string_view role) const {
        for (const Participant& row : rows) {
            if (std::find(row.roles.begin(), row.roles.end(), role) != row.roles.end()) {
                return &row;
            }
        }
        return nullptr;
    }

    /// Whether `role`'s holder lists `shape` among what it accepts. A holder accepting any
    /// registered shape answers by its list here: what it would take is the gate's, at delivery.
    bool holder_accepts(std::string_view role, const Schema& shape) const {
        const Participant* row = holder_of(role);
        return row != nullptr && lists(row->accepts, shape);
    }

    /// Whether `role`'s holder declared it emits `shape`.
    bool holder_emits(std::string_view role, const Schema& shape) const {
        const Participant* row = holder_of(role);
        return row != nullptr && lists(row->emits, shape);
    }

    static bool lists(const std::vector<DeclaredShape>& shapes, const Schema& shape) {
        return std::any_of(shapes.begin(), shapes.end(), [&](const DeclaredShape& s) {
            return s.name == shape.name() &&
                   s.version == static_cast<std::int64_t>(shape.version());
        });
    }
};

/// The claim's value, made from `bus`'s tables as they stand: the derivation Loom runs at each
/// read. Exposed so a host can read what a reader would, ungated, as it reads any claim.
inline Value participants_now(const Switchboard& bus) {
    const auto named = [](const std::vector<std::shared_ptr<const Schema>>& shapes) {
        std::vector<DeclaredShape> out;
        out.reserve(shapes.size());
        for (const std::shared_ptr<const Schema>& s : shapes) {
            if (s != nullptr) {
                out.push_back(DeclaredShape{s->name(), static_cast<std::int64_t>(s->version())});
            }
        }
        return out;
    };
    Participants table;
    for (const WeaveId id : bus.list_weaves()) {
        if (bus.sealed(id)) {
            continue; // a prepared candidate: nothing can be sent to it, and it holds no role
        }
        const ParticipantRef now = bus.participant(id);
        Participant row;
        row.id = static_cast<std::int64_t>(id.value);
        row.roles = bus.roles_of(id);
        row.alive = bus.alive(id);
        row.life = static_cast<std::int64_t>(now.life);
        row.incarnation = static_cast<std::int64_t>(now.incarnation);
        row.accepts_any = DerivedClaims::accepts_any_registered(bus, id);
        row.accepts = named(bus.accepted_schemas(id));
        row.emits = named(bus.emitted_schemas(id));
        table.rows.push_back(std::move(row));
    }
    table.count = static_cast<std::int64_t>(table.rows.size());
    Value made = to_value(table);
    if (decoded_cells(made) > kMaxDecodedCells) {
        Participants refused;
        refused.count = table.count;
        refused.whole = false;
        return to_value(refused);
    }
    return made;
}

/// Its claimant's state: nothing. The claim is the bus's tables, and a copy kept here would be a
/// second answer to drift.
struct ParticipantsClaimantState {
    ZEN_SHAPE(ParticipantsClaimantState, 1);
};

/// The claimant: it holds `kParticipantsRole`, declares `zen.Participants` and accepts nothing of
/// its own. Mount it with `mount_participants`, which makes the claim.
class ParticipantsClaimant final
    : public WeaveBase<ParticipantsClaimant, ParticipantsClaimantState, Accept<>, Emit<>,
                       Claims<Participants>> {};

/// Mount the claimant in `kParticipantsRole` and claim the participants as it, derived from
/// `bus`. Throws `std::invalid_argument` when the role is held already, as any registration into a
/// held role does, and binds nothing.
inline WeaveId mount_participants(Switchboard& bus) {
    auto weave = std::make_unique<ParticipantsClaimant>();
    ParticipantsClaimant* raw = weave.get();
    Grant grant;
    allow_poke_answers(grant);
    allow_describe_answers(grant);
    const WeaveId id = bus.register_weave(std::move(weave), std::move(grant),
                                          std::string(kParticipantsRole));
    raw->zen_set_self(id);
    const SenseClaimResult claimed =
        DerivedClaims::derive_role_claim_as(bus, id, kParticipantsRole, Participants::zen_name,
                                            Participants::zen_version, &participants_now);
    if (!claimed.accepted) {
        (void)bus.unregister_weave(id);
        throw std::logic_error("mount_participants: the participants claim was refused");
    }
    return id;
}

/// The observe rule a reader of the claim needs, and nothing more.
inline Grant& allow_observe_participants(Grant& grant) {
    return grant.allow_observe(Participants::zen_name, Participants::zen_version);
}

/// Loom's participants as a reader's handler reads them now; nothing when the reader is not
/// granted the claim, when no participants claimant is mounted, or when the table is not whole.
/// A reader that must tell those apart reads the claim itself, as the top of this file shows.
inline std::optional<Participants> read_participants(Mail& mail) {
    const SenseReading r = mail.latest_from_role<Participants>(kParticipantsRole);
    if (!r) {
        return std::nullopt;
    }
    Participants table = from_value<Participants>(*r.value);
    if (!table.whole) {
        return std::nullopt;
    }
    return table;
}

} // namespace loom::host

#endif // ZEN_HOST_PARTICIPANTS_HPP
