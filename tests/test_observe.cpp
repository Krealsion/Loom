// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// Suite `observe`: an observation relay on one bus (zen/observe/relay.hpp). What a subscriber is
// told, when it begins, whose publications count, what a window does, how a subscription ends and
// what `cause` may name. The crossing through a link is the `bridge` suite's (`link:` cases).

#include <doctest.h>
#include <zen/observe/relay.hpp>
#include <zen/observe/vocabulary.hpp>
#include <zen/registry.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace ob = loom::observe;

// ---- the producer's vocabulary: one occurrence, one state, one command -------------------------

struct ObsTick {
    std::int64_t n = 0;
    ZEN_SHAPE(ObsTick, 1, ZEN_FIELD(n));
};

struct ObsLevel {
    std::int64_t v = 0;
    ZEN_SHAPE(ObsLevel, 1, ZEN_FIELD(v));
};

/// Publish `ticks` ObsTick (numbered from `from`) and then `levels` ObsLevel.
struct ObsSay {
    std::int64_t ticks = 0;
    std::int64_t levels = 0;
    std::int64_t from = 1;
    ZEN_SHAPE(ObsSay, 1, ZEN_FIELD(ticks), ZEN_FIELD(levels), ZEN_FIELD(from));
};

struct ProducerState {
    std::int64_t recipients = 0; ///< how many listeners the last publication reached
    ZEN_SHAPE(ProducerState, 1, ZEN_FIELD(recipients));
};

class Producer final : public loom::WeaveBase<Producer, ProducerState, loom::Accept<ObsSay>,
                                              loom::Emit<ObsTick, ObsLevel>> {
public:
    void on(const ObsSay& s, loom::Mail& mail) {
        for (std::int64_t i = 0; i < s.ticks; ++i) {
            state_.recipients = static_cast<std::int64_t>(mail.publish(ObsTick{s.from + i}));
        }
        for (std::int64_t i = 0; i < s.levels; ++i) {
            state_.recipients = static_cast<std::int64_t>(mail.publish(ObsLevel{s.from + i}));
        }
    }
    std::int64_t recipients() const { return state_.recipients; }
};

// ---- a subscriber that keeps what it is told, and whether each was an attested answer ----------

struct SubscriberState {
    std::int64_t n = 0;
    ZEN_SHAPE(SubscriberState, 1, ZEN_FIELD(n));
};

class Subscriber final
    : public loom::WeaveBase<Subscriber, SubscriberState,
                             loom::Accept<ob::Subscribed, ob::Observed, ob::Gap, ob::Ended,
                                          ob::Status, loom::Refused, loom::Ack>,
                             loom::Emit<ob::Subscribe, ob::Release, ob::Acknowledge,
                                        ob::StatusRequested>> {
public:
    std::vector<ob::Subscribed> subscribed;
    std::vector<ob::Observed> observed;
    std::vector<ob::Gap> gaps;
    std::vector<ob::Ended> ended;
    std::vector<ob::Status> statuses;
    std::vector<std::string> refused;
    std::int64_t acks = 0;
    std::vector<std::string> order;     ///< "subscribed", "observed 3", "gap 4", ...
    std::vector<bool> ended_answers;    ///< per `ended`: was it an attested answer?
    std::vector<bool> observed_answers; ///< per `observed`: was it an attested answer?

    void on(const ob::Subscribed& s, loom::Mail& mail) {
        CHECK(mail.answers_ask());
        subscribed.push_back(s);
        order.push_back("subscribed");
    }
    void on(const ob::Observed& o, loom::Mail& mail) {
        observed_answers.push_back(mail.answers_ask());
        observed.push_back(o);
        order.push_back("observed " + std::to_string(o.seq));
    }
    void on(const ob::Gap& g, loom::Mail&) {
        gaps.push_back(g);
        order.push_back("gap " + std::to_string(g.seq));
    }
    void on(const ob::Ended& e, loom::Mail& mail) {
        ended_answers.push_back(mail.answers_ask());
        ended.push_back(e);
        order.push_back("ended " + std::to_string(e.seq));
    }
    void on(const ob::Status& s, loom::Mail&) { statuses.push_back(s); }
    void on(const loom::Refused& r, loom::Mail&) { refused.push_back(r.reason); }
    void on(const loom::Ack&, loom::Mail&) { ++acks; }
};

// ---- the bus the cases share ------------------------------------------------------------------

