#ifndef ZEN_WEAVE_WEAVE_HPP
#define ZEN_WEAVE_WEAVE_HPP

// Low-ceremony Weave-making. A maker writes only their state struct, their
// message structs (as ZEN_SHAPEs), and what to do per message — a typed handler
// `void on(const Ping&, Mail&)`. Everything else is derived:
//   - accepted_schemas() from the Accept<...> list (named once),
//   - snapshot()/revive() from the State struct,
//   - dispatch: a gated incoming Value is matched by content-id, converted to the
//     right struct, and handed to the matching on(); the conversion happens only
//     after the gate has blessed the Value.
// No stringly-typed set(), no hand-built schema, no hand-written snapshot/revive.

#include <zen/weave/shape.hpp>
#include <zen/kernel/schema_codec.hpp>
#include <zen/switchboard.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loom {

/// The shapes a Weave accepts (its doors) and the shapes it emits. Emit is the
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
/// and lets a Weave reply/send/publish with plain structs — no Value, no Cell,
/// no Message construction.
///
/// Mail::send/reply/publish are the **sole** outbound path for a woven Weave.
/// That makes Mail the single reserved chokepoint where emit-enforcement (gating
/// a sent T against the Weave's declared Emit<...>) will one day live. It is
/// deliberately NOT enforced now: it is not yet known whether every Weave's
/// emit-set is statically enumerable (a router/forwarder may emit shapes chosen
/// at runtime), so closing that gate before its shape is proven would couple the
/// substrate to a guess. The declaration is kept honest by test instead; the
/// gate is left off with intent.
class Mail {
public:
    Mail(loom::Bus& bus, const loom::Message& in, loom::WeaveId self)
        : bus_(bus), in_(in), self_(self) {}

    loom::Bus& bus() const { return bus_; }
    loom::WeaveId sender() const { return in_.sender; }
    loom::WeaveId reply_to() const { return in_.reply_to; }
    std::uint64_t correlation() const { return in_.correlation; }

    /// Reply to the inbound sender's reply address, echoing the correlation.
    template <class T>
    loom::Ticket reply(const T& msg) {
        return bus_.send(in_.reply_to,
                         loom::Message(to_value(msg), self_, self_, in_.correlation));
    }
    template <class T>
    loom::Ticket send(loom::WeaveId target, const T& msg, std::uint64_t correlation = 0) {
        return bus_.send(target, loom::Message(to_value(msg), self_, loom::WeaveId{},
                                                  correlation));
    }
    template <class T>
    std::size_t publish(const T& msg, std::uint64_t correlation = 0) {
        return bus_.publish(loom::Message(to_value(msg), self_, loom::WeaveId{}, correlation));
    }
    /// Send `msg` to whichever Weave holds `role` (resolved at delivery). The grant
    /// must permit the shape to that role (Grant::allow_to_role); reload-stable.
    template <class T>
    loom::Ticket send_to_role(std::string_view role, const T& msg,
                                 std::uint64_t correlation = 0) {
        return bus_.send_to_role(
            role, loom::Message(to_value(msg), self_, loom::WeaveId{}, correlation));
    }

private:
    loom::Bus& bus_;
    const loom::Message& in_;
    loom::WeaveId self_;
};

/// CRTP base. A Weave is:
///   class Node : public WeaveBase<Node, Counter, Accept<Ping>, Emit<Pong>> {
///       void on(const Ping&, loom::Mail&) { ... }   // one per accepted shape
///   };
/// State is a ZEN_SHAPE; the protected `state_` is the live state.
template <class Self, class State, class AcceptList, class EmitList = Emit<>>
class WeaveBase;

template <class Self, class State, class... A, class... E>
class WeaveBase<Self, State, Accept<A...>, Emit<E...>> : public loom::Weave {
public:
    std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const override {
        return {schema_of<A>()...};
    }

    /// The shapes this Weave declares it emits. Informational (the reserved hook);
    /// not enforced at publish in this phase.
    std::vector<std::shared_ptr<const loom::Schema>> emitted_schemas() const {
        return {schema_of<E>()...};
    }

    loom::Value snapshot() const override { return to_value(state_); }
    void revive(const loom::Value& v) override { state_ = from_value<State>(v); }

    loom::Value policy() const override {
        const LifecyclePolicy p = static_cast<const Self*>(this)->policy_config();
        loom::Value v(loom::lifecycle_policy_schema());
        v.set("max_reloads", loom::Cell::integer(p.max_reloads));
        v.set("revive_from_last_good", loom::Cell::boolean(p.revive_from_last_good));
        return v;
    }

