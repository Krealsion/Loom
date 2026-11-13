// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// Suite `ask`: an ask is followed to its answer or to one `zen.AskEnded`, told to the exact
// incarnation that asked (ANS-08). One case for each way an ask ends
// (docs/reference/messaging.md#asks-and-how-each-one-ends), in process; the loaded and boxed
// askers are suite `ask_loaded`'s (test_ask_loaded.cpp).

#include "switchboard_fixtures.hpp"
#include "weavelib/ask_protocol.hpp"

#include <zen/console/console.hpp>
#include <zen/host/lifecycle_wiring.hpp>
#include <zen/serialize.hpp>
#include <zen/weave.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/weave/role_request.hpp>

#include <doctest.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace loom;
using namespace sbfx;
using namespace asking;

namespace {

constexpr const char* kService = "ask.service";
constexpr const char* kAuthor = "ask.author";
constexpr std::uint64_t kCorrelation = 77;

Value question(std::int64_t n) { return to_value(AskQuestion{n}); }
Value reply(std::int64_t n) { return to_value(AskReply{n}); }

struct Told {
    AskEnded notice;
    std::uint64_t correlation = 0;
    bool attested = false;
};

/// An in-process asker: a probe that, when the host sends it a `Greet`, runs `act` on its own
/// bus, and keeps every ending, answer and dispatch refusal it is told.
struct Asker {
    Registered r{};
    std::vector<Told> told;
    std::size_t dispatch_refusals = 0;
    std::size_t answers = 0;
    Ticket ticket{};
    std::function<Ticket(Bus&)> act;
    std::function<void(const Message&, Bus&)> on_told;
};

std::unique_ptr<Asker> make_asker(Switchboard& sb, bool hears = true, std::string role = kAuthor,
                                  Grant grant = Grant{}.allow_any()) {
    auto a = std::make_unique<Asker>();
    std::vector<std::shared_ptr<const Schema>> doors{greet_schema(), schema_of<AskReply>(),
                                                     schema_of<DispatchRefused>()};
    if (hears) {
        doors.push_back(schema_of<AskEnded>());
    }
    auto w = std::make_unique<ProbeWeave>(std::move(doors));
    ProbeWeave* raw = w.get();
    a->r = {sb.register_weave(std::move(w), std::move(grant), std::move(role)), raw};
    Asker* self = a.get();
    raw->on_handle = [self](const Message& in, Bus& b, ProbeWeave&) {
        const std::string name = in.payload.schema().name();
        if (name == greet_schema()->name()) {
            if (self->act) {
                self->ticket = self->act(b);
            }
        } else if (name == AskEnded::zen_name) {
            self->told.push_back(
                {from_value<AskEnded>(in.payload), in.correlation, in.provenance.ask_ended()});
            if (self->on_told) {
                self->on_told(in, b);
            }
        } else if (name == DispatchRefused::zen_name && in.provenance.dispatch_refused()) {
            ++self->dispatch_refusals;
        } else if (name == AskReply::zen_name && in.provenance.answers_ask()) {
            ++self->answers;
        }
    };
    return a;
}

Registered respondent(Switchboard& sb, std::string role = kService,
                      std::vector<std::shared_ptr<const Schema>> doors = {
                          schema_of<AskQuestion>(), greet_schema()},
                      Grant grant = Grant{}.allow_any()) {
    auto w = std::make_unique<ProbeWeave>(std::move(doors), 4, true);
    ProbeWeave* raw = w.get();
    return {sb.register_weave(std::move(w), std::move(grant), std::move(role)), raw};
}

/// One of the four ask forms: 0 directed, 1 to the role, 2 directed as `kAuthor`, 3 to the role
/// as `kAuthor`.
Ticket ask_form(Bus& b, int form, WeaveId target, Value v) {
    Message m(std::move(v), WeaveId{}, WeaveId{}, kCorrelation);
    switch (form) {
    case 0:
        return b.ask(target, std::move(m));
    case 1:
        return b.ask_role(kService, std::move(m));
    case 2:
        return b.role_ask(kAuthor, target, std::move(m));
    default:
        return b.role_ask_role(kAuthor, kService, std::move(m));
    }
}

/// The host nudges the asker, whose handler asks.
void kick(Switchboard& sb, const Asker& a) {
    sb.send(a.r.id, Message(greet("ask")));
    REQUIRE(sb.pump_pending() == 1);
}

void check_told(const Asker& a, const char* stage, const char* reason, int form, WeaveId target) {
    REQUIRE(a.told.size() == 1);
    const Told& t = a.told[0];
    CHECK(t.attested);
    CHECK(t.notice.ended_attempt().seq == a.ticket.seq);
    CHECK(std::string(stage_name(t.notice.stage)) == stage);
    CHECK(reason_name(t.notice.stage) == reason);
    CHECK(t.notice.shape == AskQuestion::zen_name);
    CHECK(t.notice.version == 1);
    CHECK(t.correlation == kCorrelation);
    CHECK(t.notice.role == (form % 2 ? kService : ""));
    CHECK(t.notice.addressed_weave() == (form % 2 ? WeaveId{} : target));
    CHECK(a.answers == 0);
    CHECK(a.dispatch_refusals == 0); // an ask is told this notice and not that one
}

std::size_t refusals_of(const std::vector<TapRecord>& tap, RefusalReason why) {
    std::size_t n = 0;
    for (const TapRecord& r : tap) {
        n += r.kind == EventKind::Refused && r.reason == why ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_SUITE("ask") {
    TEST_CASE("an ask from a weave that cannot hear its end is refused at the call, in all four "
              "forms, queueing nothing and never sent as a plain send") {
        Switchboard sb;
        std::vector<TapRecord> tap;
        sb.add_observer([&](const BusEvent& e) { tap.push_back(to_record(e)); });
        auto a = make_asker(sb, /*hears=*/false);
        Registered svc = respondent(sb);
        for (int form = 0; form < 4; ++form) {
            CAPTURE(form);
            a->act = [&](Bus& b) {
                const Ticket t = ask_form(b, form, svc.id, question(1));
                CHECK(sb.pending() == 0); // nothing queued, not even for later
                return t;
            };
            kick(sb, *a);
            CHECK_FALSE(a->ticket.valid());
            CHECK(a->ticket.refusal == RefusalReason::AskEndedNotAccepted);
        }
        sb.drain_until_idle();
        CHECK(svc.weave->handled_names.empty());
        CHECK(refusals_of(tap, RefusalReason::AskEndedNotAccepted) == 4);
        CHECK(std::string(name_of(RefusalReason::AskEndedNotAccepted)) == "AskEndedNotAccepted");
    }

    TEST_CASE("each pre-handler refusal ends the ask `queued`, told once, in all four forms") {
        static const char* reasons[] = {"CapabilityDenied", "NoSuchTarget", "TargetUnavailable",
                                        "NotAccepted", "GateRefused"};
        for (int form = 0; form < 4; ++form) {
            for (int reason = 0; reason < 5; ++reason) {
                CAPTURE(form);
                CAPTURE(reason);
                Switchboard sb;
                Grant grant;
                Registered svc = respondent(
                    sb, reason == 1 ? "" : kService,
                    reason == 3 ? std::vector{greet_schema()}
                                : std::vector{schema_of<AskQuestion>(), greet_schema()});
                if (reason != 0) {
                    grant.allow(AskQuestion::zen_name, 1, svc.id)
                        .allow_to_role(AskQuestion::zen_name, 1, kService);
                }
                auto a = make_asker(sb, true, kAuthor, std::move(grant));
                if (reason == 2) {
                    sb.kill(svc.id);
                }
                if (reason == 1 && form % 2 == 0) {
                    REQUIRE(sb.unregister_weave(svc.id));
                }
                a->act = [&](Bus& b) {
                    return ask_form(b, form, svc.id,
                                    reason == 4 ? Value(schema_of<AskQuestion>()) : question(7));
                };
                kick(sb, *a);
                REQUIRE(a->ticket.valid());
                sb.drain_until_idle();
                check_told(*a, "queued", reasons[reason], form, svc.id);
                CHECK(a->told[0].notice.never_delivered());
                CHECK(sb.pending() == 0);
            }
        }
    }

    TEST_CASE("an ask whose asker is retired before its delivery ends `queued`, AskerSealed") {
        Switchboard sb;
        Registered coordinator = register_probe(sb, {pong_schema()});
        Registered svc = respondent(sb);
        auto a = make_asker(sb, true, "");
        a->act = [&](Bus& b) { return ask_form(b, 0, svc.id, question(1)); };
        kick(sb, *a);
        REQUIRE(sb.pending() == 1);
        REQUIRE(sb.seal_weave(a->r.id, coordinator.id));
        sb.drain_until_idle();
        check_told(*a, "queued", "AskerSealed", 0, svc.id);
        CHECK(svc.weave->handled_names.empty());
    }

    TEST_CASE("an ask a handler returns from with its right neither spent nor kept ends "
              "`delivered`: Unanswered, or HandlerFailed told before the exception leaves") {
        for (int form = 0; form < 4; ++form) {
            CAPTURE(form);
            Switchboard sb;
            Registered svc = respondent(sb);
            auto a = make_asker(sb);
            a->act = [&](Bus& b) { return ask_form(b, form, svc.id, question(1)); };
            kick(sb, *a);
            sb.drain_until_idle();
            check_told(*a, "delivered", "Unanswered", form, svc.id);
            CHECK_FALSE(a->told[0].notice.never_delivered());
        }
        Switchboard sb;
        Registered svc = respondent(sb);
        svc.weave->on_handle = [](const Message&, Bus&, ProbeWeave&) {
            throw std::runtime_error("the respondent did not finish");
        };
        auto a = make_asker(sb);
        a->act = [&](Bus& b) { return ask_form(b, 0, svc.id, question(1)); };
        kick(sb, *a);
        CHECK_THROWS_AS(sb.drain_until_idle(), std::runtime_error);
        CHECK(sb.pending() == 1); // the notice was queued before the exception left
        sb.drain_until_idle();
        check_told(*a, "delivered", "HandlerFailed", 0, svc.id);
    }

    TEST_CASE("an answered ask is told nothing more, by either answer door") {
        for (const bool deferred : {false, true}) {
            CAPTURE(deferred);
            Switchboard sb;
            Registered svc = respondent(sb);
            svc.weave->on_handle = [deferred](const Message& in, Bus& b, ProbeWeave& self) {
                if (in.payload.schema().name() == AskQuestion::zen_name) {
                    if (deferred) {
                        self.pending = b.make_deferred_answer();
                    } else {
                        CHECK(b.answer(Message(reply(1))).valid());
                    }
                } else {
                    CHECK(b.spend_deferred(self.pending, Message(reply(2))).valid());
                }
            };
            auto a = make_asker(sb);
            a->act = [&](Bus& b) { return ask_form(b, 1, svc.id, question(1)); };
            kick(sb, *a);
            sb.drain_until_idle();
            if (deferred) {
                REQUIRE(svc.weave->pending.valid());
                CHECK(a->answers == 0);
                sb.send(svc.id, Message(greet("spend")));
                sb.drain_until_idle();
            }
            CHECK(a->answers == 1);
            CHECK(a->told.empty());
            CHECK(sb.pending() == 0);
        }
    }

    TEST_CASE("a kept answer that ends unspent ends the ask `deferred`, with each reason") {
        static const char* reasons[] = {"Released", "RespondentReplaced", "RespondentRemoved",
                                        "RespondentDied", "RespondentRetired"};
        for (int reason = 0; reason < 5; ++reason) {
            CAPTURE(reason);
            Switchboard sb;
            Registered svc = respondent(sb);
            svc.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave& self) {
                if (in.payload.schema().name() == AskQuestion::zen_name) {
                    self.pending = b.make_deferred_answer();
                } else {
                    b.release_deferred(self.pending);
                }
            };
            auto a = make_asker(sb);
            a->act = [&](Bus& b) { return ask_form(b, 0, svc.id, question(1)); };
            kick(sb, *a);
            sb.drain_until_idle();
            REQUIRE(svc.weave->pending.valid());
            REQUIRE(a->told.empty()); // kept: nothing has ended yet
            switch (reason) {
            case 0:
                sb.send(svc.id, Message(greet("let go")));
                break;
            case 1: {
                Value fresh(counter_schema());
                fresh.set("count", Cell::integer(0));
                REQUIRE(sb.swap_state(svc.id, serialize(fresh)).revived);
                break;
            }
            case 2:
                REQUIRE(sb.unregister_weave(svc.id));
                break;
            case 3:
                sb.kill(svc.id);
                break;
            default: {
                Registered coordinator = register_probe(sb, {pong_schema()});
                Registered cand = respondent(
                    sb, "", {schema_of<Activated>(), schema_of<AskQuestion>(), greet_schema()});
                REQUIRE(sb.seal_weave(cand.id, coordinator.id));
                REQUIRE(sb.admit_candidate(cand.id, svc.id, kService,
                                           host_lifecycle_authority(sb),
                                           Message(to_value(Activated{2})), 2)
                            .scheduled);
                break;
            }
            }
            sb.drain_until_idle();
            check_told(*a, "deferred", reasons[reason], 0, svc.id);
            CHECK(sb.pending() == 0);
        }
    }

    TEST_CASE("an answer Loom refuses ends the ask `answered`, by either answer door") {
        for (const bool deferred : {false, true}) {
            CAPTURE(deferred);
            Switchboard sb;
            Registered svc = respondent(sb);
            // A Pong the asker does not accept: the answer is made, and refused NotAccepted.
            svc.weave->on_handle = [deferred](const Message& in, Bus& b, ProbeWeave& self) {
                if (in.payload.schema().name() == AskQuestion::zen_name && !deferred) {
                    CHECK(b.answer(Message(pong(1))).valid());
                } else if (in.payload.schema().name() == AskQuestion::zen_name) {
                    self.pending = b.make_deferred_answer();
                } else {
                    CHECK(b.spend_deferred(self.pending, Message(pong(2))).valid());
                }
            };
            auto a = make_asker(sb);
            a->act = [&](Bus& b) { return ask_form(b, 0, svc.id, question(1)); };
            kick(sb, *a);
            sb.drain_until_idle();
            if (deferred) {
                sb.send(svc.id, Message(greet("spend")));
                sb.drain_until_idle();
            }
            check_told(*a, "answered", "AnswerRefused", 0, svc.id);
        }
    }

    TEST_CASE("an ending is told to the exact asker: not to its reloaded code, not to its revived "
              "life") {
        // 0: the asker's code is replaced while its ask waits in the queue. A reload keeps the
        // life, so the ask is delivered, and its handler returns without answering: the ending is
        // made, for an incarnation that is gone. 1: the same with the asker unchanged, the canary.
        // 2: the asker dies and is revived while a respondent keeps its answer: the right ends
        // with the life that asked, and the release finds nothing to tell.
        for (int how = 0; how < 3; ++how) {
            CAPTURE(how);
            Switchboard sb;
            Registered svc = respondent(sb);
            svc.weave->on_handle = [how](const Message& in, Bus& b, ProbeWeave& self) {
                if (how == 2 && in.payload.schema().name() == AskQuestion::zen_name) {
                    self.pending = b.make_deferred_answer();
                } else if (how == 2) {
                    b.release_deferred(self.pending);
                }
            };
            auto a = make_asker(sb);
            a->act = [&](Bus& b) { return ask_form(b, 0, svc.id, question(1)); };
            kick(sb, *a);
            const std::string state = sb.snapshot_bytes(a->r.id);
            if (how == 0) {
                REQUIRE(sb.swap_state(a->r.id, state).revived);
            }
            sb.drain_until_idle();
            REQUIRE(svc.weave->handled_names.size() == 1); // the ask was delivered
            if (how == 2) {
                REQUIRE(svc.weave->pending.valid());
                sb.kill(a->r.id);
                REQUIRE(sb.reload(a->r.id, state).revived);
                sb.send(svc.id, Message(greet("let go")));
                sb.drain_until_idle();
            }
            if (how == 1) {
                check_told(*a, "delivered", "Unanswered", 0, svc.id);
            } else {
                CHECK(a->told.empty());
            }
            CHECK(sb.pending() == 0);
        }
    }

    TEST_CASE("an ending the respondent's reload makes is told to the asker; its successor's book "
              "is not") {
        // The respondent's new code ends what the old code kept: RespondentReplaced, above. The
        // asker's own new code is the other side: told nothing, since the exact asker is gone.
        Switchboard sb;
        Registered svc = respondent(sb);
        svc.weave->on_handle = [](const Message& in, Bus& b, ProbeWeave& self) {
            if (in.payload.schema().name() == AskQuestion::zen_name) {
                self.pending = b.make_deferred_answer();
            }
        };
        auto a = make_asker(sb);
        a->act = [&](Bus& b) { return ask_form(b, 0, svc.id, question(1)); };
        kick(sb, *a);
        sb.drain_until_idle();
        REQUIRE(sb.swap_state(a->r.id, sb.snapshot_bytes(a->r.id)).revived);
        Value fresh(counter_schema());
        fresh.set("count", Cell::integer(0));
        REQUIRE(sb.swap_state(svc.id, serialize(fresh)).revived);
        sb.drain_until_idle();
        CHECK(a->told.empty());
    }

    TEST_CASE("a plain send is told only zen.DispatchRefused, as before the ask door") {
        Switchboard sb;
        auto a = make_asker(sb);
        a->act = [&](Bus& b) {
            return b.send(WeaveId{4242}, Message(question(1), WeaveId{}, WeaveId{}, kCorrelation));
        };
        kick(sb, *a);
        sb.drain_until_idle();
        CHECK(a->dispatch_refusals == 1);
        CHECK(a->told.empty());
    }

    TEST_CASE("each respondent keeps its own bound of answers: one at its bound leaves another "
              "free to keep one") {
        Switchboard sb;
        std::vector<TapRecord> tap;
        sb.add_observer([&](const BusEvent& e) { tap.push_back(to_record(e)); });
        std::vector<DeferredAnswer> kept_a;
        std::vector<DeferredAnswer> kept_b;
        Registered hoard_a = respondent(sb, "");
        Registered hoard_b = respondent(sb, "");
        hoard_a.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
            kept_a.push_back(b.make_deferred_answer());
        };
        hoard_b.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
            kept_b.push_back(b.make_deferred_answer());
        };
        Registered asker = register_probe(sb, {schema_of<AskReply>()});
        for (std::size_t i = 0; i <= Switchboard::kMaxDeferredPerWeave; ++i) {
            sb.send_as(asker.id, hoard_a.id, Message(question(static_cast<std::int64_t>(i))));
        }
        sb.drain_until_idle();
        REQUIRE(kept_a.size() == Switchboard::kMaxDeferredPerWeave + 1);
        for (std::size_t i = 0; i < Switchboard::kMaxDeferredPerWeave; ++i) {
            REQUIRE(kept_a[i].valid());
        }
        CHECK_FALSE(kept_a.back().valid()); // its own bound, refused visibly...
        CHECK(refusals_of(tap, RefusalReason::Exhausted) == 1);
        sb.send_as(asker.id, hoard_b.id, Message(question(1)));
        sb.drain_until_idle();
        REQUIRE(kept_b.size() == 1);
        CHECK(kept_b[0].valid()); // ...and nobody else's
        CHECK(refusals_of(tap, RefusalReason::Exhausted) == 1);
    }

