// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The authenticated lifecycle conversation — Loom's side (R2B-1).
//
// THE LAW UNDER TEST:
//
//   A role tells Loom WHERE to deliver an ask.
//   An authenticated conversation tells the asker WHO actually received it,
//   and who may answer.
//
// The gap this closes is specific and was load-bearing. A weave that must
// survive its provider being replaced addresses that provider BY ROLE — which
// is exactly the case where it cannot know the provider's WeaveId, and so cannot
// pre-bind the answer's sender. Before R2B-1 all such a weave had was a shape
// and a correlation, both of which any weave holding the same grant can produce.
// The Loom now records which incarnation the routing decision actually chose,
// and lets only that incarnation, once, speak with Loom's word behind it.
//
// WHAT THESE CASES DELIBERATELY DO NOT DO: reach around the bus. Every forgery
// below is attempted by an ORDINARY REGISTERED WEAVE holding the ordinary grant
// for the shape it forges — because "the honest API cannot express the attack"
// and "the substrate defends against the attack" are different properties, and a
// test written only through the polite path would pass while proving neither.

#include "doctest.h"

#include <zen/host/lifecycle_wiring.hpp> // the harness IS a host; a weave is not
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/lifecycle.hpp>

#include <cstdint>
#include <memory>
#include <new> // placement new: the storage-reuse pin needs a GUARANTEED address collision
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace loom;

namespace {

// ---- the vocabulary these fixtures converse in --------------------------------

struct ProvAsk {
    std::string tag;
    ZEN_SHAPE(ProvAsk, 1, ZEN_FIELD(tag));
};
struct ProvAnswer {
    std::string tag;
    ZEN_SHAPE(ProvAnswer, 1, ZEN_FIELD(tag));
};
/// "Do the thing" — a nudge that gives a hostile weave a Mail of its own to act
/// from, so its forgery is an ordinary send from an ordinary handler.
struct ProvNudge {
    ZEN_SHAPE(ProvNudge, 1);
};

struct ProvState {
    std::int64_t n = 0;
    ZEN_SHAPE(ProvState, 1, ZEN_FIELD(n));
};

/// Every answer this weave was handed, with Loom's verdict on each.
struct Received {
    struct One {
        std::string tag;
        bool attested = false;
        std::uint64_t correlation = 0;
        std::uint64_t sender = 0;
    };
    std::vector<One> answers;

    std::size_t attested_count() const {
        std::size_t n = 0;
        for (const One& a : answers) {
            if (a.attested) {
                ++n;
            }
        }
        return n;
    }
};

/// The asker. It records what it was told AND what Loom said about it — the two
/// facts this phase exists to keep apart.
class Asker : public WeaveBase<Asker, ProvState, Accept<ProvAnswer>, Emit<ProvAsk>> {
public:
    explicit Asker(Received& heard) : heard_(&heard) {}
    void on(const ProvAnswer& a, Mail& mail) {
        ++state_.n;
        heard_->answers.push_back(
            Received::One{a.tag, mail.answers_ask(), mail.correlation(), mail.sender().value});
    }

private:
    Received* heard_;
};

/// How a responder chooses to speak.
enum class Speak {
    Authenticated, ///< mail.answer(...) — Loom's word
    Ordinary,      ///< mail.send(sender, ...) — the same bytes, no word
    Twice,         ///< mail.answer(...) twice: the one-shot under test
    Silent,        ///< receive and say nothing
};

/// The role holder. Ordinary in every respect: an ordinary grant for ProvAnswer,
/// an ordinary handler, no privilege anywhere.
class Responder : public WeaveBase<Responder, ProvState, Accept<ProvAsk>, Emit<ProvAnswer>> {
public:
    Responder(std::string name, Speak how) : name_(std::move(name)), how_(how) {}

    void on(const ProvAsk&, Mail& mail) {
        ++state_.n;
        switch (how_) {
        case Speak::Silent:
            return;
        case Speak::Ordinary:
            // The same shape, the same bytes, aimed by hand at the asker. This is
            // exactly what an impersonator can do, done here by the legitimate
            // holder so the pin isolates PROVENANCE from everything else.
            mail.send(mail.sender(), ProvAnswer{name_}, mail.correlation());
            return;
        case Speak::Twice:
            mail.answer(ProvAnswer{name_ + ".first"});
            mail.answer(ProvAnswer{name_ + ".second"});
            return;
        case Speak::Authenticated:
            mail.answer(ProvAnswer{name_});
            return;
        }
    }

private:
    std::string name_;
    Speak how_;
};

/// The impersonator. It knows the shape, the correlation and the role name —
/// everything the threat model grants — and holds an ordinary grant to send
/// ProvAnswer to anyone. It forges from inside its own handler, so its send is
/// an ordinary stamped weave send and nothing about the test is a shortcut.
class Forger : public WeaveBase<Forger, ProvState, Accept<ProvNudge>, Emit<ProvAnswer>> {
public:
    Forger(WeaveId victim, std::uint64_t correlation)
        : victim_(victim), correlation_(correlation) {}

    void on(const ProvNudge&, Mail& mail) {
        ++state_.n;
        mail.send(victim_, ProvAnswer{"forged"}, correlation_);
        // ...and the other half of the same idea: try to ANSWER, from a delivery
        // that is not the conversation being impersonated. The authority belongs
        // to this delivery, so this reaches whoever nudged it — never the victim.
        mail.answer(ProvAnswer{"forged.answer"});
    }

private:
    WeaveId victim_;
    std::uint64_t correlation_;
};

/// A weave that hoards a delivered Message and re-sends it verbatim later: the
/// copy-what-you-observed attack, expressed through the honest API.
class Magpie : public WeaveBase<Magpie, ProvState, Accept<ProvAnswer, ProvNudge>,
                                Emit<ProvAnswer, ProvAsk>> {
public:
    Magpie(WeaveId victim) : victim_(victim) {}

    void on(const ProvAnswer& a, Mail& mail) {
        ++state_.n;
        kept_ = a;
        kept_attested_ = mail.answers_ask();
        kept_correlation_ = mail.correlation();
    }
    void on(const ProvNudge&, Mail& mail) { mail.send(victim_, kept_, kept_correlation_); }

    bool kept_attested_ = false;

private:
    WeaveId victim_;
    ProvAnswer kept_{};
    std::uint64_t kept_correlation_ = 0;
};

/// The copy-what-you-observed attack, done PROPERLY.
///
/// The polite Magpie above re-sends the decoded SHAPE, which builds a fresh
/// Message and therefore never touches provenance at all — a test that proves
/// nothing about the clearing rule, which is exactly what a mutation caught.
/// This one implements `loom::Weave` directly so it holds the raw `Message` and
/// the raw `Bus&`, stores the delivered envelope whole, and re-sends THAT.
///
/// That is the sharpest form of the attack the threat model allows: no forgery,
/// no guessing, no reaching around the bus — just keeping a message that really
/// did carry Loom's word and saying it again. It works only if some enqueue path
/// forgets to clear.
class RawMagpie final : public Weave {
public:
    RawMagpie(WeaveId victim, Received& seen) : victim_(victim), seen_(&seen) {}

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return {schema_of<ProvAnswer>(), schema_of<ProvNudge>()};
    }
    Value snapshot() const override {
        Value v(schema_of<ProvState>());
        v.set("n", Cell::integer(n_));
        return v;
    }
    void revive(const Value& v) override { n_ = v.get("n")->as_int(); }
    Value policy() const override {
        Value v(lifecycle_policy_schema());
        v.set("max_reloads", Cell::integer(8));
        v.set("revive_from_last_good", Cell::boolean(true));
        return v;
    }

    void handle(const Message& in, Bus& bus) override {
        ++n_;
        if (in.payload.schema().name() == std::string_view(ProvAnswer::zen_name)) {
            kept_ = std::make_unique<Message>(in); // the ENVELOPE, provenance and all
            seen_->answers.push_back(Received::One{in.payload.get("tag")->as_text(),
                                                   in.provenance.answers_ask(), in.correlation,
                                                   in.sender.value});
            return;
        }
        if (kept_ != nullptr) {
            // Verbatim. Whatever the bus does to this on the way out is the
            // whole property under test.
            bus.send(victim_, Message(*kept_));
        }
    }

    /// Did the message this weave hoarded really carry Loom's word? Without
    /// this the re-send below could be a copy of nothing.
    bool kept_was_attested() const {
        return kept_ != nullptr && kept_->provenance.answers_ask();
    }

private:
    WeaveId victim_;
    Received* seen_;
    std::unique_ptr<Message> kept_;
    std::int64_t n_ = 0;
};

/// Mount with a role bound (the sugar has no role parameter).
template <class W, class... Args>
WeaveId mount_into_role(Switchboard& bus, std::string role, Args&&... args) {
    auto weave = std::make_unique<W>(std::forward<Args>(args)...);
    W* raw = weave.get();
    Grant grant = emit_default_grant(*raw);
    const WeaveId id = bus.register_weave(std::move(weave), std::move(grant), std::move(role));
    raw->zen_set_self(id);
    return id;
}

/// Count refusals of a shape, so "refused visibly" is measured rather than
/// inferred from an absence.
struct RefusalTap {
    std::int64_t capability_denied = 0;
    std::int64_t foreign_authority = 0; ///< a capability-object failure (R2B-2)
    std::int64_t exhausted = 0;         ///< a published bound was reached (R2B-2)
    std::int64_t sender_life_ended = 0; ///< the author's life is over (R2B-2b)
    std::int64_t answer_target_changed = 0; ///< the requester is not who asked (R2B-2c)
    std::int64_t other_refusals = 0;
    std::int64_t delivered_answers = 0;
    /// The (authored, current) life pair off the last event for this shape, so the
    /// generation arithmetic can be pinned rather than inferred.
    std::uint64_t last_authored_life = 0;
    std::uint64_t last_current_life = 0;
    /// The four numbers an authenticated answer's refusal reports (R2B-2c).
    std::uint64_t expected_life = 0;
    std::uint64_t expected_incarnation = 0;
    std::uint64_t found_life = 0;
    std::uint64_t found_incarnation = 0;

    void arm(Switchboard& bus, const char* shape) {
        bus.add_observer([this, shape](const BusEvent& ev) {
            if (ev.schema_name != shape) {
                return;
            }
            last_authored_life = ev.sender_life;
            last_current_life = ev.sender_life_now;
            expected_life = ev.expected_requester_life;
            expected_incarnation = ev.expected_requester_incarnation;
            found_life = ev.requester_life_now;
            found_incarnation = ev.requester_incarnation_now;
            if (ev.kind == EventKind::Delivered) {
                ++delivered_answers;
            } else if (ev.kind == EventKind::Refused) {
                if (ev.refusal.reason == RefusalReason::ForeignAuthority) {
                    ++foreign_authority;
                } else if (ev.refusal.reason == RefusalReason::Exhausted) {
                    ++exhausted;
                } else if (ev.refusal.reason == RefusalReason::SenderLifeEnded) {
                    ++sender_life_ended;
                } else if (ev.refusal.reason == RefusalReason::AnswerTargetChanged) {
                    ++answer_target_changed;
                } else if (ev.refusal.reason == RefusalReason::CapabilityDenied) {
                    ++capability_denied;
                } else {
                    ++other_refusals;
                }
            }
        });
    }
};

constexpr const char* kProvRole = "prov.steward";
constexpr std::uint64_t kPublicCorrelation = 0xC1A1; // as public as a constant gets

// ---- the compile-surface half of the authority proof (R2B-1a) -----------------
//
// The runtime cases below can only ever show that the strongest attack a weave
// can COMPILE achieves nothing. They cannot show that a stronger one is
// unwriteable — and that is the actual claim, so it is asserted here, at compile
// time, over the exact headers a weave author is supported in using.
//
// This TU includes <zen/switchboard.hpp> and <zen/weave.hpp>: the whole
// weave-authoring surface for a native weave, and a superset of what a loaded
// weave library gets (which sees no Switchboard at all). If any of these
// expressions became well-formed, the corresponding static_assert fires and this
// file stops compiling. A source-text grep would not catch a new spelling; this
// does, because it asks the compiler the question directly.
//
// R2B-1 shipped `Switchboard::lifecycle_authority()` as a PUBLIC STATIC, so the
// third assertion below was false — no instance needed, no host involved, and a
// weave with an exact `zen.Activated` grant could manufacture a lifecycle fact
// for someone else's incarnation. These are the pins that keep it closed.

/// The questions have to be asked THROUGH A TEMPLATE. A `requires` expression
/// only swallows an invalid expression inside an immediate context, so asking it
/// of a concrete type at namespace scope is a hard compile error rather than a
/// `false` — which would make this file fail to build for the very reason it is
/// meant to assert. Access checking IS part of that immediate context, so a
/// private member reads as "cannot", exactly as a missing one does.
template <class T>
concept MintsByMemberCall = requires(T& t) { t.lifecycle_authority(); };
template <class T>
concept MintsByAnyName = requires(T& t) { t.mint_lifecycle_authority(); };
template <class T>
concept MintsByStaticCall = requires { T::lifecycle_authority(); };

/// A weave's own doors: a `Bus&` in `handle`, a `Mail&` in a maker's `on`.
/// Neither may offer a way to mint, under this name or another.
static_assert(!MintsByMemberCall<loom::Bus>,
              "R2B-1a: a weave's Bus must never expose lifecycle minting");
static_assert(!MintsByMemberCall<loom::Mail>,
              "R2B-1a: Mail must never expose lifecycle minting");
static_assert(!MintsByAnyName<loom::Mail>,
              "R2B-1a: Mail must never expose lifecycle minting under another name");

/// The static factory R2B-1 actually shipped: no instance, no host, no wall.
static_assert(!MintsByStaticCall<loom::Switchboard>,
              "R2B-1a: the lifecycle mint must not be a reachable static factory");

/// And not reachable from an instance either. A weave handed a Switchboard is
/// already host infrastructure by the host's own choice, but the mint stays
/// private so that even then it comes through the one named host-wiring
/// function rather than by helping itself.
static_assert(!MintsByMemberCall<loom::Switchboard>,
              "R2B-1a: the lifecycle mint must be private to the Switchboard");

/// Nor by naming the type: its only constructor is private, so neither a fresh
/// one nor a value-initialised one is expressible.
static_assert(!std::is_default_constructible_v<loom::LifecycleAuthority>,
              "R2B-1a: LifecycleAuthority must not be default-constructible");

/// THE POSITIVE CONTROL, without which every assertion above is satisfied just
/// as happily by a typo. Copying an authority one was HANDED is ordinary and
/// must keep working — that is how the control door holds its own.
static_assert(std::is_copy_constructible_v<loom::LifecycleAuthority>,
              "an authority you were given is yours to hold");

} // namespace

TEST_SUITE("provenance") {

TEST_CASE("the request-time role holder is the only weave whose answer carries Loom's word") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId holder = mount_into_role<Responder>(bus, kProvRole, "holder", Speak::Authenticated);
    CHECK(holder.valid());

    // The asker addresses a ROLE. It does not know, and cannot know, which
    // incarnation will receive this.
    bus.send_as(asker, holder,
                Message(to_value(ProvAsk{"one"}), asker, WeaveId{}, kPublicCorrelation));
    bus.pump();

    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].tag == "holder");
    CHECK(heard.answers[0].attested);                          // Loom vouched for it
    CHECK(heard.answers[0].correlation == kPublicCorrelation); // and Loom chose the label
    CHECK(heard.answers[0].sender == holder.value);
}