struct Rig {
    loom::Switchboard bus;
    ob::Relay* relay = nullptr;
    loom::WeaveId relay_id{};
    Producer* producer = nullptr;
    loom::WeaveId producer_id{};
    std::uint64_t corr = 0;

    explicit Rig(ob::ObservePolicy policy, ob::FenceOrigins origins = {}) {
        relay = ob::mount_relay(bus, std::move(policy), std::move(origins));
        relay_id = bus.role_holder(ob::kObserveRole);
        auto p = std::make_unique<Producer>();
        producer = p.get();
        producer_id = bus.register_weave(std::move(p), loom::Grant{}.allow_any(), std::string("test.producer"));
        producer->zen_set_self(producer_id);
    }

    /// A subscriber that may speak to the relay's office -- and, with `commands`, may also tell
    /// the producer to publish (a session acting on the producer, for the `cause` case).
    Subscriber* subscriber(loom::WeaveId* id, bool commands = false) {
        loom::Grant g;
        for (const char* shape : {ob::Subscribe::zen_name, ob::Release::zen_name,
                                  ob::Acknowledge::zen_name, ob::StatusRequested::zen_name}) {
            g.allow_to_role(shape, 1, ob::kObserveRole);
        }
        if (commands) {
            g.allow_to_role(ObsSay::zen_name, ObsSay::zen_version, "test.producer");
            g.allow_to_any(ObsSay::zen_name, ObsSay::zen_version);
        }
        auto s = std::make_unique<Subscriber>();
        Subscriber* raw = s.get();
        *id = bus.register_weave(std::move(s), g);
        raw->zen_set_self(*id);
        return raw;
    }

    /// Say `msg` as `who` to the relay's office, under a fresh correlation, and pump.
    template <class T>
    void ask(loom::WeaveId who, const T& msg) {
        (void)bus.send_as_to_role(who, ob::kObserveRole,
                                  loom::Message(loom::to_value(msg), who, loom::WeaveId{}, ++corr));
        bus.drain_until_idle();
    }

    ob::Subscribe want(std::vector<ob::ShapeRef> shapes, std::int64_t window = 0,
                       std::vector<std::string> latest = {}) const {
        ob::Subscribe s;
        s.producer = "test.producer";
        s.shapes = std::move(shapes);
        s.latest = std::move(latest);
        s.window = window;
        s.label = "suite observe";
        return s;
    }

    void say(std::int64_t ticks, std::int64_t levels = 0, std::int64_t from = 1) {
        (void)bus.send(producer_id, loom::Message(loom::to_value(ObsSay{ticks, levels, from})));
        bus.drain_until_idle();
    }
};

ob::ShapeRef tick_ref() { return ob::ShapeRef{"ObsTick", 1}; }
ob::ShapeRef level_ref() { return ob::ShapeRef{"ObsLevel", 1}; }

ob::ObservePolicy allow_all() {
    return [](const ob::ObserveRequest&) { return ob::ObserveVerdict::allow(); };
}

std::int64_t tick_of(const ob::Observed& o) {
    const std::string bytes(o.payload.begin(), o.payload.end());
    loom::Admission a = loom::admit(loom::parse(bytes), loom::schema_of<ObsTick>());
    REQUIRE(a.ok());
    return a.value().get("n")->as_int();
}

} // namespace

