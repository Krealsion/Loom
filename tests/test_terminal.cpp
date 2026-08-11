// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// TERM-0 — an ordinary Loom weave that a person can drive, and the exact list of
// things it must never quietly become.
//
// What this suite is watching for, stated as the failures it must catch:
//
//   a terminal that speaks as the host                        (own-sender provenance)
//   a terminal that speaks as the seat beside it              (two identities, never one)
//   a session that approves its own authority request         (no self-approval)
//   a weave that decides because it can REACH the Weaver      (operator identity)
//   a transcript fed by the whole-bus tap                     (participant-local observation)
//   "submitted" quietly becoming "delivered"                  (the sender-fate seam, honestly)
//   any inbound message satisfying a pending ask              (Loom's provenance, not shape)
//   an answer landing on the wrong outstanding conversation   (Loom's correlation)
//   an approval that replays the action it authorized         (authority is not a broker)
//   a transcript that grows forever                           (bounded history)
//   a bound that costs a conversation somebody is waiting on  (state is not history)
//   an escape sequence reaching the terminal it is printed on (renderer-level safety)
//   a core that mutates what it stores to make itself safe    (values are kept verbatim)

#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/host/grant_wiring.hpp>
#include <zen/host/terminal_wiring.hpp>
#include <zen/terminal/session.hpp>
#include <zen/weave.hpp>
#include <zen/weaver/weaver.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace loom {
/// Reads the transcript windows' own SLOT COUNTS — "size() stays at the capacity" and "the
/// backing storage stops growing" are different claims, and only the second is what a session
/// left open for a week needs. The same instrument the console history uses, and deliberately a
/// separate name so a failure is unambiguous about which window grew.
struct TerminalHistoryProbe {
    static std::size_t entry_slots(const Transcript& t) { return t.entries_.ring_.capacity(); }
    static std::size_t received_slots(const Transcript& t) { return t.received_.ring_.capacity(); }
};
} // namespace loom

using namespace loom;
using namespace sbfx;

namespace {

constexpr const char* kServiceRole = "some.service";
constexpr const char* kWeaverRole = "loom.weaver";

// ---- the world these cases happen in ----------------------------------------
//
// Application-neutral scenery, exactly as the weaver suite keeps its own. Raw schemas rather
// than ZEN_SHAPE structs, because the terminal composes from a SCHEMA and never from a C++ type
// — driving a shape nothing in this file has a struct for is the point.

std::shared_ptr<const Schema> work_schema() {
    static const auto s = SchemaBuilder("Work", 1).field("n", Kind::Int).build();
    return s;
}
std::shared_ptr<const Schema> query_schema() {
    static const auto s = SchemaBuilder("Query", 1).field("q", Kind::Text).build();
    return s;
}
std::shared_ptr<const Schema> reply_schema() {
    static const auto s = SchemaBuilder("Reply", 1).field("answer", Kind::Text).build();
    return s;
}
std::shared_ptr<const Schema> notice_schema() {
    static const auto s = SchemaBuilder("Notice", 1).field("text", Kind::Text).build();
    return s;
}
/// Two same-typed fields, so the ladder's ambiguity rung has something to refuse.
std::shared_ptr<const Schema> pair_schema() {
    static const auto s =
        SchemaBuilder("Pair", 1).field("a", Kind::Int).field("b", Kind::Int).build();
    return s;
}

Value notice(std::string text) {
    Value v(notice_schema());
    v.set("text", Cell::text(std::move(text)));
    return v;
}

/// Did this local operation author a message? Spelled out rather than leaning on the result's
/// explicit bool, so a failing line names the outcome it actually got.
bool submitted(const TerminalResult& r) { return r.outcome == TerminalOutcome::Submitted; }

Arg bare(std::int64_t n) { return Arg{std::nullopt, FieldValue{n}}; }
Arg bare(std::string s) { return Arg{std::nullopt, FieldValue{std::move(s)}}; }
Arg named(std::string field, std::int64_t n) { return Arg{std::move(field), FieldValue{n}}; }

TerminalVocabulary session_vocabulary() {
    TerminalVocabulary v;
    v.knows(work_schema())
        .knows(query_schema())
        .knows(pair_schema())
        .knows(schema_of<RequestAuthority>())
        .knows(schema_of<DescribeAuthority>())
        // Known, so that trying it is a real measurement rather than a shape the terminal could
        // not spell. It is never granted.
        .knows(schema_of<ApproveAuthority>())
        .accepts(reply_schema())
        .accepts(notice_schema())
        .accepts(schema_of<AuthorityGranted>())
        .accepts(schema_of<AuthorityDescription>())
        .accepts(schema_of<Refused>())
        .accepts(schema_of<Ack>());
    return v;
}

TerminalVocabulary operator_vocabulary() {
    TerminalVocabulary v;
    v.knows(schema_of<ApproveAuthority>())
        .knows(schema_of<RefuseAuthority>())
        .knows(schema_of<RevokeAuthority>())
        .knows(schema_of<DescribeAuthority>())
        .accepts(schema_of<AuthorityPrompt>())
        .accepts(schema_of<AuthorityDescription>())
        .accepts(schema_of<Ack>())
        .accepts(schema_of<Refused>());
    return v;
}

Grant default_session_grant() {
    Grant g;
    g.allow_to_role("zen.RequestAuthority", 1, kWeaverRole);
    g.allow_to_role("zen.DescribeAuthority", 1, kWeaverRole);
    g.allow_to_role("Query", 1, kServiceRole);
    return g;
}

Grant operator_seat_grant() {
    Grant g;
    g.allow_to_role("zen.ApproveAuthority", 1, kWeaverRole);
    g.allow_to_role("zen.RefuseAuthority", 1, kWeaverRole);
    g.allow_to_role("zen.RevokeAuthority", 1, kWeaverRole);
    g.allow_to_role("zen.DescribeAuthority", 1, kWeaverRole);
    return g;
}

/// One bus event, as the HOST sees it. `sender` is the field that matters most here and is the
/// one `sbfx::TapRecord` does not carry: "who did the service actually hear from?" is the
/// assertion the whole architecture turns on.
struct Seen {
    EventKind kind;
    std::string schema;
    WeaveId sender;
    WeaveId target;
};

/// The whole cast, wired the way the standalone terminal wires it: a governed session, a
/// SEPARATE operator seat, a service in an office, and a real Weaver holding one capability.
///
/// The `tap` is the HOST's — this fixture owns the Switchboard, so it is entitled to one. It
/// exists to prove facts NEITHER participant is told, and nothing it collects is ever allowed to
/// appear in a transcript.
class Cast {
public:
    explicit Cast(Grant session_grant = default_session_grant())
        : order(std::make_shared<ObservationOrder>()) {
        bus.add_observer([this](const BusEvent& e) {
            tap.push_back(Seen{e.kind, e.schema_name, e.sender, e.target});
        });

        session = host_mount_terminal(
            bus, std::make_unique<TerminalSession>("session", session_vocabulary(), order),
            std::move(session_grant));
        seat = host_mount_terminal(
            bus, std::make_unique<TerminalSession>("operator", operator_vocabulary(), order),
            operator_seat_grant());

        auto owned = std::make_unique<ProbeWeave>(
            std::vector<std::shared_ptr<const Schema>>{work_schema(), query_schema()});
        service = owned.get();
        Grant service_grant;
        service_grant.allow_to_any("Reply", 1);
        service_id = bus.register_weave(std::move(owned), std::move(service_grant), kServiceRole);
        service->on_handle = [](const Message& in, Bus& b, ProbeWeave&) {
            if (in.payload.schema().name() == "Query") {
                Value v(reply_schema());
                v.set("answer", Cell::text("you asked: " + in.payload.get("q")->as_text()));
                (void)b.answer(Message(std::move(v))); // Loom's own answer authority
            }
        };

        LiveAuthority ceiling;
        ceiling.allow_to_role("Work", 1, kServiceRole);
        GrantAuthority cap = host_grant_authority(bus, session.id, std::move(ceiling));

        Grant weaver_grant;
        weaver_grant.allow("zen.AuthorityPrompt", 1, seat.id);
        weaver_grant.allow("zen.AuthorityGranted", 1, session.id);
        weaver_grant.allow("zen.AuthorityDescription", 1, seat.id);
        weaver_grant.allow("zen.AuthorityDescription", 1, session.id);
        weaver_grant.allow("zen.Ack", 1, seat.id);
        weaver_grant.allow_to_any("zen.Refused", 1);

        auto w = std::make_unique<Weaver>(std::move(cap), seat.id);
        Weaver* raw = w.get();
        weaver_id = bus.register_weave(std::move(w), std::move(weaver_grant), kWeaverRole);
        raw->zen_set_self(weaver_id);
    }