TEST_CASE("the same bytes sent ordinarily carry no word — provenance is not the shape, not the "
          "correlation, and not the sender") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    // The LEGITIMATE holder, speaking ordinarily. Everything an impersonator
    // could match is matched here — right shape, right correlation, right
    // sender, right moment — and the one thing it cannot manufacture is absent.
    const WeaveId holder = mount_into_role<Responder>(bus, kProvRole, "holder", Speak::Ordinary);

    bus.send_as(asker, holder,
                Message(to_value(ProvAsk{"one"}), asker, WeaveId{}, kPublicCorrelation));
    bus.pump();

    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].sender == holder.value);
    CHECK(heard.answers[0].correlation == kPublicCorrelation);
    CHECK_FALSE(heard.answers[0].attested); // the whole phase, in one assertion
}

TEST_CASE("an ordinary weave that knows the shape, the correlation and the victim still cannot "
          "impersonate an answer — and its own reply authority reaches only its own asker") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId forger = mount<Forger>(bus, asker, kPublicCorrelation);

    // Nudged by a root, so the forger acts from an ordinary handler with an
    // ordinary Mail — the same standing any weave has.
    bus.send(forger, Message(to_value(ProvNudge{})));
    bus.pump();

    // The forgery arrived — it is a legal message and the forger holds the grant.
    // What it does not have is Loom's word.
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].tag == "forged");
    CHECK(heard.answers[0].correlation == kPublicCorrelation);
    CHECK_FALSE(heard.answers[0].attested);

    // And the forger's attempt to ANSWER did not reach the asker at all: the
    // authority belongs to the delivery it was handling, whose sender was a root
    // — so there was nobody to answer, and no way to aim it at someone else.
    CHECK(heard.answers.size() == 1);
}

TEST_CASE("the answer's recipient and label are Loom's, so a proof for one request cannot be "
          "aimed at another weave or relabelled") {
    Switchboard bus;
    Received first;
    Received second;
    const WeaveId asker_a = mount<Asker>(bus, first);
    const WeaveId asker_b = mount<Asker>(bus, second);
    const WeaveId holder = mount_into_role<Responder>(bus, kProvRole, "holder", Speak::Authenticated);

    // Two different askers, two different correlations, one responder.
    bus.send_as(asker_a, holder, Message(to_value(ProvAsk{"a"}), asker_a, WeaveId{}, 11));
    bus.send_as(asker_b, holder, Message(to_value(ProvAsk{"b"}), asker_b, WeaveId{}, 22));
    bus.pump();

    // Each answer went to its own asker with its own correlation. The responder
    // named neither: it only said what it wanted to say.
    REQUIRE(first.answers.size() == 1);
    REQUIRE(second.answers.size() == 1);
    CHECK(first.answers[0].attested);
    CHECK(first.answers[0].correlation == 11);
    CHECK(second.answers[0].attested);
    CHECK(second.answers[0].correlation == 22);
}

TEST_CASE("one delivery authorizes exactly one answer, and the second is refused visibly rather "
          "than silently downgraded") {
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, "ProvAnswer");
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId holder = mount_into_role<Responder>(bus, kProvRole, "holder", Speak::Twice);

    bus.send_as(asker, holder, Message(to_value(ProvAsk{"one"}), asker, WeaveId{}, 7));
    bus.pump();

    // Exactly one answer exists, and it is the first.
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].tag == "holder.first");
    CHECK(heard.answers[0].attested);

    // The second attempt did not become an ordinary message — which would have
    // been the quiet, wrong outcome. It was refused, at the same altitude a
    // missing grant is refused, and a tap can see it.
    CHECK(tap.capability_denied == 1);
    CHECK(tap.delivered_answers == 1);
}

TEST_CASE("a request from a root confers no authority: there is nobody to answer, and the "
          "attempt is refused rather than aimed somewhere plausible") {
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, "ProvAnswer");
    (void)mount<Asker>(bus, heard);
    const WeaveId holder = mount_into_role<Responder>(bus, kProvRole, "holder", Speak::Authenticated);

    // Root-sent: no stamped sender, so no requester.
    bus.send(holder, Message(to_value(ProvAsk{"rootless"})));
    bus.pump();

    CHECK(heard.answers.empty());
    CHECK(tap.capability_denied == 1);
    CHECK(tap.delivered_answers == 0);
}

TEST_CASE("the role changing hands hands the new holder nothing: it never received the request, "
          "so it has no conversation to answer") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId first = mount_into_role<Responder>(bus, kProvRole, "first", Speak::Silent);

    // The ask is delivered to `first`, which deliberately says nothing.
    bus.send_as(asker, first, Message(to_value(ProvAsk{"one"}), asker, WeaveId{}, kPublicCorrelation));
    bus.pump();
    CHECK(heard.answers.empty());

    // Now the role changes hands — the exact window a successor could exploit if
    // authority followed the ROLE rather than the DELIVERY.
    bus.unregister_weave(first);
    const WeaveId second = mount_into_role<Forger>(bus, kProvRole, asker, kPublicCorrelation);
    bus.send(second, Message(to_value(ProvNudge{})));
    bus.pump();

    // The successor holds the role, knows the correlation, and holds the grant.
    // Its message arrives — and carries nothing.
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].tag == "forged");
    CHECK_FALSE(heard.answers[0].attested);
}

TEST_CASE("a copied answer is an ordinary message: hoarding a delivered one and re-sending it "
          "verbatim strips the word it never carried") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    // The magpie is the ask's sender, so IT receives the attested answer...
    Received magpie_heard;
    (void)magpie_heard;
    const WeaveId magpie = mount<Magpie>(bus, asker);
    const WeaveId holder = mount_into_role<Responder>(bus, kProvRole, "holder", Speak::Authenticated);

    bus.send_as(magpie, holder, Message(to_value(ProvAsk{"one"}), magpie, WeaveId{}, 5));
    bus.pump();
    // ...and really did see Loom's word, so the re-send below is a copy of a
    // genuinely attested message rather than of nothing.
    CHECK(static_cast<Magpie*>(bus.weave(magpie))->kept_attested_);

    // Now it re-sends that exact message to the victim.
    bus.send(magpie, Message(to_value(ProvNudge{})));
    bus.pump();

    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].correlation == 5); // the payload and label copied perfectly...
    CHECK_FALSE(heard.answers[0].attested);   // ...and the provenance did not copy at all
}

TEST_CASE("the ENVELOPE itself cannot be hoarded and replayed: re-sending a delivered message "
          "verbatim strips the word, because every enqueue path clears it") {
    Switchboard bus;
    Received victim_heard;
    Received magpie_heard;
    const WeaveId victim = mount<Asker>(bus, victim_heard);

    // A weave that keeps the raw Message and re-sends it. It needs the grant to
    // send ProvAsk (to start the conversation it will later replay) and
    // ProvAnswer (to replay it).
    auto raw = std::make_unique<RawMagpie>(victim, magpie_heard);
    RawMagpie* magpie_raw = raw.get();
    Grant g;
    g.allow_to_any(ProvAsk::zen_name, ProvAsk::zen_version);
    g.allow_to_any(ProvAnswer::zen_name, ProvAnswer::zen_version);
    const WeaveId magpie = bus.register_weave(std::move(raw), std::move(g));

    const WeaveId holder =
        mount_into_role<Responder>(bus, kProvRole, "holder", Speak::Authenticated);

    // The magpie asks, and receives a genuinely attested answer.
    bus.send_as(magpie, holder, Message(to_value(ProvAsk{"one"}), magpie, WeaveId{}, 5));
    bus.pump();
    REQUIRE(magpie_heard.answers.size() == 1);
    REQUIRE(magpie_heard.answers[0].attested);
    REQUIRE(magpie_raw->kept_was_attested()); // it really is holding Loom's word

    // Now it replays that exact envelope at the victim.
    bus.send(magpie, Message(to_value(ProvNudge{})));
    bus.pump();

    REQUIRE(victim_heard.answers.size() == 1);
    CHECK(victim_heard.answers[0].tag == "holder"); // the payload copied perfectly...
    CHECK_FALSE(victim_heard.answers[0].attested);  // ...and the word did not copy at all
    // The bus also re-stamped the sender: a replay speaks as the replayer.
    CHECK(victim_heard.answers[0].sender == magpie.value);
}

TEST_CASE("the requester dying strands the answer safely: it is refused at delivery, never "
          "re-aimed at whoever is convenient") {
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, "ProvAnswer");
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId holder = mount_into_role<Responder>(bus, kProvRole, "holder", Speak::Authenticated);

    // Park the queue in the one-instant window that exists at all: the answer is
    // enqueued (the holder has already spoken) but not yet delivered. The
    // authority itself has no window — it died with the holder's handler — so
    // this is the only place a "requester death" can be observed, and what is
    // under test is that the ANSWER behaves like every other in-flight delivery.
    bus.add_observer([&](const BusEvent& ev) {
        if (ev.kind == EventKind::Delivered && ev.schema_name == "ProvAsk") {
            bus.stop();
        }
    });
    bus.send_as(asker, holder, Message(to_value(ProvAsk{"one"}), asker, WeaveId{}, 9));
    bus.pump();
    CHECK(bus.pending() == 1); // the answer, waiting

    bus.unregister_weave(asker);
    bus.pump();

    CHECK(heard.answers.empty());
    CHECK(tap.delivered_answers == 0);
    CHECK(tap.other_refusals == 1); // NoSuchTarget: stranded, not redirected
    CHECK(tap.capability_denied == 0);
}

TEST_CASE("the respondent dying before it answers ends the conversation — nothing arrives, and "
          "nothing is left answerable") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId holder = mount_into_role<Responder>(bus, kProvRole, "holder", Speak::Silent);

    bus.send_as(asker, holder, Message(to_value(ProvAsk{"one"}), asker, WeaveId{}, 3));
    bus.pump();
    bus.unregister_weave(holder);
    bus.pump();

    CHECK(heard.answers.empty());
    // And the authority did not outlive the delivery: nothing is dispatching, so
    // there is nothing for a late arrival to inherit.
    CHECK(bus.pending() == 0);
}

TEST_CASE("ordinary messaging is untouched: send, publish and role-send carry no provenance, and "
          "keep their existing behaviour exactly") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId forger = mount<Forger>(bus, asker, 42);

    // A directed ordinary send.
    bus.send_as(forger, asker, Message(to_value(ProvAnswer{"direct"}), forger, WeaveId{}, 1));
    // A publish (fanout to every accepter).
    CHECK(bus.publish(Message(to_value(ProvAnswer{"published"}))) == 1);
    bus.pump();

    REQUIRE(heard.answers.size() == 2);
    CHECK_FALSE(heard.answers[0].attested);
    CHECK_FALSE(heard.answers[1].attested);
    CHECK(heard.answers[0].tag == "direct");
    CHECK(heard.answers[1].tag == "published");

    // The shape itself is unchanged — provenance is a DELIVERY fact and has no
    // wire representation, so nothing here was version-bumped to carry it.
    CHECK(schema_of<ProvAnswer>()->version() == 1);
    CHECK(schema_of<ProvAnswer>()->fields().size() == 1);
}


// ============================================================================
// R2B-1a — authority is handed, not minted
// ============================================================================

namespace {

/// The strongest attack the supported authoring surface permits.
///
/// This weave is given every advantage the threat model allows and one more
/// besides: it is handed the victim's real WeaveId at construction, it knows a
/// plausible positive sequence, it accepts an ordinary trigger, and its grant
/// permits EXACTLY `zen.Activated` — the very shape it wants to forge. It
/// includes the whole weave-authoring surface (this TU includes
/// <zen/switchboard.hpp> and <zen/weave.hpp>).
///
/// What it cannot write is the line that would matter. Every route that existed
/// before R2B-1a is asserted unwriteable at compile time above; what remains,
/// and what this class actually does, is the strongest thing that still
/// COMPILES: an ordinary, legal, correctly-stamped `zen.Activated`.
class ActivationImpostor : public WeaveBase<ActivationImpostor, ProvState, Accept<ProvNudge>,
                                            Emit<loom::Activated>> {
public:
    ActivationImpostor(WeaveId victim, std::int64_t sequence)
        : victim_(victim), sequence_(sequence) {}

    void on(const ProvNudge&, Mail& mail) {
        ++state_.n;
        // Everything it has. Note what is NOT reachable from here: there is no
        // `mail.announce_lifecycle(...)` it could call, because it has no
        // authority to pass and no expression that yields one.
        mail.send(victim_, loom::Activated{sequence_});
    }

private:
    WeaveId victim_;
    std::int64_t sequence_;
};

/// The victim: an ordinary activation consumer applying the rule every Zengine
/// package applies — believe it only if Loom attests it, for this exact
/// sequence. It records BOTH what arrived and what Loom said about it, so a
/// refusal is distinguishable from a message that never came.
struct ActivationLog {
    std::int64_t delivered = 0; ///< Activated messages handled at all
    std::int64_t accepted = 0;  ///< ...that Loom vouched for
    std::int64_t lineage = 0;   ///< the sequence currently believed
};

class ActivationConsumer : public WeaveBase<ActivationConsumer, ProvState,
                                            Accept<loom::Activated>, Emit<>> {
public:
    explicit ActivationConsumer(ActivationLog& log) : log_(&log) {}

    void on(const loom::Activated& a, Mail& mail) {
        ++log_->delivered;
        if (!mail.lifecycle_attested() || mail.attested_sequence() != a.sequence) {
            return; // an ordinary message wearing a lifecycle costume
        }
        if (a.sequence <= log_->lineage) {
            return; // replay or non-newer: inert
        }
        ++log_->accepted;
        log_->lineage = a.sequence;
    }

private:
    ActivationLog* log_;
};

/// A lifecycle operator, holding a real authority because a HOST handed it one.
struct AttestOrder {
    std::int64_t target = 0;
    std::int64_t announce = 0;
    std::int64_t claim = 0;
    ZEN_SHAPE(AttestOrder, 1, ZEN_FIELD(target), ZEN_FIELD(announce), ZEN_FIELD(claim));
};

class Operator : public WeaveBase<Operator, ProvState, Accept<AttestOrder>,
                                  Emit<loom::Activated>> {
public:
    explicit Operator(loom::LifecycleAuthority authority) : authority_(authority) {}
    void on(const AttestOrder& o, Mail& mail) {
        ++state_.n;
        mail.announce_lifecycle(authority_, WeaveId{static_cast<std::uint64_t>(o.target)},
                                loom::Activated{o.claim}, o.announce);
    }

private:
    loom::LifecycleAuthority authority_;
};

} // namespace

TEST_CASE("R2B-1a: an ordinary weave with an exact zen.Activated grant, the victim's id and a "
          "plausible sequence still cannot manufacture a lifecycle fact") {
    Switchboard bus;
    ActivationLog log;
    const WeaveId victim = mount<ActivationConsumer>(bus, log);
    const WeaveId impostor = mount<ActivationImpostor>(bus, victim, 1);

    // Its grant really does permit the shape it is forging — so this is a
    // rejection by AUTHORITY, not by capability, routing, or the gate.
    const Grant g = emit_default_grant(*static_cast<ActivationImpostor*>(bus.weave(impostor)));
    CHECK(g.permits(loom::Activated::zen_name, loom::Activated::zen_version, victim));

    bus.send(impostor, Message(to_value(ProvNudge{})));
    bus.pump();

    // It arrived — a legal, well-formed, correctly-stamped message.
    CHECK(log.delivered == 1);
    // And it did nothing at all.
    CHECK(log.accepted == 0);
    CHECK(log.lineage == 0);

    // THE POSITIVE CONTROL. The same victim, the same sequence, the same bus —
    // announced by a weave a HOST handed an authority. Without this the checks
    // above are satisfied just as well by a victim that ignores everything.
    const WeaveId op = mount<Operator>(bus, host_lifecycle_authority(bus));
    bus.send(op, Message(to_value(AttestOrder{static_cast<std::int64_t>(victim.value), 1, 1})));
    bus.pump();
    CHECK(log.delivered == 2);
    CHECK(log.accepted == 1); // the difference is the authority, and only that
    CHECK(log.lineage == 1);
}

