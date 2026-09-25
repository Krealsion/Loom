// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_OBSERVE_RELAY_HPP
#define ZEN_OBSERVE_RELAY_HPP

// AN OBSERVATION RELAY: how a host lets a participant it names follow another participant's
// publications, scoped, numbered and bounded -- without a tap, and without the producer knowing.
// The vocabulary and the promises are `zen/observe/vocabulary.hpp`; the contract is
// docs/reference/observation.md.
//
// HOW IT HEARS. For each subscription the relay registers one LISTENER: an ordinary participant
// that accepts exactly the subscribed shapes and may say nothing (an empty grant). A publication
// reaches it the way it reaches every listener -- at enqueue, gated by the PUBLISHER'S own grant
// -- so a producer that may not publish a shape to others cannot be observed publishing it either,
// and a producer needs no change to be observable. The relay tells the subscriber only what the
// bus-stamped holder of the named office published; the same shape from anyone else is counted
// (`foreign`) and not told. The listener leaves the bus when the subscription ends, and with it the
// schema claim its declaration made (LIFE-08).
//
// WHO MAY. Every `Subscribe` is judged by the host's `ObservePolicy`; the default admits nothing
// and says so. The policy sees the subscriber as the bus stamped it -- a guest session's proxy, a
// local weave -- and the producer and shapes asked for, and nothing it says is authority to send.
//
// WHAT SET IT IN MOTION. A publication delivered inside a host's FENCE (the send and its
// synchronous dispatch ancestry: docs/reference/messaging.md) was set in motion by the
// settle-requested send that opened it. When the host tells the relay who opened which fence
// (`FenceOrigins`, e.g. a bridge server's `settle_origin`) and that opener is the SUBSCRIBER, the
// observation carries the subscriber's own correlation as `cause`. Never another opener's: one
// session is never told what another did.
//
// BOUNDED, NEVER SILENT. At most `window` numbered things stand unacknowledged per subscription.
// Past it, an occurrence is dropped and counted and said as a `Gap` before anything later; a shape
// the subscriber named `latest` keeps only its newest, which says how many it stood for. Nothing
// the producer does waits for a subscriber. Subscriptions are bounded in all and per subscriber.
//
// HOST WIRING, NOT A TAP. The relay holds the `Switchboard` to register listeners, read who holds
// an office, and speak as itself outside its own deliveries; that reach is its host's, like a
// bridge server's. It reads no registry beyond the shapes asked for, keeps no history and
// forwards nothing it was not subscribed to.

#include <zen/kernel/schema_codec.hpp>
#include <zen/observe/vocabulary.hpp>
#include <zen/registry.hpp>
#include <zen/schema.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/standard_shapes.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace loom::observe {

// ---- limits (docs/reference/bounds.md#observation-relay) --------------------------------------

inline constexpr std::size_t kMaxSubscriptions = 64; ///< held at once by one relay
inline constexpr std::size_t kMaxPerSubscriber = 8;  ///< held at once for one subscriber
inline constexpr std::size_t kMaxShapes = 8;         ///< shapes one subscription names
inline constexpr std::int64_t kDefaultWindow = 256;  ///< unacknowledged, when a subscriber says 0
inline constexpr std::int64_t kMaxWindow = 4096;     ///< the most a subscriber is granted

// ---- the descriptor package a Subscribed carries ------------------------------------------------

/// `loom.observe.Shapes`: `zen.SchemaDesc` v1 for each subscribed shape (`shapes`) and the closure
/// they nest (`referenced`, post-order, so one forward pass resolves it) -- self-description's own
/// descriptor and closure rule (zen/weave/describe.hpp), under a name that says what these are.
inline std::shared_ptr<const Schema> shapes_schema() {
    static const auto s = SchemaBuilder("loom.observe.Shapes", 1)
                              .list("referenced", type_message(schema_desc_schema()),
                                    /*required=*/false)
                              .list("shapes", type_message(schema_desc_schema()))
                              .build();
    return s;
}