    TerminalSession& acting() const { return *session.session; }
    TerminalSession& op() const { return *seat.session; }

    /// Everything the participant recorded, in observation order.
    std::vector<TranscriptEntry> log(const TerminalSession& who) const {
        return who.transcript().entries();
    }
    std::vector<TranscriptEntry> of_kind(const TerminalSession& who, TranscriptKind kind) const {
        std::vector<TranscriptEntry> out;
        for (const TranscriptEntry& e : log(who)) {
            if (e.kind == kind) {
                out.push_back(e);
            }
        }
        return out;
    }
    /// Did the HOST see this refusal? A fact the sender is never told.
    bool tap_refused(const std::string& schema) const {
        return std::any_of(tap.begin(), tap.end(), [&](const Seen& r) {
            return r.kind == EventKind::Refused && r.schema == schema;
        });
    }
    bool tap_delivered(const std::string& schema, WeaveId to) const {
        return std::any_of(tap.begin(), tap.end(), [&](const Seen& r) {
            return r.kind == EventKind::Delivered && r.schema == schema && r.target == to;
        });
    }
    /// WHO the host saw author the delivery of `schema` to `to` — the invalid id when a root
    /// spoke, and the exact weave otherwise.
    WeaveId delivered_from(const std::string& schema, WeaveId to) const {
        for (const Seen& r : tap) {
            if (r.kind == EventKind::Delivered && r.schema == schema && r.target == to) {
                return r.sender;
            }
        }
        return WeaveId{};
    }

    Switchboard bus;
    std::shared_ptr<ObservationOrder> order;
    MountedTerminal session{};
    MountedTerminal seat{};
    WeaveId service_id{};
    ProbeWeave* service = nullptr;
    WeaveId weaver_id{};
    std::vector<Seen> tap;
};

/// Walk the whole authority story to the point where the operator has said yes.
/// Returns the session's local ask number for the authority request.
std::uint64_t request_and_approve(Cast& c) {
    const TerminalResult asked =
        c.acting().request_authority("Work", 1, kServiceRole, "so I can finish the job");
    REQUIRE(submitted(asked));
    c.bus.pump();
    const TerminalResult approved = c.op().ask(Address::to_role(kWeaverRole),
                                               "zen.ApproveAuthority", 1, {});
    REQUIRE(submitted(approved));
    c.bus.pump();
    return asked.ask;
}

} // namespace