TEST_SUITE("observe") {

TEST_CASE("observe: the default relay lets nobody observe, says so, and listens to nothing") {
    Rig rig(ob::observe_nothing());
    loom::WeaveId sid{};
    Subscriber* sub = rig.subscriber(&sid);
    rig.ask(sid, rig.want({tick_ref()}));
    REQUIRE(sub->refused.size() == 1);
    CHECK(sub->refused[0].find("nobody observe") != std::string::npos);
    CHECK(sub->subscribed.empty());
    rig.say(3);
    CHECK(rig.producer->recipients() == 0); // no listener was registered for it
    CHECK(sub->observed.empty());
    CHECK(rig.relay->active() == 0);
}

TEST_CASE("observe: an admitted subscription is answered first, then told each publication in order from 1") {
    Rig rig(allow_all());
    loom::WeaveId sid{};
    Subscriber* sub = rig.subscriber(&sid);
    rig.ask(sid, rig.want({tick_ref()}));
    REQUIRE(sub->subscribed.size() == 1);
    const ob::Subscribed& s = sub->subscribed[0];
    CHECK(s.subscription > 0);
    CHECK(s.relay == rig.relay->lifetime());
    CHECK(s.holder == static_cast<std::int64_t>(rig.producer_id.value));
    CHECK(s.window == ob::kDefaultWindow);
    CHECK(s.next == 1);
    rig.say(3, 0, 10);
    CHECK(rig.producer->recipients() == 1); // the listener, and nobody else
    REQUIRE(sub->observed.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        const ob::Observed& o = sub->observed[i];
        CHECK(o.seq == static_cast<std::int64_t>(i + 1));
        CHECK(o.subscription == s.subscription);
        CHECK(o.relay == s.relay);
        CHECK(o.shape == "ObsTick");
        CHECK(o.version == 1);
        CHECK(o.producer == static_cast<std::int64_t>(rig.producer_id.value));
        CHECK(o.incarnation == s.incarnation);
        CHECK(o.cause == 0);
        CHECK(o.link.empty());
        CHECK(o.delivery > 0);
        CHECK(o.published_in > 0);
        CHECK(tick_of(o) == 10 + static_cast<std::int64_t>(i));
        CHECK_FALSE(sub->observed_answers[i]); // ordinary words, never an answer
    }
    CHECK(sub->order.front() == "subscribed");
}

TEST_CASE("observe: ready means ready -- a publication enqueued after the subscription began is told, one enqueued before is not") {
    Rig rig(allow_all());
    loom::WeaveId sid{};
    Subscriber* sub = rig.subscriber(&sid);
    // Queued in this order, pumped once: the first command publishes BEFORE the relay handles the
    // Subscribe, the second AFTER. Publication fans out at enqueue.
    (void)rig.bus.send(rig.producer_id, loom::Message(loom::to_value(ObsSay{1, 0, 100})));
    (void)rig.bus.send_as_to_role(sid, ob::kObserveRole,
                                  loom::Message(loom::to_value(rig.want({tick_ref()})), sid,
                                                loom::WeaveId{}, 9));
    (void)rig.bus.send(rig.producer_id, loom::Message(loom::to_value(ObsSay{1, 0, 200})));
    rig.bus.drain_until_idle();
    REQUIRE(sub->subscribed.size() == 1);
    REQUIRE(sub->observed.size() == 1);
    CHECK(tick_of(sub->observed[0]) == 200);
    CHECK(sub->observed[0].seq == 1);
}

TEST_CASE("observe: the same shape from a participant that does not hold the office is counted, not told") {
    Rig rig(allow_all());
    loom::WeaveId sid{};
    Subscriber* sub = rig.subscriber(&sid);
    rig.ask(sid, rig.want({tick_ref()}));
    auto impostor = std::make_unique<Producer>();
    Producer* imp = impostor.get();
    const loom::WeaveId imp_id = rig.bus.register_weave(std::move(impostor), loom::Grant{}.allow_any());
    imp->zen_set_self(imp_id);
    (void)rig.bus.send(imp_id, loom::Message(loom::to_value(ObsSay{2, 0, 7})));
    rig.bus.drain_until_idle();
    CHECK(sub->observed.empty());
    const std::vector<ob::StatusRow> rows = rig.relay->rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].foreign == 2);
    CHECK(rows[0].heard == 0);
}

TEST_CASE("observe: an unapproved producer or shape, or a shape nobody declares, is refused whole") {
    Rig rig([](const ob::ObserveRequest& r) {
        if (r.producer != "test.producer") {
            return ob::ObserveVerdict::refuse("not that producer");
        }
        for (const ob::ShapeRef& s : r.shapes) {
            if (s.name != "ObsTick") {
                return ob::ObserveVerdict::refuse("not that shape");
            }
        }
        return ob::ObserveVerdict::allow();
    });
    loom::WeaveId sid{};
    Subscriber* sub = rig.subscriber(&sid);
    ob::Subscribe other = rig.want({tick_ref()});
    other.producer = "someone.else";
    rig.ask(sid, other);
    rig.ask(sid, rig.want({tick_ref(), level_ref()})); // one shape too many: refused whole
    rig.ask(sid, rig.want({ob::ShapeRef{"NoSuchShape", 1}}));
    rig.ask(sid, rig.want({tick_ref()}, 0, {"ObsLevel"})); // latest must be subscribed
    REQUIRE(sub->refused.size() == 4);
    CHECK(sub->refused[0] == "not that producer");
    CHECK(sub->refused[1] == "not that shape");
    CHECK(sub->refused[2].find("no participant here declares NoSuchShape v1") != std::string::npos);
    CHECK(sub->refused[3].find("latest") != std::string::npos);
    CHECK(sub->subscribed.empty());
    rig.say(1, 1);
    CHECK(sub->observed.empty());
    CHECK(rig.relay->active() == 0);
}