    TEST_CASE("Loom's notice grants nothing, and the shape alone is ordinary speech") {
        Switchboard sb;
        Registered svc = respondent(sb);
        auto a = make_asker(sb);
        bool answered = true;
        bool kept = true;
        a->on_told = [&](const Message&, Bus& b) {
            answered = b.answer(Message(reply(1))).valid();
            kept = b.make_deferred_answer().valid();
        };
        a->act = [&](Bus& b) { return ask_form(b, 0, svc.id, question(1)); };
        kick(sb, *a);
        sb.drain_until_idle();
        REQUIRE(a->told.size() == 1);
        CHECK_FALSE(answered);
        CHECK_FALSE(kept);
        // A weave granted the shape may send one; it arrives with no attestation.
        Registered forger = register_probe(sb, {pong_schema()});
        AskEnded forged = a->told[0].notice;
        sb.send_as(forger.id, a->r.id, Message(to_value(forged)));
        sb.drain_until_idle();
        REQUIRE(a->told.size() == 2);
        CHECK_FALSE(a->told[1].attested);
    }

    TEST_CASE("speech the incumbent authored while it was the service and still queued at the "
              "switch is refused SealedSpeech: a send, an answer, its asker told, and a "
              "publication") {
        // Queue at the admission: [the activation, the incumbent's question to the role, its
        // answer, its publication's copy]. The admission goes ahead of the first envelope that
        // could reach the candidate, which is ahead of the rest the incumbent said as the service.
        Switchboard sb;
        std::vector<TapRecord> tap;
        sb.add_observer([&](const BusEvent& e) { tap.push_back(to_record(e)); });
        Registered listener = register_probe(sb, {pong_schema()});
        Registered svc = respondent(sb);
        Ticket answer_ticket;
        svc.weave->on_handle = [&](const Message& in, Bus& b, ProbeWeave&) {
            if (in.payload.schema().name() == AskQuestion::zen_name &&
                in.payload.get("n")->as_int() == 1) {
                // Something for the role first, then the answer and a publication behind it.
                CHECK(b.send_to_role(kService, Message(question(9))).valid());
                answer_ticket = b.answer(Message(reply(1)));
                CHECK(b.publish(Message(pong(5))) == 1);
            }
        };
        Registered coordinator = register_probe(sb, {tick_schema()});
        Registered cand =
            respondent(sb, "", {schema_of<Activated>(), schema_of<AskQuestion>(), greet_schema()});
        REQUIRE(sb.seal_weave(cand.id, coordinator.id));
        auto a = make_asker(sb);
        a->act = [&](Bus& b) { return ask_form(b, 1, svc.id, question(1)); };
        kick(sb, *a);                    // [ask]
        REQUIRE(sb.pump_pending() == 1); // [question, answer, pong copy]
        REQUIRE(answer_ticket.valid());
        REQUIRE(sb.admit_candidate(cand.id, svc.id, kService, host_lifecycle_authority(sb),
                                   Message(to_value(Activated{2})), 2)
                    .scheduled);
        sb.drain_until_idle();
        CHECK(sb.outcome(answer_ticket).refusal.reason == RefusalReason::SealedSpeech);
        // The send to the role, the answer and the copy: all three, as they are before T1.
        CHECK(refusals_of(tap, RefusalReason::SealedSpeech) == 3);
        CHECK(listener.weave->handled_names.empty());
        CHECK(cand.weave->handled_names == std::vector<std::string>{Activated::zen_name});
        check_told(*a, "answered", "AnswerRefused", 1, svc.id);
    }

