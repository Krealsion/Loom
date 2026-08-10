// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// WEAVER-1 — one human being, sitting in one seat, deciding what one live session
// is allowed to say. The Kernel enforces, the Weaver decides, the session acts.
//
// What this suite is watching for, stated as the failures it must catch:
//
//   a decision accepted from someone who is not the user      (operator identity)
//   a request accepted from someone who is not the subject    (requester identity)
//   a session that could name itself in a payload             (no forgeable field)
//   an answer that is not Loom's answer to the real ask       (answer authority)
//   a second request quietly displacing the pending one       (one at a time)
//   a decision landing on a request nobody was shown          (no banked yes)
//   a human approval widening the Weaver's ceiling            (Kernel is final)
//   a revocation that leaves the rule effective               (the off switch)
//   a revocation that also strips the admission baseline      (union, not override)
//   a dead session's authority being installed anyway         (fail safe)
//   the Weaver becoming the author of the session's work      (provenance)
//   requester prose reaching a terminal uninterpreted         (no escape injection)
//   a description that is not what the bus will apply         (no shadow state)

#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/host/grant_wiring.hpp>
#include <zen/weave.hpp>
#include <zen/weaver/weaver.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace loom;
using namespace sbfx;

namespace {

// ---- the world the story happens in ----------------------------------------
//
// Test-local shapes: the WEAVER-1 vocabulary is permanent, but `Work` and the
// host's nudge are this suite's own scenery, exactly as GRANT-0 kept its.

constexpr const char* kServiceRole = "some.service";
constexpr const char* kWeaverRole = "loom.weaver";

struct Work {
    std::int64_t n = 0;
    ZEN_SHAPE(Work, 1, ZEN_FIELD(n));
};
struct OtherWork {
    std::int64_t n = 0;
    ZEN_SHAPE(OtherWork, 1, ZEN_FIELD(n));
};
/// The host's nudge — an ungated root send, the way a test makes a weave act.
struct Go {
    std::int64_t step = 0;
    ZEN_SHAPE(Go, 1, ZEN_FIELD(step));
};
struct Nothing {
    std::int64_t n = 0;
    ZEN_SHAPE(Nothing, 1, ZEN_FIELD(n));
};

namespace step {
inline constexpr std::int64_t kWork = 1;     ///< session: try to speak Work to the service
inline constexpr std::int64_t kAsk = 2;      ///< session/intruder: RequestAuthority
inline constexpr std::int64_t kDescribe = 3; ///< either side: DescribeAuthority
inline constexpr std::int64_t kApprove = 4;  ///< operator/intruder: ApproveAuthority
inline constexpr std::int64_t kRefuse = 5;   ///< operator: RefuseAuthority
inline constexpr std::int64_t kRevoke = 6;   ///< operator: RevokeAuthority
} // namespace step

/// An ordinary service. It says nothing; it only records WHO it heard from,
/// which is the assertion the whole architecture turns on.
class Service final : public WeaveBase<Service, Nothing, Accept<Work>> {
public:
    std::vector<WeaveId> heard_from;
    std::vector<std::int64_t> heard;