TEST_SUITE("terminal") {

// ---- what a participant may know, and what knowing is not -------------------

TEST_CASE("knowing a shape is not a door, and neither is authority") {
    Cast c;
    // The catalog carries both; the ACCEPT-SET carries only the doors. A terminal composes
    // requests it will never be sent and receives answers it will never author.
    const std::vector<VocabularyEntry> catalog = c.acting().vocabulary().catalog();
    const auto entry = [&](const std::string& name) {
        return std::find_if(catalog.begin(), catalog.end(),
                            [&](const VocabularyEntry& v) { return v.name == name; });
    };
    REQUIRE(entry("Work") != catalog.end());
    CHECK_FALSE(entry("Work")->accepted); // known, composable, never delivered here
    REQUIRE(entry("Reply") != catalog.end());
    CHECK(entry("Reply")->accepted);

    const std::vector<std::shared_ptr<const Schema>> doors = c.acting().accepted_schemas();
    const auto has_door = [&](const std::string& name) {
        return std::any_of(doors.begin(), doors.end(),
                           [&](const std::shared_ptr<const Schema>& s) { return s->name() == name; });
    };
    CHECK(has_door("Reply"));
    CHECK_FALSE(has_door("Work"));       // a door it never asked for
    CHECK_FALSE(has_door("Query"));      // ...nor for the shape it SENDS
    CHECK(doors.size() == 6);

    // And knowing `Work` confers nothing: the participant's authority says nothing about it.
    CHECK(c.acting().describe("Work", 1).has_value());
}

TEST_CASE("a shape outside this participant's vocabulary is refused HERE, not by Loom") {
    Cast c;
    const TerminalResult r = c.acting().send(Address::to_role(kServiceRole), "Nope", 1, {});
    CHECK(r.outcome == TerminalOutcome::UnknownShape);
    CHECK_FALSE(submitted(r));
    // Nothing was authored, so nothing was denied by anybody. The transcript must say so.
    CHECK(c.of_kind(c.acting(), TranscriptKind::Submitted).empty());
    REQUIRE(c.of_kind(c.acting(), TranscriptKind::LocalRefusal).size() == 1);
    c.bus.pump();
    CHECK(c.tap.empty()); // the bus never saw a thing
}

TEST_CASE("a vocabulary is not the world's registry") {
    Cast c;
    // `Notice` is registered on this bus (it is one of the session's doors) but the OPERATOR was
    // never given it, so the operator cannot compose one. A terminal knows what its host handed
    // it — never what some other weave happens to have declared.
    CHECK_FALSE(c.op().describe("Notice", 1).has_value());
    CHECK(c.op().send(Address::to_role(kServiceRole), "Notice", 1, {bare(std::string("x"))})
              .outcome == TerminalOutcome::UnknownShape);
}

// ---- typed composition ------------------------------------------------------

TEST_CASE("the composer places values from the real schema, and refuses rather than guessing") {
    Cast c;
    SUBCASE("named, positional and type-directed all reach the same message") {
        const Composition by_name = c.acting().compose("Work", 1, {named("n", 7)});
        REQUIRE(by_name.status == Composition::Status::Ready);
        CHECK(assemble(by_name).get("n")->as_int() == 7);

        const Composition positional = c.acting().compose("Work", 1, {bare(std::int64_t{9})});
        REQUIRE(positional.status == Composition::Status::Ready);
        CHECK(assemble(positional).get("n")->as_int() == 9);
    }
    SUBCASE("a field the shape does not have is an Error, naming it") {
        const Composition bad = c.acting().compose("Work", 1, {named("nope", 1)});
        REQUIRE(bad.status == Composition::Status::Error);
        CHECK(bad.error.find("has no field 'nope'") != std::string::npos);
        CHECK(bad.schema != nullptr); // the SHAPE resolved; the argument did not
    }
    SUBCASE("a wrong-typed value is an Error, naming the declared type") {
        const Composition bad = c.acting().compose("Work", 1, {named("n", 0)});
        CHECK(bad.status == Composition::Status::Ready); // control: the right type is fine
        const Composition wrong =
            c.acting().compose("Work", 1, {Arg{std::string("n"), FieldValue{std::string("x")}}});
        REQUIRE(wrong.status == Composition::Status::Error);
        CHECK(wrong.error.find("Int") != std::string::npos);
    }
    SUBCASE("genuine ambiguity prompts instead of guessing") {
        const Composition open = c.acting().compose("Pair", 1, {bare(std::int64_t{1})});
        REQUIRE(open.status == Composition::Status::NeedsInput);
        REQUIRE(open.open_fields.size() == 1); // 'a' took the positional; 'b' is still open
        CHECK(open.open_fields.front().name == "b");
    }
    SUBCASE("an unknown shape leaves the schema unresolved, which is a different answer") {
        const Composition none = c.acting().compose("Nope", 1, {});
        REQUIRE(none.status == Composition::Status::Error);
        CHECK(none.schema == nullptr);
    }
}

TEST_CASE("composing authors nothing at all") {
    Cast c;
    const Composition ready = c.acting().compose("Query", 1, {bare(std::string("hi"))});
    REQUIRE(ready.status == Composition::Status::Ready);
    c.bus.pump();
    CHECK(c.tap.empty());
    CHECK(c.acting().transcript().size() == 0);
    CHECK(c.service->handled_names.empty());
}

// ---- who the sender is ------------------------------------------------------

TEST_CASE("a message a terminal authors comes from the TERMINAL, never from the host") {
    Cast c;
    REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Query", 1, {bare(std::string("hi"))})));
    c.bus.pump();
    REQUIRE(c.service->handled_names.size() == 1);
    CHECK(c.service->handled_names.front() == "Query");
    // THE ASSERTION THE WHOLE ARCHITECTURE TURNS ON. A host-root send would arrive stamped with
    // the invalid id; a send through the seat beside it would arrive stamped as the operator.
    CHECK(c.delivered_from("Query", c.service_id) == c.session.id);
}

TEST_CASE("the two participants are different weaves, and neither can be the other") {
    Cast c;
    CHECK(c.session.id != c.seat.id);
    CHECK(c.acting().id() == c.session.id);
    CHECK(c.op().id() == c.seat.id);

    // The desk is where the refusal to merge them lives.
    CHECK_NOTHROW((TerminalDesk{c.acting(), c.op()}));
    CHECK_THROWS_AS((TerminalDesk{c.acting(), c.acting()}), std::invalid_argument);

    // ...and a merged chronology has to mean something: two independent counters do not merge.
    auto lonely = std::make_unique<TerminalSession>("lonely", session_vocabulary());
    const MountedTerminal other =
        host_mount_terminal(c.bus, std::move(lonely), default_session_grant());
    CHECK_THROWS_AS((TerminalDesk{c.acting(), *other.session}), std::invalid_argument);
}

TEST_CASE("one identity for life: a participant cannot be re-attached to another weave") {
    Cast c;
    CHECK_THROWS_AS(c.acting().attach(host_participant_channel(c.bus, c.seat.id)),
                    std::invalid_argument);
    CHECK(c.acting().id() == c.session.id);
}

TEST_CASE("a channel names an identity and confers no authority whatever") {
    // The same terminal core, the same vocabulary, the same door — and an EMPTY grant.
    Cast c(Grant::nothing());
    REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Query", 1, {bare(std::string("hi"))})));
    c.bus.pump();
    CHECK(c.service->handled_names.empty());   // the Kernel refused it
    CHECK(c.tap_refused("Query"));             // ...and only the HOST was told
    // The participant's own record says exactly what it knows and no more.
    REQUIRE(c.of_kind(c.acting(), TranscriptKind::Submitted).size() == 1);
    CHECK(c.of_kind(c.acting(), TranscriptKind::Received).empty());
}

// ---- addressing -------------------------------------------------------------

TEST_CASE("the three addressing modes, and the fourth that is not a mode") {
    Cast c;
    SUBCASE("role: resolved to whoever holds the office at delivery") {
        REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Query", 1,
                                {bare(std::string("hi"))})));
        c.bus.pump();
        CHECK(c.service->handled_names.size() == 1);
        const TranscriptEntry e = c.of_kind(c.acting(), TranscriptKind::Submitted).front();
        CHECK(e.addressing == Addressing::Role);
        CHECK(e.role == kServiceRole);
    }
    SUBCASE("an exact weave id") {
        // A role rule never authorizes a direct id send, so this is refused at delivery — which
        // is the point: the addressing MODE reached the bus intact.
        REQUIRE(submitted(c.acting().send(Address::to_weave(c.service_id), "Query", 1,
                                {bare(std::string("hi"))})));
        c.bus.pump();
        CHECK(c.service->handled_names.empty());
        CHECK(c.tap_refused("Query"));
        CHECK(c.of_kind(c.acting(), TranscriptKind::Submitted).front().addressing ==
              Addressing::Weave);
    }
    SUBCASE("publish reports the fanout it queued, and not one delivery more") {
        const TerminalResult r =
            c.acting().send(Address::to_all(), "Query", 1, {bare(std::string("hi"))});
        REQUIRE(submitted(r));
        const TranscriptEntry e = c.of_kind(c.acting(), TranscriptKind::Submitted).front();
        CHECK(e.addressing == Addressing::Publish);
        CHECK(e.recipients == 1); // the service accepts Query; nobody else does
        c.bus.pump();
        CHECK(c.service->handled_names.empty()); // ...and it was still refused for want of a rule
    }
    SUBCASE("an unaddressed send is not a mode") {
        CHECK(c.acting().send(Address::to_weave(WeaveId{}), "Query", 1, {}).outcome ==
              TerminalOutcome::BadAddress);
        CHECK(c.acting().send(Address::to_role(""), "Query", 1, {}).outcome ==
              TerminalOutcome::BadAddress);
    }
    SUBCASE("a publication cannot be an ask") {
        CHECK(c.acting().ask(Address::to_all(), "Query", 1, {bare(std::string("hi"))}).outcome ==
              TerminalOutcome::BadAddress);
        CHECK_FALSE(c.acting().awaiting());
    }
}