TEST_CASE("R2B-1a: a genuine authority still binds to one target and one sequence, and a "
          "replayed activation stays inert") {
    Switchboard bus;
    ActivationLog first;
    ActivationLog second;
    const WeaveId a = mount<ActivationConsumer>(bus, first);
    // The bystander: never named in any minting call, and its log is what makes
    // "bound to the target" an observation rather than an assumption.
    (void)mount<ActivationConsumer>(bus, second);
    const WeaveId op = mount<Operator>(bus, host_lifecycle_authority(bus));

    const auto attest = [&](WeaveId target, std::int64_t announce, std::int64_t claim) {
        bus.send(op, Message(to_value(AttestOrder{static_cast<std::int64_t>(target.value),
                                                  announce, claim})));
        bus.pump();
    };

    // Honest: accepted, once.
    attest(a, 1, 1);
    CHECK(first.accepted == 1);
    CHECK(second.accepted == 0); // bound to the target named in the minting call

    // SEQUENCE MISMATCH: Loom is asked to attest 5 while the payload claims 6.
    attest(a, 5, 6);
    CHECK(first.accepted == 1); // unchanged
    CHECK(first.lineage == 1);

    // REPLAY of a genuine, correctly-matched activation: attested, and still
    // inert, because lineage is the consumer's half of the rule.
    attest(a, 1, 1);
    CHECK(first.accepted == 1);

    // ...and a newer one is honoured, so the refusals above are refusals.
    attest(a, 9, 9);
    CHECK(first.accepted == 2);
    CHECK(first.lineage == 9);
    CHECK(second.accepted == 0); // the bystander was never touched at all
}

TEST_CASE("R2B-1a: a request sent BY ROLE binds its one answer to the incarnation that actually "
          "received it — not to whoever holds the role afterwards") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);

    // `first` holds the role and will answer from inside the delivery.
    const WeaveId first =
        mount_into_role<Responder>(bus, kProvRole, "first", Speak::Authenticated);

    // THE REAL ROUTING DOOR: the asker names a ROLE, not an id. It does not know
    // and cannot know which incarnation will receive this — which is the whole
    // reason a correlation and a shape were never enough. Nothing here ever
    // mentions a responder's WeaveId to the asker.
    bus.send_as_to_role(asker, kProvRole,
                        Message(to_value(ProvAsk{"byrole"}), asker, WeaveId{},
                                kPublicCorrelation));

    // ...and now the holder CHANGES BEFORE THE REQUEST IS DELIVERED. This is
    // what makes the case about role resolution rather than about delivery to a
    // known id: if routing were decided when the asker spoke, this request would
    // be bound to `first` and would die with it. It is decided at DELIVERY, so
    // it reaches whoever holds the role then.
    bus.unregister_weave(first);
    const WeaveId second =
        mount_into_role<Responder>(bus, kProvRole, "second", Speak::Authenticated);
    bus.pump();

    // It arrived at `second`, which answered from inside that delivery — with
    // Loom's word behind it, and with the correlation the ASKER chose.
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].tag == "second"); // resolved at delivery, not at send
    CHECK(heard.answers[0].attested);
    CHECK(heard.answers[0].correlation == kPublicCorrelation);
    CHECK(heard.answers[0].sender == second.value);

    // NOW THE ROLE CHANGES HANDS AGAIN, to a weave that wants that conversation.
    //
    // (Freeing a role means unregistering its holder, and an unregistered
    // weave's still-queued replies die with it — the substrate's in-flight rule,
    // pinned in the manager suite. So this move happens after the answer has
    // landed; what is under test here is the SUCCESSOR's standing, not that
    // rule.)
    bus.unregister_weave(second);
    const WeaveId third = mount_into_role<Forger>(bus, kProvRole, asker, kPublicCorrelation);

    // `third` owns the role, knows the correlation, holds the grant, and knows
    // the asker. It still cannot answer that conversation: its own reply
    // authority belongs to whatever delivery IT is handling, so its `answer`
    // reaches its own nudger and never the asker, and its ordinary direct send
    // of the response shape arrives unauthenticated.
    bus.send(third, Message(to_value(ProvNudge{})));
    bus.pump();
    REQUIRE(heard.answers.size() == 2);
    CHECK(heard.answers[1].tag == "forged");
    CHECK_FALSE(heard.answers[1].attested);
    CHECK(heard.answers[1].sender == third.value);

    // The legitimate answer was accepted exactly once, and nothing reopened it.
    CHECK(heard.attested_count() == 1);
}


// ============================================================================
// R2B-1b — every Loom is its own authority domain
// ============================================================================
//
// R2B-1a required a `Switchboard&` to mint an authority. It did not make the
// result mean anything about WHICH Switchboard — the authority was an empty
// marker, so every board honoured every board's. The attack below is therefore
// not a compile-surface question at all: it is entirely legal to write, it
// compiles, and it must fail at RUNTIME.

namespace {

/// "Attack this target with this sequence."
struct DecoyAttack {
    std::int64_t target = 0;
    std::int64_t sequence = 0;
    ZEN_SHAPE(DecoyAttack, 1, ZEN_FIELD(target), ZEN_FIELD(sequence));
};

/// THE DECOY-BOARD ATTACK, by an ordinary native weave.
///
/// It does everything the threat model permits and nothing it forbids: it
/// includes the host-wiring header (any code may), constructs its OWN
/// Switchboard (any code may — a Switchboard is an ordinary object), and mints a
/// completely genuine LifecycleAuthority from it through the same public
/// function the real host uses. Then it spends that authority through the REAL
/// delivery's `Mail`.
///
/// Its grant permits exactly `zen.Activated`, so nothing here is refused for
/// want of capability. The authority is not a forgery — it is real, and real
/// somewhere else.
class DecoyBoardAttacker : public WeaveBase<DecoyBoardAttacker, ProvState, Accept<DecoyAttack>,
                                            Emit<loom::Activated>> {
public:
    void on(const DecoyAttack& a, Mail& mail) {
        ++state_.n;
        // A second Loom, owned entirely by this weave.
        Switchboard decoy;
        // A real authority — for that Loom.
        const LifecycleAuthority forged = host_lifecycle_authority(decoy);
        // Spent through THIS Loom's Mail.
        mail.announce_lifecycle(forged, WeaveId{static_cast<std::uint64_t>(a.target)},
                                loom::Activated{a.sequence}, a.sequence);
    }
};

/// The same attack, but with an authority whose issuing board has already been
/// DESTROYED by the time it is spent — the lifetime half of the same rule.
class DeadBoardAttacker : public WeaveBase<DeadBoardAttacker, ProvState, Accept<DecoyAttack>,
                                           Emit<loom::Activated>> {
public:
    void on(const DecoyAttack& a, Mail& mail) {
        ++state_.n;
        LifecycleAuthority stale = [] {
            Switchboard shortlived;
            return host_lifecycle_authority(shortlived);
        }(); // the board dies here; the authority outlives it as an object
        mail.announce_lifecycle(stale, WeaveId{static_cast<std::uint64_t>(a.target)},
                                loom::Activated{a.sequence}, a.sequence);
    }
};

/// Counts refusals of `zen.Activated`, so "refused" is measured rather than
/// inferred from an absence.
struct ActivationTap {
    std::int64_t delivered = 0;
    std::int64_t foreign_authority = 0; ///< the accurate reason (R2B-2)
    std::int64_t exhausted = 0;         ///< a published bound was reached (R2B-2)
    std::int64_t capability_denied = 0;
    std::int64_t other_refusals = 0;

    void arm(Switchboard& bus) {
        bus.add_observer([this](const BusEvent& ev) {
            if (ev.schema_name != std::string_view(loom::Activated::zen_name)) {
                return;
            }
            if (ev.kind == EventKind::Delivered) {
                ++delivered;
            } else if (ev.kind == EventKind::Refused) {
                if (ev.refusal.reason == RefusalReason::ForeignAuthority) {
                    ++foreign_authority;
                } else if (ev.refusal.reason == RefusalReason::Exhausted) {
                    ++exhausted;
                } else if (ev.refusal.reason == RefusalReason::CapabilityDenied) {
                    ++capability_denied;
                } else {
                    ++other_refusals;
                }
            }
        });
    }
};

} // namespace

TEST_CASE("R2B-1b: an ordinary weave mints a REAL authority from its own decoy board — and the "
          "running Loom refuses it, because it belongs to another world") {
    Switchboard bus;
    ActivationTap tap;
    tap.arm(bus);
    ActivationLog log;
    const WeaveId victim = mount<ActivationConsumer>(bus, log);
    const WeaveId attacker = mount<DecoyBoardAttacker>(bus);

    // Its grant really does permit the shape — so what follows is a refusal of
    // AUTHORITY, not of capability, routing, or conformance.
    const Grant g = emit_default_grant(*static_cast<DecoyBoardAttacker*>(bus.weave(attacker)));
    CHECK(g.permits(loom::Activated::zen_name, loom::Activated::zen_version, victim));

    bus.send(attacker, Message(to_value(DecoyAttack{static_cast<std::int64_t>(victim.value), 1})));
    bus.pump();

    // Nothing was delivered at all: the attestation was refused where it was
    // asked for, not swallowed quietly at the far end.
    CHECK(tap.delivered == 0);
    // THE DIAGNOSTIC IS ACCURATE, not merely present (R2B-2). The grant here is
    // perfectly correct; what is wrong is the authority DOMAIN, and saying
    // "CapabilityDenied" would send an operator looking at grants.
    CHECK(tap.foreign_authority == 1);
    CHECK(tap.capability_denied == 0);
    CHECK(tap.other_refusals == 0);

    // And the victim's own view agrees: nothing arrived, nothing was accepted,
    // no lineage was installed.
    CHECK(log.delivered == 0);
    CHECK(log.accepted == 0);
    CHECK(log.lineage == 0);

    // THE POSITIVE CONTROL. Same bus, same victim, same target, same sequence —
    // an authority issued by THIS board. Without this the checks above would be
    // satisfied just as well by an attacker that never fired.
    const WeaveId op = mount<Operator>(bus, host_lifecycle_authority(bus));
    bus.send(op, Message(to_value(AttestOrder{static_cast<std::int64_t>(victim.value), 1, 1})));
    bus.pump();
    CHECK(tap.delivered == 1);
    CHECK(log.accepted == 1);
    CHECK(log.lineage == 1);
}

TEST_CASE("R2B-1b: an authority whose issuing Loom has been destroyed cannot be spent — the "
          "lifetime rule, not a special case") {
    Switchboard bus;
    ActivationTap tap;
    tap.arm(bus);
    ActivationLog log;
    const WeaveId victim = mount<ActivationConsumer>(bus, log);
    const WeaveId attacker = mount<DeadBoardAttacker>(bus);

    bus.send(attacker, Message(to_value(DecoyAttack{static_cast<std::int64_t>(victim.value), 1})));
    bus.pump();

    CHECK(tap.delivered == 0);
    CHECK(tap.foreign_authority == 1);
    CHECK(tap.capability_denied == 0);
    CHECK(log.accepted == 0);

    // Destroying somebody else's board did nothing to THIS one's authority.
    const WeaveId op = mount<Operator>(bus, host_lifecycle_authority(bus));
    bus.send(op, Message(to_value(AttestOrder{static_cast<std::int64_t>(victim.value), 1, 1})));
    bus.pump();
    CHECK(log.accepted == 1);
}

TEST_CASE("R2B-1b: authority does not follow a board's ADDRESS — reusing a dead Loom's storage "
          "cannot revive it") {
    // THE CASE THAT JUSTIFIES THE REPRESENTATION, deterministically rather than
    // by argument. "Identify a board by its address" is the obvious design and
    // the wrong one: destroy a board, construct another in the same storage, and
    // an authority from the dead world would validate against the living one.
    //
    // Placement-new makes the address collision GUARANTEED instead of hoped for,
    // which is what turns this from a comment into a pin. (The two boards'
    // identity OBJECTS are separate heap allocations; only the boards share an
    // address — exactly the confusion a raw board pointer would fall for.)
    alignas(Switchboard) static unsigned char storage[sizeof(Switchboard)];

    Switchboard* first = new (static_cast<void*>(storage)) Switchboard();
    const LifecycleAuthority from_first = host_lifecycle_authority(*first);
    first->~Switchboard();

    Switchboard* second = new (static_cast<void*>(storage)) Switchboard();
    REQUIRE(static_cast<void*>(second) == static_cast<void*>(storage)); // same address, proven

    ActivationTap tap;
    tap.arm(*second);
    ActivationLog log;
    const WeaveId victim = mount<ActivationConsumer>(*second, log);
    const WeaveId smuggler = mount<Operator>(*second, from_first);

    second->send(smuggler,
                 Message(to_value(AttestOrder{static_cast<std::int64_t>(victim.value), 1, 1})));
    second->pump();
    CHECK(tap.delivered == 0);
    CHECK(tap.foreign_authority == 1);
    CHECK(tap.capability_denied == 0);
    CHECK(log.accepted == 0);

    // THE POSITIVE CONTROL, on this very board at this very address.
    const WeaveId op = mount<Operator>(*second, host_lifecycle_authority(*second));
    second->send(op,
                 Message(to_value(AttestOrder{static_cast<std::int64_t>(victim.value), 1, 1})));
    second->pump();
    CHECK(log.accepted == 1);

    second->~Switchboard();
}

TEST_CASE("R2B-1b: two worlds, the same logical ids and the same sequences — and authority does "
          "not cross between them") {
    // Two independent Looms. No world/fork machinery: two Switchboards IS two
    // worlds, which is exactly the point being made.
    Switchboard world_a;
    Switchboard world_b;
    ActivationLog log_a;
    ActivationLog log_b;

    // Mounted in the same order in both, so the WeaveIds MATCH across worlds —
    // the confusion this case exists to rule out.
    const WeaveId victim_a = mount<ActivationConsumer>(world_a, log_a);
    const WeaveId victim_b = mount<ActivationConsumer>(world_b, log_b);
    REQUIRE(victim_a.value == victim_b.value); // identical logical identity

    // Each world's operator holds ITS OWN board's authority...
    const WeaveId op_a = mount<Operator>(world_a, host_lifecycle_authority(world_a));
    const WeaveId op_b = mount<Operator>(world_b, host_lifecycle_authority(world_b));
    // ...and a smuggler in each world holding the OTHER world's authority.
    const WeaveId smuggler_a = mount<Operator>(world_a, host_lifecycle_authority(world_b));
    const WeaveId smuggler_b = mount<Operator>(world_b, host_lifecycle_authority(world_a));

    const auto attest = [](Switchboard& bus, WeaveId op, WeaveId target, std::int64_t seq) {
        bus.send(op, Message(to_value(AttestOrder{static_cast<std::int64_t>(target.value), seq,
                                                  seq})));
        bus.pump();
    };

    // Authority A works through board A; authority B works through board B.
    attest(world_a, op_a, victim_a, 1);
    attest(world_b, op_b, victim_b, 1);
    CHECK(log_a.accepted == 1);
    CHECK(log_b.accepted == 1);
    CHECK(log_a.lineage == 1);
    CHECK(log_b.lineage == 1);

    // Authority B fails through board A, and authority A fails through board B —
    // with the SAME target id and the SAME next sequence in both worlds, so
    // nothing here can be passing for an unrelated reason.
    attest(world_a, smuggler_a, victim_a, 2);
    attest(world_b, smuggler_b, victim_b, 2);
    CHECK(log_a.accepted == 1); // unchanged
    CHECK(log_b.accepted == 1);
    CHECK(log_a.lineage == 1);
    CHECK(log_b.lineage == 1);

    // ...and the legitimate operators still work afterwards, so the refusals
    // above are refusals rather than a wedged world.
    attest(world_a, op_a, victim_a, 2);
    attest(world_b, op_b, victim_b, 2);
    CHECK(log_a.accepted == 2);
    CHECK(log_b.accepted == 2);
}

