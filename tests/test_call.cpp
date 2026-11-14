// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// CALLS (MSG-14): a weave calls the function the holder of a role provides, inside its own
// handler, and has the answer or a refusal before the handler goes on. The function is handed its
// argument alone; no handler runs and nothing is delivered; a call rule, absent by default, is the
// only rule read; the argument and the answer each meet the gate; every refusal is its own word;
// and the tap tells each call and how it ended before the caller's own delivery.

#include <doctest.h>

#include "weavelib/call_protocol.hpp"

#include <zen/switchboard/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace loom;
using namespace calling;

namespace {

/// A provider of CallAdd to CallSum: `by` says which one answered. It counts what is delivered to
/// it, so a call that delivered something would show.
template <std::int64_t By>
class Adder final : public WeaveBase<Adder<By>, CallHeard, Accept<CallPeek>, Emit<>, Claims<>,
                                     Provides<CallAdd>> {
public:
    void on(const CallPeek&, Mail&) { ++this->state_.knocks; }
    static CallSum provide(const CallAdd& add) {
        if (add.a == kCallFails) {
            throw std::runtime_error("asked to fail");
        }
        return CallSum{add.a + add.b, By};
    }
};

/// A caller whose handler does what the case hands it.
class Caller final : public WeaveBase<Caller, CallHeard, Accept<CallKnock>> {
public:
    explicit Caller(std::function<void(Mail&)> act) : act_(std::move(act)) {}
    void on(const CallKnock&, Mail& mail) {
        ++state_.knocks;
        act_(mail);
    }

private:
    std::function<void(Mail&)> act_;
};

/// A participant whose functions or whereabouts a case sets: what no `WeaveBase` can say.
class Raw final : public Weave {
public:
    Raw(std::vector<ProvidedFunction> provides, CallRefusal elsewhere)
        : provides_(std::move(provides)), elsewhere_(elsewhere) {}
    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override { return {}; }
    void handle(const Message&, Bus&) override {}
    Value snapshot() const override { return to_value(CallHeard{}); }
    Value policy() const override {
        Value v(lifecycle_policy_schema());
        v.set("max_reloads", Cell::integer(0));
        v.set("revive_from_last_good", Cell::boolean(true));
        return v;
    }
    void revive(const Value&) override {}
    std::vector<ProvidedFunction> provided_functions() const override { return provides_; }
    CallRefusal elsewhere() const override { return elsewhere_; }

private:
    std::vector<ProvidedFunction> provides_;
    CallRefusal elsewhere_;
};

template <class W, class... Args>
WeaveId place(Switchboard& sb, Grant grant, std::string role, Args&&... args) {
    auto w = std::make_unique<W>(std::forward<Args>(args)...);
    W* raw = w.get();
    const WeaveId id = role.empty() ? sb.register_weave(std::move(w), std::move(grant))
                                    : sb.register_weave(std::move(w), std::move(grant), role);
    if constexpr (requires { raw->zen_set_self(id); }) {
        raw->zen_set_self(id);
    }
    return id;
}

Grant may_call(std::string role, std::string shape = "CallAdd", std::uint32_t version = 1) {
    return Grant{}.allow_call(std::move(role), std::move(shape), version);
}

/// Run `act` inside one delivery to a caller granted `grant`, and return what it kept.
template <class T>
std::optional<T> inside(Switchboard& sb, Grant grant, std::function<T(Mail&)> act) {
    std::optional<T> kept;
    const WeaveId c = place<Caller>(sb, std::move(grant), "", [&](Mail& m) { kept = act(m); });
    sb.send(c, Message(to_value(CallKnock{"", 0, 0})));
    sb.drain_until_idle();
    sb.unregister_weave(c);
    return kept;
}

Called<CallSum> call_add(Switchboard& sb, Grant grant, const std::string& role, CallAdd add) {
    return *inside<Called<CallSum>>(sb, std::move(grant), [&](Mail& m) {
        return m.call<CallSum>(role, add);
    });
}

/// The caller's own `CallAdd v1`, which is not the provider's: the gate reads the fields.
namespace wrong {
struct CallAdd {
    std::string a;
    ZEN_SHAPE(CallAdd, 1, ZEN_FIELD(a));
};
/// A weave holding that definition as one of its doors.
class Holder final : public WeaveBase<Holder, CallHeard, Accept<CallAdd>> {
public:
    void on(const CallAdd&, Mail&) {}
};
} // namespace wrong

struct Seen {
    EventKind kind;
    std::uint64_t seq;
    WeaveId sender;
    WeaveId target;
    std::string role;
    std::string shape;
    CallRefusal refusal;
    std::string words;
};

} // namespace