// ---- submitted is not delivered ---------------------------------------------

TEST_CASE("SUBMITTED means authored, and says nothing about delivery — in either direction") {
    Cast c;
    // Two sends whose fates are OPPOSITE: one lands, one is refused for want of authority.
    REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Query", 1, {bare(std::string("a"))})));
    REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Work", 1, {bare(std::int64_t{1})})));
    c.bus.pump();

    // The HOST can tell them apart. The participant cannot, and its transcript does not pretend.
    CHECK(c.tap_delivered("Query", c.service_id));
    CHECK(c.tap_refused("Work"));
    const std::vector<TranscriptEntry> submitted =
        c.of_kind(c.acting(), TranscriptKind::Submitted);
    REQUIRE(submitted.size() == 2);
    CHECK(submitted[0].kind == submitted[1].kind); // identical records for opposite outcomes
    CHECK(std::string(name_of(TranscriptKind::Submitted)) == "submitted");
    // There is no delivery/outcome/refusal field on a Submitted entry to be filled in later, and
    // the participant's only inbound record is the answer it actually received.
    CHECK(c.of_kind(c.acting(), TranscriptKind::Received).empty());
}

// ---- receiving --------------------------------------------------------------

TEST_CASE("an unsolicited message is recorded as one, with its trusted provenance") {
    Cast c;
    c.bus.send(c.session.id, Message(notice("nobody asked")));
    c.bus.pump();
    const std::vector<TranscriptEntry> got = c.of_kind(c.acting(), TranscriptKind::Received);
    REQUIRE(got.size() == 1);
    CHECK(got.front().shape == "Notice");
    CHECK_FALSE(got.front().answers_ask);
    CHECK(got.front().answers == 0);
    CHECK(got.front().authored_role.empty()); // spoken personally, and empty is never an office

    const std::optional<ReceivedMessage> m = c.acting().received(got.front().message);
    REQUIRE(m);
    CHECK(m->value.get("text")->as_text() == "nobody asked");
    CHECK_FALSE(m->answers_ask);
}

TEST_CASE("a message this participant did not declare a door for never arrives") {
    Cast c;
    // `Work` is in the session's catalog but is not one of its doors.
    Value w(work_schema());
    w.set("n", Cell::integer(1));
    c.bus.send(c.session.id, Message(std::move(w)));
    c.bus.pump();
    CHECK(c.of_kind(c.acting(), TranscriptKind::Received).empty());
    CHECK(c.tap_refused("Work"));
}

// ---- ask, answer, and which conversation an answer belongs to ---------------

TEST_CASE("an ask is settled by LOOM's answer, and by nothing that merely looks like one") {
    Cast c;
    // The responder takes its answer away with it, so the ask is genuinely outstanding while the
    // impostor arrives — the whole question is what settles a conversation.
    std::vector<DeferredAnswer> held;
    c.service->on_handle = [&held](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Query") {
            held.push_back(b.make_deferred_answer());
        }
    };
    const TerminalResult asked =
        c.acting().ask(Address::to_role(kServiceRole), "Query", 1, {bare(std::string("hi"))});
    REQUIRE(submitted(asked));
    REQUIRE(asked.ask == 1);
    CHECK(c.acting().awaiting());
    c.bus.pump();
    REQUIRE(held.size() == 1);
    CHECK(c.acting().waiting_on(1));

    // An ordinary message of the very shape the answer will have, from a weave that holds the
    // grant for it. It carries no answer provenance, so it settles nothing.
    Value pretender(reply_schema());
    pretender.set("answer", Cell::text("I am not your answer"));
    c.bus.send(c.session.id, Message(std::move(pretender)));
    c.bus.pump();
    CHECK(c.acting().waiting_on(1));
    REQUIRE(c.of_kind(c.acting(), TranscriptKind::Received).size() == 1);
    CHECK(c.of_kind(c.acting(), TranscriptKind::AnswerReceived).empty());

    // ...and now the real one, through Loom's own answer door.
    Value real(reply_schema());
    real.set("answer", Cell::text("you asked: hi"));
    c.service->on_handle = [&held, &real](const Message&, Bus& b, ProbeWeave&) {
        (void)b.spend_deferred(held[0], Message(real));
    };
    Value wake(query_schema());
    wake.set("q", Cell::text("wake"));
    c.bus.send(c.service_id, Message(std::move(wake)));
    c.bus.pump();
    CHECK_FALSE(c.acting().waiting_on(1));
    CHECK_FALSE(c.acting().awaiting());
    const std::vector<TranscriptEntry> answers =
        c.of_kind(c.acting(), TranscriptKind::AnswerReceived);
    REQUIRE(answers.size() == 1);
    CHECK(answers.front().answers_ask);
    CHECK(answers.front().answers == 1);
    CHECK(answers.front().sender == c.service_id);
}

