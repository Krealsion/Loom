#include <doctest.h>

// The poke phase: live inspect / manipulate by message, under the
// ZEN_EXPOSE / ZEN_HIDE access model, enforced by the target's own
// construction layer — and the Poke weave, an ordinary participant that
// relays operator commands. Portable (in-process, no OS boundary).

#include <zen/console/console.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include "switchboard_fixtures.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace loom;
namespace au = loom;

namespace {

// The motivating fixture: a metrics-ish weave. `rate` is manipulable
// (ZEN_EXPOSE), `raw_total` is meaningful only through the weave's own
// interface (ZEN_HIDE), `label` keeps the default (readable, not writable).
struct MetricsState {
    std::int64_t rate = 0;
    std::int64_t raw_total = 0;
    std::string label;
    ZEN_SHAPE(MetricsState, 1, ZEN_EXPOSE(rate), ZEN_HIDE(raw_total), ZEN_FIELD(label));
};

// The weave's own front door: ask it, and it answers a COMPUTED value — the
// intended access path to what ZEN_HIDE keeps from raw scraping.
struct MetricsQuery {
    ZEN_SHAPE(MetricsQuery, 1);
};
struct MetricsAnswer {
    std::int64_t total;
    ZEN_SHAPE(MetricsAnswer, 1, ZEN_FIELD(total));
};

class MetricsWeave : public au::WeaveBase<MetricsWeave, MetricsState, au::Accept<MetricsQuery>,
                                          au::Emit<MetricsAnswer>> {
public:
    void on(const MetricsQuery&, au::Mail& mail) {
        mail.reply(MetricsAnswer{state_.raw_total * 2});
    }
    // The maker's own code touches its own state freely, of course.
    std::int64_t rate() const { return state_.rate; }
    const std::string& label() const { return state_.label; }
    void seed(std::int64_t raw_total, std::string label) {
        state_.raw_total = raw_total;
        state_.label = std::move(label);
    }
};

// Weave-scope vs field-scope twins for the one-primitive proof.
struct FreeState { // bare ZEN_EXPOSE(); — apply-to-all
    std::int64_t a = 7;
    std::int64_t b = 9;
    ZEN_SHAPE(FreeState, 1, ZEN_FIELD(a), ZEN_FIELD(b));
    ZEN_EXPOSE();
};
struct AllTaggedState { // the same access, spelled per-field
    std::int64_t a = 7;
    std::int64_t b = 9;
    ZEN_SHAPE(AllTaggedState, 1, ZEN_EXPOSE(a), ZEN_EXPOSE(b));
};
struct SealedState { // bare ZEN_HIDE(); — every value message-only
    std::int64_t x = 0;
    ZEN_SHAPE(SealedState, 1, ZEN_FIELD(x));
    ZEN_HIDE();
};

// The whole-state detection is VALUE-checked, not presence-checked: a
// hand-written flag that is `false`, or a field merely NAMED like the flag,
// must NOT widen access (the widening direction must never fail open).
struct FalseFlagState {
    std::int64_t v = 0;
    static constexpr bool zen_expose_all = false; // explicitly NOT exposed
    using ZenSelf = FalseFlagState;
    static constexpr const char* zen_name = "FalseFlagState";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(v)); }
};

// Wire-identity twins: same (name, version, fields), tags on one only. Their
// registration blocks are hand-written so both claim the SAME wire name.
struct TagTwinPlain {
    std::int64_t n = 0;
    using ZenSelf = TagTwinPlain;
    static constexpr const char* zen_name = "TagTwin";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(n)); }
};
struct TagTwinTagged {
    std::int64_t n = 0;
    using ZenSelf = TagTwinTagged;
    static constexpr const char* zen_name = "TagTwin";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(
            ::loom::field_entry("n", &TagTwinTagged::n,
                                ::loom::access::kExpose | ::loom::access::kHide));
    }
};

