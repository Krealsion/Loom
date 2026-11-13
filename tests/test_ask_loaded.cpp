// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// Suite `ask_loaded`: the ask door across the seam (ANS-08). The asker probe (weavelib/
// ask_probe.cpp), loaded, asks a native respondent each way an ask can end and keeps what Loom
// told it; the same body runs on the probe built as a module, as cases of suite `box`. And an
// image built against ABI 14's kept header, which has no ask door, loads and works here.

#include "box_twin.hpp"
#include "switchboard_fixtures.hpp"
#include "weavelib/addressed_protocol.hpp"
#include "weavelib/ask_protocol.hpp"

#include <zen/host/lifecycle_wiring.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/serialize.hpp>
#include <zen/weave.hpp>
#include <zen/weave/lifecycle.hpp>

#include <doctest.h>

#include <stdexcept>
#include <string>

using namespace loom;
using namespace sbfx;
using namespace asking;

namespace {

constexpr const char* kService = "ask.service";
constexpr const char* kAuthor = "ask.author";

AskHeard heard_by(Switchboard& sb, WeaveId id) {
    return from_value<AskHeard>(sb.weave(id)->snapshot());
}

/// How the native respondent ends what it is asked.
enum class Ends { Unanswered, Throws, Keeps, AnswersPong, Answers };

Registered respondent(Switchboard& sb, Ends how, std::string role = kService) {
    auto w = std::make_unique<ProbeWeave>(
        std::vector<std::shared_ptr<const Schema>>{schema_of<AskQuestion>(), greet_schema()}, 4,
        true);
    ProbeWeave* raw = w.get();
    raw->on_handle = [how](const Message& in, Bus& b, ProbeWeave& self) {
        if (in.payload.schema().name() != AskQuestion::zen_name) {
            b.release_deferred(self.pending); // the host's nudge: let a kept answer go
            return;
        }
        switch (how) {
        case Ends::Unanswered:
            break;
        case Ends::Throws:
            throw std::runtime_error("the respondent did not finish");
        case Ends::Keeps:
            self.pending = b.make_deferred_answer();
            break;
        case Ends::AnswersPong:
            (void)b.answer(Message(pong(1))); // a shape the asker does not accept
            break;
        case Ends::Answers:
            (void)b.answer(Message(to_value(AskReply{1})));
            break;
        }
    };
    return {sb.register_weave(std::move(w), Grant{}.allow_any(), std::move(role)), raw};
}

void kick(Switchboard& sb, WeaveId asker, std::int64_t form, WeaveId target) {
    sb.send(asker, Message(to_value(AskKick{form, static_cast<std::int64_t>(target.value),
                                            kService, kAuthor, 1, 77})));
}

void drain_through(Switchboard& sb) {
    try {
        sb.drain_until_idle();
    } catch (const std::runtime_error&) {
        sb.drain_until_idle(); // a respondent that throws; its ending is queued before it leaves
    }
}

void check_heard(const AskHeard& h, const char* stage, const char* reason, std::int64_t form,
                 WeaveId target) {
    CHECK(h.refusal.empty());
    REQUIRE(h.ticket != 0);
    CHECK(h.endings == 1);
    CHECK(h.forged == 0);
    CHECK(h.stage == stage);
    CHECK(h.reason == reason);
    CHECK(h.ended_attempt == h.ticket);
    CHECK(h.correlation == 77);
    CHECK(h.shape == AskQuestion::zen_name);
    CHECK(h.role == (form % 2 ? kService : ""));
    CHECK(h.target == (form % 2 ? 0 : static_cast<std::int64_t>(target.value)));
    CHECK(h.answers == 0);
}

/// Each way an ask ends, the asker loaded from `image`; the respondent is native.
void a_loaded_asker_is_told_each_ending(const char* image) {
    struct Row {
        const char* stage;
        const char* reason;
        Ends how;
        int after; // 0 nothing; 1 release; 2 reload; 3 remove; 4 kill; 5 the switch
        bool missing = false;
    };
    const Row rows[] = {
        {"queued", "NoSuchTarget", Ends::Unanswered, 0, true},
        {"delivered", "Unanswered", Ends::Unanswered, 0},
        {"delivered", "HandlerFailed", Ends::Throws, 0},
        {"deferred", "Released", Ends::Keeps, 1},
        {"deferred", "RespondentReplaced", Ends::Keeps, 2},
        {"deferred", "RespondentRemoved", Ends::Keeps, 3},
        {"deferred", "RespondentDied", Ends::Keeps, 4},
        {"deferred", "RespondentRetired", Ends::Keeps, 5},
        {"answered", "AnswerRefused", Ends::AnswersPong, 0},
    };
    for (const Row& row : rows) {
        CAPTURE(row.reason);
        Switchboard sb;
        Kernel kernel(sb, fixture_admission());
        const LoadResult asker = kernel.load("asker", image, kAuthor);
        REQUIRE_MESSAGE(asker.ok, asker.error);
        Registered svc = respondent(sb, row.how);
        const WeaveId to = row.missing ? WeaveId{4242} : svc.id;
        kick(sb, asker.id, 0, to);
        drain_through(sb);
        switch (row.after) {
        case 1:
            sb.send(svc.id, Message(greet("let go")));
            break;
        case 2: {
            Value fresh(counter_schema());
            fresh.set("count", Cell::integer(0));
            REQUIRE(sb.swap_state(svc.id, serialize(fresh)).revived);
            break;
        }
        case 3:
            REQUIRE(sb.unregister_weave(svc.id));
            break;
        case 4:
            sb.kill(svc.id);
            break;
        case 5: {
            Registered coordinator = register_probe(sb, {tick_schema()});
            auto cw = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{
                schema_of<Activated>(), schema_of<AskQuestion>()});
            const WeaveId cand = sb.register_weave(std::move(cw), Grant{}.allow_any());
            REQUIRE(sb.seal_weave(cand, coordinator.id));
            REQUIRE(sb.admit_candidate(cand, svc.id, kService, host_lifecycle_authority(sb),
                                       Message(to_value(Activated{2})), 2)
                        .scheduled);
            break;
        }
        default:
            break;
        }
        drain_through(sb);
        check_heard(heard_by(sb, asker.id), row.stage, row.reason, 0, to);
    }