inline Value encode_shapes(const std::vector<std::shared_ptr<const Schema>>& roots) {
    Value v(shapes_schema());
    std::vector<std::shared_ptr<const Schema>> referenced;
    for (const auto& s : roots) {
        collect_referenced(*s, referenced);
    }
    if (!referenced.empty()) {
        std::vector<Cell> refs;
        refs.reserve(referenced.size());
        for (const auto& s : referenced) {
            refs.push_back(Cell::message(encode_schema(*s)));
        }
        v.set("referenced", Cell::list(std::move(refs)));
    }
    std::vector<Cell> out;
    out.reserve(roots.size());
    for (const auto& s : roots) {
        out.push_back(Cell::message(encode_schema(*s)));
    }
    v.set("shapes", Cell::list(std::move(out)));
    return v;
}

/// The subscribed shapes, reconstructed from an admitted `loom.observe.Shapes` value; the closure
/// is registered into `deps` first. Throws, as `decode_schema` does, on a descriptor it cannot
/// rebuild.
inline std::vector<std::shared_ptr<const Schema>> decode_shapes(const Value& v, Registry& deps) {
    if (const Cell* refs = v.get("referenced")) {
        for (const Cell& c : refs->as_list()) {
            deps.register_schema(decode_schema(*c.as_message(), deps));
        }
    }
    std::vector<std::shared_ptr<const Schema>> out;
    if (const Cell* roots = v.get("shapes")) {
        for (const Cell& c : roots->as_list()) {
            out.push_back(decode_schema(*c.as_message(), deps));
        }
    }
    return out;
}

// ---- the host's seams ---------------------------------------------------------------------------

/// What a subscriber asked for, as the relay's host policy sees it.
struct ObserveRequest {
    WeaveId subscriber{}; ///< as the bus stamped the ask
    std::string producer;
    std::vector<ShapeRef> shapes;
    std::string label;
};

struct ObserveVerdict {
    bool allowed = false;
    std::string reason; ///< a refusal's words, told to the subscriber verbatim
    static ObserveVerdict allow() { return ObserveVerdict{true, {}}; }
    static ObserveVerdict refuse(std::string why) { return ObserveVerdict{false, std::move(why)}; }
};

using ObservePolicy = std::function<ObserveVerdict(const ObserveRequest&)>;

/// The default: nobody observes anything on this host, and each asker is told so.
inline ObservePolicy observe_nothing() {
    return [](const ObserveRequest&) {
        return ObserveVerdict::refuse("this host lets nobody observe its participants");
    };
}

/// Who opened a fence: a session and the correlation of its settle-requested send.
struct FenceOrigin {
    WeaveId session{};
    std::uint64_t correlation = 0;
};

/// The host's record of fences it opened on others' behalf (a bridge server's `settle_origin`).
using FenceOrigins = std::function<std::optional<FenceOrigin>(Fence)>;

/// What a relay says to itself when a listener must leave the bus but is inside its own delivery:
/// the relay's next turn retires it. It carries nothing, and hearing it from anyone else only
/// retires what was already retiring.
struct RelayRetire {
    using ZenSelf = RelayRetire;
    static constexpr const char* zen_name = "loom.observe.RelayRetire";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// The relay's own counters, poke-readable like any state.
struct RelayState {
    std::int64_t subscribed = 0; ///< subscriptions admitted, all time
    std::int64_t refused = 0;    ///< subscriptions refused, all time
    std::int64_t ended = 0;      ///< subscriptions ended, all time
    using ZenSelf = RelayState;
    static constexpr const char* zen_name = "loom.observe.RelayState";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(subscribed), ZEN_FIELD(refused), ZEN_FIELD(ended));
    }
};

class Relay;