TEST_CASE("observe: past the window, occurrences become one counted Gap and a latest shape keeps its newest") {
    Rig rig(allow_all());
    loom::WeaveId sid{};
    Subscriber* sub = rig.subscriber(&sid);
    rig.ask(sid, rig.want({tick_ref(), level_ref()}, 2, {"ObsLevel"}));
    REQUIRE(sub->subscribed.size() == 1);
    CHECK(sub->subscribed[0].window == 2);
    rig.say(5, 3, 1); // ticks 1..5, then levels 1..3, in one handler: nobody acknowledges between
    REQUIRE(sub->observed.size() == 2);
    CHECK(tick_of(sub->observed[0]) == 1);
    CHECK(tick_of(sub->observed[1]) == 2);
    CHECK(sub->gaps.empty());
    // Ticks 3..5 were dropped and counted; levels 1..3 were held as the newest one.
    ob::Acknowledge ack;
    ack.subscription = sub->subscribed[0].subscription;
    ack.relay = sub->subscribed[0].relay;
    ack.through = 2;
    rig.ask(sid, ack);
    CHECK(sub->acks == 1);
    REQUIRE(sub->gaps.size() == 1);
    CHECK(sub->gaps[0].seq == 3);
    CHECK(sub->gaps[0].lost == 3);
    REQUIRE(sub->observed.size() == 3);
    const ob::Observed& level = sub->observed[2];
    CHECK(level.seq == 4);
    CHECK(level.shape == "ObsLevel");
    CHECK(level.coalesced == 2); // it stands for levels 1 and 2
    const std::string bytes(level.payload.begin(), level.payload.end());
    loom::Admission a = loom::admit(loom::parse(bytes), loom::schema_of<ObsLevel>());
    REQUIRE(a.ok());
    CHECK(a.value().get("v")->as_int() == 3);
    std::vector<std::string> expected{"subscribed", "observed 1", "observed 2", "gap 3", "observed 4"};
    CHECK(sub->order == expected);
    const std::vector<ob::StatusRow> rows = rig.relay->rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].lost == 3);
    CHECK(rows[0].coalesced == 2);
    CHECK(rows[0].said == 4);
    CHECK(rows[0].acked == 2);
}

TEST_CASE("observe: an acknowledgement for another's subscription, or a number never said, is refused") {
    Rig rig(allow_all());
    loom::WeaveId a_id{};
    loom::WeaveId b_id{};
    Subscriber* a = rig.subscriber(&a_id);
    Subscriber* b = rig.subscriber(&b_id);
    rig.ask(a_id, rig.want({tick_ref()}));
    REQUIRE(a->subscribed.size() == 1);
    rig.say(1);
    ob::Acknowledge ack;
    ack.subscription = a->subscribed[0].subscription;
    ack.relay = a->subscribed[0].relay;
    ack.through = 1;
    rig.ask(b_id, ack); // not B's
    REQUIRE(b->refused.size() == 1);
    CHECK(b->refused[0].find("is yours") != std::string::npos);
    ack.through = 99; // never said
    rig.ask(a_id, ack);
    REQUIRE(a->refused.size() == 1);
    CHECK(a->refused[0].find("99") != std::string::npos);
    ob::Release other;
    other.subscription = a->subscribed[0].subscription;
    other.relay = a->subscribed[0].relay;
    rig.ask(b_id, other); // not B's to release either
    CHECK(b->refused.size() == 2);
    CHECK(rig.relay->active() == 1);
}

TEST_CASE("observe: release answers Ended, the listener leaves the bus, and nothing more is told") {
    Rig rig(allow_all());
    loom::WeaveId sid{};
    Subscriber* sub = rig.subscriber(&sid);
    rig.ask(sid, rig.want({tick_ref()}));
    rig.say(2);
    CHECK(rig.producer->recipients() == 1);
    ob::Release r;
    r.subscription = sub->subscribed[0].subscription;
    r.relay = sub->subscribed[0].relay;
    rig.ask(sid, r);
    REQUIRE(sub->ended.size() == 1);
    CHECK(sub->ended_answers[0]); // the answer to the release, attested
    CHECK(sub->ended[0].kind == ob::kEndedReleased);
    CHECK(sub->ended[0].seq == 3);
    CHECK(sub->ended[0].last == 2);
    CHECK(rig.relay->active() == 0);
    rig.say(4);
    CHECK(rig.producer->recipients() == 0); // no listener left for the shape
    CHECK(sub->observed.size() == 2);
    rig.ask(sid, r); // a second release names nothing that is still open
    CHECK(sub->refused.size() == 1);
}