    void handle(const loom::Message& in, loom::Bus& bus) override {
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
                "loom::WeaveBase: delivered message of shape '" +
                in.payload.schema().name() + " v" +
                std::to_string(in.payload.schema().version()) +
                "' matched no handler — accept-set and handler set are out of sync");
        }
    }

    /// Set by mount(); used as the sender id of emitted messages.
    void zen_set_self(loom::WeaveId id) { self_ = id; }

    /// Default lifecycle policy; a Self may declare its own policy_config().
    LifecyclePolicy policy_config() const { return LifecyclePolicy{}; }

    /// The capabilities this Weave *asks* of the host — advice, never authority.
    /// Defaults to nothing (the floor); a Self declares its own via ZEN_ASK. The
    /// host reads the ask to know what to surface, but a declaration never becomes a
    /// grant — the host alone decides (the floor-factory + grant-record).
    loom::CapabilityAsk ask_config() const { return {}; }

    /// The manifest hook the export layer calls. Returns the ask iff non-empty, so a
    /// Weave with no ask produces a clean manifest with no requests section.
    std::optional<loom::CapabilityAsk> zen_requested_capabilities() const {
        loom::CapabilityAsk a = static_cast<const Self*>(this)->ask_config();
        if (!a.network && a.filesystem.empty() && a.roles.empty()) {
            return std::nullopt;
        }
        return a;
    }

protected:
    State state_{};
    loom::WeaveId self_{};

private:
    // Select the handler the same way the bus selected the door: by true schema
    // identity, via the canonical same_identity helper (which now compares
    // (name, version, content_id)) instead of re-deriving the comparison inline.
    // The delivered payload already passed the gate against the accept-set entry
    // the bus chose by (name, version), so matching the handler the same way makes
    // from_value<S>'s precondition (every field present and well-typed) a
    // guarantee, not a probability.
    //
    // The (name, version) terms are what make this collision-safe. Matching on
    // content_id ALONE could, on a 64-bit FNV collision within one accept-set,
    // select the wrong handler and call from_value<S> on a value the gate
    // validated against a *different* schema — and since from_value reads
    // *v.get(field) for each of S's fields, a field the colliding shape does not
    // carry is a null dereference, not merely a mislabeled value. same_identity
    // checks (name, version) too, so that path is unreachable; its content_id
    // term is the redundant-but-true integrity check (post-gate the payload's
    // content_id already equals the door's).
    template <class S>
    bool dispatch_to(Self* self, const loom::Message& in, Mail& mail) {
        if (!loom::same_identity(*schema_of<S>(), in.payload.schema())) {
            return false;
        }
        self->on(from_value<S>(in.payload), mail);
        return true;
    }
};

/// The grant a Weave's declared Emit<...> implies: it may send each emitted shape
/// to any accepter. This is the *trusted in-process* default — the Weave's
/// self-declared outbound intent is taken as its authority — and it closes the
/// previously-deferred emit-enforcement seam: delivery now checks a real grant
/// that matches the declaration. An untrusted Weave must be given an explicit
/// grant instead (mount_granted), where the declaration is not trusted.
template <class Weave>
loom::Grant emit_default_grant(const Weave& weave) {
    loom::Grant grant;
    for (const auto& schema : weave.emitted_schemas()) {
        grant.allow_to_any(schema->name(), schema->version());
    }
    return grant;
}

/// Construct a trusted Weave, grant it its declared Emit set, register it (its
/// derived schemas flow into the registry as usual), wire its self-id, and return
/// its WeaveId — registration + policy + lifecycle + authority in one call.
template <class Self, class... Args>
loom::WeaveId mount(loom::Switchboard& bus, Args&&... args) {
    auto weave = std::make_unique<Self>(std::forward<Args>(args)...);
    Self* raw = weave.get();
    loom::Grant grant = emit_default_grant(*raw);
    loom::WeaveId id = bus.register_weave(std::move(weave), std::move(grant));
    raw->zen_set_self(id);
    return id;
}

/// As mount(), but with an explicit host-supplied grant (for an untrusted Weave,
/// whose self-declared Emit is not trusted as its authority).
template <class Self, class... Args>
loom::WeaveId mount_granted(loom::Switchboard& bus, loom::Grant grant, Args&&... args) {
    auto weave = std::make_unique<Self>(std::forward<Args>(args)...);
    Self* raw = weave.get();
    loom::WeaveId id = bus.register_weave(std::move(weave), std::move(grant));
    raw->zen_set_self(id);
    return id;
}

} // namespace loom

/// Declare the capabilities a Weave *asks* the host for — a ZEN_SHAPE-sibling that
/// shadows the floor default (WeaveBase::ask_config) with designated initializers:
///   ZEN_ASK(.network = true, .filesystem = "write-scoped", .roles = {"storage"});
/// It is advice only: the host reads the declaration, but the grant remains the
/// host's decision. Omit it entirely to ask for nothing (the floor).
#define ZEN_ASK(...)                                                                                \
    ::loom::CapabilityAsk ask_config() const {                                               \
        return ::loom::CapabilityAsk{__VA_ARGS__};                                           \
    }                                                                                               \
    static_assert(true, "") /* swallow the trailing semicolon */

#endif // ZEN_WEAVE_WEAVE_HPP
