// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/role_request.hpp>
#include <doctest.h>

namespace {
struct RequestState { ZEN_SHAPE(RequestState, 1); };
struct BeginRequests { ZEN_SHAPE(BeginRequests, 1); };
struct RequestValue { std::int64_t n = 0; ZEN_SHAPE(RequestValue, 1, ZEN_FIELD(n)); };
struct AnswerValue { std::int64_t n = 0; ZEN_SHAPE(AnswerValue, 1, ZEN_FIELD(n)); };
struct AnswerLast { std::int64_t n = 0; ZEN_SHAPE(AnswerLast, 1, ZEN_FIELD(n)); };

class Requester : public loom::WeaveBase<Requester, RequestState,
    loom::Accept<BeginRequests, AnswerValue, loom::DispatchRefused>, loom::Emit<RequestValue>> {
public:
    loom::RoleRequest first, second;
    std::uint64_t sequence = 0;
    int accepted = 0, refused = 0;
    void on(const BeginRequests&, loom::Mail& mail) {
        REQUIRE(first.send_to_role(mail, "service", RequestValue{1}, ++sequence));
        REQUIRE(second.send_to_role(mail.as_role("requester"), "service", RequestValue{2}, ++sequence));
        auto original = first.attempt();
        CHECK_FALSE(first.send_to_role(mail, "other", RequestValue{3}, ++sequence));
        CHECK(first.attempt().seq == original.seq);
    }
    void on(const AnswerValue& value, loom::Mail& mail) {
        for (auto* request : {&first, &second}) {
            if (!request->matches_answer(mail)) continue;
            if (value.n < 0) return; // Domain validation happens before forgetting.
            request->forget();
            ++accepted;
        }
    }
    void on(const loom::DispatchRefused& value, loom::Mail& mail) {
        for (auto* request : {&first, &second}) {
            if (!request->matches_refusal(value, mail)) continue;
            CHECK_FALSE(request->matches_answer(mail));
            auto changed = value;
            changed.attempt = "0";
            CHECK_FALSE(request->matches_refusal(changed, mail));
            changed = value; changed.role = "other";
            CHECK_FALSE(request->matches_refusal(changed, mail));
            changed = value; changed.target = "123";
            CHECK_FALSE(request->matches_refusal(changed, mail));
            changed = value; changed.shape = AnswerValue::zen_name;
            CHECK_FALSE(request->matches_refusal(changed, mail));
            changed = value; ++changed.version;
            CHECK_FALSE(request->matches_refusal(changed, mail));
            request->forget();
            ++refused;
        }
    }
};

class Respondent : public loom::WeaveBase<Respondent, RequestState,
    loom::Accept<RequestValue, AnswerLast>, loom::Emit<AnswerValue>> {
public:
    std::vector<loom::DeferredAnswer> held;
    void on(const RequestValue&, loom::Mail& mail) { held.push_back(mail.defer_answer()); }
    void on(const AnswerLast& answer, loom::Mail& mail) {
        REQUIRE_FALSE(held.empty());
        loom::answer_deferred(held.back(), mail, AnswerValue{answer.n});
        held.pop_back();
    }
};

struct RequestRig {
    loom::Switchboard bus;
    Requester* requester;
    Respondent* respondent;
    loom::WeaveId asker, teller;
    explicit RequestRig(bool permit = true) {
        auto a = std::make_unique<Requester>(); requester = a.get();
        loom::Grant asks;
        if (permit) asks.allow_to_role(RequestValue::zen_name, 1, "service");
        asker = bus.register_weave(std::move(a), std::move(asks), "requester");
        requester->zen_set_self(asker);
        auto r = std::make_unique<Respondent>(); respondent = r.get();
        loom::Grant answers; answers.allow_to_any(AnswerValue::zen_name, 1);
        teller = bus.register_weave(std::move(r), std::move(answers), "service");
        respondent->zen_set_self(teller);
    }
    template<class T> void deliver(loom::WeaveId to, T value, std::uint64_t correlation = 0) {
        bus.send(to, loom::Message(loom::to_value(value), {}, {}, correlation));
        bus.drain_until_idle();
    }
};
} // namespace

TEST_SUITE("ask_book") {
TEST_CASE("role requests keep two independent attempts and require authentic answers") {
    RequestRig r;
    r.deliver(r.asker, BeginRequests{});
    CHECK(r.requester->first.is<RequestValue>());
    REQUIRE(r.respondent->held.size() == 2);
    auto first = r.requester->first.correlation();
    auto second = r.requester->second.correlation();
    CHECK(first != second);
    r.bus.send_as(r.teller, r.asker, loom::Message(loom::to_value(AnswerValue{9}), {}, {}, first));
    r.bus.drain_until_idle();
    CHECK(r.requester->accepted == 0);
    r.deliver(r.teller, AnswerLast{2});
    CHECK(r.requester->first.pending());
    CHECK_FALSE(r.requester->second.pending());
    r.deliver(r.asker, AnswerValue{2}, second); // Ordinary duplicate is inert.
    CHECK(r.requester->accepted == 1);
    r.deliver(r.teller, AnswerLast{1});
    CHECK(r.requester->accepted == 2);
    CHECK_FALSE(r.requester->first.pending());
}

TEST_CASE("role request matching leaves validation and delivered silence with the owner") {
    RequestRig r;
    r.deliver(r.asker, BeginRequests{});
    r.deliver(r.teller, AnswerLast{-1});
    CHECK(r.requester->accepted == 0);
    CHECK(r.requester->first.pending());
    CHECK(r.requester->second.pending());
    CHECK(r.bus.pump_pending() == 0);
    r.requester->first.forget();
    r.deliver(r.teller, AnswerLast{1}); // Forgetting did not cancel the remote work.
    CHECK(r.requester->accepted == 0);
    CHECK(r.requester->second.pending());
}

TEST_CASE("role requests match the authenticated refused attempt and every authored address fact") {
    RequestRig r(false);
    // Start both asks, but do not dispatch them until the ordinary lookalike is queued.
    r.bus.send(r.asker, loom::Message(loom::to_value(BeginRequests{})));
    r.bus.pump_pending();
    REQUIRE(r.requester->first.pending());
    const auto fake = loom::DispatchRefused{std::to_string(r.requester->first.attempt().seq),
        {}, "service", RequestValue::zen_name, 1, "CapabilityDenied"};
    // Direct delivery with no provenance exercises the guard independently of real notices.
    loom::Message ordinary(loom::to_value(fake), {}, {}, r.requester->first.correlation());
    loom::Mail mail(r.bus, ordinary, r.asker);
    CHECK_FALSE(r.requester->first.matches_refusal(fake, mail));
    r.bus.drain_until_idle();
    CHECK(r.requester->refused == 2);
    CHECK(r.respondent->held.empty());
    CHECK_FALSE(r.requester->first.pending());
    CHECK_FALSE(r.requester->second.pending());
}
} // TEST_SUITE