    void on(const Work& w, Mail& mail) {
        heard_from.push_back(mail.sender());
        heard.push_back(w.n);
    }
};

/// THE GOVERNED SUBJECT — deliberately the most boring weave here.
///
/// It is not called `TerminalSession`: it has no transcript, no composer and no
/// command language, and claiming TERM-0's noun for something that has none of
/// them would mislead the next reader far more than a plain name costs. What it
/// does have is exactly what the security story needs — an identity, a baseline
/// grant, an ordinary ask, an ordinary answer, and an ordinary retry it decides
/// on for itself.
class Session final
    : public WeaveBase<Session, Nothing, Accept<Go, AuthorityGranted, Refused, AuthorityDescription>,
                       Emit<Work, RequestAuthority, DescribeAuthority>> {
public:
    /// What the next `kAsk` will request. A field of the test, never of the wire.
    RequestAuthority want{"Work", 1, kServiceRole, ""};

    struct Answer {
        std::string kind;   ///< "granted" | "refused"
        std::string detail; ///< the basis, or the reason
        bool answers_ask = false;
    };
    std::vector<Answer> answers;
    std::vector<AuthorityDescription> described;
    int works_attempted = 0;

    void on(const Go& g, Mail& mail) {
        if (g.step == step::kWork) {
            // AN EXPLICIT RETRY, IN THE SESSION'S OWN CODE. Nothing in Loom or
            // in the Weaver replays a refused message; if this line were not
            // here, no Work would ever reach the service.
            ++works_attempted;
            (void)mail.send_to_role(kServiceRole, Work{works_attempted});
        } else if (g.step == step::kAsk) {
            (void)mail.send_to_role(kWeaverRole, want);
        } else if (g.step == step::kDescribe) {
            (void)mail.send_to_role(kWeaverRole, DescribeAuthority{});
        }
    }
    void on(const AuthorityGranted& a, Mail& mail) {
        answers.push_back({"granted", a.basis, mail.answers_ask()});
    }
    void on(const Refused& r, Mail& mail) {
        answers.push_back({"refused", r.reason, mail.answers_ask()});
    }
    void on(const AuthorityDescription& d, Mail&) { described.push_back(d); }
};

/// THE OPERATOR SEAT — a stand-in for the human's console. It composes ordinary
/// registered messages and sends them down the ordinary gated path; it calls no
/// grant API and holds no capability.
class OperatorSeat final
    : public WeaveBase<OperatorSeat, Nothing,
                       Accept<Go, AuthorityPrompt, Ack, Refused, AuthorityDescription>,
                       Emit<ApproveAuthority, RefuseAuthority, RevokeAuthority, DescribeAuthority>> {
public:
    std::vector<AuthorityPrompt> prompts;
    std::vector<std::string> refusals;
    std::vector<AuthorityDescription> described;
    int acks = 0;

    void on(const Go& g, Mail& mail) {
        if (g.step == step::kApprove) {
            (void)mail.send_to_role(kWeaverRole, ApproveAuthority{});
        } else if (g.step == step::kRefuse) {
            (void)mail.send_to_role(kWeaverRole, RefuseAuthority{});
        } else if (g.step == step::kRevoke) {
            (void)mail.send_to_role(kWeaverRole, RevokeAuthority{});
        } else if (g.step == step::kDescribe) {
            (void)mail.send_to_role(kWeaverRole, DescribeAuthority{});
        }
    }
    void on(const AuthorityPrompt& p, Mail&) { prompts.push_back(p); }
    void on(const Ack&, Mail&) { ++acks; }
    void on(const Refused& r, Mail&) { refusals.push_back(r.reason); }
    void on(const AuthorityDescription& d, Mail&) { described.push_back(d); }
};

/// A weave that can REACH the Weaver and is neither the operator nor the
/// governed session. It accepts every answer the Weaver could possibly send it,
/// so a wrong answer would be seen rather than dropped unnoticed.
class Intruder final
    : public WeaveBase<Intruder, Nothing,
                       Accept<Go, Refused, Ack, AuthorityGranted, AuthorityDescription>,
                       Emit<ApproveAuthority, RequestAuthority, DescribeAuthority>> {
public:
    std::vector<std::string> refusals;
    std::vector<std::string> granted;
    std::vector<AuthorityDescription> described;
    int acks = 0;

    void on(const Go& g, Mail& mail) {
        if (g.step == step::kApprove) {
            (void)mail.send_to_role(kWeaverRole, ApproveAuthority{});
        } else if (g.step == step::kAsk) {
            (void)mail.send_to_role(kWeaverRole, RequestAuthority{"Work", 1, kServiceRole, ""});
        } else if (g.step == step::kDescribe) {
            (void)mail.send_to_role(kWeaverRole, DescribeAuthority{});
        }
    }
    void on(const Refused& r, Mail&) { refusals.push_back(r.reason); }
    void on(const Ack&, Mail&) { ++acks; }
    void on(const AuthorityGranted& a, Mail&) { granted.push_back(a.basis); }
    void on(const AuthorityDescription& d, Mail&) { described.push_back(d); }
};

/// Mount a woven weave and keep a non-owning handle on it (the bus owns it).
template <class W, class... Args>
std::pair<WeaveId, W*> place(Switchboard& bus, Grant grant, const std::string& role,
                             Args&&... args) {
    auto owned = std::make_unique<W>(std::forward<Args>(args)...);
    W* raw = owned.get();
    const WeaveId id = role.empty()
                           ? bus.register_weave(std::move(owned), std::move(grant))
                           : bus.register_weave(std::move(owned), std::move(grant), role);
    raw->zen_set_self(id);
    return {id, raw};
}

/// THE WHOLE CAST, BOOTSTRAPPED THE WAY A HOST WOULD.
///
/// The mount order is itself a finding. A governed session's baseline names its
/// Weaver BY ROLE, so the session and the operator can both be admitted before
/// the Weaver exists — which is necessary, because the Weaver cannot be
/// constructed until the capability naming the session exists, and that needs
/// the session's id. The role is pure routing: it confers no authority
/// whatever (pinned by its own case below), and the administrative power is the
/// capability the host mints separately.
struct Cast {
    Switchboard bus;
    WeaveId service_id{}, session_id{}, operator_id{}, intruder_id{}, weaver_id{};
    Service* service = nullptr;
    Session* session = nullptr;
    OperatorSeat* op = nullptr;
    Intruder* intruder = nullptr;
    Weaver* weaver = nullptr;
    std::vector<TapRecord> tap;

    /// `ceiling` is what the host lets this Weaver ever delegate. The default is
    /// exactly one rule: Work v1 may be spoken to the service's office.
    explicit Cast(LiveAuthority ceiling = default_ceiling()) {
        // The service: holds the office, says nothing at all.
        std::tie(service_id, service) =
            place<Service>(bus, Grant::nothing(), std::string(kServiceRole));

        // The governed session's FLOOR: it may ask its Weaver two questions and
        // may do nothing else. In particular it may NOT speak Work.
        Grant session_base;
        session_base.allow_to_role("zen.RequestAuthority", 1, kWeaverRole);
        session_base.allow_to_role("zen.DescribeAuthority", 1, kWeaverRole);
        std::tie(session_id, session) = place<Session>(bus, std::move(session_base), "");

        // The operator's bootstrap: it may decide, and may inspect. It holds no
        // capability and no grant over anything else.
        Grant op_base;
        op_base.allow_to_role("zen.ApproveAuthority", 1, kWeaverRole);
        op_base.allow_to_role("zen.RefuseAuthority", 1, kWeaverRole);
        op_base.allow_to_role("zen.RevokeAuthority", 1, kWeaverRole);
        op_base.allow_to_role("zen.DescribeAuthority", 1, kWeaverRole);
        std::tie(operator_id, op) = place<OperatorSeat>(bus, std::move(op_base), "");

        // ARTIFICIALLY ABLE TO REACH THE WEAVER, on purpose: the point of the
        // wrong-operator and wrong-requester cases is that reachability is not
        // identity, and that cannot be shown by a weave the gate stops first.
        Grant intruder_base;
        intruder_base.allow_to_role("zen.ApproveAuthority", 1, kWeaverRole);
        intruder_base.allow_to_role("zen.RequestAuthority", 1, kWeaverRole);
        intruder_base.allow_to_role("zen.DescribeAuthority", 1, kWeaverRole);
        std::tie(intruder_id, intruder) = place<Intruder>(bus, std::move(intruder_base), "");

        // THE TWO POWERS, MINTED SEPARATELY BY THE HOST.
        //
        // What the Weaver may DELEGATE: the ceiling, on exactly one subject.
        GrantAuthority cap = host_grant_authority(bus, session_id, std::move(ceiling));
        // What the Weaver may SAY: an ordinary grant, and it is not the ceiling.
        // Note the asymmetry — it may say "no" to anyone who reaches it (a
        // refusal is speech, and silence would be indistinguishable from a lost
        // message), but it may say "yes" to exactly two weaves.
        std::tie(weaver_id, weaver) =
            place<Weaver>(bus, weaver_grant(session_id, operator_id), std::string(kWeaverRole),
                          std::move(cap), operator_id);

        bus.add_observer([this](const BusEvent& e) { tap.push_back(to_record(e)); });
    }