TEST_CASE("two conversations at once, each answer landing on its own — by Loom's correlation") {
    Cast c;
    // A responder that DEFERS, so both asks are genuinely outstanding at the same time and the
    // answers come back in the reverse order.
    std::vector<DeferredAnswer> held;
    c.service->on_handle = [&held](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Query") {
            held.push_back(b.make_deferred_answer());
        }
    };
    const TerminalResult first =
        c.acting().ask(Address::to_role(kServiceRole), "Query", 1, {bare(std::string("one"))});
    const TerminalResult second =
        c.acting().ask(Address::to_role(kServiceRole), "Query", 1, {bare(std::string("two"))});
    REQUIRE(submitted(first));
    REQUIRE(submitted(second));
    CHECK(first.ask == 1);
    CHECK(second.ask == 2);
    CHECK(c.acting().outstanding() == 2);
    c.bus.pump();
    REQUIRE(held.size() == 2);

    // Answer the SECOND one first. Nothing in the payload says which is which — the whole
    // question is whether the terminal reads Loom's record or guesses from arrival order.
    Value b_answer(reply_schema());
    b_answer.set("answer", Cell::text("for two"));
    {
        // Spending needs the respondent's own live Mail, so poke the service to get one.
        c.service->on_handle = [&held, &b_answer](const Message&, Bus& b, ProbeWeave&) {
            (void)b.spend_deferred(held[1], Message(b_answer));
        };
        Value q(query_schema());
        q.set("q", Cell::text("wake"));
        c.bus.send(c.service_id, Message(std::move(q)));
        c.bus.pump();
    }
    CHECK(c.acting().waiting_on(1));
    CHECK_FALSE(c.acting().waiting_on(2));
    const std::vector<TranscriptEntry> so_far =
        c.of_kind(c.acting(), TranscriptKind::AnswerReceived);
    REQUIRE(so_far.size() == 1);
    CHECK(so_far.front().answers == 2); // the SECOND ask, answered second-first

    Value a_answer(reply_schema());
    a_answer.set("answer", Cell::text("for one"));
    {
        c.service->on_handle = [&held, &a_answer](const Message&, Bus& b, ProbeWeave&) {
            (void)b.spend_deferred(held[0], Message(a_answer));
        };
        Value q(query_schema());
        q.set("q", Cell::text("wake"));
        c.bus.send(c.service_id, Message(std::move(q)));
        c.bus.pump();
    }
    CHECK_FALSE(c.acting().awaiting());
    const std::vector<TranscriptEntry> both =
        c.of_kind(c.acting(), TranscriptKind::AnswerReceived);
    REQUIRE(both.size() == 2);
    CHECK(both[1].answers == 1);
}

TEST_CASE("a participant will not track more conversations than it said it would") {
    Cast c;
    c.service->on_handle = [](const Message&, Bus&, ProbeWeave&) {}; // never answers
    for (std::size_t i = 0; i < kMaxOutstandingAsks; ++i) {
        REQUIRE(submitted(c.acting().ask(Address::to_role(kServiceRole), "Query", 1,
                               {bare(std::string("q"))})));
    }
    CHECK(c.acting().outstanding() == kMaxOutstandingAsks);
    const std::size_t submitted_before =
        c.of_kind(c.acting(), TranscriptKind::Submitted).size();

    const TerminalResult over =
        c.acting().ask(Address::to_role(kServiceRole), "Query", 1, {bare(std::string("q"))});
    CHECK(over.outcome == TerminalOutcome::TooManyAsks);
    // NOTHING WAS AUTHORED, and the conversations already outstanding are untouched: a new ask
    // must never be able to displace one somebody is waiting on.
    CHECK(c.acting().outstanding() == kMaxOutstandingAsks);
    CHECK(c.of_kind(c.acting(), TranscriptKind::Submitted).size() == submitted_before);
    CHECK(c.acting().waiting_on(1));
}

TEST_CASE("stopping waiting is local, and says so; the answer may still arrive") {
    Cast c;
    std::vector<DeferredAnswer> held;
    c.service->on_handle = [&held](const Message& in, Bus& b, ProbeWeave&) {
        if (in.payload.schema().name() == "Query") {
            held.push_back(b.make_deferred_answer());
        }
    };
    const TerminalResult asked =
        c.acting().ask(Address::to_role(kServiceRole), "Query", 1, {bare(std::string("hi"))});
    REQUIRE(submitted(asked));
    c.bus.pump();
    REQUIRE(held.size() == 1);

    REQUIRE(submitted(c.acting().cancel_ask(asked.ask)));
    CHECK_FALSE(c.acting().awaiting());
    const TranscriptEntry note = c.of_kind(c.acting(), TranscriptKind::LocalNotice).back();
    CHECK(note.text.find("NOT cancelled") != std::string::npos);
    CHECK(c.acting().cancel_ask(asked.ask).outcome == TerminalOutcome::NoSuchAsk);

    // The far end was never told, so it answers anyway — and that answer is recorded as the
    // authenticated answer it genuinely is, matched to nothing.
    Value r(reply_schema());
    r.set("answer", Cell::text("late"));
    c.service->on_handle = [&held, &r](const Message&, Bus& b, ProbeWeave&) {
        (void)b.spend_deferred(held[0], Message(r));
    };
    Value q(query_schema());
    q.set("q", Cell::text("wake"));
    c.bus.send(c.service_id, Message(std::move(q)));
    c.bus.pump();
    const std::vector<TranscriptEntry> answers =
        c.of_kind(c.acting(), TranscriptKind::AnswerReceived);
    REQUIRE(answers.size() == 1);
    CHECK(answers.front().answers_ask); // it IS an authenticated answer
    CHECK(answers.front().answers == 0); // ...to a conversation this participant let go
}

// ---- the authority story, through the durable core --------------------------