// A weave whose state carries a non-scalar field (still fully visible in the
// structure; not message-read/writable this phase).
struct ListyState {
    std::int64_t k = 0;
    std::vector<std::int64_t> items;
    ZEN_SHAPE(ListyState, 1, ZEN_EXPOSE(k), ZEN_EXPOSE(items));
    // NOTE: items is exposed on purpose — the refusal must come from the
    // scalar boundary, not the access model.
};
struct ListyNudge {
    std::int64_t seq;
    ZEN_SHAPE(ListyNudge, 1, ZEN_FIELD(seq));
};
class ListyWeave
    : public au::WeaveBase<ListyWeave, ListyState, au::Accept<ListyNudge>, au::Emit<>> {
public:
    void on(const ListyNudge&, au::Mail&) {}
};

// The forger: an ordinary granted participant that emits an answer-SHAPED
// message, trying to speak for a poked target. Emitting the shape is sayable
// through the honest API on purpose — the wall is the Poke weave's
// stamped-sender check, and that is what the test pins.
struct NudgeState {
    std::int64_t z = 0;
    ZEN_SHAPE(NudgeState, 1, ZEN_FIELD(z));
};
struct Nudge {
    std::int64_t seq;
    ZEN_SHAPE(Nudge, 1, ZEN_FIELD(seq));
};
class FalseVoice
    : public au::WeaveBase<FalseVoice, NudgeState, au::Accept<Nudge>, au::Emit<au::PokeValue>> {
public:
    au::WeaveId poke_weave{};
    std::uint64_t forged_corr = 0;
    void on(const Nudge&, au::Mail& mail) {
        mail.send(poke_weave, au::PokeValue{"rate", "Int", "999"}, forged_corr);
    }
};

// Register a probe that keeps full answer payloads (ProbeWeave records names
// only; the hook copies the Values out).
struct AnswerLog {
    sbfx::Registered reg{};
    std::vector<loom::Value> got;
};
AnswerLog register_answer_probe(Switchboard& bus) {
    AnswerLog log;
    log.reg = sbfx::register_probe(bus, {au::schema_of<au::PokeStructure>(),
                                         au::schema_of<au::PokeValue>(),
                                         au::schema_of<au::PokeAck>(),
                                         au::schema_of<au::PokeRefused>()});
    return log;
}
void capture(AnswerLog& log) {
    log.reg.weave->on_handle = [&log](const Message& in, Bus&, sbfx::ProbeWeave&) {
        log.got.push_back(in.payload);
    };
}

// Compose-arg helpers (test-local, mirroring test_console.cpp's style).
loom::Arg lit(std::int64_t v) { return loom::Arg{std::nullopt, loom::FieldValue{v}}; }
loom::Arg lit(const char* s) { return loom::Arg{std::nullopt, loom::FieldValue{std::string(s)}}; }

std::string text_field(const loom::Value& v, const char* field) {
    const loom::Cell* c = v.get(field);
    REQUIRE_MESSAGE(c != nullptr, "missing field: " << field);
    return c->as_text();
}

} // namespace