TEST_CASE("R2B-1b: a Switchboard is an authority domain, and is deliberately neither copyable "
          "nor movable") {
    // "The same Loom, at a different address" is not a state this design has a
    // meaning for: weaves hold references into a board and its identity anchors
    // every authority it ever issued. The meaningless case is unrepresentable
    // rather than accidentally supported.
    static_assert(!std::is_copy_constructible_v<Switchboard>);
    static_assert(!std::is_copy_assignable_v<Switchboard>);
    static_assert(!std::is_move_constructible_v<Switchboard>);
    static_assert(!std::is_move_assignable_v<Switchboard>);

    // And an authority is copyable — that is how the control door holds its own.
    static_assert(std::is_copy_constructible_v<LifecycleAuthority>);
    CHECK(true); // the assertions above are the case
}


// ============================================================================
// R2B-2 — the answer may wait
// ============================================================================
//
// THE LAW: an answer may outlive the handler, but never the conversation or the
// incarnation that earned it.

namespace {

/// "Finish the job" — the later message a deferring responder is waiting for.
struct ProvFinish {
    std::string tag;
    ZEN_SHAPE(ProvFinish, 1, ZEN_FIELD(tag));
};

/// A responder that DEFERS: it takes the answer right away with it, returns from
/// the request handler without answering, and answers from a later handler.
class Deferrer : public WeaveBase<Deferrer, ProvState, Accept<ProvAsk, ProvFinish>,
                                  Emit<ProvAnswer>> {
public:
    enum class Mode {
        Normal,        ///< defer, then answer on Finish
        DeferTwice,    ///< a second defer_answer() must fail
        AnswerAfter,   ///< mail.answer() after deferring must provide nothing
        SpendTwice,    ///< a second spend must fail
        ReleaseThenSpend, ///< release, then try to spend
        HoardAll,      ///< keep EVERY capability, then try to spend them all (R2B-2a)
    };

    explicit Deferrer(Mode mode) : mode_(mode) {}

    void on(const ProvAsk&, Mail& mail) {
        ++state_.n;
        if (mode_ == Mode::HoardAll) {
            // Several conversations open at once, which is what a selectivity test
            // needs: one weave party to more than one unfinished conversation.
            retained_.push_back(mail.defer_answer());
            deferred_valid_ = retained_.back().valid();
            return;
        }
        pending_ = mail.defer_answer();
        deferred_valid_ = pending_.valid();
        if (mode_ == Mode::DeferTwice) {
            // ONE REQUEST, ONE ANSWER: deferring CONVERTED the immediate right, so
            // there is nothing left to convert a second time.
            second_defer_valid_ = mail.defer_answer().valid();
        }
        if (mode_ == Mode::AnswerAfter) {
            // ...and nothing left to answer with, either.
            immediate_after_defer_ = mail.answer(ProvAnswer{"immediate-after-defer"}).valid();
        }
        // NOTE WHAT DOES NOT HAPPEN HERE: no answer. The handler returns.
    }

    void on(const ProvFinish& f, Mail& mail) {
        ++state_.n;
        if (mode_ == Mode::HoardAll) {
            spent_from_retained_ = 0;
            for (const loom::DeferredAnswer& a : retained_) {
                if (answer_deferred(a, mail, ProvAnswer{f.tag}).valid()) {
                    ++spent_from_retained_;
                }
            }
            return;
        }
        if (mode_ == Mode::ReleaseThenSpend) {
            release_deferred(pending_, mail);
        }
        first_spend_ = answer_deferred(pending_, mail, ProvAnswer{f.tag}).valid();
        if (mode_ == Mode::SpendTwice || mode_ == Mode::ReleaseThenSpend) {
            second_spend_ = answer_deferred(pending_, mail, ProvAnswer{f.tag + ".again"}).valid();
        }
    }

    /// Test-only: the opaque number the board handed it. A test needs this to
    /// hand a THIEF the strongest thing a thief could ever obtain.
    std::uint64_t token() const { return pending_.opaque_token(); }

    /// Test-only: move the CAPABILITY ITSELF out, and into another weave. No weave
    /// can do this — `DeferredAnswer` has no wire form, so there is no message that
    /// carries one — which is precisely why a test has to reach in by hand to ask
    /// the question "what if one really did cross?".
    loom::DeferredAnswer take() { return std::move(pending_); }
    void adopt(loom::DeferredAnswer a) { pending_ = std::move(a); }

    /// How many deliveries this weave has actually handled. The instrument for
    /// "the handler never ran at all", which is a stronger statement than "it did
    /// not answer".
    std::int64_t deliveries() const { return state_.n; }

    /// HoardAll only: how many of the capabilities it kept the BOARD accepted.
    int spent_from_retained_ = 0;
    std::size_t retained_count() const { return retained_.size(); }

    bool deferred_valid_ = false;
    bool second_defer_valid_ = false;
    bool immediate_after_defer_ = false;
    bool first_spend_ = false;
    bool second_spend_ = false;

private:
    Mode mode_;
    loom::DeferredAnswer pending_{};
    std::vector<loom::DeferredAnswer> retained_{}; ///< HoardAll only
};

/// A weave that is handed everything publicly representable about somebody
/// else's unfinished conversation and tries to finish it.
class AnswerThief : public WeaveBase<AnswerThief, ProvState, Accept<ProvFinish>,
                                     Emit<ProvAnswer>> {
public:
    AnswerThief(WeaveId victim, std::uint64_t correlation)
        : victim_(victim), correlation_(correlation) {}

    void on(const ProvFinish&, Mail& mail) {
        ++state_.n;
        // Everything it knows: the requester's id, the correlation, the shape.
        // What it cannot do is hold the capability — `DeferredAnswer` is move-only,
        // has no wire form, and is not a message field. So the strongest thing it
        // can express is an ordinary send.
        mail.send(victim_, ProvAnswer{"stolen"}, correlation_);
        // ...and its own answer authority reaches only its own asker.
        mail.answer(ProvAnswer{"stolen.answer"});
    }

private:
    WeaveId victim_;
    std::uint64_t correlation_;
};

/// A weave handed the victim's exact token NUMBER, which it rebuilds into a
/// capability with the one public door that exists for the library side of the C
/// seam. This is the strongest position a thief can reach: not a guess, the real
/// number, presented from its own live handler at its own live incarnation.
class TokenForger : public WeaveBase<TokenForger, ProvState, Accept<ProvFinish>,
                                     Emit<ProvAnswer>> {
public:
    explicit TokenForger(std::uint64_t token) : token_(token) {}

    void on(const ProvFinish&, Mail& mail) {
        ++state_.n;
        loom::DeferredAnswer forged = loom::DeferredAnswer::from_host_token(token_);
        forged_valid_ = forged.valid(); // it really did build one
        spent_ = answer_deferred(forged, mail, ProvAnswer{"forged-token"}).valid();
    }

    bool forged_valid_ = false;
    bool spent_ = false;

private:
    std::uint64_t token_;
};

} // namespace

TEST_CASE("R2B-2: the answer waits — a responder defers, the handler returns, and a LATER "
          "handler answers the original request") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);

    // The ask, by role — the responder is chosen at delivery as always.
    bus.send_as_to_role(asker, kProvRole,
                        Message(to_value(ProvAsk{"prepare"}), asker, WeaveId{},
                                kPublicCorrelation));
    bus.pump();

    // THE HANDLER RETURNED WITHOUT ANSWERING, and the proof is that the queue is
    // empty and the asker has heard nothing — not merely that the answer came
    // later than we looked.
    CHECK(static_cast<Deferrer*>(bus.weave(steward))->deferred_valid_);
    CHECK(heard.answers.empty());
    CHECK(bus.pending() == 0);

    // AN UNRELATED QUEUE TURN, so "later" is real rather than an artifact of one
    // pump: an ordinary message to somebody else, handled in between.
    const WeaveId bystander = mount<Asker>(bus, heard);
    bus.send(bystander, Message(to_value(ProvAnswer{"unrelated"})));
    bus.pump();
    REQUIRE(heard.answers.size() == 1); // the bystander's, not an answer to the ask
    CHECK_FALSE(heard.answers[0].attested);

    // Now the completion arrives, and the SAME LIVING INCARNATION spends what it
    // kept.
    bus.send(steward, Message(to_value(ProvFinish{"prepared"})));
    bus.pump();

    REQUIRE(heard.answers.size() == 2);
    CHECK(heard.answers[1].tag == "prepared");
    CHECK(heard.answers[1].attested);                          // authentic answer provenance...
    CHECK(heard.answers[1].correlation == kPublicCorrelation); // ...to the ORIGINAL request
    CHECK(heard.answers[1].sender == steward.value);
    CHECK(static_cast<Deferrer*>(bus.weave(steward))->first_spend_);
}

TEST_CASE("R2B-2: deferring CONSUMES the immediate opportunity — no second deferral, no "
          "immediate answer afterwards, and no second spend") {
    // One request grants one answer, whichever door it leaves by.
    {
        Switchboard bus;
        Received heard;
        const WeaveId asker = mount<Asker>(bus, heard);
        const WeaveId steward =
            mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::DeferTwice);
        bus.send_as_to_role(asker, kProvRole,
                            Message(to_value(ProvAsk{"x"}), asker, WeaveId{}, 5));
        bus.pump();
        Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
        CHECK(d->deferred_valid_);
        CHECK_FALSE(d->second_defer_valid_); // nothing left to convert
    }
    {
        Switchboard bus;
        Received heard;
        const WeaveId asker = mount<Asker>(bus, heard);
        const WeaveId steward =
            mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::AnswerAfter);
        bus.send_as_to_role(asker, kProvRole,
                            Message(to_value(ProvAsk{"x"}), asker, WeaveId{}, 5));
        bus.pump();
        Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
        CHECK(d->deferred_valid_);
        CHECK_FALSE(d->immediate_after_defer_); // the immediate door is closed
        CHECK(heard.answers.empty());           // and nothing leaked out of it
    }
    {
        Switchboard bus;
        Received heard;
        const WeaveId asker = mount<Asker>(bus, heard);
        const WeaveId steward =
            mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::SpendTwice);
        bus.send_as_to_role(asker, kProvRole,
                            Message(to_value(ProvAsk{"x"}), asker, WeaveId{}, 5));
        bus.pump();
        bus.send(steward, Message(to_value(ProvFinish{"once"})));
        bus.pump();
        Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
        CHECK(d->first_spend_);
        CHECK_FALSE(d->second_spend_);        // consumed before queueing
        REQUIRE(heard.answers.size() == 1);   // exactly one answer exists
        CHECK(heard.answers[0].attested);
    }
}

TEST_CASE("R2B-2: releasing abandons the conversation — silently to the requester, immediately "
          "to the bus") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::ReleaseThenSpend);
    bus.send_as_to_role(asker, kProvRole,
                        Message(to_value(ProvAsk{"x"}), asker, WeaveId{}, 5));
    bus.pump();
    bus.send(steward, Message(to_value(ProvFinish{"too late"})));
    bus.pump();

    Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
    CHECK_FALSE(d->first_spend_);  // released, so there is nothing to spend
    CHECK_FALSE(d->second_spend_);
    CHECK(heard.answers.empty());  // and the requester is told nothing at all
}

TEST_CASE("R2B-2: another weave knowing every public value cannot finish somebody else's "
          "conversation") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);
    const WeaveId thief = mount<AnswerThief>(bus, asker, kPublicCorrelation);

    bus.send_as_to_role(asker, kProvRole,
                        Message(to_value(ProvAsk{"prepare"}), asker, WeaveId{},
                                kPublicCorrelation));
    bus.pump();
    REQUIRE(heard.answers.empty());

    // The thief knows the requester, the correlation and the shape. The
    // capability is move-only, has no wire representation, and is not a message
    // field — so there is nothing for it to hold, and its best effort is an
    // ordinary send.
    bus.send(thief, Message(to_value(ProvFinish{"now"})));
    bus.pump();
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].tag == "stolen");
    CHECK_FALSE(heard.answers[0].attested); // no provenance, so not an answer

    // And the real steward can still finish, so the refusal above cost nothing.
    bus.send(steward, Message(to_value(ProvFinish{"prepared"})));
    bus.pump();
    REQUIRE(heard.answers.size() == 2);
    CHECK(heard.answers[1].attested);
    CHECK(heard.answers[1].correlation == kPublicCorrelation);
}

TEST_CASE("R2B-2: the real token, forged into a capability by a weave that never received the "
          "request, buys nothing") {
    // THE RESIDUAL, PINNED RATHER THAN ARGUED. `from_host_token` must be public —
    // it is how the library side of the C seam rebuilds a capability from the
    // integer the host handed it — so a native weave can mint a token-shaped value
    // at will. What makes that safe is not secrecy: it is that the record names
    // its respondent, so presenting a number nobody gave you reaches nothing.
    //
    // NOTE the incarnations: both weaves are mounted fresh, so both are at
    // incarnation 1. The forger is therefore refused by RESPONDENT IDENTITY and by
    // nothing else — the incarnation term cannot mask it here.
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);

    bus.send_as_to_role(asker, kProvRole,
                        Message(to_value(ProvAsk{"prepare"}), asker, WeaveId{},
                                kPublicCorrelation));
    bus.pump();
    Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
    REQUIRE(d->deferred_valid_);
    const std::uint64_t real_token = d->token();
    REQUIRE(real_token != 0);

    // Handed the genuine number — not a guess.
    const WeaveId forger = mount<TokenForger>(bus, real_token);
    bus.send(forger, Message(to_value(ProvFinish{"take it"})));
    bus.pump();
    TokenForger* f = static_cast<TokenForger*>(bus.weave(forger));
    CHECK(f->forged_valid_); // the capability object really was constructible...
    CHECK_FALSE(f->spent_);  // ...and spending it reached nothing
    CHECK(heard.answers.empty());

    // And the conversation is untouched: still open, still the steward's to finish.
    bus.send(steward, Message(to_value(ProvFinish{"mine"})));
    bus.pump();
    CHECK(d->first_spend_);
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].tag == "mine");
    CHECK(heard.answers[0].attested);
}

TEST_CASE("R2B-2: the role moving on does not carry the unfinished conversation with it") {
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId first =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);

    bus.send_as_to_role(asker, kProvRole,
                        Message(to_value(ProvAsk{"prepare"}), asker, WeaveId{},
                                kPublicCorrelation));
    bus.pump();
    REQUIRE(heard.answers.empty());

    // A ROLE CHOOSES WHO RECEIVES AN ASK. IT DOES NOT INHERIT UNFINISHED
    // CONVERSATIONS. `first` keeps living — it is only the role that moves — so
    // this isolates succession from death.
    bus.unregister_weave(first);
    const WeaveId second =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);

    // The successor gets the completion message and has nothing to spend.
    bus.send(second, Message(to_value(ProvFinish{"not mine"})));
    bus.pump();
    Deferrer* d = static_cast<Deferrer*>(bus.weave(second));
    CHECK_FALSE(d->deferred_valid_); // it never received the ask
    CHECK_FALSE(d->first_spend_);
    CHECK(heard.answers.empty());
}