    static LiveAuthority default_ceiling() {
        LiveAuthority ceiling;
        ceiling.allow_to_role("Work", 1, kServiceRole);
        return ceiling;
    }

    static Grant weaver_grant(WeaveId session, WeaveId op) {
        Grant g;
        g.allow("zen.AuthorityPrompt", 1, op);
        g.allow("zen.AuthorityGranted", 1, session);
        g.allow("zen.AuthorityDescription", 1, op);
        g.allow("zen.AuthorityDescription", 1, session);
        g.allow("zen.Ack", 1, op);
        g.allow_to_any("zen.Refused", 1);
        return g;
    }

    /// Nudge a weave and drain the bus.
    void go(WeaveId who, std::int64_t s) {
        bus.send(who, Message(to_value(Go{s})));
        bus.pump();
    }
    void clear_tap() { tap.clear(); }

    /// Did the bus refuse a `shape` addressed at the service's office for want of
    /// authority? (A role-targeted refusal is recorded before the role resolves,
    /// so the target id is not yet the service's.)
    bool denied(const std::string& shape) const {
        for (const TapRecord& r : tap) {
            if (r.kind == EventKind::Refused && r.reason == RefusalReason::CapabilityDenied &&
                r.schema == shape) {
                return true;
            }
        }
        return false;
    }
};

/// Every field of a shape, in declaration order — so a case can assert the
/// COMPLETE field set rather than only the fields it thought to look for.
std::vector<std::string> field_names(const Schema& s) {
    std::vector<std::string> out;
    for (const Field& f : s.fields()) {
        out.push_back(f.name);
    }
    return out;
}

// ---- compile-time proofs ----------------------------------------------------
//
// The Weaver's power must be a capability the host handed it, and nothing it
// could reach for on its own. A constructor taking a Switchboard& would be
// exactly that reach — being handed one IS being a host — so the strongest
// available statement is that no such constructor exists to call.

static_assert(std::is_constructible_v<Weaver, GrantAuthority, WeaveId>,
              "WEAVER-1: a Weaver is built from a capability and an operator seat");
static_assert(!std::is_constructible_v<Weaver, Switchboard&>,
              "WEAVER-1: a Weaver must never be constructible from a Switchboard");
static_assert(!std::is_constructible_v<Weaver, Switchboard&, GrantAuthority, WeaveId>,
              "WEAVER-1: a Weaver must never be constructible from a Switchboard");
// It is a weave like any other, dispatched through the same vtable as every
// other participant — not a privileged object the bus knows about.
static_assert(std::is_base_of_v<loom::Weave, Weaver>, "WEAVER-1: the Weaver is an ordinary weave");

} // namespace