TEST_CASE("a person puts one session in reach of one service, and nothing is replayed") {
    Cast c;
    // The premise: it may not speak Work.
    REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Work", 1, {bare(std::int64_t{1})})));
    c.bus.pump();
    CHECK(c.service->handled_names.empty());

    const TerminalResult asked =
        c.acting().request_authority("Work", 1, kServiceRole, "so I can finish the job");
    REQUIRE(submitted(asked));
    c.bus.pump();

    // The prompt reaches the OPERATOR SEAT, as an ordinary unsolicited message — the operator
    // asked nothing, so it is not an answer.
    const std::vector<TranscriptEntry> prompts = c.of_kind(c.op(), TranscriptKind::Received);
    REQUIRE(prompts.size() == 1);
    CHECK(prompts.front().shape == "zen.AuthorityPrompt");
    CHECK(prompts.front().sender == c.weaver_id);
    CHECK_FALSE(prompts.front().answers_ask);
    CHECK(c.of_kind(c.acting(), TranscriptKind::Received).empty()); // the session saw nothing

    // The prompt's trusted facts are the WEAVER's, and the untrusted prose is kept apart.
    const std::optional<ReceivedMessage> prompt = c.op().received(prompts.front().message);
    REQUIRE(prompt);
    CHECK(prompt->value.get("requester")->as_int() == static_cast<std::int64_t>(c.session.id.value));
    CHECK(prompt->value.get("shape")->as_text() == "Work");
    CHECK(prompt->value.get("requester_says")->as_text() == "so I can finish the job");
    CHECK(prompt->sender == c.weaver_id);

    const std::size_t submitted_before = c.of_kind(c.acting(), TranscriptKind::Submitted).size();
    REQUIRE(submitted(c.op().ask(Address::to_role(kWeaverRole), "zen.ApproveAuthority", 1, {})));
    c.bus.pump();

    // The session hears yes, as LOOM's answer to the request it actually sent.
    const std::vector<TranscriptEntry> answers =
        c.of_kind(c.acting(), TranscriptKind::AnswerReceived);
    REQUIRE(answers.size() == 1);
    CHECK(answers.front().shape == "zen.AuthorityGranted");
    CHECK(answers.front().answers == asked.ask);
    CHECK(c.acting().received(answers.front().message)->value.get("basis")->as_text() ==
          "delegated");

    // ...AND NOTHING ELSE HAPPENED. Approval grants authority; it does not resurrect intent.
    CHECK(c.service->handled_names.empty());
    CHECK(c.of_kind(c.acting(), TranscriptKind::Submitted).size() == submitted_before);

    // The retry is the session's own explicit act, under its own identity.
    REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Work", 1, {bare(std::int64_t{7})})));
    c.bus.pump();
    REQUIRE(c.service->handled_names.size() == 1);
    CHECK(c.service->handled_names.front() == "Work");
    CHECK(c.delivered_from("Work", c.service_id) == c.session.id);
}

TEST_CASE("the operator's conveniences are ordinary messages from the operator's own door") {
    Cast c;
    (void)request_and_approve(c);
    // Every decision the operator made was a message it AUTHORED, recorded in its own transcript
    // with its own correlation — never a call into a Weaver, and never a host act.
    const std::vector<TranscriptEntry> authored = c.of_kind(c.op(), TranscriptKind::Submitted);
    REQUIRE(authored.size() == 1);
    CHECK(authored.front().shape == "zen.ApproveAuthority");
    CHECK(authored.front().addressing == Addressing::Role);
    CHECK(authored.front().role == kWeaverRole);
    // ...and the Weaver's acknowledgement came back as the authenticated answer to it.
    const std::vector<TranscriptEntry> acks = c.of_kind(c.op(), TranscriptKind::AnswerReceived);
    REQUIRE(acks.size() == 1);
    CHECK(acks.front().shape == "zen.Ack");
    CHECK(acks.front().answers == 1);
}

TEST_CASE("a session cannot approve its own request, even holding the shape and the route") {
    // The session is deliberately given the reach the ordinary one lacks, so the refusal being
    // measured is a POLICY refusal rather than "it could not spell the sentence".
    Grant reaches = default_session_grant();
    reaches.allow_to_role("zen.ApproveAuthority", 1, kWeaverRole);
    Cast c(std::move(reaches));

    const TerminalResult asked =
        c.acting().request_authority("Work", 1, kServiceRole, "let me in");
    REQUIRE(submitted(asked));
    c.bus.pump();

    // The SESSION authors the approval. The presentation does not reroute it; it is authored by
    // the participant that was asked to author it, and it leaves through that participant's door.
    REQUIRE(submitted(c.acting().ask(Address::to_role(kWeaverRole), "zen.ApproveAuthority", 1, {})));
    c.bus.pump();

    // The Weaver refuses: reaching it is not being the user.
    const std::vector<TranscriptEntry> answers =
        c.of_kind(c.acting(), TranscriptKind::AnswerReceived);
    REQUIRE_FALSE(answers.empty());
    CHECK(answers.back().shape == "zen.Refused");
    CHECK(c.acting().received(answers.back().message)->value.get("reason")->as_text().find(
              "operator seat") != std::string::npos);

    // Authority did not change, and the operator's request is still waiting for a person.
    REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Work", 1, {bare(std::int64_t{1})})));
    c.bus.pump();
    CHECK(c.service->handled_names.empty());
    // The operator authored nothing at all.
    CHECK(c.of_kind(c.op(), TranscriptKind::Submitted).empty());
}

TEST_CASE("the session's approval attempt is refused by the KERNEL when it has no route") {
    Cast c; // the ordinary baseline: no ApproveAuthority rule of any kind
    REQUIRE(submitted(c.acting().request_authority("Work", 1, kServiceRole, "let me in")));
    c.bus.pump();
    const std::size_t weaver_heard = c.tap.size();

    REQUIRE(submitted(c.acting().send(Address::to_role(kWeaverRole), "zen.ApproveAuthority", 1, {})));
    c.bus.pump();
    CHECK(c.tap_refused("zen.ApproveAuthority"));
    CHECK(c.tap.size() > weaver_heard);
    // ...and the session was told nothing about it, which is the sender-fate seam, honestly.
    CHECK(c.of_kind(c.acting(), TranscriptKind::AnswerReceived).empty());
    // Authority unchanged.
    REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Work", 1, {bare(std::int64_t{1})})));
    c.bus.pump();
    CHECK(c.service->handled_names.empty());
}