namespace detail {

/// The relay a listener reports to, while that relay lives: cleared by the relay as it dies, so a
/// listener the bus destroys later never reaches a freed relay.
struct RelayHub {
    Relay* relay = nullptr;
};

inline std::shared_ptr<const Schema> listener_state_schema() {
    static const auto s =
        SchemaBuilder("loom.observe.ListenerState", 1).field("subscription", Kind::Int).build();
    return s;
}

/// ONE SUBSCRIPTION'S EAR: accepts exactly the subscribed shapes, says nothing, and hands what it
/// hears to its relay. It holds no grant and no role.
class Listener final : public Weave {
public:
    Listener(std::shared_ptr<RelayHub> hub, std::uint64_t subscription,
             std::vector<std::shared_ptr<const Schema>> shapes)
        : hub_(std::move(hub)), subscription_(subscription), shapes_(std::move(shapes)) {}

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override { return shapes_; }
    void handle(const Message& in, Bus& bus) override;
    Value snapshot() const override {
        Value v(listener_state_schema());
        v.set("subscription", Cell::integer(static_cast<std::int64_t>(subscription_)));
        return v;
    }
    Value policy() const override {
        Value v(lifecycle_policy_schema());
        v.set("max_reloads", Cell::integer(0));
        v.set("revive_from_last_good", Cell::boolean(true));
        return v;
    }
    void revive(const Value&) override {}

private:
    std::shared_ptr<RelayHub> hub_;
    std::uint64_t subscription_;
    std::vector<std::shared_ptr<const Schema>> shapes_;
};

} // namespace detail

