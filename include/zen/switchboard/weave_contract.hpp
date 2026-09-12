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

/// THE SELF-DESCRIPTION DOOR, NAMED HERE BECAUSE THE SWITCHBOARD HAS TO KNOW IT.
///
/// The shape itself lives a layer up (zen/weave/describe.hpp), where it is
/// declared and answered. Its NAME lives here because `register_weave`'s
/// accept-mode rule is stated in terms of it: a weave whose accept-set declares
/// this door is promising to answer "these are the shapes I accept" from that
/// declared set, and `AcceptMode::AnyRegistered` would make the promise false —
/// the wildcard widens the door set at delivery, on the Switchboard's side,
/// where the weave cannot see it and so cannot describe it.
///
/// So the two are refused together rather than left to produce a truthful-
/// looking understatement. This is the same rule WeaveBase already keeps at
/// compile time for the zen.Poke* shapes, one layer down and at runtime: a
/// weave must not be able to advertise one vocabulary and enforce another.
/// Nothing in Loom or Zengine registers both today — every AnyRegistered weave
/// (the console, the bridge's operator proxy) is a raw loom::Weave that carries
/// no substrate doors at all — so this pins a property rather than changing one.
///
/// A string rather than a schema pointer deliberately: this header is the
/// bottom of the weave layering and must not reach up into it, and matching by
/// (name, version) is exactly how every other door is keyed.
inline constexpr const char* kDescribeAcceptedShapeName = "zen.DescribeAccepted";

/// The answer to `kDescribeAcceptedShapeName`. Declared beside it so the pair
/// has one spelling; used only by the layer above.
inline constexpr const char* kAcceptedShapesShapeName = "zen.AcceptedShapes";

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

    /// WHAT A SHOWING CAME TO AT THIS WEAVE: the three answers `claim_published`
    /// can give, and what the bus does with each (SENSE-06;
    /// docs/reference/joint-publication.md#the-showing-and-its-three-answers).
    enum class PublishedClaim : std::uint8_t {
        /// The weave applied the value and stands behind it. Nothing is owed.
        Applied,
        /// The weave, functioning, deliberately did NOT apply it: it keeps or
        /// reconciles state of its own and re-claims that truth at its next delivery.
        /// Recorded Declined against this participant and publication; not held;
        /// the operator is told. Expected non-application, said as an answer.
        Declined,
        /// The weave could not complete the showing. Recorded Failed; HELD until it
        /// is reloaded or removed; the operator is told. A native weave says this by
        /// throwing (recorded, then re-raised) or by returning it (recorded, with no
        /// exception to re-raise); a loaded weave's non-OK status crosses back as it.
        Failed,
    };

    /// ONE OF THIS WEAVE'S OWN LATEST CLAIMS WAS PUBLISHED BY A JOINT OPERATION,
    /// and this weave has not run since.
    ///
    /// The bus calls it BEFORE the weave's next delivery and BEFORE its next
    /// snapshot, outside any dispatch: there is no Mail, no answer right and no
    /// send. The weave folds the published value into whatever native state it
    /// derives from its claims, so that no reader — a poke, a snapshot, a message
    /// handler — can observe the weave standing behind its own published claim.
    /// A weave that never offers into a joint operation never hears this; the
    /// default is the honest nothing. See zen/switchboard/sense.hpp.
    ///
    /// WHAT IT ANSWERS: `PublishedClaim`. Applied means the
    /// weave now stands behind the published value. Declined means it does NOT and
    /// is not broken: a functioning owner shown a value it will not or cannot make
    /// its own -- a successor shown what its predecessor prepared -- keeps or
    /// reconciles state of its own and re-claims that truth at its next delivery;
    /// the bus records Declined against exactly this participant and publication,
    /// holds nothing, and tells the operator. Failed -- returned, or an exception,
    /// which the bus records the same way and re-raises afterwards for native code
    /// (MSG-10's discipline) -- means the showing did not complete: the bus records
    /// a FAILED application, holds the weave (no delivery, no ordinary snapshot, no
    /// re-run of this hook) until it is reloaded or removed, and tells the operator.
    /// A loaded weave answers through its ABI status, which the host does not
    /// discard. The default applies nothing and owes nothing, truthfully.
    virtual PublishedClaim claim_published(const Value& value) {
        (void)value;
        return PublishedClaim::Applied;
    }

protected:
    Weave() = default;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_WEAVE_CONTRACT_HPP
