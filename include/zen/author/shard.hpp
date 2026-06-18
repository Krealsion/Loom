#ifndef ZEN_AUTHOR_SHARD_HPP
#define ZEN_AUTHOR_SHARD_HPP

// Low-ceremony Shard authoring. An author writes only their state struct, their
// message structs (as ZEN_SHAPEs), and what to do per message — a typed handler
// `void on(const Ping&, Mail&)`. Everything else is derived:
//   - accepted_schemas() from the Accept<...> list (named once),
//   - snapshot()/revive() from the State struct,
//   - dispatch: a gated incoming Value is matched by content-id, converted to the
//     right struct, and handed to the matching on(); the conversion happens only
//     after the gate has blessed the Value.
// No stringly-typed set(), no hand-built schema, no hand-written snapshot/revive.

#include <zen/author/shape.hpp>
#include <zen/switchboard.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zen::author {

/// The shapes a Shard accepts (its doors) and the shapes it emits. Emit is the
/// reserved hook for completing the wiring silhouette later; it is informational
/// now and not enforced at publish.
template <class... S>
struct Accept {};
template <class... S>
struct Emit {};

/// The Switchboard's fixed lifecycle grammar, typed. (Not a registered shape —
/// it targets lifecycle_policy_schema() directly.)
struct LifecyclePolicy {
    std::int64_t max_reloads = 4;
    bool revive_from_last_good = true;
};

/// The typed send context handed to a handler: it carries the inbound envelope
/// and lets a Shard reply/send/publish with plain structs — no Value, no Cell,
/// no Message construction.
class Mail {
public:
    Mail(zen::sb::Bus& bus, const zen::sb::Message& in, zen::sb::ShardId self)
        : bus_(bus), in_(in), self_(self) {}

    zen::sb::Bus& bus() const { return bus_; }
    zen::sb::ShardId sender() const { return in_.sender; }
    zen::sb::ShardId reply_to() const { return in_.reply_to; }
    std::uint64_t correlation() const { return in_.correlation; }

    /// Reply to the inbound sender's reply address, echoing the correlation.
    template <class T>
    zen::sb::Ticket reply(const T& msg) {
        return bus_.send(in_.reply_to,
                         zen::sb::Message(to_value(msg), self_, self_, in_.correlation));
    }
    template <class T>
    zen::sb::Ticket send(zen::sb::ShardId target, const T& msg, std::uint64_t correlation = 0) {
        return bus_.send(target, zen::sb::Message(to_value(msg), self_, zen::sb::ShardId{},
                                                  correlation));
    }
    template <class T>
    std::size_t publish(const T& msg, std::uint64_t correlation = 0) {
        return bus_.publish(zen::sb::Message(to_value(msg), self_, zen::sb::ShardId{}, correlation));
    }

private:
    zen::sb::Bus& bus_;
    const zen::sb::Message& in_;
    zen::sb::ShardId self_;
};

/// CRTP base. A Shard is:
///   class Node : public ShardBase<Node, Counter, Accept<Ping>, Emit<Pong>> {
///       void on(const Ping&, zen::author::Mail&) { ... }   // one per accepted shape
///   };
/// State is a ZEN_SHAPE; the protected `state_` is the live state.
template <class Self, class State, class AcceptList, class EmitList = Emit<>>
class ShardBase;

template <class Self, class State, class... A, class... E>
class ShardBase<Self, State, Accept<A...>, Emit<E...>> : public zen::sb::Shard {
public:
    std::vector<std::shared_ptr<const zen::Schema>> accepted_schemas() const override {
        return {schema_of<A>()...};
    }

    /// The shapes this Shard declares it emits. Informational (the reserved hook);
    /// not enforced at publish in this phase.
    std::vector<std::shared_ptr<const zen::Schema>> emitted_schemas() const {
        return {schema_of<E>()...};
    }

    zen::Value snapshot() const override { return to_value(state_); }
    void revive(const zen::Value& v) override { state_ = from_value<State>(v); }

    zen::Value policy() const override {
        const LifecyclePolicy p = static_cast<const Self*>(this)->policy_config();
        zen::Value v(zen::sb::lifecycle_policy_schema());
        v.set("max_reloads", zen::Cell::integer(p.max_reloads));
        v.set("revive_from_last_good", zen::Cell::boolean(p.revive_from_last_good));
        return v;
    }

    void handle(const zen::sb::Message& in, zen::sb::Bus& bus) override {
        Self* self = static_cast<Self*>(this);
        Mail mail(bus, in, self_);
        // A delivered message has passed the gate against one accepted schema, and
        // the handler is selected by the same (name, version) the bus used to pick
        // that door — so exactly one of these matches and the fold short-circuits
        // there. A no-match is therefore impossible for a *delivered* message: it
        // would require accepted_schemas() and the handler set to have drifted
        // apart, which they cannot, since both are A.... Make that impossibility
        // loud rather than a silent drop.
        const bool routed = (dispatch_to<A>(self, in, mail) || ...);
        if (!routed) {
            throw std::logic_error(
                "zen::author::ShardBase: delivered message of shape '" +
                in.payload.schema().name() + " v" +
                std::to_string(in.payload.schema().version()) +
                "' matched no handler — accept-set and handler set are out of sync");
        }
    }

    /// Set by mount(); used as the sender id of emitted messages.
    void zen_set_self(zen::sb::ShardId id) { self_ = id; }

    /// Default lifecycle policy; a Self may declare its own policy_config().
    LifecyclePolicy policy_config() const { return LifecyclePolicy{}; }

protected:
    State state_{};
    zen::sb::ShardId self_{};

private:
    // Select the handler the same way the bus selected the door: by resolvable
    // identity (name, version), exactly as Switchboard::accept_match does. The
    // delivered payload already passed the gate against the accept-set entry the
    // bus chose by (name, version), so matching the handler the same way makes
    // from_value<S>'s precondition (every field present and well-typed) a
    // guarantee, not a probability.
    //
    // This is deliberately NOT a content_id compare. content_id is a 64-bit FNV
    // hash; a collision within one accept-set would select the wrong handler and
    // call from_value<S> on a value the gate validated against a *different*
    // schema. from_value reads *v.get(field) for each of S's fields, and get()
    // returns null for a field that schema does not carry — so a wrong match is a
    // null dereference, not merely a mislabeled value. Matching on (name, version)
    // makes that path unreachable.
    template <class S>
    bool dispatch_to(Self* self, const zen::sb::Message& in, Mail& mail) {
        const zen::Schema& accepted = *schema_of<S>();
        const zen::Schema& delivered = in.payload.schema();
        if (accepted.name() != delivered.name() || accepted.version() != delivered.version()) {
            return false;
        }
        self->on(from_value<S>(in.payload), mail);
        return true;
    }
};

/// Construct a Shard, register it (its derived schemas flow into the registry as
/// usual), wire its self-id, and return its ShardId — the registration + policy +
/// lifecycle wiring in one call.
template <class Self, class... Args>
zen::sb::ShardId mount(zen::sb::Switchboard& bus, Args&&... args) {
    auto shard = std::make_unique<Self>(std::forward<Args>(args)...);
    Self* raw = shard.get();
    zen::sb::ShardId id = bus.register_shard(std::move(shard));
    raw->zen_set_self(id);
    return id;
}

} // namespace zen::author

#endif // ZEN_AUTHOR_SHARD_HPP