TEST_SUITE("weaver") {

// =========================================================================
// The vocabulary itself — the laws that live in the field lists.
// =========================================================================

TEST_CASE("the request language carries no identity and no subject to forge") {
    // THE FIELD SET, COMPLETE. Asserting only "there is no `requester`" would
    // pass for a shape that grew `subject`, `on_behalf_of` or `as_user` instead.
    // This is the line that keeps the impersonation surface from being reopened
    // one plausible field at a time.
    CHECK(field_names(*schema_of<RequestAuthority>()) ==
          std::vector<std::string>{"shape", "version", "to_role", "purpose"});

    // The operator's four decision shapes carry NOTHING. There is no subject to
    // point at another session, and no boolean whose default could mean yes.
    CHECK(schema_of<ApproveAuthority>()->fields().empty());
    CHECK(schema_of<RefuseAuthority>()->fields().empty());
    CHECK(schema_of<RevokeAuthority>()->fields().empty());
    CHECK(schema_of<DescribeAuthority>()->fields().empty());

    // Approve and refuse are DIFFERENT SHAPES, so the decision is an identity
    // rather than a value: a mistyped shape name is a gate refusal, never a yes.
    CHECK(schema_of<ApproveAuthority>()->content_id() != schema_of<RefuseAuthority>()->content_id());

    // The answers, and the prompt, pinned whole.
    CHECK(field_names(*schema_of<AuthorityGranted>()) == std::vector<std::string>{"basis"});
    CHECK(field_names(*schema_of<AuthorityPrompt>()) ==
          std::vector<std::string>{"requester", "shape", "version", "to_role", "until",
                                   "requester_says"});
    CHECK(field_names(*schema_of<AuthorityDescription>()) ==
          std::vector<std::string>{"subject", "base", "delegated"});

    // Ordinary registered shapes, carrying the substrate's wire prefix — nothing
    // here is special-cased by the bus.
    CHECK(schema_of<RequestAuthority>()->name() == "zen.RequestAuthority");
    CHECK(schema_of<RequestAuthority>()->version() == 1);
    CHECK(schema_of<AuthorityPrompt>()->name() == "zen.AuthorityPrompt");
    CHECK(schema_of<ApproveAuthority>()->name() == "zen.ApproveAuthority");
    CHECK(schema_of<RefuseAuthority>()->name() == "zen.RefuseAuthority");
    CHECK(schema_of<RevokeAuthority>()->name() == "zen.RevokeAuthority");
    CHECK(schema_of<DescribeAuthority>()->name() == "zen.DescribeAuthority");
    CHECK(schema_of<AuthorityGranted>()->name() == "zen.AuthorityGranted");
    CHECK(schema_of<AuthorityDescription>()->name() == "zen.AuthorityDescription");
}

// =========================================================================
// The phase, in one test: the defining WEAVER-1 witness.
// =========================================================================

TEST_CASE("a human puts one session in reach of one service, and takes it back") {
    Cast c;

    // 1. THE SESSION TRIES, AND IS DENIED. Its baseline says it may ask its
    //    Weaver two questions. It says nothing about Work.
    c.go(c.session_id, step::kWork);
    CHECK(c.service->heard.empty());
    CHECK(c.denied("Work"));

    // NOTE what is NOT asserted: that the session LEARNED it was denied. A
    // sender receives no asynchronous send fate today (a standing Loom seam),
    // so the session below asks because its own logic says to, not because it
    // observed the refusal.
    CHECK(c.session->answers.empty());

    // 2. THE SESSION ASKS. An ordinary send of an ordinary registered shape,
    //    carrying no claim about who it is.
    c.go(c.session_id, step::kAsk);

    // 3. THE OPERATOR IS SHOWN THE REQUEST — the requester as the bus stamped
    //    it, and the rule as the Weaver parsed it.
    REQUIRE(c.op->prompts.size() == 1);
    const AuthorityPrompt& p = c.op->prompts[0];
    CHECK(p.requester == static_cast<std::int64_t>(c.session_id.value));
    CHECK(p.shape == "Work");
    CHECK(p.version == 1);
    CHECK(p.to_role == kServiceRole);
    // ...and the prompt does not offer a one-shot allow, because there is no
    // such thing to offer.
    CHECK(p.until.find("NOT a one-time allow") != std::string::npos);
    // Nothing has been installed merely by asking.
    CHECK(c.session->answers.empty());

    // 4. THE OPERATOR SAYS YES — as an ordinary Loom message, through its
    //    ordinary gated send path. No host call, no grant API.
    c.go(c.operator_id, step::kApprove);
    CHECK(c.op->acks == 1);
    CHECK(c.op->refusals.empty());

    // 5. THE SESSION HEARS LOOM'S OWN ANSWER to the request it actually sent.
    REQUIRE(c.session->answers.size() == 1);
    CHECK(c.session->answers[0].kind == "granted");
    CHECK(c.session->answers[0].detail == "delegated");
    CHECK(c.session->answers[0].answers_ask); // Loom's word, not a shape lookalike

    // 6. ...AND NOTHING ELSE HAPPENED. Approval changed authority; it did not
    //    perform, replay or broker the work.
    CHECK(c.service->heard.empty());

    // 7. THE SESSION RETRIES, EXPLICITLY, in its own code.
    c.go(c.session_id, step::kWork);
    REQUIRE(c.service->heard.size() == 1);
    CHECK(c.service->heard[0] == 2); // the RETRY, not the message refused in step 1

    // 8. THE PROVENANCE ASSERTION — the line the whole architecture turns on.
    //    If the Weaver had brokered the retry, this is what would fail.
    REQUIRE(c.service->heard_from.size() == 1);
    CHECK(c.service->heard_from[0] == c.session_id);
    CHECK(c.service->heard_from[0] != c.weaver_id);
    CHECK(c.service->heard_from[0] != c.operator_id);

    // 9. THE OPERATOR INSPECTS what the Kernel is actually enforcing.
    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->described.size() == 1);
    const AuthorityDescription& d = c.op->described[0];
    CHECK(d.subject == static_cast<std::int64_t>(c.session_id.value));
    // The baseline the host admitted: the two questions, and nothing more.
    CHECK(d.base == std::vector<std::string>{
                        "zen.RequestAuthority v1 -> role loom.weaver",
                        "zen.DescribeAuthority v1 -> role loom.weaver"});
    // ...and, separately, what the user delegated.
    CHECK(d.delegated == std::vector<std::string>{"Work v1 -> role some.service"});

    // 10. THE OFF SWITCH.
    c.go(c.operator_id, step::kRevoke);
    CHECK(c.op->acks == 2);
    CHECK(c.op->refusals.empty());

    // 11. THE SESSION IS DENIED AGAIN — at DELIVERY, off the live record.
    c.clear_tap();
    c.go(c.session_id, step::kWork);
    CHECK(c.service->heard.size() == 1); // still only the one from step 7
    CHECK(c.denied("Work"));

    // 12. ...AND THE BASELINE SURVIVED. Revocation takes back what was
    //     delegated; it cannot subtract what the host granted at admission.
    c.go(c.session_id, step::kDescribe);
    REQUIRE(c.session->described.size() == 1);
    CHECK(c.session->described[0].delegated.empty());
    CHECK(c.session->described[0].base.size() == 2);
}

// =========================================================================
// The user says no.
// =========================================================================

TEST_CASE("a refusal changes nothing, and the session is told so by Loom") {
    Cast c;
    c.go(c.session_id, step::kAsk);
    REQUIRE(c.op->prompts.size() == 1);

    c.go(c.operator_id, step::kRefuse);
    CHECK(c.op->acks == 1);

    // The session hears no — as the authenticated answer to its own ask, never
    // as silence.
    REQUIRE(c.session->answers.size() == 1);
    CHECK(c.session->answers[0].kind == "refused");
    CHECK(c.session->answers[0].answers_ask);
    CHECK(c.session->answers[0].detail.find("operator refused") != std::string::npos);

    // Nothing partial was installed.
    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->described.size() == 1);
    CHECK(c.op->described[0].delegated.empty());

    c.clear_tap();
    c.go(c.session_id, step::kWork);
    CHECK(c.service->heard.empty());
    CHECK(c.denied("Work"));
}

// =========================================================================
// The human does not outrank the capability.
// =========================================================================