TEST_CASE("R2B-2: reload behind a stable WeaveId is a NEW incarnation — the successor cannot "
          "spend its predecessor's answer right") {
    // THE LOAD-BEARING IDENTITY QUESTION, pinned. A WeaveId is never reused, so it
    // already distinguishes a SWAP successor. It does NOT distinguish a RELOAD
    // successor, because reload deliberately keeps the id — so without an
    // incarnation counter, handler-surviving authority would silently become
    // reload-surviving authority.
    Switchboard bus;
    Received heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);

    bus.send_as_to_role(asker, kProvRole,
                        Message(to_value(ProvAsk{"prepare"}), asker, WeaveId{},
                                kPublicCorrelation));
    bus.pump();
    REQUIRE(static_cast<Deferrer*>(bus.weave(steward))->deferred_valid_);

    // Reload it in place: SAME WeaveId, same role, new code behind it.
    Value state(schema_of<ProvState>());
    state.set("n", Cell::integer(0));
    const ReviveOutcome ro = bus.swap_state(steward, serialize(state));
    REQUIRE(ro.revived);

    // The completion arrives at the same id — and the right is gone with the
    // incarnation that earned it.
    bus.send(steward, Message(to_value(ProvFinish{"after reload"})));
    bus.pump();
    CHECK_FALSE(static_cast<Deferrer*>(bus.weave(steward))->first_spend_);
    CHECK(heard.answers.empty());
}

TEST_CASE("R2B-2: the requester dying prevents delivery, and no successor inherits the answer") {
    Switchboard bus;
    Received heard;
    Received successor_heard;
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);

    bus.send_as_to_role(asker, kProvRole,
                        Message(to_value(ProvAsk{"prepare"}), asker, WeaveId{},
                                kPublicCorrelation));
    bus.pump();
    REQUIRE(static_cast<Deferrer*>(bus.weave(steward))->deferred_valid_);

    // The requester goes away, and a fresh weave takes its place in the world.
    bus.unregister_weave(asker);
    const WeaveId newcomer = mount<Asker>(bus, successor_heard);

    bus.send(steward, Message(to_value(ProvFinish{"prepared"})));
    bus.pump();

    CHECK_FALSE(static_cast<Deferrer*>(bus.weave(steward))->first_spend_);
    CHECK(heard.answers.empty());
    // AND CRUCIALLY the answer did not land on whoever came next. WeaveIds are
    // never reused, so `newcomer` is a different id — but this pins that the
    // answer is addressed to the recorded requester and nothing else.
    CHECK(successor_heard.answers.empty());
    CHECK(newcomer.value != asker.value);
}

TEST_CASE("R2B-2: an ordinary delivery has no answer authority to defer") {
    Switchboard bus;
    Received heard;
    (void)mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);

    // Root-sent: nobody to answer, so nothing to convert into a deferred answer.
    bus.send(steward, Message(to_value(ProvAsk{"rootless"})));
    bus.pump();
    CHECK_FALSE(static_cast<Deferrer*>(bus.weave(steward))->deferred_valid_);

    // And spending an invalid capability is safe and loud rather than silent: the
    // completion handler's attempt simply fails.
    bus.send(steward, Message(to_value(ProvFinish{"nothing to say"})));
    bus.pump();
    CHECK_FALSE(static_cast<Deferrer*>(bus.weave(steward))->first_spend_);
    CHECK(heard.answers.empty());
}

TEST_CASE("R2B-2: a deferred answer is board-relative — World A's capability has no standing "
          "in World B, even with identical ids and correlations") {
    Switchboard world_a;
    Switchboard world_b;
    Received heard_a;
    Received heard_b;

    // Mounted in the same order, so the logical ids MATCH across worlds.
    const WeaveId asker_a = mount<Asker>(world_a, heard_a);
    const WeaveId asker_b = mount<Asker>(world_b, heard_b);
    REQUIRE(asker_a.value == asker_b.value);
    const WeaveId steward_a =
        mount_into_role<Deferrer>(world_a, kProvRole, Deferrer::Mode::Normal);
    const WeaveId steward_b =
        mount_into_role<Deferrer>(world_b, kProvRole, Deferrer::Mode::Normal);
    REQUIRE(steward_a.value == steward_b.value);

    // Both worlds run the same conversation with the same correlation, so both
    // registries hold a record with the SAME token number — which is exactly the
    // trap a token-only design would fall into.
    world_a.send_as_to_role(asker_a, kProvRole,
                            Message(to_value(ProvAsk{"a"}), asker_a, WeaveId{},
                                    kPublicCorrelation));
    world_a.pump();
    world_b.send_as_to_role(asker_b, kProvRole,
                            Message(to_value(ProvAsk{"b"}), asker_b, WeaveId{},
                                    kPublicCorrelation));
    world_b.pump();

    // Each world finishes its own conversation, and only its own.
    world_a.send(steward_a, Message(to_value(ProvFinish{"from-a"})));
    world_a.pump();
    world_b.send(steward_b, Message(to_value(ProvFinish{"from-b"})));
    world_b.pump();

    REQUIRE(heard_a.answers.size() == 1);
    REQUIRE(heard_b.answers.size() == 1);
    CHECK(heard_a.answers[0].tag == "from-a");
    CHECK(heard_b.answers[0].tag == "from-b");
    CHECK(heard_a.answers[0].attested);
    CHECK(heard_b.answers[0].attested);
}

TEST_CASE("R2B-2: and the capability ITSELF does not cross — World A's answer right, presented by "
          "World B's steward against a matching record, is refused") {
    // THE PREVIOUS CASE IS NOT ENOUGH, and saying why is the point. There, each
    // world spent its own capability, so the issuer never had to be the thing that
    // refused anything — the tokens stayed home by construction. This case makes
    // the capability actually cross: the object minted by World A is moved, by
    // hand, into World B's steward, where a record with the SAME token number
    // names that very steward at that very incarnation. Every check but one
    // agrees. The issuing Loom is the one that says no.
    Switchboard world_a;
    Switchboard world_b;
    Received heard_a;
    Received heard_b;

    const WeaveId asker_a = mount<Asker>(world_a, heard_a);
    const WeaveId asker_b = mount<Asker>(world_b, heard_b);
    const WeaveId steward_a =
        mount_into_role<Deferrer>(world_a, kProvRole, Deferrer::Mode::Normal);
    const WeaveId steward_b =
        mount_into_role<Deferrer>(world_b, kProvRole, Deferrer::Mode::Normal);
    REQUIRE(asker_a.value == asker_b.value);
    REQUIRE(steward_a.value == steward_b.value);

    world_a.send_as_to_role(asker_a, kProvRole,
                            Message(to_value(ProvAsk{"a"}), asker_a, WeaveId{},
                                    kPublicCorrelation));
    world_a.pump();
    world_b.send_as_to_role(asker_b, kProvRole,
                            Message(to_value(ProvAsk{"b"}), asker_b, WeaveId{},
                                    kPublicCorrelation));
    world_b.pump();

    Deferrer* da = static_cast<Deferrer*>(world_a.weave(steward_a));
    Deferrer* db = static_cast<Deferrer*>(world_b.weave(steward_b));
    REQUIRE(da->deferred_valid_);
    REQUIRE(db->deferred_valid_);
    // The trap, made real: the two worlds minted the same NUMBER.
    REQUIRE(da->token() == db->token());

    // World B's steward now holds World A's capability — and nothing else about
    // it changed: same id, same role, same incarnation, same live record.
    db->adopt(da->take());
    world_b.send(steward_b, Message(to_value(ProvFinish{"with a's right"})));
    world_b.pump();

    CHECK_FALSE(db->first_spend_);
    CHECK(heard_b.answers.empty()); // nothing was answered in B...
    CHECK(heard_a.answers.empty()); // ...and certainly nothing crossed into A
}

namespace {

/// Fill the deferred-answer registry to its published bound using an ordinary
/// weave doing something entirely ordinary: it holds exactly ONE capability
/// member, so every ask after the first overwrites — and thereby LEAKS — the one
/// before it. Nothing will ever spend those records. This is the hole the bound
/// exists to close, and each reclamation claim below has to face a FULL registry
/// or it proves nothing.
struct FullRegistry {
    Received heard;
    RefusalTap tap;
    WeaveId asker{};
    WeaveId steward{};

    void ask(Switchboard& bus, const char* tag) {
        bus.send_as_to_role(asker, kProvRole,
                            Message(to_value(ProvAsk{tag}), asker, WeaveId{},
                                    kPublicCorrelation));
        bus.pump();
    }

    Deferrer* fill(Switchboard& bus, Deferrer::Mode mode,
                   std::size_t count = Switchboard::kMaxDeferredAnswers) {
        tap.arm(bus, ProvAsk::zen_name);
        asker = mount<Asker>(bus, heard);
        steward = mount_into_role<Deferrer>(bus, kProvRole, mode);
        Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
        bool every_one_deferred = true;
        for (std::size_t i = 0; i < count; ++i) {
            ask(bus, "leak");
            every_one_deferred = every_one_deferred && d->deferred_valid_;
        }
        // ONE assertion, not sixty-four: a per-iteration REQUIRE would make this
        // suite's assertion count a function of the bound.
        REQUIRE(every_one_deferred);
        REQUIRE(tap.exhausted == 0); // the bound is not hit EARLY, either
        return d;
    }
};

} // namespace

TEST_CASE("R2B-2: the registry is bounded, the overflow says CAPACITY, and the caller keeps the "
          "immediate opportunity it never spent") {
    // A deferred answer is host-side state a weave asks for, so an unbounded one
    // would be a memory hole any weave could dig by deferring and never answering.
    // The bound is published — and DRIVEN here, because a bound nobody reaches is
    // a comment.
    static_assert(Switchboard::kMaxDeferredAnswers == 64);

    Switchboard bus;
    FullRegistry world;
    // AnswerAfter tries to answer immediately after deferring. While deferring
    // SUCCEEDS that attempt must fail (the right was converted, not copied) — and
    // once the registry is full it must SUCCEED, which is how "nothing was
    // consumed" is measured rather than asserted.
    Deferrer* d = world.fill(bus, Deferrer::Mode::AnswerAfter);
    CHECK_FALSE(d->immediate_after_defer_);
    CHECK(world.heard.answers.empty());

    world.ask(bus, "one too many");

    CHECK_FALSE(d->deferred_valid_);      // no capability...
    CHECK(world.tap.exhausted == 1);      // ...refused VISIBLY, and as capacity...
    CHECK(world.tap.foreign_authority == 0);
    CHECK(world.tap.capability_denied == 0);
    // ...and the immediate opportunity intact, so a well-written weave degrades to
    // answering now instead of losing the conversation.
    CHECK(d->immediate_after_defer_);
    REQUIRE(world.heard.answers.size() == 1);
    CHECK(world.heard.answers[0].tag == "immediate-after-defer");
    CHECK(world.heard.answers[0].attested); // and it is still a REAL answer
}

TEST_CASE("R2B-2: a leaked capability costs one slot only until NEW CODE lands behind its owner") {
    // Reclamation on reload, facing a FULL registry — which is the only way to see
    // it. Reclamation and the incarnation comparison in spend_deferred_as are two
    // independent guards against the same mistake; this one watches reclamation,
    // and it must not be preceded by anything that empties the registry first.
    Switchboard bus;
    FullRegistry world;
    (void)world.fill(bus, Deferrer::Mode::Normal);

    Value fresh(schema_of<ProvState>());
    fresh.set("n", Cell::integer(0));
    REQUIRE(bus.swap_state(world.steward, serialize(fresh)).revived);

    // The successor is an ordinary live weave and can defer at once: all 64 slots
    // came back with the incarnation that owned them.
    world.ask(bus, "after the reload");
    CHECK(static_cast<Deferrer*>(bus.weave(world.steward))->deferred_valid_);
    CHECK(world.tap.exhausted == 0);
}

TEST_CASE("R2B-2: a leaked capability costs one slot only until its owner DIES") {
    // The same claim, the other event, and a separate full registry — because
    // reclaiming on reload would otherwise have emptied this one before death got
    // a chance to be the thing that reclaimed anything.
    Switchboard bus;
    FullRegistry world;
    (void)world.fill(bus, Deferrer::Mode::Normal);

    bus.unregister_weave(world.steward);
    const WeaveId reborn =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);

    world.ask(bus, "after the reaping");
    CHECK(static_cast<Deferrer*>(bus.weave(reborn))->deferred_valid_);
    CHECK(world.tap.exhausted == 0);
}

// ---- R2B-2a: death ends the answer -------------------------------------------
//
// R2B-2 said "an answer may outlive the handler, but never the conversation or the
// incarnation that earned it" — and proved the CODE-replacement and permanent-
// removal halves. It did not prove the RECOVERABLE DEATH half, and the real code
// did not implement it: `kill()` leaves both the WeaveId and the incarnation
// untouched, so a crashed weave revived from its own snapshot — the isolation
// supervisor's ordinary recovery path — came back holding its predecessor's answer
// rights. These cases pin the law at the transition that actually says so:
//
//     A handler may end without ending the conversation. A life may not.

namespace {

/// Count Died/Revived for one weave, so "Loom announced the death" is observed
/// rather than assumed from the fact that the test called kill().
struct LifeTap {
    WeaveId watched{};
    int died = 0;
    int revived = 0;
    int revived_from_lkg = 0;

    void arm(Switchboard& bus, WeaveId id) {
        watched = id;
        bus.add_observer([this](const BusEvent& ev) {
            if (!(ev.target == watched)) {
                return;
            }
            if (ev.kind == EventKind::Died) {
                ++died;
            } else if (ev.kind == EventKind::Revived) {
                ++revived;
                if (ev.from_last_known_good) {
                    ++revived_from_lkg;
                }
            }
        });
    }
};

/// The one ask every case below opens a conversation with.
void ask_by_role(Switchboard& bus, WeaveId asker, const char* role, const char* tag) {
    bus.send_as_to_role(asker, role,
                        Message(to_value(ProvAsk{tag}), asker, WeaveId{}, kPublicCorrelation));
    bus.pump();
}

constexpr const char* kOtherRole = "prov.other";

} // namespace

TEST_CASE("R2B-2a: the respondent DYING ends the conversation, and revival from its own snapshot "
          "does not bring the answer back") {
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name); // so the failed spend is MEASURED, not inferred
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);
    LifeTap life;
    life.arm(bus, steward);

    ask_by_role(bus, asker, kProvRole, "prepare");
    Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
    REQUIRE(d->deferred_valid_); // one deferred record exists, handler already returned

    // Its own state, captured while alive: revival is from something the weave
    // really produced, not from bytes invented by the test.
    const std::string own_state = bus.snapshot_bytes(steward);

    // DEATH, through Loom's actual path — and Loom really says so.
    bus.kill(steward);
    CHECK(life.died == 1);
    CHECK_FALSE(bus.alive(steward));

    // REVIVAL through the supported crash-revival path, same logical identity.
    const ReviveOutcome ro = bus.reload(steward, own_state);
    REQUIRE(ro.revived);
    CHECK_FALSE(ro.from_last_known_good);
    CHECK(life.revived == 1);
    CHECK(bus.alive(steward));

    // The completion arrives, and the revived life STILL HOLDS THE CAPABILITY
    // OBJECT — nothing library-side changed. The board is what refuses.
    bus.send(steward, Message(to_value(ProvFinish{"after revival"})));
    bus.pump();
    Deferrer* revived = static_cast<Deferrer*>(bus.weave(steward));
    CHECK_FALSE(revived->first_spend_);
    CHECK(heard.answers.empty());
    CHECK(tap.foreign_authority == 1); // refused VISIBLY, not silently dropped
    CHECK(tap.delivered_answers == 0);

    // And the new life is not crippled: a fresh ask defers and answers normally.
    ask_by_role(bus, asker, kProvRole, "again");
    CHECK(revived->deferred_valid_);
    bus.send(steward, Message(to_value(ProvFinish{"fresh"})));
    bus.pump();
    CHECK(revived->first_spend_);
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].tag == "fresh");
    CHECK(heard.answers[0].attested);
}

