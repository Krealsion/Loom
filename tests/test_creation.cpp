// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The creation value (docs/reference/dynamic-abi.md#the-creation-value): a weave made with one
// value, admitted by the gate against the shape the weave declares before any of its code runs,
// through each door a load comes through, and refused with the load when it does not admit.

#include "box_twin.hpp"
#include "switchboard_fixtures.hpp"
#include "weavelib/addressed_protocol.hpp"
#include "weavelib/made_protocol.hpp"

#include <boot_plan.hpp>

#include <zen/kernel/control.hpp>
#include <zen/kernel/creation.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/weave.hpp>

#include <doctest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace loom;
using namespace sbfx;
using namespace made;

namespace {

Value setup(const std::string& place, std::int64_t limit) { return to_value(MadeSetup{place, limit}); }

/// The value's JSON form, as a plan row or a load message carries it.
std::string setup_json(const std::string& place, std::int64_t limit) {
    return compat::serialize(setup(place, limit));
}

/// An asker of the made-with probe: each tick asks it once, and each answer is kept.
struct Asker {
    Registered probe{};
    WeaveId target{};
    std::vector<MadeTold> told;

    explicit Asker(Switchboard& bus) {
        probe = register_probe(bus, {schema_of<MadeTold>(), tick_schema()});
        probe.weave->on_handle = [this](const Message& in, Bus& b, ProbeWeave&) {
            if (in.payload.schema().name() == "Tick") {
                b.send(target, Message(to_value(MadeAsk{1})));
                return;
            }
            told.push_back(from_value<MadeTold>(in.payload));
        };
    }
    void ask(Switchboard& bus) {
        bus.send(probe.id, Message(tick(0)));
        bus.drain_until_idle();
    }
};

/// A load refused before the weave was made: nothing made and nothing loaded, in words naming
/// each of `named`.
void refused_unmade(const char* image, const Creation& creation,
                    std::initializer_list<std::string> named) {
    Switchboard bus;
    Kernel kernel(bus, fixture_admission());
    const KernelLifetimeCounts before = kernel_lifetime_counts();
    const LoadResult lr = kernel.load("m", image, {}, creation);
    const KernelLifetimeCounts after = kernel_lifetime_counts();
    CHECK_FALSE(lr.ok);
    CAPTURE(lr.error);
    for (const std::string& word : named) {
        CHECK(lr.error.find(word) != std::string::npos);
    }
    CHECK(after.instances_created == before.instances_created);
    CHECK_FALSE(kernel.is_loaded("m"));
    CHECK(bus.list_weaves().empty());
}

void made_with_its_value(const char* image) {
    Switchboard bus;
    Kernel kernel(bus, fixture_admission());
    const LoadResult lr = kernel.load("m", image, {}, Creation::of(setup("north", 3)));
    REQUIRE_MESSAGE(lr.ok, lr.error);
    Asker asker(bus);
    asker.target = lr.id;
    asker.ask(bus);
    REQUIRE(asker.told.size() == 1);
    CHECK(asker.told[0].place == "north");
    CHECK(asker.told[0].limit == 3);
    // Recorded: the kernel says what it was made with, as admitted.
    const std::optional<Value> made = kernel.made_with("m");
    REQUIRE(made.has_value());
    CHECK(from_value<MadeSetup>(*made).place == "north");
}

void refused_when_it_does_not_meet(const char* image) {
    // A value of another shape, and one of the declared name with another definition.
    refused_unmade(image, Creation::of(to_value(addressed::AddressedKnock{1})),
                   {"the creation value is refused by the gate"});
    const auto other = SchemaBuilder("MadeSetup", 1).field("place", Kind::Text).build();
    Value impostor(other);
    impostor.set("place", Cell::text("south"));
    refused_unmade(image, Creation::of(impostor), {"the creation value is refused by the gate"});
    // A weave made with a value, and a load naming none.
    refused_unmade(image, Creation{}, {"made with MadeSetup v1", "names no value"});
}

/// A native weave made with a value, mounted by the door a load's value takes.
class NativeMade final : public WeaveBase<NativeMade, MadeState, Accept<MadeAsk>, Emit<MadeTold>> {
public:
    ZEN_MADE_WITH(MadeSetup);
    explicit NativeMade(const MadeSetup& s) : setup_(s) {}
    void on(const MadeAsk&, Mail& mail) {
        ++state_.asks;
        (void)mail.reply(MadeTold{setup_.place, setup_.limit, state_.asks});
    }

private:
    MadeSetup setup_;
};

} // namespace