TEST_CASE("an approval cannot widen the authority the host let this Weaver delegate") {
    Cast c; // ceiling: Work v1 -> role some.service, and nothing else
    c.session->want = RequestAuthority{"OtherWork", 1, kServiceRole, ""};
    c.go(c.session_id, step::kAsk);

    // The Weaver does NOT pre-screen against its ceiling: there is exactly one
    // adjudicator of what may be installed, and it is the Kernel. So the
    // operator is asked...
    REQUIRE(c.op->prompts.size() == 1);
    CHECK(c.op->prompts[0].shape == "OtherWork");

    // ...says yes...
    c.go(c.operator_id, step::kApprove);

    // ...and the Kernel refuses it anyway.
    CHECK(c.op->acks == 0);
    REQUIRE(c.op->refusals.size() == 1);
    CHECK(c.op->refusals[0].find("outside the authority this Weaver may delegate") !=
          std::string::npos);
    // The refusal says nothing about whether that office exists — a policy door
    // must not become the service directory the delivery path refuses to be.
    CHECK(c.op->refusals[0].find(kServiceRole) == std::string::npos);

    // The session is told, authentically, that it did not get it.
    REQUIRE(c.session->answers.size() == 1);
    CHECK(c.session->answers[0].kind == "refused");
    CHECK(c.session->answers[0].answers_ask);

    // And nothing changed.
    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->described.size() == 1);
    CHECK(c.op->described[0].delegated.empty());

    // The pending slot is clear either way: a failed approval does not leave a
    // request hanging for the next decision to land on.
    CHECK_FALSE(c.weaver->has_pending_request());
}

// =========================================================================
// Reachability is not identity.
// =========================================================================

TEST_CASE("reaching the Weaver does not make a weave the user") {
    Cast c;
    c.go(c.session_id, step::kAsk);
    REQUIRE(c.op->prompts.size() == 1);

    // The intruder holds a real grant that really does deliver ApproveAuthority
    // to the Weaver. The bus is satisfied; the POLICY is not.
    c.go(c.intruder_id, step::kApprove);

    REQUIRE(c.intruder->refusals.size() == 1);
    CHECK(c.intruder->refusals[0].find("configured operator seat") != std::string::npos);
    CHECK(c.intruder->acks == 0);

    // No authority moved, and the operator's own request is still waiting for
    // the operator.
    CHECK(c.weaver->has_pending_request());
    CHECK(c.session->answers.empty());
    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->described.size() == 1);
    CHECK(c.op->described[0].delegated.empty());

    // ...and the real operator's yes still applies to the request it was shown.
    c.go(c.operator_id, step::kApprove);
    REQUIRE(c.session->answers.size() == 1);
    CHECK(c.session->answers[0].kind == "granted");
}

TEST_CASE("reaching the Weaver does not make a weave the governed session") {
    Cast c;

    // No payload identity exists to forge, so the intruder cannot even TRY to
    // claim it is the session: the Weaver reads the bus stamp.
    c.go(c.intruder_id, step::kAsk);

    REQUIRE(c.intruder->refusals.size() == 1);
    CHECK(c.intruder->refusals[0].find("session this Weaver governs") != std::string::npos);
    CHECK(c.intruder->granted.empty());
    // The operator was never troubled, and nothing is pending.
    CHECK(c.op->prompts.empty());
    CHECK_FALSE(c.weaver->has_pending_request());

    // An approval now has nothing to approve — the intruder's ask created no
    // pending request that a later yes could land on.
    c.go(c.operator_id, step::kApprove);
    REQUIRE(c.op->refusals.size() == 1);
    CHECK(c.op->refusals[0].find("no authority request is pending") != std::string::npos);
}

TEST_CASE("a stranger cannot read the governed session's authority") {
    Cast c;
    c.go(c.intruder_id, step::kDescribe);
    REQUIRE(c.intruder->refusals.size() == 1);
    CHECK(c.intruder->described.empty()); // it accepts the shape; it was never sent one
}

// =========================================================================
// One request at a time, and no banked decisions.
// =========================================================================

TEST_CASE("a second request cannot displace the one the operator is deciding") {
    Cast c;
    c.go(c.session_id, step::kAsk); // request A: Work v1 -> role some.service
    REQUIRE(c.op->prompts.size() == 1);

    // The same session asks for something else while A is pending.
    c.session->want = RequestAuthority{"OtherWork", 1, kServiceRole, ""};
    c.go(c.session_id, step::kAsk); // request B

    // B is refused VISIBLY — and A was not overwritten, so the operator saw one
    // prompt and only one.
    REQUIRE(c.session->answers.size() == 1);
    CHECK(c.session->answers[0].kind == "refused");
    CHECK(c.session->answers[0].answers_ask);
    CHECK(c.session->answers[0].detail.find("already awaiting the operator") != std::string::npos);
    CHECK(c.op->prompts.size() == 1);

    // THE DISCRIMINATOR: the operator's yes applies to A — the request it was
    // actually shown — and never to B, which it never saw.
    c.go(c.operator_id, step::kApprove);
    REQUIRE(c.session->answers.size() == 2);
    CHECK(c.session->answers[1].kind == "granted");
    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->described.size() == 1);
    CHECK(c.op->described[0].delegated == std::vector<std::string>{"Work v1 -> role some.service"});
}

TEST_CASE("a decision with nothing pending changes nothing and cannot be banked") {
    Cast c;

    // An approval arrives before any request exists.
    c.go(c.operator_id, step::kApprove);
    REQUIRE(c.op->refusals.size() == 1);
    CHECK(c.op->refusals[0].find("no authority request is pending") != std::string::npos);
    CHECK(c.op->acks == 0);

    // It did not wait around. The session's later request is still pending
    // afterwards — the earlier yes was not applied to it.
    c.go(c.session_id, step::kAsk);
    REQUIRE(c.op->prompts.size() == 1);
    CHECK(c.weaver->has_pending_request());
    CHECK(c.session->answers.empty());
    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->described.size() == 1);
    CHECK(c.op->described[0].delegated.empty());
}

