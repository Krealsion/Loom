#ifndef ZEN_SWITCHBOARD_SHARD_HPP
#define ZEN_SWITCHBOARD_SHARD_HPP

#include <zen/schema.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/value.hpp>

#include <memory>
#include <vector>

namespace zen::sb {

class Switchboard;

/// The Shard contract — the unit that lives behind a boundary on the bus.
///
/// This is a deliberately minimal, frozen ABI surface (virtual dispatch): the
/// five methods below are all the Switchboard needs, and they are designed to
/// survive a future move to per-Shard mailboxes and multi-threaded dispatch
/// unchanged. Lifecycle notifications are delivered to bus *observers*, not via
/// Shard callbacks, to keep this surface small.
///
/// A Shard never sees an unvalidated message: handle() is invoked only with a
/// payload that has already passed the gate against one of this Shard's accepted
/// schemas. revive() likewise receives an already-gated state value.
class Shard {
public:
    virtual ~Shard() = default;

    Shard(const Shard&) = delete;
    Shard& operator=(const Shard&) = delete;
    Shard(Shard&&) = delete;
    Shard& operator=(Shard&&) = delete;

    /// The message schemas this Shard accepts (its accept-set). Consulted at
    /// registration; each becomes one of this Shard's doors, keyed by
    /// (name, version).
    virtual std::vector<std::shared_ptr<const Schema>> accepted_schemas() const = 0;

    /// Handle a delivered, already-gated message. May call back into `bus` to
    /// send/publish — those calls enqueue; they never deliver synchronously.
    virtual void handle(const Message& in, Switchboard& bus) = 0;

    /// The Shard's persistable state, as a self-describing Value.
    virtual Value snapshot() const = 0;

    /// The Shard's lifecycle policy, as a Value the Switchboard validates against
    /// its fixed lifecycle-policy schema (and reads only those fields from).
    virtual Value policy() const = 0;

    /// Restore from an already-gated state value (produced by a prior snapshot
    /// that passed the gate).
    virtual void revive(const Value& state) = 0;

protected:
    Shard() = default;
};

} // namespace zen::sb

#endif // ZEN_SWITCHBOARD_SHARD_HPP