    // The four forms through the one door, each ending as an unanswered ask does.
    for (std::int64_t form = 1; form < 4; ++form) {
        CAPTURE(form);
        Switchboard sb;
        Kernel kernel(sb, fixture_admission());
        const LoadResult asker = kernel.load("asker", image, kAuthor);
        REQUIRE_MESSAGE(asker.ok, asker.error);
        Registered svc = respondent(sb, Ends::Unanswered);
        kick(sb, asker.id, form, svc.id);
        sb.drain_until_idle();
        check_heard(heard_by(sb, asker.id), "delivered", "Unanswered", form, svc.id);
    }

    // And an answered ask is told nothing more.
    Switchboard sb;
    Kernel kernel(sb, fixture_admission());
    const LoadResult asker = kernel.load("asker", image, kAuthor);
    REQUIRE_MESSAGE(asker.ok, asker.error);
    Registered svc = respondent(sb, Ends::Answers);
    kick(sb, asker.id, 0, svc.id);
    sb.drain_until_idle();
    const AskHeard h = heard_by(sb, asker.id);
    CHECK(h.answers == 1);
    CHECK(h.endings == 0);
}

/// A loaded asker that does not accept `zen.AskEnded` is refused at the call, in every form.
void a_loaded_deaf_asker_is_refused_at_the_call(const char* image) {
    Switchboard sb;
    Kernel kernel(sb, fixture_admission());
    const LoadResult asker = kernel.load("deaf", image, kAuthor);
    REQUIRE_MESSAGE(asker.ok, asker.error);
    Registered svc = respondent(sb, Ends::Answers);
    for (std::int64_t form = 0; form < 4; ++form) {
        CAPTURE(form);
        kick(sb, asker.id, form, svc.id);
        sb.drain_until_idle();
        const AskHeard h = heard_by(sb, asker.id);
        CHECK(h.ticket == 0);
        CHECK(h.refusal == "AskEndedNotAccepted");
    }
    CHECK(svc.weave->handled_names.empty());
}

} // namespace

TEST_SUITE("ask_loaded") {
    TEST_CASE("a loaded asker is told each way its ask ends, through the one ask door") {
        a_loaded_asker_is_told_each_ending(ZEN_SO_ASK_PROBE);
    }

    TEST_CASE("a loaded asker that cannot hear its ask end is refused at the call") {
        a_loaded_deaf_asker_is_refused_at_the_call(ZEN_SO_ASK_PROBE_DEAF);
    }

    TEST_CASE("an image built against ABI 14's header, which has no ask door, loads and works "
              "here") {
        Switchboard sb;
        Kernel kernel(sb, fixture_admission());
        const LoadResult door = kernel.load("door", ZEN_SO_ABI_V14_IMAGE, "door.a");
        REQUIRE_MESSAGE(door.ok, door.error);
        sb.send_to_role("door.a", Message(to_value(addressed::AddressedKnock{1})));
        sb.drain_until_idle();
        const auto heard = from_value<addressed::AddressedHeard>(sb.weave(door.id)->snapshot());
        CHECK(heard.knocks == 1);
        CHECK(heard.addressed == "door.a");
    }
}

ZEN_BOXED_TWIN("a boxed asker is told each way its ask ends, through the one ask door",
               a_loaded_asker_is_told_each_ending, ZEN_BOX_ASK_PROBE)
ZEN_BOXED_TWIN("a boxed asker that cannot hear its ask end is refused at the call",
               a_loaded_deaf_asker_is_refused_at_the_call, ZEN_BOX_ASK_PROBE_DEAF)