    TEST_CASE("an asker's book closes the record a told ending names, and only on Loom's word") {
        AskBook book(4);
        const AskOpened directed = book.open(WeaveId{5}, AskQuestion::zen_name, 1);
        const AskOpened to_role = book.open_to_role(kService, AskQuestion::zen_name, 1);
        REQUIRE(book.bind_attempt(directed.id, 41));
        REQUIRE(book.bind_attempt(to_role.id, 42));
        AskEnded ended;
        ended.attempt = "41";
        ended.target = "5";
        ended.shape = AskQuestion::zen_name;
        ended.version = 1;
        ended.stage.is = AskNotAnswered::Unanswered;
        const Provenance loom_said = Provenance::attested(Provenance::Kind::AskEnded, 0);
        CHECK_FALSE(book.end(ended, Provenance{}));      // the shape alone closes nothing
        AskEnded elsewhere = ended;
        elsewhere.target = "6";
        CHECK_FALSE(book.end(elsewhere, loom_said));     // another address is another ask
        const std::optional<PendingAsk> closed = book.end(ended, loom_said);
        REQUIRE(closed);
        CHECK(closed->id == directed.id);
        CHECK_FALSE(book.end(ended, loom_said));         // once
        CHECK(book.waiting_on(to_role.id));
        const std::vector<PendingAsk> inherited = book.end_all();
        REQUIRE(inherited.size() == 1);
        CHECK(inherited[0].id == to_role.id);
        CHECK_FALSE(book.awaiting());
    }