TEST_CASE("observe: revocation ends the observation visibly, and a vanished subscriber ends silently") {
    Rig rig(allow_all());
    loom::WeaveId a_id{};
    loom::WeaveId b_id{};
    Subscriber* a = rig.subscriber(&a_id);
    Subscriber* b = rig.subscriber(&b_id);
    rig.ask(a_id, rig.want({tick_ref()}));
    rig.ask(b_id, rig.want({tick_ref()}));
    rig.say(1);
    CHECK(rig.relay->revoke(a_id, "the maker stopped this guest's observation") == 1);
    rig.bus.drain_until_idle();
    REQUIRE(a->ended.size() == 1);
    CHECK_FALSE(a->ended_answers[0]); // said, not an answer
    CHECK(a->ended[0].kind == ob::kEndedRevoked);
    CHECK(a->ended[0].reason == "the maker stopped this guest's observation");
    CHECK(a->ended[0].seq == 2);
    rig.say(1, 0, 5);
    CHECK(a->observed.size() == 1);
    CHECK(b->observed.size() == 2);
    // B leaves the bus: its subscription ends with nobody to tell, at the next publication.
    (void)rig.bus.unregister_weave(b_id);
    rig.say(1, 0, 9);
    CHECK(rig.relay->active() == 0);
    rig.say(1, 0, 10);
    CHECK(rig.producer->recipients() == 0);
}

TEST_CASE("observe: cause names the subscriber's own settle-requested send, never another's") {
    loom::WeaveId sid{};
    loom::WeaveId other_id{};
    loom::Fence mine{};
    loom::Fence theirs{};
    Rig rig(allow_all(), [&](loom::Fence f) -> std::optional<ob::FenceOrigin> {
        if (f.value == mine.value) {
            return ob::FenceOrigin{sid, 41};
        }
        if (f.value == theirs.value) {
            return ob::FenceOrigin{other_id, 42};
        }
        return std::nullopt;
    });
    Subscriber* sub = rig.subscriber(&sid, /*commands=*/true);
    (void)rig.subscriber(&other_id, /*commands=*/true);
    rig.ask(sid, rig.want({tick_ref()}));
    // The host speaks for the subscriber, fenced, to the producer: what that sets in motion is its.
    (void)rig.bus.send_as_fenced(sid, rig.producer_id,
                                 loom::Message(loom::to_value(ObsSay{1, 0, 1}), sid, loom::WeaveId{}, 41),
                                 &mine);
    rig.bus.drain_until_idle();
    (void)rig.bus.send_as_fenced(other_id, rig.producer_id,
                                 loom::Message(loom::to_value(ObsSay{1, 0, 2}), other_id,
                                               loom::WeaveId{}, 42),
                                 &theirs);
    rig.bus.drain_until_idle();
    rig.say(1, 0, 3); // no fence at all
    REQUIRE(sub->observed.size() == 3);
    CHECK(sub->observed[0].cause == 41);
    CHECK(sub->observed[1].cause == 0); // another session's send is never named
    CHECK(sub->observed[2].cause == 0);
    rig.bus.release_fence(mine);
    rig.bus.release_fence(theirs);
}

TEST_CASE("observe: a subscriber holds a bounded number of subscriptions, and is told so") {
    Rig rig(allow_all());
    loom::WeaveId sid{};
    Subscriber* sub = rig.subscriber(&sid);
    for (std::size_t i = 0; i < ob::kMaxPerSubscriber; ++i) {
        rig.ask(sid, rig.want({tick_ref()}));
    }
    CHECK(sub->subscribed.size() == ob::kMaxPerSubscriber);
    rig.ask(sid, rig.want({tick_ref()}));
    REQUIRE(sub->refused.size() == 1);
    CHECK(sub->refused[0].find("release one first") != std::string::npos);
    rig.ask(sid, rig.want({tick_ref()}, ob::kMaxWindow * 4));
    CHECK(sub->refused.size() == 2); // still over the bound, whatever it asked
}