/// A HOST'S OBSERVATION RELAY. Mount it in `kObserveRole` with `relay_grant()` and `attach` the bus
/// (`mount_relay` does all three).
class Relay final
    : public WeaveBase<Relay, RelayState,
                       Accept<Subscribe, Release, Acknowledge, StatusRequested, RelayRetire>,
                       // ...AND THE TWO STANDARD REPLIES IT ANSWERS WITH, declared so they resolve
                       // on any bus the relay is mounted on: a subscriber whose door admits any
                       // registered shape can only be answered in a shape somebody registered.
                       Emit<Subscribed, Observed, Gap, Ended, Status, RelayRetire, loom::Ack,
                            loom::Refused>> {
public:
    explicit Relay(ObservePolicy policy = observe_nothing(), std::string lifetime = {})
        : policy_(std::move(policy)), lifetime_(lifetime.empty() ? mint() : std::move(lifetime)),
          hub_(std::make_shared<detail::RelayHub>()) {
        hub_->relay = this;
    }
    ~Relay() override { hub_->relay = nullptr; }
    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;

    /// Host wiring: the bus this relay registers listeners on and speaks on, as itself, and the
    /// host's record of fences (optional: without it no observation names a cause).
    void attach(Switchboard& bus, FenceOrigins origins = {}) {
        bus_ = &bus;
        origins_ = std::move(origins);
    }

    /// This relay's lifetime: minted when it was made and never reused, so a subscription number
    /// is only ever read together with it.
    const std::string& lifetime() const noexcept { return lifetime_; }

    // ---- the host's calls: outside the subscriber's reach -----------------------------------

    /// WITHDRAW every subscription `subscriber` holds, telling each `Ended` (kind `revoked`, with
    /// `why`). Returns how many ended.
    std::size_t revoke(WeaveId subscriber, const std::string& why) {
        return end_where([&](const Sub& s) { return s.subscriber == subscriber; }, kEndedRevoked,
                         why, /*tell=*/true);
    }

    /// End every subscription, telling each subscriber `kind` (`revoked`, or `gone` as a host
    /// ends) and `why`.
    std::size_t end_all(const char* kind, const std::string& why) {
        return end_where([](const Sub&) { return true; }, kind, why, /*tell=*/true);
    }

    /// The subscriber is gone (its connection closed, it died): end its subscriptions and tell
    /// nobody, since there is nobody to tell.
    std::size_t forget(WeaveId subscriber) {
        return end_where([&](const Sub& s) { return s.subscriber == subscriber; }, kEndedGone, {},
                         /*tell=*/false);
    }

    /// Every subscription held, for a host's inventory.
    std::vector<StatusRow> rows() const {
        std::vector<StatusRow> out;
        for (const Sub& s : subs_) {
            out.push_back(row_of(s));
        }
        return out;
    }
    std::size_t active() const noexcept { return subs_.size(); }

    // ---- the doors ---------------------------------------------------------------------------

    void on(const Subscribe& ask, Mail& mail) {
        retire();
        auto refuse = [&](std::string why) {
            ++state_.refused;
            (void)mail.answer(loom::Refused{std::move(why)});
        };
        const WeaveId subscriber = mail.sender();
        if (bus_ == nullptr) {
            refuse("this relay is not attached to a bus");
            return;
        }
        if (ask.producer.empty()) {
            refuse("a subscription names the office whose publications it follows");
            return;
        }
        if (ask.shapes.empty() || ask.shapes.size() > kMaxShapes) {
            refuse("a subscription names 1 to " + std::to_string(kMaxShapes) + " shapes");
            return;
        }
        if (!ask.encoding.empty() && ask.encoding != kEncodingNative &&
            ask.encoding != kEncodingCompat) {
            refuse("encoding '" + ask.encoding + "' is not native or compat");
            return;
        }
        if (ask.window < 0) {
            refuse("a window is a count of observations, not " + std::to_string(ask.window));
            return;
        }
        std::vector<std::shared_ptr<const Schema>> schemas;
        for (const ShapeRef& r : ask.shapes) {
            std::shared_ptr<const Schema> s =
                r.version > 0 ? bus_->resolve_schema(r.name, static_cast<std::uint32_t>(r.version))
                              : nullptr;
            if (!s) {
                refuse("no participant here declares " + r.name + " v" + std::to_string(r.version));
                return;
            }
            schemas.push_back(std::move(s));
        }
        for (const std::string& name : ask.latest) {
            const bool named = std::any_of(ask.shapes.begin(), ask.shapes.end(),
                                           [&](const ShapeRef& r) { return r.name == name; });
            if (!named) {
                refuse("'" + name + "' is named latest but is not one of the subscribed shapes");
                return;
            }
        }
        const ObserveVerdict verdict =
            policy_ ? policy_(ObserveRequest{subscriber, ask.producer, ask.shapes, ask.label})
                    : ObserveVerdict::refuse("this relay has no policy");
        if (!verdict.allowed) {
            refuse(verdict.reason.empty() ? std::string("refused by this host's policy")
                                          : verdict.reason);
            return;
        }
        if (subs_.size() >= kMaxSubscriptions) {
            refuse("this relay already holds " + std::to_string(kMaxSubscriptions) +
                   " subscriptions");
            return;
        }
        const std::size_t mine = static_cast<std::size_t>(std::count_if(
            subs_.begin(), subs_.end(), [&](const Sub& s) { return s.subscriber == subscriber; }));
        if (mine >= kMaxPerSubscriber) {
            refuse("this subscriber already holds " + std::to_string(kMaxPerSubscriber) +
                   " subscriptions here; release one first");
            return;
        }
        Sub s;
        s.id = ++next_id_;
        s.subscriber = subscriber;
        s.producer = ask.producer;
        s.shapes = ask.shapes;
        s.schemas = schemas;
        s.latest = ask.latest;
        s.compat = ask.encoding == kEncodingCompat;
        s.window = ask.window == 0 ? kDefaultWindow : std::min(ask.window, kMaxWindow);
        s.label = ask.label;
        // THE MOMENT THE SUBSCRIPTION BEGINS: from here, every publication enqueued reaches the
        // listener, and the answer below is enqueued now -- before anything it could be told.
        try {
            s.listener = bus_->register_weave(
                std::make_unique<detail::Listener>(hub_, s.id, schemas), Grant{});
        } catch (const std::exception& e) {
            refuse(std::string("the relay could not listen for those shapes: ") + e.what());
            return;
        }
        const WeaveId holder = bus_->role_holder(ask.producer);
        Subscribed said;
        said.subscription = static_cast<std::int64_t>(s.id);
        said.relay = lifetime_;
        said.producer = ask.producer;
        said.holder = static_cast<std::int64_t>(holder.value);
        said.incarnation =
            holder.valid() ? static_cast<std::int64_t>(bus_->participant(holder).incarnation) : 0;
        said.window = s.window;
        said.encoding = s.compat ? kEncodingCompat : kEncodingNative;
        said.latest = ask.latest;
        said.shapes = bytes_of(encode_shapes(schemas), s.compat);
        said.next = 1;
        subs_.push_back(std::move(s));
        ++state_.subscribed;
        (void)mail.answer(said);
    }

    void on(const Release& ask, Mail& mail) {
        retire();
        Sub* s = mine(ask.subscription, ask.relay, mail.sender());
        if (s == nullptr) {
            (void)mail.answer(loom::Refused{"no subscription " + std::to_string(ask.subscription) +
                                            " of this relay lifetime is yours to release"});
            return;
        }
        Ended e = ending(*s, kEndedReleased, "released at its subscriber's request");
        drop(s->id);
        (void)mail.answer(e);
    }

    void on(const Acknowledge& ack, Mail& mail) {
        retire();
        Sub* s = mine(ack.subscription, ack.relay, mail.sender());
        if (s == nullptr) {
            (void)mail.answer(loom::Refused{"no subscription " + std::to_string(ack.subscription) +
                                            " of this relay lifetime is yours"});
            return;
        }
        if (ack.through < 0 || ack.through > s->said) {
            (void)mail.answer(loom::Refused{"subscription " + std::to_string(ack.subscription) +
                                            " has said nothing numbered " +
                                            std::to_string(ack.through)});
            return;
        }
        s->acked = std::max(s->acked, ack.through);
        flush(*s);
        (void)mail.answer(loom::Ack{});
    }

    void on(const RelayRetire&, Mail&) { retire(); }

    void on(const StatusRequested&, Mail& mail) {
        retire();
        Status out;
        out.relay = lifetime_;
        for (const Sub& s : subs_) {
            if (s.subscriber == mail.sender()) {
                out.rows.push_back(row_of(s));
            }
        }
        (void)mail.answer(out);
    }

    /// WHAT A LISTENER HEARD: one publication of a subscribed shape. Called inside that
    /// listener's delivery, so the bus's dispatch position is the publication's.
    void heard(std::uint64_t subscription, const Message& in) {
        Sub* s = find(subscription);
        if (s == nullptr || bus_ == nullptr) {
            return; // ended while this delivery was queued
        }
        if (!bus_->alive(s->subscriber)) {
            // THE SUBSCRIBER IS GONE and there is nobody to tell. The listener cannot leave the
            // bus from inside its own delivery, so the relay tells itself to retire it next turn.
            retiring_.push_back(s->listener);
            erase(s->id);
            ++state_.ended;
            retire_soon();
            return;
        }
        const WeaveId holder = bus_->role_holder(s->producer);
        if (!holder.valid() || in.sender != holder) {
            ++s->foreign; // the same shape from someone who does not hold the office
            return;
        }
        ++s->heard;
        const DispatchPosition at = bus_->current_dispatch();
        Observed o;
        o.subscription = static_cast<std::int64_t>(s->id);
        o.relay = lifetime_;
        o.shape = std::string(in.payload.schema().name());
        o.version = static_cast<std::int64_t>(in.payload.schema().version());
        o.payload = bytes_of(in.payload, s->compat);
        o.producer = static_cast<std::int64_t>(in.sender.value);
        o.incarnation = static_cast<std::int64_t>(bus_->participant(in.sender).incarnation);
        o.office = std::string(in.provenance.authored_role());
        o.delivery = static_cast<std::int64_t>(at.seq);
        o.published_in = static_cast<std::int64_t>(at.parent);
        if (at.fence.valid() && origins_) {
            const std::optional<FenceOrigin> from = origins_(at.fence);
            if (from && from->session == s->subscriber) {
                o.cause = static_cast<std::int64_t>(from->correlation); // the subscriber's own send
            }
        }
        flush(*s);
        if (open(*s)) {
            say(*s, std::move(o));
            return;
        }
        const bool latest =
            std::find(s->latest.begin(), s->latest.end(), o.shape) != s->latest.end();
        if (!latest) {
            ++s->pending_lost; // told as a Gap before anything later
            ++s->lost;
            return;
        }
        for (Observed& held : s->held) {
            if (held.shape == o.shape) {
                o.coalesced = held.coalesced + 1;
                held = std::move(o);
                ++s->coalesced;
                return;
            }
        }
        s->held.push_back(std::move(o));
    }

private:
    struct Sub {
        std::uint64_t id = 0;
        WeaveId subscriber{};
        std::string producer;
        std::vector<ShapeRef> shapes;
        std::vector<std::shared_ptr<const Schema>> schemas;
        std::vector<std::string> latest;
        bool compat = false;
        std::int64_t window = kDefaultWindow;
        std::string label;
        WeaveId listener{};
        std::int64_t said = 0;  ///< the last number said
        std::int64_t acked = 0; ///< the last number acknowledged
        std::int64_t heard = 0;
        std::int64_t lost = 0;
        std::int64_t coalesced = 0;
        std::int64_t foreign = 0;
        std::int64_t pending_lost = 0; ///< dropped since the last Gap was said
        std::vector<Observed> held;    ///< the newest of each `latest` shape, waiting for room
    };

    static std::string mint() {
        std::random_device rd;
        std::uniform_int_distribution<unsigned> d(0, 15);
        static const char* const hex = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 32; ++i) {
            out += hex[d(rd)];
        }
        return out;
    }

    static loom::Bytes bytes_of(const Value& v, bool compat) {
        const std::string text = compat ? loom::compat::serialize(v) : loom::serialize(v);
        return loom::Bytes(text.begin(), text.end());
    }

    static StatusRow row_of(const Sub& s) {
        StatusRow r;
        r.subscription = static_cast<std::int64_t>(s.id);
        r.subscriber = static_cast<std::int64_t>(s.subscriber.value);
        r.producer = s.producer;
        r.shapes = s.shapes;
        r.label = s.label;
        r.window = s.window;
        r.said = s.said;
        r.acked = s.acked;
        r.heard = s.heard;
        r.lost = s.lost;
        r.coalesced = s.coalesced;
        r.foreign = s.foreign;
        return r;
    }

    Sub* find(std::uint64_t id) {
        for (Sub& s : subs_) {
            if (s.id == id) {
                return &s;
            }
        }
        return nullptr;
    }

    /// The asker's own subscription `id` of THIS lifetime, or null.
    Sub* mine(std::int64_t id, const std::string& relay, WeaveId asker) {
        if (relay != lifetime_ || id <= 0) {
            return nullptr;
        }
        Sub* s = find(static_cast<std::uint64_t>(id));
        return (s != nullptr && s->subscriber == asker) ? s : nullptr;
    }

    bool open(const Sub& s) const noexcept { return s.said - s.acked < s.window; }

    /// Say one numbered thing about `s`, as this relay, to its subscriber.
    template <class T>
    void say(Sub& s, T value) {
        value.seq = ++s.said;
        (void)bus_->send_as(this->self_, s.subscriber,
                            Message(to_value(value), this->self_, WeaveId{}, 0));
    }

    /// While there is room: the Gap first, then the newest of each held `latest` shape.
    void flush(Sub& s) {
        if (s.pending_lost > 0 && open(s)) {
            Gap g;
            g.subscription = static_cast<std::int64_t>(s.id);
            g.relay = lifetime_;
            g.lost = s.pending_lost;
            g.reason = std::to_string(s.pending_lost) + " publication(s) were dropped while " +
                       std::to_string(s.window) + " stood unacknowledged";
            s.pending_lost = 0;
            say(s, std::move(g));
        }
        while (!s.held.empty() && open(s)) {
            Observed o = std::move(s.held.front());
            s.held.erase(s.held.begin());
            say(s, std::move(o));
        }
    }

    Ended ending(const Sub& s, const char* kind, std::string why) const {
        Ended e;
        e.subscription = static_cast<std::int64_t>(s.id);
        e.relay = lifetime_;
        e.seq = s.said + 1;
        e.last = s.said;
        e.lost = s.pending_lost + static_cast<std::int64_t>(s.held.size());
        e.kind = kind;
        e.reason = std::move(why);
        return e;
    }

    template <class Pred>
    std::size_t end_where(Pred pred, const char* kind, const std::string& why, bool tell) {
        retire();
        std::vector<std::uint64_t> ids;
        for (const Sub& s : subs_) {
            if (pred(s)) {
                ids.push_back(s.id);
            }
        }
        for (std::uint64_t id : ids) {
            Sub* s = find(id);
            if (tell && bus_ != nullptr) {
                Ended e = ending(*s, kind, why);
                s->said = e.seq;
                (void)bus_->send_as(this->self_, s->subscriber,
                                    Message(to_value(e), this->self_, WeaveId{}, 0));
            }
            drop(id);
        }
        return ids.size();
    }

    /// End a subscription here: its listener leaves the bus (and its schema claim with it).
    void drop(std::uint64_t id) {
        Sub* s = find(id);
        if (s == nullptr) {
            return;
        }
        if (bus_ != nullptr && s->listener.valid()) {
            if (!bus_->unregister_weave(s->listener)) {
                retiring_.push_back(s->listener); // its own delivery is running; next turn
                retire_soon();
            }
        }
        erase(id);
        ++state_.ended;
    }

    void retire_soon() {
        if (bus_ != nullptr && this->self_.valid()) {
            (void)bus_->send_as(this->self_, this->self_,
                                Message(to_value(RelayRetire{}), this->self_, WeaveId{}, 0));
        }
    }

    void erase(std::uint64_t id) {
        subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                                   [&](const Sub& s) { return s.id == id; }),
                    subs_.end());
    }

    /// Listeners that could not leave inside their own delivery leave now.
    void retire() {
        if (bus_ == nullptr) {
            return;
        }
        std::vector<WeaveId> still;
        for (WeaveId id : retiring_) {
            if (bus_->alive(id) && !bus_->unregister_weave(id)) {
                still.push_back(id);
            }
        }
        retiring_ = std::move(still);
    }

    ObservePolicy policy_;
    std::string lifetime_;
    std::shared_ptr<detail::RelayHub> hub_;
    Switchboard* bus_ = nullptr;
    FenceOrigins origins_;
    std::vector<Sub> subs_;
    std::vector<WeaveId> retiring_;
    std::uint64_t next_id_ = 0; ///< the last subscription minted; never reused
};