TEST_CASE("R2B-2a: LAST-KNOWN-GOOD revival cannot restore an answer right either") {
    // The other real revival branch, and it must not be a back door. A malformed
    // candidate plus a policy that permits the fallback is the whole difference; the
    // conversation is just as over.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);
    LifeTap life;
    life.arm(bus, steward);

    ask_by_role(bus, asker, kProvRole, "prepare");
    REQUIRE(static_cast<Deferrer*>(bus.weave(steward))->deferred_valid_);

    bus.kill(steward);
    REQUIRE(life.died == 1);

    // Garbage candidate: the gate refuses it, and the weave's policy (the WeaveBase
    // default) permits returning as its last-known-good.
    const ReviveOutcome ro = bus.reload(steward, "not a serialized value");
    REQUIRE(ro.revived);
    REQUIRE(ro.from_last_known_good);
    CHECK(life.revived_from_lkg == 1);
    CHECK(bus.alive(steward));

    bus.send(steward, Message(to_value(ProvFinish{"from the old life"})));
    bus.pump();
    CHECK_FALSE(static_cast<Deferrer*>(bus.weave(steward))->first_spend_);
    CHECK(heard.answers.empty());
    CHECK(tap.foreign_authority == 1);
    CHECK(tap.delivered_answers == 0);
}

TEST_CASE("R2B-2a: the REQUESTER dying and reviving does not inherit the answer its previous life "
          "was owed") {
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);
    LifeTap life;
    life.arm(bus, asker);

    ask_by_role(bus, asker, kProvRole, "prepare");
    Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
    REQUIRE(d->deferred_valid_);

    const std::string own_state = bus.snapshot_bytes(asker);
    bus.kill(asker);
    CHECK(life.died == 1);

    // REVIVED FIRST, THEN SPENT — deliberately. A spend attempted while the
    // requester is still dead would be refused for an entirely different reason
    // (there is nobody alive to deliver to), and would prove nothing about whether
    // the conversation survived the life.
    const ReviveOutcome ro = bus.reload(asker, own_state);
    REQUIRE(ro.revived);
    CHECK(life.revived == 1);
    CHECK(bus.alive(asker));

    bus.send(steward, Message(to_value(ProvFinish{"owed to your predecessor"})));
    bus.pump();
    CHECK_FALSE(d->first_spend_);
    CHECK(heard.answers.empty()); // the new life is not handed the old life's answer
    CHECK(tap.foreign_authority == 1);
    CHECK(tap.delivered_answers == 0);

    // ...and the revived requester is a perfectly ordinary participant again.
    ask_by_role(bus, asker, kProvRole, "my own question");
    CHECK(d->deferred_valid_);
    bus.send(steward, Message(to_value(ProvFinish{"mine"})));
    bus.pump();
    CHECK(d->first_spend_);
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].tag == "mine");
    CHECK(heard.answers[0].attested);
}

TEST_CASE("R2B-2a: death reclaims registry capacity AT THE DEATH, in both ownership directions") {
    // The bound is 64 and a leaked capability costs a slot. R2B-2 proved code
    // replacement and permanent removal give the slots back; this proves DEATH does
    // — immediately, and before any revival, because a quarantined weave is never
    // revived at all and its slots must not be hostage to an event that never comes.

    SUBCASE("the dead participant is the RESPONDENT") {
        Switchboard bus;
        FullRegistry world;
        Deferrer* d = world.fill(bus, Deferrer::Mode::Normal);
        world.ask(bus, "one too many");
        REQUIRE_FALSE(d->deferred_valid_);
        REQUIRE(world.tap.exhausted == 1);

        bus.kill(world.steward);

        // IMMEDIATELY, with no revival anywhere: an unrelated pair can open a
        // conversation, which it could not have done a moment earlier.
        Received other_heard;
        const WeaveId other_asker = mount<Asker>(bus, other_heard);
        const WeaveId other_steward =
            mount_into_role<Deferrer>(bus, kOtherRole, Deferrer::Mode::Normal);
        ask_by_role(bus, other_asker, kOtherRole, "with the reclaimed capacity");
        CHECK(static_cast<Deferrer*>(bus.weave(other_steward))->deferred_valid_);
        CHECK(world.tap.exhausted == 1); // no second overflow

        // And the revived original can use it too.
        REQUIRE(bus.reload(world.steward, bus.snapshot_bytes(world.steward)).revived);
        world.ask(bus, "after revival");
        CHECK(static_cast<Deferrer*>(bus.weave(world.steward))->deferred_valid_);
        CHECK(world.tap.exhausted == 1);
    }

    SUBCASE("the dead participant is the REQUESTER") {
        // Cleanup indexes both sides, so both sides are watched.
        Switchboard bus;
        FullRegistry world;
        Deferrer* d = world.fill(bus, Deferrer::Mode::Normal);
        world.ask(bus, "one too many");
        REQUIRE_FALSE(d->deferred_valid_);
        REQUIRE(world.tap.exhausted == 1);

        bus.kill(world.asker);

        Received other_heard;
        const WeaveId other_asker = mount<Asker>(bus, other_heard);
        const WeaveId other_steward =
            mount_into_role<Deferrer>(bus, kOtherRole, Deferrer::Mode::Normal);
        ask_by_role(bus, other_asker, kOtherRole, "with the reclaimed capacity");
        CHECK(static_cast<Deferrer*>(bus.weave(other_steward))->deferred_valid_);
        CHECK(world.tap.exhausted == 1);

        REQUIRE(bus.reload(world.asker, bus.snapshot_bytes(world.asker)).revived);
        world.ask(bus, "after revival");
        CHECK(d->deferred_valid_);
        CHECK(world.tap.exhausted == 1);
    }
}

TEST_CASE("R2B-2a: death cleanup is SELECTIVE — killing one participant ends only the "
          "conversations it was a party to") {
    // A -> B, E -> B, C -> D. Killing B must end the first two and leave the third
    // untouched. Clearing the whole registry would make every other case in this
    // suite pass just as green, which is exactly why this case exists.
    Switchboard bus;
    Received heard_a;
    Received heard_c;
    Received heard_e;
    const WeaveId a = mount<Asker>(bus, heard_a);
    const WeaveId c = mount<Asker>(bus, heard_c);
    const WeaveId e = mount<Asker>(bus, heard_e);
    // B holds TWO conversations at once, which is the only way to see that BOTH of
    // its records went and not merely the last one.
    const WeaveId b = mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::HoardAll);
    const WeaveId d = mount_into_role<Deferrer>(bus, kOtherRole, Deferrer::Mode::Normal);

    ask_by_role(bus, a, kProvRole, "a-asks-b");
    ask_by_role(bus, e, kProvRole, "e-asks-b");
    ask_by_role(bus, c, kOtherRole, "c-asks-d");
    Deferrer* bw = static_cast<Deferrer*>(bus.weave(b));
    Deferrer* dw = static_cast<Deferrer*>(bus.weave(d));
    REQUIRE(bw->retained_count() == 2);
    REQUIRE(dw->deferred_valid_);

    bus.kill(b);
    REQUIRE(bus.reload(b, bus.snapshot_bytes(b)).revived);

    // B still holds both capability objects and tries both. Neither is honoured.
    bus.send(b, Message(to_value(ProvFinish{"revived-b"})));
    bus.pump();
    CHECK(bw->spent_from_retained_ == 0);
    CHECK(heard_a.answers.empty());
    CHECK(heard_e.answers.empty());

    // C -> D was never B's business, and it still works — the registry was not
    // simply emptied.
    bus.send(d, Message(to_value(ProvFinish{"c-gets-this"})));
    bus.pump();
    CHECK(dw->first_spend_);
    REQUIRE(heard_c.answers.size() == 1);
    CHECK(heard_c.answers[0].tag == "c-gets-this");
    CHECK(heard_c.answers[0].attested);
}

TEST_CASE("R2B-2a: a reclaimed token and a never-issued one are refused IDENTICALLY, on purpose") {
    // Deliberate, and worth writing down: after a death the record is gone, so a
    // persisted token names nothing — the same nothing a fabricated number names.
    // Both come back as ForeignAuthority with no requester on the refusal, so the
    // refusal cannot be read as an oracle for whether a conversation once existed.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);

    ask_by_role(bus, asker, kProvRole, "prepare");
    Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
    REQUIRE(d->deferred_valid_);
    const std::uint64_t real_token = d->token();
    REQUIRE(real_token != 0);

    bus.kill(steward);
    REQUIRE(bus.reload(steward, bus.snapshot_bytes(steward)).revived);

    // The real (now reclaimed) token, presented by the weave that earned it.
    bus.send(steward, Message(to_value(ProvFinish{"reclaimed"})));
    bus.pump();
    CHECK_FALSE(d->first_spend_);
    const std::int64_t after_reclaimed = tap.foreign_authority;
    CHECK(after_reclaimed == 1);

    // A number that never named anything, presented by a weave that never asked.
    const WeaveId forger = mount<TokenForger>(bus, real_token + 12345u);
    bus.send(forger, Message(to_value(ProvFinish{"never-existed"})));
    bus.pump();
    CHECK_FALSE(static_cast<TokenForger*>(bus.weave(forger))->spent_);
    CHECK(tap.foreign_authority == after_reclaimed + 1); // same reason, same altitude
    CHECK(tap.delivered_answers == 0);
    CHECK(heard.answers.empty());
}

// ---- R2B-2b: the message belongs to a life ------------------------------------
//
// R2B-2a ends every conversation a dying participant was already IN. A message it
// had merely QUEUED names no conversation yet, so there was nothing for that
// cleanup to find — and delivered later it would have become speech from whatever
// now answers to the same id:
//
//     life A queues an ask -> A dies -> A is revived under the same WeaveId
//     -> the ask is delivered -> the responder answers -> the NEW life is answered
//
// So every weave-originated envelope is stamped, at enqueue, with the sender's
// current LIFE, and delivery refuses when that life is over.
//
//     A weave-originated message belongs to the life that authored it.

namespace {

/// A weave that speaks when nudged, in whichever of the three ways a weave can
/// speak. Its whole purpose is to author one message and get out of the way, so
/// the message can sit in the queue while the test kills and revives its author.
class Speaker : public WeaveBase<Speaker, ProvState, Accept<ProvNudge, ProvAnswer>,
                                Emit<ProvAsk>> {
public:
    enum class How { Direct, ByRole, Publish };

    Speaker(How how, Received& heard) : how_(how), heard_(&heard) {}

    /// Direct sends need a target the test only knows after mounting it.
    void aim(WeaveId target) { target_ = target; }
    void say(std::string tag) { tag_ = std::move(tag); }

    void on(const ProvNudge&, Mail& mail) {
        ++state_.n;
        switch (how_) {
        case How::Direct:
            mail.send(target_, ProvAsk{tag_}, kPublicCorrelation);
            return;
        case How::ByRole:
            mail.send_to_role(kProvRole, ProvAsk{tag_}, kPublicCorrelation);
            return;
        case How::Publish:
            mail.publish(ProvAsk{tag_});
            return;
        }
    }

    void on(const ProvAnswer& a, Mail& mail) {
        ++state_.n;
        heard_->answers.push_back(
            Received::One{a.tag, mail.answers_ask(), mail.correlation(), mail.sender().value});
    }

private:
    How how_;
    Received* heard_;
    WeaveId target_{};
    std::string tag_ = "queued";
};

/// Make `speaker` author exactly one message and leave it in the queue.
///
/// The nudge is root-sent, so the pump delivers it, the handler enqueues the real
/// utterance, and the observer stops the pump the instant the nudge's own delivery
/// is announced — which happens after the handler has returned. What the handler
/// enqueued is therefore still queued, and `pending()` is the proof rather than
/// the assumption.
void queue_one_utterance(Switchboard& bus, WeaveId speaker) {
    const ObserverId stopper = bus.add_observer([&bus](const BusEvent& ev) {
        if (ev.kind == EventKind::Delivered && ev.schema_name == ProvNudge::zen_name) {
            bus.stop();
        }
    });
    bus.send(speaker, Message(to_value(ProvNudge{})));
    bus.pump();
    bus.remove_observer(stopper);
}

/// Kill and revive through the real paths, asserting Loom announced both.
void kill_and_revive(Switchboard& bus, WeaveId id, LifeTap& life) {
    const std::string own_state = bus.snapshot_bytes(id);
    bus.kill(id);
    REQUIRE(life.died == 1);
    const ReviveOutcome ro = bus.reload(id, own_state);
    REQUIRE(ro.revived);
    REQUIRE(life.revived == 1);
    REQUIRE(bus.alive(id));
}

} // namespace

TEST_CASE("R2B-2b: an ask queued by a life that then died is not speech from its revival") {
    // THE CORE PROOF. Everything after it is another shape of send, or another
    // thing the stale message must fail to reach.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAsk::zen_name);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);
    const WeaveId speaker = mount<Speaker>(bus, Speaker::How::Direct, heard);
    static_cast<Speaker*>(bus.weave(speaker))->aim(steward);
    LifeTap life;
    life.arm(bus, speaker);

    queue_one_utterance(bus, speaker);
    REQUIRE(bus.pending() == 1); // GENUINELY QUEUED, and provably not delivered
    Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
    REQUIRE(d->deliveries() == 0);

    kill_and_revive(bus, speaker, life);

    // The queued ask now comes up for delivery, and its author is a different life.
    bus.pump();
    CHECK(tap.sender_life_ended == 1);   // refused, and NAMED
    CHECK(tap.capability_denied == 0);   // not blamed on a grant
    CHECK(tap.other_refusals == 0);      // nor on the target or the payload
    CHECK(tap.last_authored_life == 1);  // the journal can say WHY, in numbers
    CHECK(tap.last_current_life == 2);
    // Nothing downstream happened at all: no handler, so no answer authority and
    // no deferred record could even be asked for.
    CHECK(d->deliveries() == 0);
    CHECK_FALSE(d->deferred_valid_);
    CHECK(heard.answers.empty());

    // THE POSITIVE CONTROL: the revived life is a perfectly ordinary speaker.
    static_cast<Speaker*>(bus.weave(speaker))->say("after revival");
    queue_one_utterance(bus, speaker);
    bus.pump();
    CHECK(d->deliveries() == 1);
    CHECK(d->deferred_valid_);
    // ...and its speech carries the NEW generation, matching the sender's own.
    CHECK(tap.last_authored_life == 2);
    CHECK(tap.last_current_life == 2);
    bus.send(steward, Message(to_value(ProvFinish{"answered"})));
    bus.pump();
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].tag == "answered");
    CHECK(heard.answers[0].attested);
}

TEST_CASE("R2B-2b: a queued message from a sender that is DEAD AND NOT YET REVIVED is refused "
          "too — and the two life numbers are EQUAL, which is why aliveness is its own term") {
    // THE TERM NO OTHER CASE REACHES. Everywhere else the author is killed AND
    // revived before the pump, so the generation has moved and the generation check
    // is what refuses. Here the author is simply dead: the stamped life and the
    // current life are IDENTICAL, and only aliveness can tell the difference.
    //
    // This case exists because a mutation deleting the aliveness term came back
    // GREEN. It was not redundant — it was unwatched.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAsk::zen_name);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);
    const WeaveId speaker = mount<Speaker>(bus, Speaker::How::Direct, heard);
    static_cast<Speaker*>(bus.weave(speaker))->aim(steward);
    LifeTap life;
    life.arm(bus, speaker);

    queue_one_utterance(bus, speaker);
    REQUIRE(bus.pending() == 1);

    bus.kill(speaker);
    REQUIRE(life.died == 1);
    REQUIRE(life.revived == 0); // dead, and staying dead for the length of this test

    bus.pump();
    CHECK(tap.sender_life_ended == 1);
    CHECK(tap.last_authored_life == 1);
    CHECK(tap.last_current_life == 1); // EQUAL — and still refused
    CHECK(static_cast<Deferrer*>(bus.weave(steward))->deliveries() == 0);
    CHECK(heard.answers.empty());
}

