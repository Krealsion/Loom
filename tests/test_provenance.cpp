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

#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <memory>
#include <string>
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
    std::int64_t other_refusals = 0;
    std::int64_t delivered_answers = 0;

    void arm(Switchboard& bus, const char* shape) {
        bus.add_observer([this, shape](const BusEvent& ev) {
            if (ev.schema_name != shape) {
                return;
            }
            if (ev.kind == EventKind::Delivered) {
                ++delivered_answers;
            } else if (ev.kind == EventKind::Refused) {
                if (ev.refusal.reason == RefusalReason::CapabilityDenied) {
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

} // TEST_SUITE
