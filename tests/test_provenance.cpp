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
                if (ev.refusal.reason == RefusalReason::CapabilityDenied) {
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
    CHECK(tap.capability_denied == 1); // visible on the tap, at the same altitude as a grant refusal
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
    CHECK(tap.capability_denied == 1);
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
    CHECK(tap.capability_denied == 1);
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

} // TEST_SUITE