TEST_CASE("a decision after the decision changes nothing") {
    Cast c;
    c.go(c.session_id, step::kAsk);
    c.go(c.operator_id, step::kApprove);
    REQUIRE(c.session->answers.size() == 1);

    // A second yes, with nothing outstanding.
    c.go(c.operator_id, step::kApprove);
    REQUIRE(c.op->refusals.size() == 1);
    CHECK(c.op->refusals[0].find("no authority request is pending") != std::string::npos);
    CHECK(c.session->answers.size() == 1); // the session heard nothing new

    // ...and no duplicate rule grew.
    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->described.size() == 1);
    CHECK(c.op->described[0].delegated == std::vector<std::string>{"Work v1 -> role some.service"});
}

// =========================================================================
// Asking for what you already have.
// =========================================================================

TEST_CASE("a request the session's authority already covers is answered, not put to the human") {
    Cast c;
    // Its BASELINE already permits this exact rule.
    c.session->want = RequestAuthority{"zen.DescribeAuthority", 1, kWeaverRole, ""};
    c.go(c.session_id, step::kAsk);

    // Nobody was woken, nothing was installed, and the session was told the
    // truth about WHY it may do this — the Weaver does not take credit for
    // authority the host granted.
    CHECK(c.op->prompts.empty());
    REQUIRE(c.session->answers.size() == 1);
    CHECK(c.session->answers[0].kind == "granted");
    CHECK(c.session->answers[0].detail == "already-permitted");
    CHECK(c.session->answers[0].answers_ask);
    CHECK_FALSE(c.weaver->has_pending_request());

    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->described.size() == 1);
    CHECK(c.op->described[0].delegated.empty());
}

TEST_CASE("re-asking for a rule already delegated cannot grow a duplicate") {
    Cast c;
    c.go(c.session_id, step::kAsk);
    c.go(c.operator_id, step::kApprove);
    REQUIRE(c.session->answers.size() == 1);

    // The very same request again. It is now covered by the DELEGATED half, so
    // it short-circuits exactly as a baseline-covered one does.
    c.go(c.session_id, step::kAsk);
    REQUIRE(c.session->answers.size() == 2);
    CHECK(c.session->answers[1].detail == "already-permitted");
    CHECK(c.op->prompts.size() == 1); // the operator was asked once, ever

    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->described.size() == 1);
    CHECK(c.op->described[0].delegated == std::vector<std::string>{"Work v1 -> role some.service"});
}

// =========================================================================
// Malformed requests.
// =========================================================================

TEST_CASE("a structurally invalid request is refused without touching the pending slot") {
    Cast c;

    struct BadCase {
        RequestAuthority ask;
        const char* expect;
    };
    const std::vector<BadCase> bad = {
        {{"", 1, kServiceRole, ""}, "shape name"},
        {{"Work", 0, kServiceRole, ""}, "version"},
        {{"Work", 1, "", ""}, "destination role"},
        {{"Work", 4294967296LL, kServiceRole, ""}, "version"},
        {{"Wo rk", 1, kServiceRole, ""}, "shape name"},
        {{"Work", 1, std::string("bad\x1b[2Jrole"), ""}, "destination role"},
    };
    std::size_t answered = 0;
    for (const BadCase& b : bad) {
        c.session->want = b.ask;
        c.go(c.session_id, step::kAsk);
        REQUIRE(c.session->answers.size() == ++answered);
        CHECK(c.session->answers.back().kind == "refused");
        CHECK(c.session->answers.back().answers_ask);
        CHECK(c.session->answers.back().detail.find(b.expect) != std::string::npos);
        CHECK_FALSE(c.weaver->has_pending_request());
    }
    CHECK(c.op->prompts.empty()); // no malformed request ever reached the human

    // ...and a well-formed one still works afterwards.
    c.session->want = RequestAuthority{"Work", 1, kServiceRole, ""};
    c.go(c.session_id, step::kAsk);
    CHECK(c.op->prompts.size() == 1);
}

// =========================================================================
// The operator's decision surface is not a terminal the requester can drive.
// =========================================================================

TEST_CASE("requester-authored prose reaches the operator escaped, never interpreted") {
    Cast c;
    // A purpose string that, printed verbatim, would clear the screen and
    // repaint the trusted facts of the very request being decided.
    c.session->want = RequestAuthority{"Work", 1, kServiceRole,
                                       std::string("\x1b[2J\x1b[HGRANTED BY ADMIN\r\n\x07")};
    c.go(c.session_id, step::kAsk);

    REQUIRE(c.op->prompts.size() == 1);
    const std::string& says = c.op->prompts[0].requester_says;
    // Not one byte outside printable ASCII survives.
    for (const char ch : says) {
        const auto byte = static_cast<unsigned char>(ch);
        CHECK(byte >= 0x20);
        CHECK(byte <= 0x7e);
    }
    CHECK(says.find("\x1b") == std::string::npos);
    // The bytes are still SHOWN, as escapes — an operator can see exactly what
    // was sent, which is the point of escaping rather than stripping.
    CHECK(says.find("\\x1b") != std::string::npos);
    CHECK(says.find("GRANTED BY ADMIN") != std::string::npos);

    // And the trusted facts are unaffected by anything the requester wrote.
    CHECK(c.op->prompts[0].requester == static_cast<std::int64_t>(c.session_id.value));
    CHECK(c.op->prompts[0].shape == "Work");
}

TEST_CASE("a flood of prose cannot push the trusted facts off the operator's screen") {
    Cast c;
    c.session->want =
        RequestAuthority{"Work", 1, kServiceRole, std::string(64 * 1024, 'A')};
    c.go(c.session_id, step::kAsk);

    REQUIRE(c.op->prompts.size() == 1);
    const std::string& says = c.op->prompts[0].requester_says;
    CHECK(says.size() <= kMaxPurposeBytes + 32);
    // Bounded, and it SAYS it is bounded rather than silently ending early.
    CHECK(says.find("[truncated]") != std::string::npos);
}

TEST_CASE("the escaping is unambiguous about text the requester actually wrote") {
    // A requester writing the literal characters \x1b must not be able to make
    // an operator believe an escape byte was escaped, or vice versa.
    CHECK(safe_operator_text("\\x1b", 64) == "\\\\x1b");
    CHECK(safe_operator_text("\x1b", 64) == "\\x1b");
    CHECK(safe_operator_text("plain text", 64) == "plain text");
    CHECK(safe_operator_text("", 64).empty());
}