TEST_CASE("R2B-2b: a stale ROLE-addressed message reaches neither the old holder nor whoever "
          "holds the role by the time it is delivered") {
    // Role resolution is delivery-time behaviour, and that is the point of danger:
    // a stale message would otherwise be handed to whoever happens to hold the slot
    // later. The life check runs BEFORE resolution, so it never gets that far.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAsk::zen_name);
    const WeaveId first_holder =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);
    const WeaveId speaker = mount<Speaker>(bus, Speaker::How::ByRole, heard);
    LifeTap life;
    life.arm(bus, speaker);

    queue_one_utterance(bus, speaker);
    REQUIRE(bus.pending() == 1);
    kill_and_revive(bus, speaker, life);

    // The role changes hands while the stale message waits — so if it were
    // delivered, it would be delivered to a weave that never existed when it was
    // authored.
    bus.unregister_weave(first_holder);
    const WeaveId new_holder =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);

    bus.pump();
    CHECK(tap.sender_life_ended == 1);
    CHECK(tap.other_refusals == 0); // NOT "no such target", NOT a role miss
    CHECK(static_cast<Deferrer*>(bus.weave(new_holder))->deliveries() == 0);
    CHECK(heard.answers.empty());
}

TEST_CASE("R2B-2b: a stale PUBLICATION reaches no subscriber, and every recipient's copy is "
          "checked on its own") {
    // A publish becomes one envelope per subscriber, so a single check at fan-out
    // time would be exactly the wrong shape: it would let a stale publication reach
    // everyone on the strength of one answer. Each delivery answers for itself, and
    // the refusal count is how that is measured.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAsk::zen_name);
    const WeaveId sub_a = mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);
    const WeaveId sub_b = mount_into_role<Deferrer>(bus, "prov.other", Deferrer::Mode::Normal);
    const WeaveId speaker = mount<Speaker>(bus, Speaker::How::Publish, heard);
    LifeTap life;
    life.arm(bus, speaker);

    queue_one_utterance(bus, speaker);
    REQUIRE(bus.pending() == 2); // one envelope per subscriber
    kill_and_revive(bus, speaker, life);

    bus.pump();
    CHECK(tap.sender_life_ended == 2); // BOTH copies refused, individually
    CHECK(tap.delivered_answers == 0);
    CHECK(static_cast<Deferrer*>(bus.weave(sub_a))->deliveries() == 0);
    CHECK(static_cast<Deferrer*>(bus.weave(sub_b))->deliveries() == 0);
}

TEST_CASE("R2B-2b: a stale ask creates no IMMEDIATE answer authority") {
    // The responder here answers every ask it receives. If a stale ask reached its
    // handler, an authenticated answer would exist — addressed, by Loom, to the
    // revived life that never asked anything.
    Switchboard bus;
    Received heard;
    RefusalTap answers;
    answers.arm(bus, ProvAnswer::zen_name);
    const WeaveId responder =
        mount_into_role<Responder>(bus, kProvRole, "eager", Speak::Authenticated);
    const WeaveId speaker = mount<Speaker>(bus, Speaker::How::Direct, heard);
    static_cast<Speaker*>(bus.weave(speaker))->aim(responder);
    LifeTap life;
    life.arm(bus, speaker);

    queue_one_utterance(bus, speaker);
    REQUIRE(bus.pending() == 1);
    kill_and_revive(bus, speaker, life);

    bus.pump();
    CHECK(heard.answers.empty());
    CHECK(answers.delivered_answers == 0);
    CHECK(answers.capability_denied == 0); // no answer was even attempted

    // POSITIVE CONTROL: the same responder answers the revived life's own ask.
    queue_one_utterance(bus, speaker);
    bus.pump();
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].attested);
}

TEST_CASE("R2B-2b: a stale ask creates no DEFERRED answer authority and occupies no registry "
          "capacity") {
    // Capacity is the exact instrument. The registry is filled to one slot short of
    // its bound; if the stale ask were delivered and deferred, it would take the
    // last slot and the living conversation below would be refused as Exhausted.
    Switchboard bus;
    FullRegistry world;
    Deferrer* filler = world.fill(bus, Deferrer::Mode::Normal,
                                  Switchboard::kMaxDeferredAnswers - 1);
    REQUIRE(filler->deferred_valid_);
    REQUIRE(world.tap.exhausted == 0);

    Received heard;
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, "prov.other", Deferrer::Mode::Normal);
    const WeaveId speaker = mount<Speaker>(bus, Speaker::How::Direct, heard);
    static_cast<Speaker*>(bus.weave(speaker))->aim(steward);
    LifeTap life;
    life.arm(bus, speaker);

    queue_one_utterance(bus, speaker);
    REQUIRE(bus.pending() == 1);
    kill_and_revive(bus, speaker, life);
    bus.pump();

    Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
    CHECK(d->deliveries() == 0);
    CHECK_FALSE(d->deferred_valid_);

    // The last slot is still there for a living conversation — which is what
    // "occupies no capacity" means, measured rather than asserted.
    world.ask(bus, "the last slot");
    CHECK(filler->deferred_valid_);
    CHECK(world.tap.exhausted == 0);
}

TEST_CASE("R2B-2b: killing one speaker leaves another living speaker's queued message alone") {
    // Cleanup and validation must both be about ONE life. Invalidating the queue
    // wholesale would make every case above just as green.
    Switchboard bus;
    Received heard_a;
    Received heard_b;
    RefusalTap tap;
    tap.arm(bus, ProvAsk::zen_name);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::HoardAll);
    const WeaveId a = mount<Speaker>(bus, Speaker::How::Direct, heard_a);
    const WeaveId b = mount<Speaker>(bus, Speaker::How::Direct, heard_b);
    static_cast<Speaker*>(bus.weave(a))->aim(steward);
    static_cast<Speaker*>(bus.weave(b))->aim(steward);
    LifeTap life;
    life.arm(bus, a);

    // BOTH utterances must be in the queue AT ONCE, which means one pump: two
    // nudges go in, and the pump stops only after the second nudge is delivered.
    // (Nudging them one at a time would let the second pump deliver the first
    // speaker's ask before the kill, and the case would prove nothing.)
    int nudges = 0;
    const ObserverId stopper = bus.add_observer([&](const BusEvent& ev) {
        if (ev.kind == EventKind::Delivered && ev.schema_name == ProvNudge::zen_name &&
            ++nudges == 2) {
            bus.stop();
        }
    });
    bus.send(a, Message(to_value(ProvNudge{})));
    bus.send(b, Message(to_value(ProvNudge{})));
    bus.pump();
    bus.remove_observer(stopper);
    REQUIRE(bus.pending() == 2);

    kill_and_revive(bus, a, life);
    bus.pump();

    CHECK(tap.sender_life_ended == 1); // exactly one of the two
    Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
    CHECK(d->deliveries() == 1);       // and B's really was delivered
    CHECK(d->retained_count() == 1);
}

TEST_CASE("R2B-2b: a queued message from a PERMANENTLY REMOVED sender is refused as a life that "
          "ended, not as a missing grant") {
    // The same fact the swap window has always had (an unloaded weave's in-flight
    // replies die with it) — now reported by its real cause. Before R2B-2b this
    // arrived at the right answer down the wrong road: a vanished sender has no
    // grant to check, so the AUTHORIZATION term failed and the tap said
    // CapabilityDenied, sending an operator to edit a grant that was never wrong.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAsk::zen_name);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);
    const WeaveId speaker = mount<Speaker>(bus, Speaker::How::Direct, heard);
    static_cast<Speaker*>(bus.weave(speaker))->aim(steward);

    queue_one_utterance(bus, speaker);
    REQUIRE(bus.pending() == 1);
    bus.unregister_weave(speaker);

    bus.pump();
    CHECK(tap.sender_life_ended == 1);
    CHECK(tap.capability_denied == 0);
    CHECK(static_cast<Deferrer*>(bus.weave(steward))->deliveries() == 0);
}

TEST_CASE("R2B-2b: a LIVE code reload is not a death — speech already in the queue is still that "
          "same living weave's") {
    // THE OTHER HALF OF THE LAW, and the reason this phase added a second field
    // instead of reusing the incarnation. A weave whose code is replaced never
    // stopped living; a sentence it was already mid-way through delivering is
    // still its own. Collapsing the two concepts would silently discard it.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAsk::zen_name);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);
    const WeaveId speaker = mount<Speaker>(bus, Speaker::How::Direct, heard);
    static_cast<Speaker*>(bus.weave(speaker))->aim(steward);

    queue_one_utterance(bus, speaker);
    REQUIRE(bus.pending() == 1);

    // New code behind the same id, while alive: a new INCARNATION, the same LIFE.
    Value fresh(schema_of<ProvState>());
    fresh.set("n", Cell::integer(0));
    REQUIRE(bus.swap_state(speaker, serialize(fresh)).revived);

    bus.pump();
    CHECK(tap.sender_life_ended == 0);
    CHECK(static_cast<Deferrer*>(bus.weave(steward))->deliveries() == 1);
    CHECK(tap.last_authored_life == 1);
    CHECK(tap.last_current_life == 1);
}

TEST_CASE("R2B-2b: the life stamp is not carried by anything a weave can hold — a replayed "
          "envelope speaks with its REPLAYER's life") {
    // Cross-Loom and replay are structural here rather than checked: the stamp
    // lives on the bus's private Envelope, which has no wire form, no schema and no
    // constructor a weave can reach. So there is nothing to copy out of a delivered
    // message and nothing to carry into another Loom.
    //
    // What that MEANS is worth pinning, because a reader might expect the opposite:
    // a weave that hoards someone else's message and re-sends it is not relaying
    // that author's speech, it is speaking itself — and it is checked as itself.
    Switchboard bus;
    Received victim_heard;
    Received magpie_heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId victim = mount<Asker>(bus, victim_heard);

    auto raw = std::make_unique<RawMagpie>(victim, magpie_heard);
    RawMagpie* magpie_raw = raw.get();
    Grant g;
    g.allow_to_any(ProvAsk::zen_name, ProvAsk::zen_version);
    g.allow_to_any(ProvAnswer::zen_name, ProvAnswer::zen_version);
    const WeaveId magpie = bus.register_weave(std::move(raw), std::move(g));
    const WeaveId holder =
        mount_into_role<Responder>(bus, kProvRole, "holder", Speak::Authenticated);

    bus.send_as(magpie, holder, Message(to_value(ProvAsk{"one"}), magpie, WeaveId{}, 5));
    bus.pump();
    REQUIRE(magpie_raw->kept_was_attested()); // it is holding a real answer

    // The RESPONDER — the life that authored the hoarded envelope — dies.
    bus.kill(holder);

    // And the magpie replays it anyway. Delivered, because the speaker is the
    // magpie and the magpie is alive; stripped of provenance, exactly as R2B-2
    // pinned. The dead author's life was never the question, because the dead
    // author is not who is speaking.
    bus.send(magpie, Message(to_value(ProvNudge{})));
    bus.pump();
    REQUIRE(victim_heard.answers.size() == 1);
    CHECK_FALSE(victim_heard.answers[0].attested);
    CHECK(victim_heard.answers[0].sender == magpie.value);
    CHECK(tap.sender_life_ended == 0);
}

// ---- R2B-2c: the answer belongs to the life that asked -------------------------
//
// R2B-2b bound a message to the life that AUTHORED it, which protects the
// answering side. This is the other half — the participant an answer was earned
// FOR:
//
//     requester A asks -> responder queues an authentic answer -> A dies
//     -> A is revived under the same WeaveId -> the answer lands on the revival
//
// and its quieter twin, where A never dies at all:
//
//     requester A asks -> the answer is queued -> A's CODE is replaced in place
//     -> successor code B inherits A's completed conversation
//
// Ordinary messages deliberately keep their old behaviour: a direct or
// role-addressed send is aimed at a logical destination and should reach whoever
// legitimately occupies it. An authenticated answer is different in kind, because
// its meaning already names one conversation between two exact participants.
//
//     An authenticated answer belongs to the requester life and code
//     incarnation that asked.

namespace {

/// Drive one ask, and stop the pump the instant the ASK is delivered — so the
/// authenticated answer the responder produced during that delivery is sitting in
/// the queue, undelivered, while the test changes the requester underneath it.
void queue_one_answer(Switchboard& bus, WeaveId asker, const char* role, const char* tag) {
    const ObserverId stopper = bus.add_observer([&bus](const BusEvent& ev) {
        if (ev.kind == EventKind::Delivered && ev.schema_name == ProvAsk::zen_name) {
            bus.stop();
        }
    });
    bus.send_as_to_role(asker, role,
                        Message(to_value(ProvAsk{tag}), asker, WeaveId{}, kPublicCorrelation));
    bus.pump();
    bus.remove_observer(stopper);
}

/// The deferred twin: ask (deferred, nothing queued), then the completion — and
/// stop on the COMPLETION's delivery, leaving the spent answer queued.
void queue_one_deferred_answer(Switchboard& bus, WeaveId asker, WeaveId steward,
                               const char* role, const char* tag) {
    bus.send_as_to_role(asker, role,
                        Message(to_value(ProvAsk{"defer me"}), asker, WeaveId{},
                                kPublicCorrelation));
    bus.pump();
    const ObserverId stopper = bus.add_observer([&bus](const BusEvent& ev) {
        if (ev.kind == EventKind::Delivered && ev.schema_name == ProvFinish::zen_name) {
            bus.stop();
        }
    });
    bus.send(steward, Message(to_value(ProvFinish{tag})));
    bus.pump();
    bus.remove_observer(stopper);
}

} // namespace

TEST_CASE("R2B-2c: an authenticated answer queued for a requester that then died is not the "
          "revival's answer — immediate and deferred alike") {
    // THE CORE PROOF, run down both answer doors, because patching only the
    // deferred registry would leave the immediate path exactly as broken.
    bool deferred = false;
    SUBCASE("immediate answer") { deferred = false; }
    SUBCASE("deferred answer, spent before the death") { deferred = true; }

    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId responder =
        deferred ? mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal)
                 : mount_into_role<Responder>(bus, kProvRole, "r", Speak::Authenticated);
    LifeTap life;
    life.arm(bus, asker);

    if (deferred) {
        queue_one_deferred_answer(bus, asker, responder, kProvRole, "deferred");
    } else {
        queue_one_answer(bus, asker, kProvRole, "immediate");
    }
    REQUIRE(bus.pending() == 1); // the ANSWER is queued, and provably not delivered
    REQUIRE(heard.answers.empty());

    // The requester dies and comes back at the same logical address.
    const std::string own_state = bus.snapshot_bytes(asker);
    bus.kill(asker);
    REQUIRE(life.died == 1);
    REQUIRE(bus.reload(asker, own_state).revived);
    REQUIRE(life.revived == 1);

    bus.pump();
    CHECK(heard.answers.empty());          // the revival was told nothing
    CHECK(tap.answer_target_changed == 1); // and the refusal NAMES the cause
    CHECK(tap.delivered_answers == 0);
    CHECK(tap.capability_denied == 0);
    CHECK(tap.other_refusals == 0);
    CHECK(tap.expected_life == 1); // ...in numbers a journal reader can act on
    CHECK(tap.found_life == 2);
    CHECK(tap.expected_incarnation == 1);
    CHECK(tap.found_incarnation == 1);

    // THE POSITIVE CONTROL: the revived life's own question is answered normally.
    if (deferred) {
        queue_one_deferred_answer(bus, asker, responder, kProvRole, "fresh");
    } else {
        queue_one_answer(bus, asker, kProvRole, "fresh");
    }
    bus.pump();
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].attested);
    CHECK(heard.answers[0].correlation == kPublicCorrelation);
}