TEST_SUITE("poke") {

// ---- the access model, standalone (pure functions, no bus) -----------------

TEST_CASE("value <-> text at the poke boundary is exact and strict") {
    // Render: shortest exact forms.
    CHECK(au::poke_render(std::int64_t{-42}) == "-42");
    CHECK(au::poke_render(true) == "true");
    CHECK(au::poke_render(std::string("plain")) == "plain");

    // Parse: full-match only, kind-strict; a float survives a round trip.
    std::int64_t i = 0;
    CHECK_FALSE(au::poke_parse("12x", i));
    CHECK_FALSE(au::poke_parse("", i));
    CHECK(au::poke_parse("-7", i));
    CHECK(i == -7);
    double d = 0.0;
    CHECK(au::poke_parse(au::poke_render(0.1), d));
    CHECK(d == 0.1);
    bool b = false;
    CHECK_FALSE(au::poke_parse("TRUE", b)); // exact spelling, no coercion
    CHECK(au::poke_parse("true", b));
    CHECK(b);
}

TEST_CASE("the default with no tag: a field is read-exposed and write-hidden") {
    MetricsState s;
    s.label = "steady";

    // Read is free and universal — no tag needed.
    const auto read = au::poke_read(s, "label");
    REQUIRE(std::holds_alternative<au::PokeValue>(read));
    CHECK(std::get<au::PokeValue>(read).value == "steady");
    CHECK(std::get<au::PokeValue>(read).type == "Text");

    // Write is opt-in — an untagged field refuses, with the reason stated.
    const auto write = au::poke_write(s, "label", "hijacked");
    REQUIRE(std::holds_alternative<au::PokeRefused>(write));
    CHECK(std::get<au::PokeRefused>(write).reason ==
          "field 'label' is not exposed (ZEN_EXPOSE opts a field into manipulation)");
    CHECK(s.label == "steady"); // untouched
}

TEST_CASE("ZEN_EXPOSE opts a field into manipulation; the literal parses against the declared "
          "kind") {
    MetricsState s;
    const auto ok = au::poke_write(s, "rate", "42");
    REQUIRE(std::holds_alternative<au::PokeAck>(ok));
    CHECK(s.rate == 42);

    // A bad literal is a clean refusal, never a mis-write.
    const auto bad = au::poke_write(s, "rate", "fast");
    REQUIRE(std::holds_alternative<au::PokeRefused>(bad));
    CHECK(std::get<au::PokeRefused>(bad).reason == "value \"fast\" does not parse as Int");
    CHECK(s.rate == 42);
}

TEST_CASE("ZEN_HIDE gates the value, never the existence: the structure stays complete") {
    MetricsState s;
    s.raw_total = 1234;

    // The raw read is refused...
    const auto read = au::poke_read(s, "raw_total");
    REQUIRE(std::holds_alternative<au::PokeRefused>(read));
    CHECK(std::get<au::PokeRefused>(read).reason ==
          "field 'raw_total' is hidden (ZEN_HIDE): its value is message-only — ask the weave "
          "through its own interface");

    // ...but the field's existence, name, type, and tag-state are all still
    // fully visible — the no-secret-state pin.
    const au::PokeStructure st = au::poke_structure<MetricsState>();
    CHECK(st.state_schema == "MetricsState");
    REQUIRE(st.fields.size() == 3);
    CHECK(st.fields[0].name == "rate");
    CHECK(st.fields[0].writable);
    CHECK_FALSE(st.fields[0].hidden);
    CHECK(st.fields[1].name == "raw_total");
    CHECK(st.fields[1].type == "Int");
    CHECK(st.fields[1].hidden);
    CHECK_FALSE(st.fields[1].writable);
    CHECK(st.fields[2].name == "label");
    CHECK_FALSE(st.fields[2].writable);
    CHECK_FALSE(st.fields[2].hidden);
}

TEST_CASE("weave scope IS apply-to-all: one primitive, two spellings") {
    const auto whole = au::access_of<FreeState>();
    const auto each = au::access_of<AllTaggedState>();
    REQUIRE(whole.size() == each.size());
    for (std::size_t i = 0; i < whole.size(); ++i) {
        CHECK(whole[i].name == each[i].name);
        CHECK(whole[i].writable == each[i].writable);
        CHECK(whole[i].hidden == each[i].hidden);
    }
    // And the symmetric tag: a bare ZEN_HIDE(); hides every value.
    const auto sealed = au::access_of<SealedState>();
    REQUIRE(sealed.size() == 1);
    CHECK(sealed[0].hidden);
    CHECK_FALSE(sealed[0].writable);
}

TEST_CASE("whole-state detection is value-checked: a false flag does not widen access") {
    // The dangerous (widening) direction must never fail open: a
    // `zen_expose_all = false`, or a field merely named like the flag, keeps
    // the default (read-exposed, write-hidden).
    const auto acc = au::access_of<FalseFlagState>();
    REQUIRE(acc.size() == 1);
    CHECK(acc[0].name == "v");
    CHECK_FALSE(acc[0].writable); // the `= false` was honored, not merely detected
    CHECK_FALSE(acc[0].hidden);
    CHECK(au::shape_access_bits<FalseFlagState>() == au::access::kNone);
}

TEST_CASE("the tags never touch wire identity: tagged and untagged twins share one content-id") {
    const auto plain = au::schema_of<TagTwinPlain>();
    const auto tagged = au::schema_of<TagTwinTagged>();
    CHECK(plain->content_id() == tagged->content_id());
    CHECK(loom::same_identity(*plain, *tagged));

    // Both equal the hand-built schema — the tags are invisible to the wire.
    const auto hand = SchemaBuilder("TagTwin", 1).field("n", Kind::Int).build();
    CHECK(hand->content_id() == tagged->content_id());
}

TEST_CASE("reset restores the default state, and only when every field is writable") {
    FreeState f;
    f.a = 100;
    f.b = 200;
    const auto ok = au::poke_reset(f);
    REQUIRE(std::holds_alternative<au::PokeAck>(ok));
    CHECK(f.a == 7); // the default-constructed values
    CHECK(f.b == 9);

    MetricsState m;
    m.rate = 5;
    const auto refused = au::poke_reset(m);
    REQUIRE(std::holds_alternative<au::PokeRefused>(refused));
    CHECK(std::get<au::PokeRefused>(refused).field == "raw_total");
    CHECK(std::get<au::PokeRefused>(refused).reason ==
          "field 'raw_total' is not exposed — reset rewrites every field, so it requires a "
          "fully-exposed weave");
    CHECK(m.rate == 5); // untouched
}

TEST_CASE("a non-scalar field is fully visible in the structure but not message-read/writable "
          "this phase") {
    ListyState s;
    const au::PokeStructure st = au::poke_structure<ListyState>();
    REQUIRE(st.fields.size() == 2);
    CHECK(st.fields[1].name == "items");
    CHECK(st.fields[1].type == "List<Int>");
    CHECK(st.fields[1].writable); // the tag-state is honest even when the op is unsupported

    const auto read = au::poke_read(s, "items");
    REQUIRE(std::holds_alternative<au::PokeRefused>(read));
    CHECK(std::get<au::PokeRefused>(read).reason ==
          "field 'items' has kind List<Int> — only scalar fields are message-readable this phase");
    const auto write = au::poke_write(s, "items", "[1,2]");
    REQUIRE(std::holds_alternative<au::PokeRefused>(write));
}

// ---- the doors on the bus (the target's construction layer answers) --------

TEST_CASE("a poke is an ordinary gated message: the substrate doors answer on the bus") {
    Switchboard bus;
    WeaveId metrics = au::mount<MetricsWeave>(bus);
    AnswerLog asker = register_answer_probe(bus);
    capture(asker);

    // Describe, read, write — root-sent with the probe as the reply address.
    bus.send(metrics, Message(au::to_value(au::PokeDescribe{}), WeaveId{}, asker.reg.id));
    bus.send(metrics, Message(au::to_value(au::PokeRead{"rate"}), WeaveId{}, asker.reg.id));
    bus.send(metrics, Message(au::to_value(au::PokeWrite{"rate", "42"}), WeaveId{}, asker.reg.id));
    bus.pump();

    REQUIRE(asker.got.size() == 3);
    CHECK(asker.got[0].schema().name() == "zen.PokeStructure");
    CHECK(asker.got[1].schema().name() == "zen.PokeValue");
    CHECK(text_field(asker.got[1], "value") == "0");
    CHECK(asker.got[2].schema().name() == "zen.PokeAck");

    // The write actually landed in the live weave.
    auto* m = static_cast<MetricsWeave*>(bus.weave(metrics));
    REQUIRE(m != nullptr);
    CHECK(m->rate() == 42);
}

TEST_CASE("the access model holds on the wire: hidden read and un-exposed write are refused by "
          "message, with reasons") {
    Switchboard bus;
    WeaveId metrics = au::mount<MetricsWeave>(bus);
    auto* m = static_cast<MetricsWeave*>(bus.weave(metrics));
    m->seed(777, "steady");
    AnswerLog asker = register_answer_probe(bus);
    capture(asker);

    bus.send(metrics, Message(au::to_value(au::PokeRead{"raw_total"}), WeaveId{}, asker.reg.id));
    bus.send(metrics,
             Message(au::to_value(au::PokeWrite{"label", "hijacked"}), WeaveId{}, asker.reg.id));
    bus.pump();

    REQUIRE(asker.got.size() == 2);
    CHECK(asker.got[0].schema().name() == "zen.PokeRefused");
    CHECK(text_field(asker.got[0], "op") == "read");
    CHECK(text_field(asker.got[0], "reason") ==
          "field 'raw_total' is hidden (ZEN_HIDE): its value is message-only — ask the weave "
          "through its own interface");
    CHECK(asker.got[1].schema().name() == "zen.PokeRefused");
    CHECK(text_field(asker.got[1], "op") == "write");
    CHECK(m->label() == "steady"); // the wall held
}

TEST_CASE("the front door stays sovereign: a hidden value is still reachable through the weave's "
          "own interface") {
    Switchboard bus;
    WeaveId metrics = au::mount<MetricsWeave>(bus);
    static_cast<MetricsWeave*>(bus.weave(metrics))->seed(777, "steady");

    sbfx::Registered asker = sbfx::register_probe(bus, {au::schema_of<MetricsAnswer>()});
    std::vector<loom::Value> got;
    asker.weave->on_handle = [&](const Message& in, Bus&, sbfx::ProbeWeave&) {
        got.push_back(in.payload);
    };

    bus.send(metrics, Message(au::to_value(MetricsQuery{}), WeaveId{}, asker.id));
    bus.pump();

    // The weave ANSWERED the hidden data through its own message interface —
    // computed, on its own terms. ZEN_HIDE never touches the front door.
    REQUIRE(got.size() == 1);
    CHECK(got[0].schema().name() == "MetricsAnswer");
    CHECK(got[0].get("total")->as_int() == 1554);
}

TEST_CASE("answering pokes confers no authority: an ungranted weave's answer is CapabilityDenied "
          "at delivery") {
    Switchboard bus;
    // mount_granted with the EMPTY grant: the host said nothing may be sent.
    WeaveId metrics = au::mount_granted<MetricsWeave>(bus, Grant::nothing());
    AnswerLog asker = register_answer_probe(bus);
    capture(asker);

    std::vector<sbfx::TapRecord> tap;
    bus.add_observer([&](const BusEvent& e) { tap.push_back(sbfx::to_record(e)); });

    bus.send(metrics, Message(au::to_value(au::PokeRead{"rate"}), WeaveId{}, asker.reg.id));
    bus.pump();

    // The request arrived and was enforced; the ANSWER bowed to the grant.
    CHECK(asker.got.empty());
    bool request_delivered = false;
    bool answer_denied = false;
    for (const auto& r : tap) {
        if (r.schema == "zen.PokeRead" && r.kind == EventKind::Delivered) {
            request_delivered = true;
        }
        if (r.schema == "zen.PokeValue" && r.kind == EventKind::Refused &&
            r.reason == RefusalReason::CapabilityDenied) {
            answer_denied = true;
        }
    }
    CHECK(request_delivered);
    CHECK(answer_denied);
}

TEST_CASE("a fire-and-forget poke is performed but has nowhere to answer") {
    Switchboard bus;
    WeaveId metrics = au::mount<MetricsWeave>(bus);

    std::vector<sbfx::TapRecord> from_metrics;
    bus.add_observer([&](const BusEvent& e) {
        if (e.sender == metrics) {
            from_metrics.push_back(sbfx::to_record(e));
        }
    });

    // Root-sent, no sender, no reply address: the write happens, the answer
    // has no destination, and nothing is emitted (not even a misfire to id 0).
    bus.send(metrics, Message(au::to_value(au::PokeWrite{"rate", "9"}), WeaveId{}, WeaveId{}));
    bus.pump();

    CHECK(static_cast<MetricsWeave*>(bus.weave(metrics))->rate() == 9);
    CHECK(from_metrics.empty());
}

// ---- the Poke weave: an ordinary participant relaying operator commands ----

TEST_CASE("the Poke weave relays: command in, protocol out, answer back to the asker") {
    Switchboard bus;
    WeaveId metrics = au::mount<MetricsWeave>(bus);
    WeaveId poke = au::mount<au::PokeWeave>(bus);
    AnswerLog asker = register_answer_probe(bus);
    capture(asker);
    const auto tid = static_cast<std::int64_t>(metrics.value);

    bus.send(poke, Message(au::to_value(au::PokeInspect{tid}), WeaveId{}, asker.reg.id, 11));
    bus.send(poke, Message(au::to_value(au::PokeSet{tid, "rate", "42"}), WeaveId{}, asker.reg.id, 12));
    bus.send(poke, Message(au::to_value(au::PokeGet{tid, "rate"}), WeaveId{}, asker.reg.id, 13));
    bus.send(poke, Message(au::to_value(au::PokeReset{tid}), WeaveId{}, asker.reg.id, 14));
    bus.pump();

    REQUIRE(asker.got.size() == 4);
    CHECK(asker.got[0].schema().name() == "zen.PokeStructure");
    CHECK(text_field(asker.got[0], "state_schema") == "MetricsState");
    CHECK(asker.got[1].schema().name() == "zen.PokeAck");
    CHECK(asker.got[2].schema().name() == "zen.PokeValue");
    CHECK(text_field(asker.got[2], "value") == "42");
    // Reset is refused: MetricsState is not fully exposed. A refusal is an
    // answer, and it relays like one.
    CHECK(asker.got[3].schema().name() == "zen.PokeRefused");
    CHECK(text_field(asker.got[3], "op") == "reset");

    CHECK(static_cast<MetricsWeave*>(bus.weave(metrics))->rate() == 42);

    // Every pending poke was matched and drained (host-side snapshot read —
    // the host's own authority, not a message path).
    const loom::Value snap = bus.weave(poke)->snapshot();
    CHECK(snap.get("pending")->as_list().empty());
}

TEST_CASE("a forged answer is not the target's voice: relay requires the stamped sender to match "
          "the poked target") {
    Switchboard bus;
    WeaveId poke = au::mount<au::PokeWeave>(bus);
    WeaveId liar = au::mount<FalseVoice>(bus);
    AnswerLog asker = register_answer_probe(bus);
    capture(asker);

    // A target that will never answer: a raw probe has no poke doors, so the
    // forwarded zen.PokeRead is refused NotAccepted and the pending poke stays.
    sbfx::Registered mute = sbfx::register_probe(bus, {sbfx::ping_schema()});

    bus.send(poke, Message(au::to_value(au::PokeGet{static_cast<std::int64_t>(mute.id.value),
                                                    "rate"}),
                           WeaveId{}, asker.reg.id, 21));
    bus.pump();
    CHECK(asker.got.empty()); // nothing answered; the pending poke is parked

    // The forger emits a perfectly-shaped zen.PokeValue with the pending
    // correlation (seq 1). Emitting the SHAPE is sayable through the honest
    // API — the wall is the Poke weave's stamped-sender check.
    auto* voice = static_cast<FalseVoice*>(bus.weave(liar));
    voice->poke_weave = poke;
    voice->forged_corr = 1;
    bus.send(liar, Message(au::to_value(Nudge{1})));
    bus.pump();

    // Not relayed: the stamped sender (the forger) is not the poked target.
    CHECK(asker.got.empty());
    const loom::Value snap = bus.weave(poke)->snapshot();
    CHECK(snap.get("pending")->as_list().size() == 1); // still parked, not consumed
}

TEST_CASE("an unsolicited answer with no pending poke is dropped, not relayed") {
    Switchboard bus;
    WeaveId poke = au::mount<au::PokeWeave>(bus);
    WeaveId liar = au::mount<FalseVoice>(bus);
    AnswerLog asker = register_answer_probe(bus);
    capture(asker);

    auto* voice = static_cast<FalseVoice*>(bus.weave(liar));
    voice->poke_weave = poke;
    voice->forged_corr = 99; // matches nothing
    bus.send(liar, Message(au::to_value(Nudge{1})));
    bus.pump();

    CHECK(asker.got.empty());
}

// ---- driven from the existing console: the phase's standalone payoff -------

TEST_CASE("driven from the console, end to end: inspect, manipulate, and honest refusals — no UI "
          "needed") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    WeaveId metrics = au::mount<MetricsWeave>(bus);
    WeaveId poke = au::mount<au::PokeWeave>(bus);
    static_cast<MetricsWeave*>(bus.weave(metrics))->seed(777, "steady");
    const auto tid = static_cast<std::int64_t>(metrics.value);
    // The console's wildcard-accept admits only REGISTERED shapes (pinned in
    // the console suite). The zen.Poke* answers are registered by the Poke
    // weave's own accept-set; MetricsAnswer needs a consumer to be known —
    // mirror the console suite's convention with a listening probe.
    sbfx::register_probe(bus, {au::schema_of<MetricsAnswer>()});

    // The console discovers the Poke weave like any participant.
    bool poke_listed = false;
    for (const auto& w : engine.weaves()) {
        if (w.id == poke) {
            poke_listed = true;
        }
    }
    CHECK(poke_listed);

    // "inspect weave X" — the operator composes a command AT the Poke weave.
    Composed inspect = engine.compose(poke, "zen.PokeInspect", 1, {lit(tid)});
    REQUIRE(inspect.status == Composed::Status::Ready);
    engine.pump();
    CHECK(engine.outcome(inspect.ticket).delivered);
    REQUIRE(engine.buffer_size() == 1);
    const auto m1 = engine.buffer_at(1);
    REQUIRE(m1.has_value());
    CHECK(m1->name == "zen.PokeStructure");
    // The structure is complete — the hidden field is visible AS hidden.
    const auto& fields = m1->value.get("fields")->as_list();
    REQUIRE(fields.size() == 3);
    const loom::Value& raw_total = *fields[1].as_message();
    CHECK(text_field(raw_total, "name") == "raw_total");
    CHECK(text_field(raw_total, "type") == "Int");
    CHECK(raw_total.get("hidden")->as_bool());
    CHECK_FALSE(raw_total.get("writable")->as_bool());

    // "set field Y on weave X to Z" — succeeds on the exposed field...
    Composed set_ok = engine.compose(poke, "zen.PokeSet", 1, {lit(tid), lit("rate"), lit("42")});
    REQUIRE(set_ok.status == Composed::Status::Ready);
    engine.pump();
    REQUIRE(engine.buffer_size() == 2);
    CHECK(engine.buffer_at(2)->name == "zen.PokeAck");
    CHECK(static_cast<MetricsWeave*>(bus.weave(metrics))->rate() == 42);

    // ...and is REFUSED on the un-exposed field, with the reason in hand.
    Composed set_no = engine.compose(poke, "zen.PokeSet", 1, {lit(tid), lit("label"), lit("x")});
    REQUIRE(set_no.status == Composed::Status::Ready);
    engine.pump();
    REQUIRE(engine.buffer_size() == 3);
    CHECK(engine.buffer_at(3)->name == "zen.PokeRefused");
    CHECK(text_field(engine.buffer_at(3)->value, "reason") ==
          "field 'label' is not exposed (ZEN_EXPOSE opts a field into manipulation)");

    // A raw read of the hidden value is refused...
    Composed get_hidden = engine.compose(poke, "zen.PokeGet", 1, {lit(tid), lit("raw_total")});
    REQUIRE(get_hidden.status == Composed::Status::Ready);
    engine.pump();
    REQUIRE(engine.buffer_size() == 4);
    CHECK(engine.buffer_at(4)->name == "zen.PokeRefused");

    // ...while the weave's own front door still serves it, computed.
    std::string err;
    Ticket q = engine.submit(metrics, "MetricsQuery", 1, {}, &err);
    engine.pump();
    CHECK(engine.outcome(q).delivered);
    REQUIRE(engine.buffer_size() == 5);
    CHECK(engine.buffer_at(5)->name == "MetricsAnswer");
    CHECK(engine.buffer_at(5)->value.get("total")->as_int() == 1554);
}