// =========================================================================
// Lifetimes.
// =========================================================================

TEST_CASE("a session that dies while pending gains nothing when the operator says yes") {
    Cast c;
    c.go(c.session_id, step::kAsk);
    REQUIRE(c.op->prompts.size() == 1);
    CHECK(c.weaver->has_pending_request());

    // The subject leaves the world while the human is deciding.
    (void)c.bus.unregister_weave(c.session_id);
    c.go(c.operator_id, step::kApprove);

    // Nothing was installed, the operator was told exactly why, and the pending
    // slot was cleared rather than left for a later decision to find.
    CHECK(c.op->acks == 0);
    REQUIRE(c.op->refusals.size() == 1);
    CHECK(c.op->refusals[0].find("session that asked is gone") != std::string::npos);
    CHECK_FALSE(c.weaver->has_pending_request());

    // WeaveIds are never reused, so nothing that mounts next can inherit this.
    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->refusals.size() == 2);
    CHECK(c.op->described.empty());
}

TEST_CASE("delegated authority dies with the session that held it") {
    Cast c;
    c.go(c.session_id, step::kAsk);
    c.go(c.operator_id, step::kApprove);
    c.go(c.session_id, step::kWork);
    REQUIRE(c.service->heard.size() == 1);

    (void)c.bus.unregister_weave(c.session_id);

    // The capability now names nothing at all, permanently.
    c.go(c.operator_id, step::kDescribe);
    CHECK(c.op->described.empty());
    REQUIRE(c.op->refusals.size() == 1);
    // Revoking a subject that is gone is a visible failure, not a silent no-op.
    c.go(c.operator_id, step::kRevoke);
    REQUIRE(c.op->refusals.size() == 2);
    CHECK(c.op->refusals[1].find("gone") != std::string::npos);
    CHECK(c.op->acks == 1); // only the approval's
}

TEST_CASE("the Weaver's death does not revoke what the operator already granted") {
    Cast c;
    c.go(c.session_id, step::kAsk);
    c.go(c.operator_id, step::kApprove);
    REQUIRE(c.session->answers.size() == 1);

    // The user's policy delegate leaves. An installed grant is not a lease, and
    // WEAVER-1 deliberately does not add RAII revocation.
    (void)c.bus.unregister_weave(c.weaver_id);

    c.go(c.session_id, step::kWork);
    REQUIRE(c.service->heard.size() == 1);
    CHECK(c.service->heard_from[0] == c.session_id);

    // ...and the consequence, stated: with the Weaver gone there is now no way
    // to revoke it by message. That is real, and it is the pressure this phase
    // records rather than papering over with a speculative lease.
    c.clear_tap();
    c.go(c.operator_id, step::kRevoke);
    CHECK(c.op->acks == 1); // still just the approval's; the revoke reached nobody
    c.go(c.session_id, step::kWork);
    CHECK(c.service->heard.size() == 2);
}

// =========================================================================
// The description is the enforcement.
// =========================================================================

TEST_CASE("what the operator reads is what the bus will apply") {
    Cast c;
    c.go(c.session_id, step::kAsk);
    c.go(c.operator_id, step::kApprove);
    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->described.size() == 1);

    // The rendered line and the enforcement decision come from the same value:
    // ask the bus the same question by trying it.
    CHECK(c.op->described[0].delegated == std::vector<std::string>{"Work v1 -> role some.service"});
    c.go(c.session_id, step::kWork);
    CHECK(c.service->heard.size() == 1);

    // A ROLE IS DESCRIBED AS A ROLE. The service holds the office right now, and
    // the description still does not name it — role authority follows whoever
    // holds the office at delivery, and saying "weave #N" would describe a
    // narrower, more permanent grant than the one that was made.
    // `weave #N` is the ONLY form a WeaveId can take in a rendered rule, so its
    // absence is the whole claim. (Hunting for the bare digits instead would
    // match the version in "v1" — a looser assertion that fails for the wrong
    // reason and would pass for the wrong reason too.)
    CHECK(c.op->described[0].delegated[0].find("weave #") == std::string::npos);
    CHECK(c.op->described[0].delegated[0].find("weave #" + std::to_string(c.service_id.value)) ==
          std::string::npos);

    // The session may inspect ITSELF, and sees the same two lists.
    c.go(c.session_id, step::kDescribe);
    REQUIRE(c.session->described.size() == 1);
    CHECK(c.session->described[0].subject == static_cast<std::int64_t>(c.session_id.value));
    CHECK(c.session->described[0].delegated == c.op->described[0].delegated);
    CHECK(c.session->described[0].base == c.op->described[0].base);
}

TEST_CASE("the rendered vocabulary states every kind of rule a description can carry") {
    LiveAuthority a;
    a.allow("Work", 1, WeaveId{7});
    a.allow_to_role("Work", 2, "some.service");
    a.allow_to_any("Ping", 1);
    a.allow_any_to(WeaveId{9});
    a.allow_any();
    a.allow_observe("Tick", 3);
    a.allow_observe_any();
    CHECK(render_authority(a) == std::vector<std::string>{
                                     "Work v1 -> weave #7",
                                     "Work v2 -> role some.service",
                                     "Ping v1 -> any target",
                                     "any shape -> weave #9",
                                     "any shape -> any target",
                                     "observe Tick v3",
                                     "observe any shape",
                                 });
    CHECK(render_authority(LiveAuthority::nothing()).empty());
}

// =========================================================================
// The role is an address, never a power.
// =========================================================================