TEST_CASE("R2B-2c: a LIVE code reload of the requester does not inherit the conversation its "
          "predecessor completed") {
    // THE REASON INCARNATION IS REQUIRED AS WELL AS LIFE. Nothing died here: the
    // weave never stopped living, so its life generation is untouched and a
    // life-only check would hand A's finished conversation to successor code B —
    // code that never asked anything and would read the answer as its own.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    (void)mount_into_role<Responder>(bus, kProvRole, "r", Speak::Authenticated);

    queue_one_answer(bus, asker, kProvRole, "asked by incarnation 1");
    REQUIRE(bus.pending() == 1);

    // New code behind the same id, while alive.
    Value fresh(schema_of<ProvState>());
    fresh.set("n", Cell::integer(0));
    REQUIRE(bus.swap_state(asker, serialize(fresh)).revived);
    CHECK(bus.alive(asker)); // it never stopped living

    bus.pump();
    CHECK(heard.answers.empty());
    CHECK(tap.answer_target_changed == 1);
    CHECK(tap.expected_life == 1);
    CHECK(tap.found_life == 1); // the LIFE is unchanged — only the code moved
    CHECK(tap.expected_incarnation == 1);
    CHECK(tap.found_incarnation == 2);

    // ...and the successor's own question is answered normally.
    queue_one_answer(bus, asker, kProvRole, "asked by incarnation 2");
    bus.pump();
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].attested);
}

TEST_CASE("R2B-2c: ORDINARY messages keep their logical targeting — a queued direct send still "
          "reaches the address it named, even after that address gets new code") {
    // THE BOUNDARY, PINNED FROM THE OTHER SIDE. It would be easy to "improve"
    // delivery by pinning every message to the exact recipient it was aimed at
    // when queued. That is deliberately NOT the law: an ordinary send names a
    // logical destination and should reach whoever legitimately occupies it —
    // which is what makes hot-reload usable at all.
    //
    // Only envelopes that leave by an answer door carry a target expectation, and
    // this case is what would go red if that ever stopped being true.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAsk::zen_name);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);
    const WeaveId speaker = mount<Speaker>(bus, Speaker::How::Direct, heard);
    static_cast<Speaker*>(bus.weave(speaker))->aim(steward);

    queue_one_utterance(bus, speaker);
    REQUIRE(bus.pending() == 1);

    // The TARGET's code is replaced while the message waits.
    Value fresh(schema_of<ProvState>());
    fresh.set("n", Cell::integer(0));
    REQUIRE(bus.swap_state(steward, serialize(fresh)).revived);

    bus.pump();
    CHECK(tap.answer_target_changed == 0);
    CHECK(tap.other_refusals == 0);
    CHECK(static_cast<Deferrer*>(bus.weave(steward))->deliveries() == 1); // it arrived
}

TEST_CASE("R2B-2c: a requester that is live-reloaded BETWEEN the deferral and the spend is "
          "refused at the spend, before any answer is queued") {
    // THE OTHER WINDOW. A deferred conversation can be invalidated in two places:
    // before the answer is written (here) and after it is queued (the live-reload
    // case above). This one is the older guard — R2B-2's spend-time incarnation
    // check — and it is pinned here because it is what makes the R2B-2c capture
    // rule LOOK redundant: with it in place, a spend that recomputed the
    // requester's identity instead of using the record's could never observe a
    // difference. Cutting both together is the only way to see the pair.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId steward =
        mount_into_role<Deferrer>(bus, kProvRole, Deferrer::Mode::Normal);

    bus.send_as_to_role(asker, kProvRole,
                        Message(to_value(ProvAsk{"defer me"}), asker, WeaveId{},
                                kPublicCorrelation));
    bus.pump();
    Deferrer* d = static_cast<Deferrer*>(bus.weave(steward));
    REQUIRE(d->deferred_valid_);

    // New code behind the requester's id, while it is alive and while the
    // conversation is still open.
    Value fresh(schema_of<ProvState>());
    fresh.set("n", Cell::integer(0));
    REQUIRE(bus.swap_state(asker, serialize(fresh)).revived);

    bus.send(steward, Message(to_value(ProvFinish{"too late"})));
    bus.pump();
    CHECK_FALSE(d->first_spend_);   // the spend itself failed...
    CHECK(bus.pending() == 0);      // ...so no answer was ever queued
    CHECK(heard.answers.empty());
    CHECK(tap.delivered_answers == 0);

    // ...and the successor's own conversation works.
    bus.send_as_to_role(asker, kProvRole,
                        Message(to_value(ProvAsk{"mine"}), asker, WeaveId{},
                                kPublicCorrelation));
    bus.pump();
    bus.send(steward, Message(to_value(ProvFinish{"answered"})));
    bus.pump();
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].attested);
}

TEST_CASE("R2B-2c: an UNCHANGED requester still gets its answer, whatever else the world does") {
    // The positive control for the whole phase. If the target expectation were
    // even slightly too strict, this is the case that would go quiet — and every
    // "no answer arrived" assertion elsewhere would become meaningless.
    Switchboard bus;
    Received heard;
    Received bystander_heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId bystander = mount<Asker>(bus, bystander_heard);
    const WeaveId responder =
        mount_into_role<Responder>(bus, kProvRole, "r", Speak::Authenticated);

    queue_one_answer(bus, asker, kProvRole, "mine");
    REQUIRE(bus.pending() == 1);

    // Unrelated lifecycle churn everywhere EXCEPT the requester: a third weave
    // dies and revives, and the RESPONDER's own code is replaced in place. The
    // answerer's life is what R2B-2b binds, and a code reload is not a death — so
    // neither of these is this conversation's business.
    const std::string bystander_state = bus.snapshot_bytes(bystander);
    bus.kill(bystander);
    REQUIRE(bus.reload(bystander, bystander_state).revived);
    Value fresh(schema_of<ProvState>());
    fresh.set("n", Cell::integer(0));
    REQUIRE(bus.swap_state(responder, serialize(fresh)).revived);

    bus.pump();
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].attested);                       // authentic
    CHECK(heard.answers[0].correlation == kPublicCorrelation); // original label
    CHECK(heard.answers[0].tag == "r");                     // expected payload
    CHECK(tap.answer_target_changed == 0);
    CHECK(bystander_heard.answers.empty());
}

TEST_CASE("R2B-2c: an answer pumped while the requester is STILL DEAD is refused as a dead "
          "target, which is a different fact from a changed one") {
    // Two honest outcomes that must not be conflated: an address whose occupant is
    // dead (nobody can be delivered to, whoever they are) versus an address whose
    // occupant is alive and is somebody else. This case is deliberately NOT a
    // substitute for the death-then-revival proof above.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    (void)mount_into_role<Responder>(bus, kProvRole, "r", Speak::Authenticated);

    queue_one_answer(bus, asker, kProvRole, "q");
    REQUIRE(bus.pending() == 1);
    bus.kill(asker);

    bus.pump();
    CHECK(heard.answers.empty());
    CHECK(tap.answer_target_changed == 0); // NOT "changed" — it has not changed yet
    CHECK(tap.other_refusals == 1);        // TargetUnavailable, the pre-existing truth
    CHECK(tap.delivered_answers == 0);
}

TEST_CASE("R2B-2c: LAST-KNOWN-GOOD revival is no back door either") {
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    (void)mount_into_role<Responder>(bus, kProvRole, "r", Speak::Authenticated);
    LifeTap life;
    life.arm(bus, asker);

    queue_one_answer(bus, asker, kProvRole, "q");
    REQUIRE(bus.pending() == 1);

    bus.kill(asker);
    const ReviveOutcome ro = bus.reload(asker, "not a serialized value");
    REQUIRE(ro.revived);
    REQUIRE(ro.from_last_known_good);
    REQUIRE(life.revived_from_lkg == 1);

    bus.pump();
    CHECK(heard.answers.empty());
    CHECK(tap.answer_target_changed == 1);

    // And a fresh conversation works afterwards.
    queue_one_answer(bus, asker, kProvRole, "again");
    bus.pump();
    REQUIRE(heard.answers.size() == 1);
    CHECK(heard.answers[0].attested);
}

TEST_CASE("R2B-2c: when BOTH participants change, the answer still does not arrive") {
    // Two independent guards can both apply, and correctness must not depend on
    // which one is consulted first. This records which one fires today — the
    // sender's, because it is checked before the target is even resolved — while
    // asserting only the thing that matters: nothing was delivered.
    Switchboard bus;
    Received heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    const WeaveId responder =
        mount_into_role<Responder>(bus, kProvRole, "r", Speak::Authenticated);

    queue_one_answer(bus, asker, kProvRole, "q");
    REQUIRE(bus.pending() == 1);

    const std::string asker_state = bus.snapshot_bytes(asker);
    const std::string responder_state = bus.snapshot_bytes(responder);
    bus.kill(asker);
    REQUIRE(bus.reload(asker, asker_state).revived);
    bus.kill(responder);
    REQUIRE(bus.reload(responder, responder_state).revived);

    bus.pump();
    CHECK(heard.answers.empty());
    CHECK(tap.delivered_answers == 0);
    // Exactly one refusal, whichever cause it names.
    CHECK(tap.sender_life_ended + tap.answer_target_changed == 1);
    CHECK(tap.sender_life_ended == 1); // today: the author's life, checked first
}

TEST_CASE("R2B-2c: changing one requester does not invalidate an answer queued for another") {
    // The cleanup-everything shortcut would make every case above just as green.
    Switchboard bus;
    Received heard_a;
    Received heard_b;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId a = mount<Asker>(bus, heard_a);
    const WeaveId b = mount<Asker>(bus, heard_b);
    (void)mount_into_role<Responder>(bus, kProvRole, "r", Speak::Authenticated);

    // Two conversations, both answered, both answers queued: one pump, stopped
    // after the SECOND ask is delivered.
    int asks = 0;
    const ObserverId stopper = bus.add_observer([&](const BusEvent& ev) {
        if (ev.kind == EventKind::Delivered && ev.schema_name == ProvAsk::zen_name &&
            ++asks == 2) {
            bus.stop();
        }
    });
    bus.send_as_to_role(a, kProvRole,
                        Message(to_value(ProvAsk{"a"}), a, WeaveId{}, kPublicCorrelation));
    bus.send_as_to_role(b, kProvRole,
                        Message(to_value(ProvAsk{"b"}), b, WeaveId{}, kPublicCorrelation));
    bus.pump();
    bus.remove_observer(stopper);
    REQUIRE(bus.pending() == 2);

    // Only B changes.
    const std::string b_state = bus.snapshot_bytes(b);
    bus.kill(b);
    REQUIRE(bus.reload(b, b_state).revived);

    bus.pump();
    CHECK(tap.answer_target_changed == 1); // exactly one, and it is B's
    CHECK(heard_b.answers.empty());
    REQUIRE(heard_a.answers.size() == 1);  // A's conversation was never anyone's business
    CHECK(heard_a.answers[0].attested);
}

TEST_CASE("R2B-2c: WeaveIds are never reused, so a later participant cannot inherit an earlier "
          "one's conversation by numbering") {
    // The prompt's "numerically reused id" case is unreachable rather than
    // untested, and the honest thing is to pin the allocation law that makes it so
    // rather than manufacture a scenario the bus cannot produce.
    Switchboard bus;
    Received heard;
    const WeaveId first = mount<Asker>(bus, heard);
    bus.unregister_weave(first);
    Received later_heard;
    const WeaveId second = mount<Asker>(bus, later_heard);
    CHECK(second.value != first.value);
    CHECK(second.value > first.value); // monotonic, never recycled

    // And an answer owed to a weave that has been permanently removed reaches
    // nobody at all — the address resolves to no record.
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    (void)mount_into_role<Responder>(bus, kProvRole, "r", Speak::Authenticated);
    queue_one_answer(bus, asker, kProvRole, "q");
    REQUIRE(bus.pending() == 1);
    bus.unregister_weave(asker);
    bus.pump();
    CHECK(heard.answers.empty());
    CHECK(later_heard.answers.empty());
    CHECK(tap.delivered_answers == 0);
}

TEST_CASE("R2B-2c: a SWAP successor holds a different WeaveId, so an answer owed to its "
          "predecessor cannot reach it") {
    // Distinct from a reload: a swap creates a NEW record with a NEW id, so the
    // predecessor's conversation cannot even be addressed at the successor. Pinned
    // rather than argued, because "a different id" is exactly the assumption the
    // rest of this phase rests on.
    Switchboard bus;
    Received heard;
    Received successor_heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId asker = mount<Asker>(bus, heard);
    (void)mount_into_role<Responder>(bus, kProvRole, "r", Speak::Authenticated);

    queue_one_answer(bus, asker, kProvRole, "q");
    REQUIRE(bus.pending() == 1);

    // The requester is replaced by a successor: unregistered, and a fresh weave
    // mounted in its place — which is what a swap is at the bus's altitude.
    bus.unregister_weave(asker);
    const WeaveId successor = mount<Asker>(bus, successor_heard);
    CHECK(successor.value != asker.value);

    bus.pump();
    CHECK(heard.answers.empty());
    CHECK(successor_heard.answers.empty());
    CHECK(tap.delivered_answers == 0);
}

TEST_CASE("R2B-2c: a replayed raw envelope carries no target expectation, because it carries no "
          "private provenance at all") {
    // The existing replay proof covers answer provenance; this adds the new
    // private fact to the same statement. A weave that hoards a delivered answer
    // and re-sends it is speaking as itself under ordinary rules — so the copy
    // reaches its victim even though the ORIGINAL conversation named somebody else
    // entirely, and it arrives with no authenticity to inherit.
    Switchboard bus;
    Received victim_heard;
    Received magpie_heard;
    RefusalTap tap;
    tap.arm(bus, ProvAnswer::zen_name);
    const WeaveId victim = mount<Asker>(bus, victim_heard);

    auto raw = std::make_unique<RawMagpie>(victim, magpie_heard);
    RawMagpie* magpie_raw = raw.get();
    Grant g;
    g.allow_to_any(ProvAsk::zen_name, ProvAsk::zen_version);
    g.allow_to_any(ProvAnswer::zen_name, ProvAnswer::zen_version);
    const WeaveId magpie = bus.register_weave(std::move(raw), std::move(g));
    const WeaveId holder =
        mount_into_role<Responder>(bus, kProvRole, "holder", Speak::Authenticated);

    bus.send_as(magpie, holder, Message(to_value(ProvAsk{"one"}), magpie, WeaveId{}, 5));
    bus.pump();
    REQUIRE(magpie_raw->kept_was_attested());

    // The magpie's own conversation was with the MAGPIE. Replaying it at the
    // victim delivers an ordinary message — the target expectation did not come
    // along, because it was never in anything the magpie could hold.
    bus.send(magpie, Message(to_value(ProvNudge{})));
    bus.pump();
    REQUIRE(victim_heard.answers.size() == 1);
    CHECK_FALSE(victim_heard.answers[0].attested);
    CHECK(victim_heard.answers[0].sender == magpie.value);
    CHECK(tap.answer_target_changed == 0); // no expectation to violate
}

} // TEST_SUITE
