// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_WEAVE_CONTRACT_HPP
#define ZEN_SWITCHBOARD_WEAVE_CONTRACT_HPP

#include <zen/schema.hpp>
#include <zen/switchboard/bus.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/value.hpp>

#include <memory>
#include <vector>

namespace loom {

/// The Weave contract — the unit that lives behind a boundary on the bus.
///
/// (The three weave-layer headers, at a glance: THIS file is the raw contract
/// the bus dispatches through; zen/weave/weave.hpp is the WeaveBase authoring
/// sugar a maker writes against; zen/weave.hpp is the umbrella include.)
///
/// This is a deliberately minimal, frozen ABI surface (virtual dispatch): the
/// five methods below are all the Switchboard needs, and they are designed to
/// survive a future move to per-Weave mailboxes and multi-threaded dispatch
/// unchanged. Lifecycle notifications are delivered to bus *observers*, not via
/// Weave callbacks, to keep this surface small.
///
/// A Weave never sees an unvalidated message: handle() is invoked only with a
/// payload that has already passed the gate against one of this Weave's accepted
/// schemas. revive() likewise receives an already-gated state value.
class Weave {
public:
    virtual ~Weave() = default;

    Weave(const Weave&) = delete;
    Weave& operator=(const Weave&) = delete;
    Weave(Weave&&) = delete;
    Weave& operator=(Weave&&) = delete;

    /// The message schemas this Weave accepts (its accept-set). Consulted at
    /// registration; each becomes one of this Weave's doors, keyed by
    /// (name, version).
    virtual std::vector<std::shared_ptr<const Schema>> accepted_schemas() const = 0;

    /// Handle a delivered, already-gated message. May call back into `bus` to
    /// send/publish — those calls enqueue; they never deliver synchronously. The
    /// bus is the abstract send surface, so a Weave is agnostic to whether it is
    /// hosted natively or loaded from a library.
    virtual void handle(const Message& in, Bus& bus) = 0;

    /// The Weave's persistable state, as a self-describing Value.
    virtual Value snapshot() const = 0;

    /// The Weave's lifecycle policy, as a Value the Switchboard validates against
    /// its fixed lifecycle-policy schema (and reads only those fields from).
    virtual Value policy() const = 0;

    /// Restore from an already-gated state value (produced by a prior snapshot
    /// that passed the gate).
    virtual void revive(const Value& state) = 0;

    /// THE SENSES THIS WEAVE DECLARES IT CAN CLAIM (SENSE-04) — its claim-set.
    /// Consulted at registration: each schema is registered (so the shape
    /// resolves and is discoverable BEFORE any runtime claim), and the set is
    /// what `claim` is checked against, so a weave cannot claim a shape it never
    /// declared.
    ///
    /// Deliberately a virtual with a default rather than a sixth pure virtual:
    /// this contract is a frozen surface every existing Weave implements, and a
    /// weave that declares no Senses claims none — which is the honest default,
    /// not a silent one. `Claims<...>` on `WeaveBase` writes it.
    ///
    /// Distinct from `accepted_schemas()` (what may be DELIVERED here) and from
    /// `emitted_schemas()` (what may be SENT from here): a claim is neither a
    /// door nor a message.
    virtual std::vector<std::shared_ptr<const Schema>> claimed_schemas() const { return {}; }

protected:
    Weave() = default;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_WEAVE_CONTRACT_HPP