TEST_CASE("holding the Weaver's role confers no authority at all") {
    Switchboard bus;
    // A weave in the Weaver's own office, wearing the Weaver's own grant — and
    // holding an INERT capability, because the host never minted it one.
    auto [id, weaver] = place<Weaver>(bus, Cast::weaver_grant(WeaveId{}, WeaveId{}),
                                      std::string(kWeaverRole), GrantAuthority{}, WeaveId{});
    CHECK_FALSE(weaver->governed_session().valid());

    Grant asker;
    asker.allow_to_role("zen.RequestAuthority", 1, kWeaverRole);
    auto [session_id, session] = place<Session>(bus, std::move(asker), "");
    (void)id;
    (void)session_id;

    bus.send(session_id, Message(to_value(Go{step::kAsk})));
    bus.pump();

    // It governs nobody, so it grants nobody anything. The office got the
    // message delivered; it did not make its holder an administrator.
    REQUIRE(session->answers.size() == 1);
    CHECK(session->answers[0].kind == "refused");
    CHECK_FALSE(weaver->has_pending_request());
}

TEST_CASE("a session cannot be appointed the decider of its own requests") {
    Switchboard bus;
    Grant nothing_at_all;
    auto [subject_id, subject] = place<Session>(bus, std::move(nothing_at_all), "");
    (void)subject;

    // The whole seat means nothing if the subject can sit in it: a session that
    // approves its own requests widens itself up to the ceiling, and GATE-05's
    // "no weave can widen its own authority" would be defeated by host wiring
    // rather than by a bug. Refused at the boot that wires it.
    GrantAuthority cap = host_grant_authority(bus, subject_id, Cast::default_ceiling());
    CHECK_THROWS_AS(Weaver(std::move(cap), subject_id), std::invalid_argument);

    // ...and any OTHER seat is fine, including one that does not exist yet.
    GrantAuthority cap2 = host_grant_authority(bus, subject_id, Cast::default_ceiling());
    CHECK_NOTHROW(Weaver(std::move(cap2), WeaveId{subject_id.value + 1}));
    // An inert capability governs nobody, so there is nobody for the seat to be.
    CHECK_NOTHROW(Weaver(GrantAuthority{}, WeaveId{}));
}

// =========================================================================
// The Weaver keeps no second opinion about authority.
// =========================================================================

TEST_CASE("the Weaver reads authority fresh and remembers only the human's question") {
    Cast c;
    c.go(c.session_id, step::kAsk);
    c.go(c.operator_id, step::kApprove);

    // A SECOND ADMINISTRATOR — the host itself, through a capability of its own
    // — narrows the subject behind the Weaver's back.
    const GrantAuthority host_cap =
        host_grant_authority(c.bus, c.session_id, Cast::default_ceiling());
    // (the host is not a weave; it administers through a probe holding the cap)
    Registered admin = register_probe(c.bus, {ping_schema()}, 2, true, Grant::nothing());
    admin.weave->on_handle = [&host_cap](const Message&, Bus& b, ProbeWeave&) {
        (void)b.delegate_authority(host_cap, LiveAuthority::nothing());
    };
    c.bus.send(admin.id, Message(ping(1)));
    c.bus.pump();

    // The Weaver never noticed, was never told, and does not care: its next
    // description reports the truth, because it has no picture of its own to be
    // out of date.
    c.go(c.operator_id, step::kDescribe);
    REQUIRE(c.op->described.size() == 1);
    CHECK(c.op->described[0].delegated.empty());

    // Its workflow state is the only thing it kept, and it counts events rather
    // than describing authority.
    const Value snap = c.bus.weave(c.weaver_id)->snapshot();
    REQUIRE(snap.get("prompts") != nullptr);
    CHECK(snap.get("prompts")->as_int() == 1);
    CHECK(snap.get("installed")->as_int() == 1);
    CHECK(field_names(snap.schema()) == std::vector<std::string>{"prompts", "installed"});
}

// =========================================================================
// Revocation is the whole overlay, and only the overlay.
// =========================================================================

TEST_CASE("revoking takes back every delegated rule and no baseline one") {
    LiveAuthority wide;
    wide.allow_to_role("Work", 1, kServiceRole);
    wide.allow_to_role("OtherWork", 1, kServiceRole);
    Cast c2(std::move(wide));

    // Two separate approvals, so the overlay holds two rules.
    c2.go(c2.session_id, step::kAsk);
    c2.go(c2.operator_id, step::kApprove);
    c2.session->want = RequestAuthority{"OtherWork", 1, kServiceRole, ""};
    c2.go(c2.session_id, step::kAsk);
    c2.go(c2.operator_id, step::kApprove);

    c2.go(c2.operator_id, step::kDescribe);
    REQUIRE(c2.op->described.size() == 1);
    CHECK(c2.op->described[0].delegated.size() == 2); // additive, not replaced

    // One revoke, both gone.
    c2.go(c2.operator_id, step::kRevoke);
    c2.go(c2.operator_id, step::kDescribe);
    REQUIRE(c2.op->described.size() == 2);
    CHECK(c2.op->described[1].delegated.empty());
    // ...and the two baseline rules are exactly as the host left them.
    CHECK(c2.op->described[1].base == c2.op->described[0].base);

    // The session can still come back and ask, which is the whole point of the
    // baseline surviving.
    c2.session->want = RequestAuthority{"Work", 1, kServiceRole, ""};
    c2.go(c2.session_id, step::kAsk);
    CHECK(c2.op->prompts.size() == 3);
}

TEST_CASE("revoking does not answer a question the operator has not answered") {
    Cast c;
    c.go(c.session_id, step::kAsk);
    REQUIRE(c.op->prompts.size() == 1);

    c.go(c.operator_id, step::kRevoke);
    CHECK(c.op->acks == 1);

    // Revocation acts on what is installed; the human's outstanding question is
    // a different thing and is still there to be answered either way.
    CHECK(c.weaver->has_pending_request());
    CHECK(c.session->answers.empty());
    c.go(c.operator_id, step::kApprove);
    REQUIRE(c.session->answers.size() == 1);
    CHECK(c.session->answers[0].kind == "granted");
}

} // TEST_SUITE("weaver")