TEST_CASE("the protocol shapes are ordinary messages: the console can poke a target directly") {
    Switchboard bus;
    ConsoleEngine engine(bus);
    WeaveId metrics = au::mount<MetricsWeave>(bus);
    // No Poke weave anywhere: zen.PokeRead is just a registered shape and the
    // console is just a granted participant. (A consumer of the answer shapes
    // must exist for the console's wildcard-accept to admit them — normally
    // the Poke weave's accept-set does that job; here a probe stands in.)
    sbfx::register_probe(bus, {au::schema_of<au::PokeValue>()});

    Composed c = engine.compose(metrics, "zen.PokeRead", 1, {lit("rate")});
    REQUIRE(c.status == Composed::Status::Ready);
    engine.pump();
    REQUIRE(engine.buffer_size() == 1);
    CHECK(engine.buffer_at(1)->name == "zen.PokeValue");
    CHECK(text_field(engine.buffer_at(1)->value, "value") == "0");

    // Directness, pinned: the answer's stamped sender is the TARGET itself —
    // no intermediary spoke for it.
    bool direct = false;
    for (const auto& t : engine.tap()) {
        if (t.kind == "Delivered" && t.schema == "zen.PokeValue" && t.sender == metrics) {
            direct = true;
        }
    }
    CHECK(direct);
}

} // TEST_SUITE
