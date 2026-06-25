#ifndef ZEN_SWITCHBOARD_WEAVE_HPP
#define ZEN_SWITCHBOARD_WEAVE_HPP

#include <zen/schema.hpp>
#include <zen/switchboard/bus.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/value.hpp>

#include <memory>
#include <vector>

namespace loom {

/// The Weave contract — the unit that lives behind a boundary on the bus.
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

protected:
    Weave() = default;
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_WEAVE_HPP