TEST_CASE("a third participant that can reach the Weaver is still not the user") {
    Cast c;
    REQUIRE(submitted(c.acting().request_authority("Work", 1, kServiceRole, "let me in")));
    c.bus.pump();

    // An ordinary terminal participant, with exactly the operator's grant and none of its
    // standing. The terminal core privileges nobody; the Weaver decides.
    Grant intruder_grant = operator_seat_grant();
    const MountedTerminal intruder = host_mount_terminal(
        c.bus, std::make_unique<TerminalSession>("intruder", operator_vocabulary(), c.order),
        std::move(intruder_grant));
    REQUIRE(submitted(intruder.session->ask(Address::to_role(kWeaverRole), "zen.ApproveAuthority", 1, {})));
    c.bus.pump();

    const std::vector<TranscriptEntry> answers =
        intruder.session->transcript().entries();
    const auto refusal = std::find_if(answers.begin(), answers.end(), [](const TranscriptEntry& e) {
        return e.kind == TranscriptKind::AnswerReceived;
    });
    REQUIRE(refusal != answers.end());
    CHECK(refusal->shape == "zen.Refused");
    // Nothing was installed.
    REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Work", 1, {bare(std::int64_t{1})})));
    c.bus.pump();
    CHECK(c.service->handled_names.empty());
}

TEST_CASE("a participant reads its authority from the Kernel, and keeps no copy of it") {
    Cast c;
    (void)request_and_approve(c);

    const TerminalResult described = c.acting().describe_authority();
    REQUIRE(described);
    c.bus.pump();
    const std::vector<TranscriptEntry> answers =
        c.of_kind(c.acting(), TranscriptKind::AnswerReceived);
    REQUIRE(answers.size() == 2); // the grant, then the description
    const std::optional<ReceivedMessage> desc = c.acting().received(answers.back().message);
    REQUIRE(desc);
    CHECK(desc->value.schema().name() == "zen.AuthorityDescription");
    CHECK(desc->value.get("subject")->as_int() == static_cast<std::int64_t>(c.session.id.value));
    const Cell::Array& base = desc->value.get("base")->as_list();
    const Cell::Array& delegated = desc->value.get("delegated")->as_list();
    CHECK(base.size() == 3);
    REQUIRE(delegated.size() == 1);
    CHECK(delegated[0].as_text() == "Work v1 -> role some.service");
}

TEST_CASE("revoking takes back the delegated rule, and the session is not told it was denied") {
    Cast c;
    (void)request_and_approve(c);
    REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Work", 1, {bare(std::int64_t{1})})));
    c.bus.pump();
    REQUIRE(c.service->handled_names.size() == 1); // it worked once

    REQUIRE(submitted(c.op().ask(Address::to_role(kWeaverRole), "zen.RevokeAuthority", 1, {})));
    c.bus.pump();

    // The delegated overlay is empty; the admission baseline is untouched.
    REQUIRE(submitted(c.acting().describe_authority()));
    c.bus.pump();
    const std::vector<TranscriptEntry> answers =
        c.of_kind(c.acting(), TranscriptKind::AnswerReceived);
    const std::optional<ReceivedMessage> desc = c.acting().received(answers.back().message);
    REQUIRE(desc);
    CHECK(desc->value.get("delegated")->as_list().empty());
    CHECK(desc->value.get("base")->as_list().size() == 3);

    // The next Work is denied — and THE HOST is the only party that knows.
    const std::size_t before = c.service->handled_names.size();
    REQUIRE(submitted(c.acting().send(Address::to_role(kServiceRole), "Work", 1, {bare(std::int64_t{2})})));
    c.bus.pump();
    CHECK(c.service->handled_names.size() == before);
    CHECK(c.tap_refused("Work"));
    // The participant's transcript records an authored message and never a denial it was not told.
    for (const TranscriptEntry& e : c.log(c.acting())) {
        CHECK(e.kind != TranscriptKind::Received);
    }
}

// ---- what a participant may observe -----------------------------------------

TEST_CASE("a transcript holds this participant's own experience, and no third party's traffic") {
    Cast c;
    // Two other weaves, talking to each other, loudly and repeatedly.
    Registered a = register_probe(c.bus, {ping_schema()});
    Registered b = register_probe(c.bus, {pong_schema()});
    for (int i = 0; i < 5; ++i) {
        c.bus.send(a.id, Message(ping(i)));
        c.bus.send(b.id, Message(pong(i)));
    }
    // ...and a refusal, which is exactly the kind of fact a tap would hand over.
    c.bus.send(a.id, Message(malformed_ping()));
    c.bus.pump();

    CHECK(a.weave->count == 5);
    CHECK(b.weave->count == 5);
    CHECK(c.tap.size() > 10);       // the HOST saw all of it
    CHECK(c.acting().transcript().size() == 0); // the participant saw NONE of it
    CHECK(c.op().transcript().size() == 0);
    CHECK(c.acting().transcript().received_size() == 0);
}

TEST_CASE("a merged chronology is the order this presentation observed, and labels every lens") {
    Cast c;
    TerminalDesk desk(c.acting(), c.op());
    c.acting().record_command("request Work 1 @some.service");
    (void)request_and_approve(c);

    const std::vector<DeskEntry> merged = desk.chronology();
    REQUIRE(merged.size() > 4);
    for (std::size_t i = 1; i < merged.size(); ++i) {
        CHECK(merged[i - 1].entry.seq < merged[i].entry.seq); // one order, strictly increasing
    }
    const bool both_lenses =
        std::any_of(merged.begin(), merged.end(),
                    [](const DeskEntry& d) { return d.lens == "session"; }) &&
        std::any_of(merged.begin(), merged.end(),
                    [](const DeskEntry& d) { return d.lens == "operator"; });
    CHECK(both_lenses);
    // A merged VIEW is not a merged identity: every entry still names the weave that knows it.
    for (const DeskEntry& d : merged) {
        CHECK(d.entry.lens == (d.lens == "session" ? c.session.id : c.seat.id));
    }
}

// ---- bounded history, and the state that must survive it --------------------

