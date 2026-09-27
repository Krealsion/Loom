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

/// The self-description door's name, here because `register_weave` refuses a weave that
/// declares it together with `AcceptMode::AnyRegistered`: the wildcard widens the doors on the
/// Switchboard's side, where the weave cannot see or describe them, so its description would
/// understate what it accepts. A string, keyed by (name, version) like every door, because this
/// header is below the weave layer (zen/weave/describe.hpp) that declares the shape.
/// docs/reference/messaging.md#self-description--what-may-be-said-to-this-weave
inline constexpr const char* kDescribeAcceptedShapeName = "zen.DescribeAccepted";

/// The answer to `kDescribeAcceptedShapeName`, spelled beside it.
inline constexpr const char* kAcceptedShapesShapeName = "zen.AcceptedShapes";

/// The Weave contract: the unit behind a boundary on the bus, and the raw interface the bus
/// dispatches through (zen/weave/weave.hpp is the authoring layer over it, zen/weave.hpp the
/// umbrella). The five pure methods are all dispatch needs; lifecycle events go to observers,
/// not to the Weave. The optional parts are defaulted virtuals with empty defaults. Adding one
/// changes the vtable, so a consumer compiled against another copy of this header must rebuild.
///
/// A Weave never sees an unvalidated message: handle() receives only a payload that passed the
/// gate against one of its accepted schemas, and revive() only a gated state.
class Weave {
public:
    virtual ~Weave() = default;

    Weave(const Weave&) = delete;
    Weave& operator=(const Weave&) = delete;
    Weave(Weave&&) = delete;
    Weave& operator=(Weave&&) = delete;

    /// The accept-set: each schema becomes one of this Weave's doors at registration, keyed by
    /// (name, version).
    virtual std::vector<std::shared_ptr<const Schema>> accepted_schemas() const = 0;

    /// Handle a delivered, gated message. Sends through `bus` enqueue and never deliver
    /// synchronously; the same code runs natively or loaded from a library.
    virtual void handle(const Message& in, Bus& bus) = 0;

    /// The Weave's persistable state, as a self-describing Value.
    virtual Value snapshot() const = 0;

    /// The lifecycle policy, as a Value the Switchboard validates against its fixed schema and
    /// reads only those fields from.
    virtual Value policy() const = 0;

    /// Restore from a state value that has passed the gate.
    virtual void revive(const Value& state) = 0;

    /// The Senses this weave declares it can claim: its claim-set (SENSE-04), registered at
    /// registration so each shape is discoverable before any claim, and checked by `claim`.
    /// Empty by default; `Claims<...>` writes it. Not a door, not a message.
    virtual std::vector<std::shared_ptr<const Schema>> claimed_schemas() const { return {}; }

    /// The message shapes this weave declares it may send: its emit-set, registered with every
    /// component they nest in the same transaction as the accept-set, claim-set and state shape,
    /// so a producer's definition of a (name, version) meets every acceptor's and a disagreement
    /// refuses the registration (`SchemaConflict`). `Emit<...>` writes it; a library's manifest
    /// carries it (`emits`), and the host adapter answers from that.
    ///
    /// Vocabulary, not authority: it grants no send rule (GATE-03, GATE-05), so a declared
    /// emitter with no rule for a shape is still `CapabilityDenied`. Not an exhaustive send list
    /// either. Empty by default.
    /// docs/decisions/declared-vocabulary-is-agreed-at-admission.md
    virtual std::vector<std::shared_ptr<const Schema>> emitted_schemas() const { return {}; }

    /// What a showing came to at this weave (SENSE-06;
    /// docs/reference/joint-publication.md#the-showing-and-its-three-answers).
    enum class PublishedClaim : std::uint8_t {
        /// The weave applied the value and stands behind it. Nothing is owed.
        Applied,
        /// Functioning, it did not apply the value: it keeps state of its own and re-claims it
        /// at its next delivery. Recorded Declined against this weave and publication, not
        /// held; the operator is told.
        Declined,
        /// It could not complete the showing: recorded Failed, and the weave is held until
        /// reloaded or removed; the operator is told. Said by throwing (recorded, then rethrown)
        /// or by returning it. A loaded weave's slot answering neither `ZEN_OK` nor
        /// `ZEN_CLAIM_DECLINED` crosses back as this.
        Failed,
    };

    /// One of this weave's own latest claims was published by a joint operation and the weave
    /// has not run since. Called before its next delivery and its next snapshot, outside any
    /// dispatch: no Mail, no answer, no send. The weave folds the value into whatever state it
    /// derives from its claims, so no reader sees it standing behind its own published claim. A
    /// weave that never offers into a joint operation never hears this.
    ///
    /// It answers with `PublishedClaim`. A native exception is recorded as Failed and then
    /// rethrown (MSG-10); a loaded weave answers through its ABI status. The default is
    /// Applied, for a weave that derives nothing from its claims. See zen/switchboard/sense.hpp.
    virtual PublishedClaim claim_published(const Value& value) {
        (void)value;
        return PublishedClaim::Applied;
    }

protected:
    Weave() = default;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_WEAVE_CONTRACT_HPP