TEST_SUITE("creation") {

TEST_CASE("a loaded weave is made with the value its load names, and the kernel records it") {
    made_with_its_value(ZEN_SO_MADE);
}

ZEN_BOXED_TWIN("a boxed weave is made with the value its load names, as a loaded one is",
               made_with_its_value, ZEN_BOX_MADE)

TEST_CASE("a value the declared shape does not admit, or none for a weave made with one, refuses "
          "the load before the weave is made") {
    refused_when_it_does_not_meet(ZEN_SO_MADE);
}

ZEN_BOXED_TWIN("a boxed weave refuses a value its declared shape does not admit, and a load naming "
               "none, before it is made",
               refused_when_it_does_not_meet, ZEN_BOX_MADE)

TEST_CASE("a weave made with no value refuses a load that names one, before it is made") {
    refused_unmade(ZEN_SO_ADDRESSED, Creation::of(setup("north", 3)),
                   {"made with no value", "MadeSetup v1"});
}

TEST_CASE("a reload makes the new image with the value the load named") {
    Switchboard bus;
    Kernel kernel(bus, fixture_admission());
    const LoadResult lr = kernel.load("m", ZEN_SO_MADE, {}, Creation::of(setup("east", 7)));
    REQUIRE_MESSAGE(lr.ok, lr.error);
    Asker asker(bus);
    asker.target = lr.id;
    asker.ask(bus);
    const KernelLifetimeCounts before = kernel_lifetime_counts();
    const ReloadResult rr = kernel.reload_from("m", ZEN_SO_MADE);
    REQUIRE_MESSAGE(rr.reloaded, rr.error);
    CHECK(kernel_lifetime_counts().instances_created == before.instances_created + 1);
    asker.ask(bus);
    REQUIRE(asker.told.size() == 2);
    // The value is the new instance's own (not state), and the state came across as ever.
    CHECK(asker.told[1].place == "east");
    CHECK(asker.told[1].limit == 7);
    CHECK(asker.told[1].asks == 2);
}

TEST_CASE("a prepared candidate is made with the value it names") {
    Switchboard bus;
    Kernel kernel(bus, fixture_admission());
    const Registered coordinator = register_probe(bus, {schema_of<MadeTold>()});
    const LoadResult lr =
        kernel.load_candidate("cand", ZEN_SO_MADE, coordinator.id, Creation::of(setup("west", 1)));
    REQUIRE_MESSAGE(lr.ok, lr.error);
    const std::optional<Value> made = kernel.made_with("cand");
    REQUIRE(made.has_value());
    CHECK(from_value<MadeSetup>(*made).place == "west");
}

TEST_CASE("a load message names the value in its JSON form, and the door's list says what each "
          "weave was made with") {
    Switchboard bus;
    Kernel kernel(bus, fixture_admission());
    const WeaveId control = mount_control(kernel, bus);
    std::vector<std::string> answers;
    Registered asker = register_probe(bus, {schema_of<Result>(), schema_of<Refused>()});
    asker.weave->on_handle = [&](const Message& in, Bus&, ProbeWeave&) {
        answers.push_back(in.payload.schema().name() + ":" + in.payload.get(
            in.payload.schema().name() == "zen.Result" ? "value" : "reason")->as_text());
    };
    bus.send(control, Message(to_value(LoadLibrary{"m", ZEN_SO_MADE, {}, setup_json("north", 3)}),
                              asker.id, asker.id));
    bus.send(control, Message(to_value(LoadLibrary{"n", ZEN_SO_MADE, {}, {}}), asker.id, asker.id));
    bus.send(control, Message(to_value(ListLibraries{}), asker.id, asker.id));
    bus.drain_until_idle();
    REQUIRE(answers.size() == 3);
    CHECK(answers[0].rfind("zen.Result:", 0) == 0);
    CHECK(answers[1].find("names no value") != std::string::npos);
    CHECK(answers[2] == "zen.Result:m<MadeSetup v1>");
}

TEST_CASE("a native weave is mounted with its value by the door a load's value takes") {
    Switchboard bus;
    std::string why;
    const WeaveId id = mount_made<NativeMade>(bus, setup("north", 3), why);
    REQUIRE_MESSAGE(id.valid(), why);
    Asker asker(bus);
    asker.target = id;
    asker.ask(bus);
    REQUIRE(asker.told.size() == 1);
    CHECK(asker.told[0].place == "north");
    // The same gate's refusal, in the same words.
    const WeaveId refused = mount_made<NativeMade>(bus, to_value(addressed::AddressedKnock{1}), why);
    CHECK_FALSE(refused.valid());
    CHECK(why.find("the creation value is refused by the gate") != std::string::npos);
}

TEST_CASE("a boot row names the value its weave is made with") {
    const std::string path = "creation-boot-plan.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"boot":[{"name":"m","path":"/x/m.so"}]})";
    }
    host::BootPlan plan;
    std::string why;
    // The canary: a row naming none is made with none.
    REQUIRE_MESSAGE(host::read_boot_plan(path, &plan, &why), why);
    REQUIRE(plan.entries.size() == 1);
    CHECK(plan.entries[0].creation.empty());
    {
        std::ofstream out(path, std::ios::binary);
        std::string quoted;
        for (const char c : setup_json("north", 3)) {
            if (c == '"' || c == '\\') {
                quoted += '\\';
            }
            quoted += c;
        }
        out << R"({"boot":[{"name":"m","path":"/x/m.so","creation":")" << quoted << "\"}]}";
    }
    REQUIRE_MESSAGE(host::read_boot_plan(path, &plan, &why), why);
    REQUIRE(plan.entries.size() == 1);
    CHECK(plan.entries[0].creation == setup_json("north", 3));
    std::remove(path.c_str());
}

TEST_CASE("an image compiled against ABI 13's kept header loads and works on this host, made with "
          "no value, and refuses one") {
    Switchboard bus;
    Kernel kernel(bus, fixture_admission());
    const LoadResult door = kernel.load("door", ZEN_SO_ABI_V13_IMAGE, "door.a");
    REQUIRE_MESSAGE(door.ok, door.error);
    bus.send_to_role("door.a", Message(to_value(addressed::AddressedKnock{1})));
    bus.drain_until_idle();
    const Value heard = bus.weave(door.id)->snapshot();
    CHECK(heard.get("knocks")->as_int() == 1);
    CHECK(heard.get("addressed")->as_text() == "door.a");
    CHECK_FALSE(kernel.made_with("door").has_value());
    refused_unmade(ZEN_SO_ABI_V13_IMAGE, Creation::of(setup("north", 3)), {"made with no value"});
}

} // TEST_SUITE("creation")