TEST_CASE("observe: a window asked past the maximum is granted the maximum, and says so") {
    Rig rig(allow_all());
    loom::WeaveId sid{};
    Subscriber* sub = rig.subscriber(&sid);
    rig.ask(sid, rig.want({tick_ref()}, ob::kMaxWindow * 4));
    REQUIRE(sub->subscribed.size() == 1);
    CHECK(sub->subscribed[0].window == ob::kMaxWindow);
}

TEST_CASE("observe: a new holder of the office is told by its own id") {
    Rig rig(allow_all());
    loom::WeaveId sid{};
    Subscriber* sub = rig.subscriber(&sid);
    rig.ask(sid, rig.want({tick_ref()}));
    rig.say(1);
    // The office changes hands: the old holder leaves, a new weave takes the role.
    (void)rig.bus.unregister_weave(rig.producer_id);
    auto next = std::make_unique<Producer>();
    Producer* raw = next.get();
    const loom::WeaveId next_id =
        rig.bus.register_weave(std::move(next), loom::Grant{}.allow_any(), std::string("test.producer"));
    raw->zen_set_self(next_id);
    (void)rig.bus.send(next_id, loom::Message(loom::to_value(ObsSay{1, 0, 50})));
    rig.bus.drain_until_idle();
    REQUIRE(sub->observed.size() == 2);
    CHECK(sub->observed[0].producer == static_cast<std::int64_t>(rig.producer_id.value));
    CHECK(sub->observed[1].producer == static_cast<std::int64_t>(next_id.value));
    CHECK(sub->observed[1].producer != sub->observed[0].producer);
}

TEST_CASE("observe: the answer carries descriptors a stranger decodes a payload with, in either encoding") {
    Rig rig(allow_all());
    loom::WeaveId sid{};
    Subscriber* sub = rig.subscriber(&sid);
    ob::Subscribe compat = rig.want({tick_ref()});
    compat.encoding = ob::kEncodingCompat;
    rig.ask(sid, compat);
    rig.ask(sid, rig.want({tick_ref()}));
    REQUIRE(sub->subscribed.size() == 2);
    rig.say(1, 0, 77);
    REQUIRE(sub->observed.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        const ob::Subscribed& s = sub->subscribed[i];
        const bool as_compat = s.encoding == ob::kEncodingCompat;
        CHECK(as_compat == (i == 0));
        const std::string shapes(s.shapes.begin(), s.shapes.end());
        loom::Unverified u = as_compat ? loom::compat::parse(shapes) : loom::parse(shapes);
        loom::Admission a = loom::admit(u, ob::shapes_schema());
        REQUIRE(a.ok());
        loom::Registry deps;
        const auto schemas = ob::decode_shapes(a.value(), deps); // a registry that never met ObsTick
        REQUIRE(schemas.size() == 1);
        CHECK(schemas[0]->name() == "ObsTick");
        const ob::Observed* o = nullptr;
        for (const ob::Observed& each : sub->observed) {
            if (each.subscription == s.subscription) {
                o = &each;
            }
        }
        REQUIRE(o != nullptr);
        const std::string payload(o->payload.begin(), o->payload.end());
        loom::Admission p = loom::admit(as_compat ? loom::compat::parse(payload) : loom::parse(payload),
                                        schemas[0]);
        REQUIRE(p.ok());
        CHECK(p.value().get("n")->as_int() == 77);
    }
}

TEST_CASE("observe: Status answers the asker's own subscriptions only") {
    Rig rig(allow_all());
    loom::WeaveId a_id{};
    loom::WeaveId b_id{};
    Subscriber* a = rig.subscriber(&a_id);
    Subscriber* b = rig.subscriber(&b_id);
    rig.ask(a_id, rig.want({tick_ref()}));
    rig.ask(b_id, rig.want({tick_ref()}));
    rig.ask(b_id, rig.want({tick_ref()}));
    rig.ask(a_id, ob::StatusRequested{});
    REQUIRE(a->statuses.size() == 1);
    REQUIRE(a->statuses[0].rows.size() == 1);
    CHECK(a->statuses[0].rows[0].subscriber == static_cast<std::int64_t>(a_id.value));
    CHECK(a->statuses[0].relay == rig.relay->lifetime());
    CHECK(rig.relay->rows().size() == 3);
    (void)b;
}

} // TEST_SUITE("observe")