    TEST_CASE("a role request asked through the ask door matches its ending, not a refusal") {
        struct Asks {
            Ticket ask_role(const std::string&, const AskQuestion&, std::uint64_t) {
                return Ticket{9};
            }
        };
        RoleRequest request;
        REQUIRE(request.ask_role(Asks{}, kService, AskQuestion{1}, kCorrelation));
        AskEnded ended;
        ended.attempt = "9";
        ended.role = kService;
        ended.shape = AskQuestion::zen_name;
        ended.version = 1;
        ended.stage.is = AskNotDelivered::NoSuchTarget;
        Switchboard sb;
        Message attested(to_value(ended));
        attested.provenance = Provenance::attested(Provenance::Kind::AskEnded, 0);
        CHECK(request.matches_ending(ended, Mail(sb, attested, WeaveId{1})));
        CHECK_FALSE(request.matches_ending(ended, Mail(sb, Message(to_value(ended)), WeaveId{1})));
        ended.attempt = "8";
        CHECK_FALSE(request.matches_ending(ended, Mail(sb, attested, WeaveId{1})));
    }

    TEST_CASE("a console conversation asked through the ask door is held ENDED for its caller, "
              "and taking it frees the slot") {
        Switchboard sb;
        ConsoleEngine engine(sb);
        Registered svc = respondent(sb); // returns without answering
        std::string err;
        const Submitted asked =
            engine.submit(svc.id, AskQuestion::zen_name, 1, {{"n", std::int64_t{1}}},
                          ConsoleTracking::Asked, &err);
        REQUIRE_MESSAGE(asked.sent(), err);
        REQUIRE(asked.ask != 0);
        engine.pump();
        sb.drain_until_idle();
        CHECK_FALSE(engine.awaiting(asked.ask));
        const std::optional<BufferEntry> held = engine.settled(asked.ask);
        REQUIRE(held);
        CHECK(held->ask_ended);
        CHECK_FALSE(held->answers_ask);
        const AskEnded ended = from_value<AskEnded>(held->value);
        CHECK(reason_name(ended.stage) == "Unanswered");
        CHECK(ended.ended_attempt().seq == asked.ticket.seq);
        REQUIRE(engine.take_settled(asked.ask));
        CHECK(engine.asks_held() == 0);
    }
}