TEST_SUITE("call") {

TEST_CASE("a call answers inside the caller's handler, and nothing is delivered and no handler "
          "runs") {
    Switchboard sb;
    const WeaveId adder = place<Adder<1>>(sb, Grant{}, "arith");
    std::optional<Called<CallSum>> got;
    std::int64_t after = 0;
    const WeaveId caller = place<Caller>(sb, may_call("arith"), "", [&](Mail& m) {
        got.emplace(m.call<CallSum>("arith", CallAdd{2, 3}));
        after = got && *got ? (*got)->value : -1; // the answer is here before the next line
    });
    const std::size_t pending = sb.pending();
    sb.send(caller, Message(to_value(CallKnock{"", 0, 0})));
    sb.drain_until_idle();
    REQUIRE(got.has_value());
    CHECK(got->refusal() == CallRefusal::None);
    CHECK((*got)->value == 5);
    CHECK((*got)->by == 1);
    CHECK(after == 5);
    CHECK(sb.pending() == pending);
    // The provider was handed nothing: no delivery, no handler, its state as it was.
    CHECK(from_value<CallHeard>(sb.weave(adder)->snapshot()).knocks == 0);
}

TEST_CASE("a call needs a call rule, absent by default, and no send or observe rule is read for "
          "one") {
    Switchboard sb;
    (void)place<Adder<1>>(sb, Grant{}, "arith");
    CHECK(call_add(sb, Grant{}, "arith", {1, 1}).refusal() == CallRefusal::NotAuthorized);
    CHECK(call_add(sb, Grant{}.allow_any().allow_observe_any().allow_to_role("CallAdd", 1, "arith"),
                   "arith", {1, 1})
              .refusal() == CallRefusal::NotAuthorized);
    CHECK(call_add(sb, may_call("other"), "arith", {1, 1}).refusal() == CallRefusal::NotAuthorized);
    CHECK(call_add(sb, may_call("arith", "CallAdd", 2), "arith", {1, 1}).refusal() ==
          CallRefusal::NotAuthorized);
    // Asked before the role is resolved: an unauthorized caller learns nothing of who holds it.
    CHECK(call_add(sb, Grant{}, "nobody", {1, 1}).refusal() == CallRefusal::NotAuthorized);
    CHECK(call_add(sb, may_call("arith"), "arith", {1, 1}).refusal() == CallRefusal::None);
}

TEST_CASE("each refusal is its own word") {
    Switchboard sb;
    (void)place<Adder<1>>(sb, Grant{}, "arith");
    SUBCASE("no holder") {
        const Called<CallSum> r = call_add(sb, may_call("nobody"), "nobody", {1, 1});
        CHECK(r.refusal() == CallRefusal::NoHolder);
        CHECK_FALSE(r);
    }
    SUBCASE("no function from the argument's shape") {
        const auto r = *inside<Called<CallSum>>(sb, may_call("arith", "CallPeek"), [](Mail& m) {
            return m.call<CallSum>("arith", CallPeek{1});
        });
        CHECK(r.refusal() == CallRefusal::NotProvided);
        CHECK(r.words().find("CallPeek") != std::string::npos);
    }
    SUBCASE("no function answering the shape the caller asks") {
        const auto r = *inside<Called<CallHeard>>(sb, may_call("arith"), [](Mail& m) {
            return m.call<CallHeard>("arith", CallAdd{1, 1});
        });
        CHECK(r.refusal() == CallRefusal::NotProvided);
    }
    SUBCASE("the argument refused at the function's input shape") {
        const auto r = *inside<Called<CallSum>>(sb, may_call("arith"), [](Mail& m) {
            return m.call<CallSum>("arith", wrong::CallAdd{"two"});
        });
        CHECK(r.refusal() == CallRefusal::ArgumentRefused);
        CHECK_FALSE(r.words().empty());
    }
    SUBCASE("the provider's code failed") {
        const Called<CallSum> r = call_add(sb, may_call("arith"), "arith", {kCallFails, 1});
        CHECK(r.refusal() == CallRefusal::Failed);
        CHECK(r.words() == "asked to fail");
    }
    SUBCASE("the answer refused at the function's output shape") {
        ProvidedFunction lying{schema_of<CallAdd>(), schema_of<CallSum>(), [](const Value&) {
                                   return ProvidedAnswer{to_value(CallPeek{1}), std::nullopt, {}};
                               }};
        (void)place<Raw>(sb, Grant{}, "liar", std::vector<ProvidedFunction>{lying},
                         CallRefusal::None);
        const Called<CallSum> r = call_add(sb, may_call("liar"), "liar", {1, 1});
        CHECK(r.refusal() == CallRefusal::AnswerRefused);
        CHECK_FALSE(r);
    }
    SUBCASE("the holder runs elsewhere: an isolated child, a link") {
        for (const CallRefusal far : {CallRefusal::Isolated, CallRefusal::Linked}) {
            CAPTURE(name_of(far));
            Switchboard here;
            (void)place<Raw>(here, Grant{}, "far", std::vector<ProvidedFunction>{}, far);
            CHECK(call_add(here, may_call("far"), "far", {1, 1}).refusal() == far);
        }
    }
    SUBCASE("each name is its own") {
        std::vector<std::string> names;
        for (int i = 0; i <= static_cast<int>(CallRefusal::AnswerRefused); ++i) {
            const std::string n = name_of(static_cast<CallRefusal>(i));
            for (const std::string& seen : names) {
                CHECK(seen != n);
            }
            CHECK(n != "?");
            names.push_back(n);
        }
    }
}

TEST_CASE("the tap tells each call and how it ended, inside the caller's delivery and before "
          "it") {
    Switchboard sb;
    const WeaveId adder = place<Adder<1>>(sb, Grant{}, "arith");
    std::vector<Seen> tap;
    (void)sb.add_observer([&tap](const BusEvent& e) {
        tap.push_back(Seen{e.kind, e.seq, e.sender, e.target, e.addressed_role, e.schema_name,
                           e.call_refusal, e.call_words});
    });
    const WeaveId caller = place<Caller>(sb, may_call("arith"), "", [](Mail& m) {
        (void)m.call<CallSum>("arith", CallAdd{kCallFails, 0});
        (void)m.call<CallSum>("arith", CallAdd{1, 2});
    });
    const Ticket t = sb.send(caller, Message(to_value(CallKnock{"", 0, 0})));
    sb.drain_until_idle();
    REQUIRE(tap.size() == 3);
    CHECK(tap[0].kind == EventKind::Called);
    CHECK(tap[0].refusal == CallRefusal::Failed);
    CHECK(tap[0].words == "asked to fail");
    CHECK(tap[1].kind == EventKind::Called);
    CHECK(tap[1].refusal == CallRefusal::None);
    for (const std::size_t i : {std::size_t{0}, std::size_t{1}}) {
        CHECK(tap[i].seq == t.seq); // inside the caller's delivery
        CHECK(tap[i].sender == caller);
        CHECK(tap[i].target == adder);
        CHECK(tap[i].role == "arith");
        CHECK(tap[i].shape == "CallAdd");
    }
    // The handler finished, and the observer was told of the failed call inside it first.
    CHECK(tap[2].kind == EventKind::Delivered);
    CHECK(tap[2].seq == t.seq);
    CHECK(tap[2].target == caller);
}

TEST_CASE("a provider replaced between two calls answers the second as its successor") {
    Switchboard sb;
    const WeaveId first = place<Adder<1>>(sb, Grant{}, "arith");
    CHECK(call_add(sb, may_call("arith"), "arith", {1, 1})->by == 1);
    (void)sb.unregister_weave(first);
    CHECK(call_add(sb, may_call("arith"), "arith", {1, 1}).refusal() == CallRefusal::NoHolder);
    (void)place<Adder<2>>(sb, Grant{}, "arith");
    const Called<CallSum> second = call_add(sb, may_call("arith"), "arith", {1, 1});
    REQUIRE(second);
    CHECK(second->by == 2);
    CHECK(second->value == 2);
}

TEST_CASE("a call rule is read at the call: delegated live, and contained only by the same "
          "rule") {
    LiveAuthority rule = LiveAuthority{}.allow_call("arith", "CallAdd", 1);
    CHECK_FALSE(rule.empty());
    CHECK(rule.contains(LiveAuthority{}.allow_call("arith", "CallAdd", 1)));
    CHECK_FALSE(rule.contains(LiveAuthority{}.allow_call("arith", "CallAdd", 2)));
    CHECK_FALSE(rule.contains(LiveAuthority{}.allow_call("other", "CallAdd", 1)));
    CHECK_FALSE(LiveAuthority{}.allow_any().allow_observe_any().contains(rule));
    CHECK(effective_permits_call(LiveAuthority{}, rule, "arith", "CallAdd", 1));

    Switchboard sb;
    (void)place<Adder<1>>(sb, Grant{}, "arith");
    std::optional<CallRefusal> seen;
    const WeaveId caller = place<Caller>(sb, Grant{}, "", [&seen](Mail& m) {
        seen = m.call<CallSum>("arith", CallAdd{1, 1}).refusal();
    });
    sb.send(caller, Message(to_value(CallKnock{"", 0, 0})));
    sb.drain_until_idle();
    CHECK(seen == CallRefusal::NotAuthorized);
    GrantAuthority authority = host_grant_authority(sb, caller, rule);
    std::optional<GrantChange> change;
    const WeaveId admin = place<Caller>(sb, Grant{}, "", [&](Mail& m) {
        change = m.delegate_authority(authority, rule);
    });
    sb.send(admin, Message(to_value(CallKnock{"", 0, 0})));
    sb.drain_until_idle();
    REQUIRE(change.has_value());
    REQUIRE(static_cast<bool>(*change));
    sb.send(caller, Message(to_value(CallKnock{"", 0, 0})));
    sb.drain_until_idle();
    CHECK(seen == CallRefusal::None);
}

TEST_CASE("a provided function's shapes meet the agreement wall, and one shape names one "
          "function") {
    SUBCASE("a provider whose input disagrees with a definition the bus holds is refused") {
        Switchboard sb;
        (void)place<wrong::Holder>(sb, Grant{}, "");
        CHECK_THROWS_AS(place<Adder<1>>(sb, Grant{}, "arith"), SchemaConflict);
    }
    SUBCASE("two functions from one shape are refused") {
        ProvidedFunction one{schema_of<CallAdd>(), schema_of<CallSum>(), [](const Value&) {
                                 return ProvidedAnswer{};
                             }};
        Switchboard sb;
        CHECK_THROWS_AS(place<Raw>(sb, Grant{}, "twice", std::vector<ProvidedFunction>{one, one},
                                   CallRefusal::None),
                        std::invalid_argument);
    }
}

} // TEST_SUITE("call")