TEST_CASE("the transcript is bounded, counts what it dropped, and never loses a conversation") {
    Cast c;
    CHECK(TerminalHistoryProbe::entry_slots(c.acting().transcript()) == kTranscriptCapacity);
    CHECK(TerminalHistoryProbe::received_slots(c.acting().transcript()) == kReceivedCapacity);

    // An ask nobody will answer, made FIRST, so it is the oldest thing in the window.
    c.service->on_handle = [](const Message&, Bus&, ProbeWeave&) {};
    const TerminalResult asked =
        c.acting().ask(Address::to_role(kServiceRole), "Query", 1, {bare(std::string("hi"))});
    REQUIRE(submitted(asked));
    c.bus.pump();

    for (std::size_t i = 0; i < kTranscriptCapacity * 2; ++i) {
        c.acting().record_notice("noise " + std::to_string(i));
    }
    CHECK(c.acting().transcript().size() == kTranscriptCapacity);
    CHECK(c.acting().transcript().evicted() >= kTranscriptCapacity);
    // Storage stops growing, which is the claim a week-long session needs.
    CHECK(TerminalHistoryProbe::entry_slots(c.acting().transcript()) == kTranscriptCapacity);

    // The Submitted entry for the ask is long gone from the window...
    const std::vector<TranscriptEntry> visible = c.acting().transcript().entries();
    CHECK(std::none_of(visible.begin(), visible.end(), [](const TranscriptEntry& e) {
        return e.kind == TranscriptKind::Submitted;
    }));
    // ...and the CONVERSATION is untouched, because state is not history.
    CHECK(c.acting().waiting_on(asked.ask));
    REQUIRE(c.acting().pending().size() == 1);
    CHECK(c.acting().pending().front().shape == "Query");
}

TEST_CASE("a received-message id is an identity: once evicted it refuses, never re-binds") {
    Cast c;
    for (std::size_t i = 0; i < kReceivedCapacity + 5; ++i) {
        c.bus.send(c.session.id, Message(notice("n" + std::to_string(i))));
    }
    c.bus.pump();
    CHECK(c.acting().transcript().received_size() == kReceivedCapacity);
    CHECK(c.acting().transcript().received_evicted() == 5);

    CHECK_FALSE(c.acting().received(1).has_value()); // evicted, and it says so
    const std::optional<ReceivedMessage> newest =
        c.acting().received(c.acting().transcript().last_received_id());
    REQUIRE(newest);
    CHECK(newest->value.get("text")->as_text() == "n" + std::to_string(kReceivedCapacity + 4));

    std::string why;
    CHECK_FALSE(c.acting().resolve_ref(Ref{"r1", "text"}, &why).has_value());
    CHECK(why.find("evicted") != std::string::npos);
    CHECK_FALSE(c.acting().resolve_ref(Ref{"r99999", "text"}, &why).has_value());
    CHECK(why.find("no such received message") != std::string::npos);
}

TEST_CASE("one message's output wires into another's input, by reference") {
    Cast c;
    c.bus.send(c.session.id, Message(notice("carried")));
    c.bus.pump();
    const std::uint64_t id = c.acting().transcript().last_received_id();
    const Composition wired =
        c.acting().compose("Query", 1, {Arg{std::nullopt, Ref{"r" + std::to_string(id), "text"}}});
    REQUIRE(wired.status == Composition::Status::Ready);
    CHECK(assemble(wired).get("q")->as_text() == "carried");

    // A reference matches its resolved kind EXACTLY — no coercion, so wiring is predictable.
    const Composition mismatched =
        c.acting().compose("Work", 1, {Arg{std::nullopt, Ref{"r" + std::to_string(id), "text"}}});
    CHECK(mismatched.status != Composition::Status::Ready);
}

// ---- text safety ------------------------------------------------------------

TEST_CASE("the renderer escapes what can steer a terminal, and keeps everything else") {
    CHECK(safe_terminal_text("ordinary text") == "ordinary text");
    // C0 controls and DEL become visible, unambiguously.
    CHECK(safe_terminal_text("\x1b[2J") == "\\x1b[2J");
    CHECK(safe_terminal_text("a\rb\nc") == "a\\x0db\\x0ac");
    CHECK(safe_terminal_text("\x7f") == "\\x7f");
    // A literal backslash is doubled, so an escape is never confusable with authored text.
    CHECK(safe_terminal_text("\\x1b") == "\\\\x1b");
    // ...and UTF-8 survives, which is where this differs from the operator sanitizer: a terminal
    // that escaped every non-ASCII byte would corrupt every ordinary message carrying a name.
    CHECK(safe_terminal_text("naïve — 世界") == "naïve — 世界");
    CHECK(safe_operator_text("naïve", 64) != "naïve"); // the Weaver's, deliberately stricter
}

TEST_CASE("the core keeps exactly the bytes that arrived, and escapes nothing") {
    Cast c;
    const std::string hostile = "\x1b[2Jcleared\x07";
    c.bus.send(c.session.id, Message(notice(hostile)));
    c.bus.pump();
    const std::optional<ReceivedMessage> m =
        c.acting().received(c.acting().transcript().last_received_id());
    REQUIRE(m);
    // VERBATIM in the model. A core that sanitized what it stored would decide, once and for
    // everybody, what every future renderer is allowed to see.
    CHECK(m->value.get("text")->as_text() == hostile);
    // ...and safe at the last moment, where the terminal is.
    CHECK(safe_terminal_text(m->value.get("text")->as_text()).find('\x1b') == std::string::npos);
}

// ---- the shape of the handler ----------------------------------------------

TEST_CASE("receiving authors nothing: the terminal's handler has no send in it") {
    Cast c;
    // Every kind of inbound a terminal sees — an answer, an unsolicited message, a refusal —
    // and after all of them the bus has carried nothing this participant authored in response.
    REQUIRE(submitted(c.acting().ask(Address::to_role(kServiceRole), "Query", 1, {bare(std::string("hi"))})));
    c.bus.pump();
    c.bus.send(c.session.id, Message(notice("hello")));
    c.bus.pump();
    const std::size_t authored = c.of_kind(c.acting(), TranscriptKind::Submitted).size();
    CHECK(authored == 1); // the ask, and nothing else

    const std::size_t events = c.tap.size();
    c.bus.pump();
    c.bus.pump();
    CHECK(c.tap.size() == events); // pumping a settled bus produces nothing new
}

TEST_CASE("an unattached participant authors nothing and says why") {
    TerminalSession lonely("nobody", session_vocabulary());
    CHECK_FALSE(lonely.attached());
    CHECK_FALSE(lonely.id().valid());
    const TerminalResult r =
        lonely.send(Address::to_role(kServiceRole), "Query", 1, {bare(std::string("hi"))});
    CHECK(r.outcome == TerminalOutcome::NotAttached);
    CHECK(lonely.transcript().size() == 1); // the local refusal, and nothing else
}

} // TEST_SUITE("terminal")