inline void detail::Listener::handle(const Message& in, Bus&) {
    if (hub_->relay != nullptr) {
        hub_->relay->heard(subscription_, in);
    }
}

/// What a relay may say: its own vocabulary and the standard replies, to whoever asked.
inline Grant relay_grant() {
    Grant g;
    g.allow_to_any(Subscribed::zen_name, Subscribed::zen_version);
    g.allow_to_any(Observed::zen_name, Observed::zen_version);
    g.allow_to_any(Gap::zen_name, Gap::zen_version);
    g.allow_to_any(Ended::zen_name, Ended::zen_version);
    g.allow_to_any(Status::zen_name, Status::zen_version);
    g.allow_to_any(loom::Ack::zen_name, loom::Ack::zen_version);
    g.allow_to_any(loom::Refused::zen_name, loom::Refused::zen_version);
    g.allow_to_any(RelayRetire::zen_name, RelayRetire::zen_version); // said only to itself
    return g;
}

/// Register a relay in `kObserveRole` on `bus` with `relay_grant()`, attached. The bus owns it;
/// the pointer is for the host's calls (`revoke`, `forget`, `rows`).
inline Relay* mount_relay(Switchboard& bus, ObservePolicy policy, FenceOrigins origins = {}) {
    auto relay = std::make_unique<Relay>(std::move(policy));
    Relay* raw = relay.get();
    const WeaveId id = bus.register_weave(std::move(relay), relay_grant(), kObserveRole);
    raw->zen_set_self(id);
    raw->attach(bus, std::move(origins));
    return raw;
}

} // namespace loom::observe

#endif // ZEN_OBSERVE_RELAY_HPP
