// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The seam that grows (docs/reference/dynamic-abi.md): what each table's size lets a reader take,
// what a host refuses by the number and the sizes, before any slot of the image is called, and
// the addressed role a delivery carries to a loaded weave as to a native one.

#include "box_twin.hpp"
#include "switchboard_fixtures.hpp"
#include "weavelib/addressed_protocol.hpp"

#include <zen/kernel/descriptor.hpp>
#include <zen/kernel/export.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/weave.hpp>

#include <doctest.h>

#include <cstddef>
#include <memory>
#include <string>

using namespace loom;
using namespace sbfx;
using namespace addressed;

namespace {

/// The probe's state as the bus reads it.
AddressedHeard heard(Switchboard& bus, WeaveId id) {
    const Value v = bus.weave(id)->snapshot();
    return AddressedHeard{v.get("addressed")->as_text(), v.get("authored")->as_text(),
                          v.get("knocks")->as_int()};
}

Value knock(std::int64_t n) { return to_value(AddressedKnock{n}); }

/// The probe, native: the same `on` as weavelib/addressed_probe.cpp.
class NativeProbe final : public WeaveBase<NativeProbe, AddressedHeard, Accept<AddressedKnock>> {
public:
    void on(const AddressedKnock&, Mail& mail) {
        state_.addressed = std::string(mail.addressed_role());
        state_.authored = std::string(mail.authored_role());
        ++state_.knocks;
    }
};

/// A weave that holds the role `voice` and, at each tick, knocks on the role `door.a` speaking as
/// `voice`: a delivery with both an authored and an addressed role, two different ones.
Registered register_voice(Switchboard& bus) {
    Registered voice{};
    auto owned = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{
        tick_schema()});
    voice.weave = owned.get();
    voice.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        (void)b.role_send_to_role("voice", "door.a", Message(knock(1)));
    };
    voice.id = bus.register_weave(std::move(owned), Grant{}.allow_any(), "voice");
    return voice;
}

/// What a weave reads of each kind of delivery, through whichever image `load` made it from:
/// the role a knock was addressed to, empty for a directed send and a publication, and apart
/// from the role it was authored as.
void reads_the_addressed_role(Switchboard& bus, WeaveId door) {
    bus.send_to_role("door.a", Message(knock(1)));
    bus.drain_until_idle();
    AddressedHeard h = heard(bus, door);
    CHECK(h.knocks == 1);
    CHECK(h.addressed == "door.a");
    CHECK(h.authored.empty());

    bus.send(door, Message(knock(2)));
    bus.drain_until_idle();
    h = heard(bus, door);
    CHECK(h.knocks == 2);
    CHECK(h.addressed.empty()); // a directed send names no role

    bus.publish(Message(knock(3)));
    bus.drain_until_idle();
    h = heard(bus, door);
    CHECK(h.knocks == 3);
    CHECK(h.addressed.empty()); // nor does a publication

    const Registered voice = register_voice(bus);
    bus.send(voice.id, Message(tick(1)));
    bus.drain_until_idle();
    h = heard(bus, door);
    CHECK(h.knocks == 4);
    CHECK(h.addressed == "door.a"); // where it went
    CHECK(h.authored == "voice");   // who spoke, apart
}

void loaded_reads_the_addressed_role(const char* image) {
    Switchboard bus;
    Kernel kernel(bus, fixture_admission());
    const LoadResult door = kernel.load("door", image, "door.a");
    REQUIRE_MESSAGE(door.ok, door.error);
    reads_the_addressed_role(bus, door.id);
}

/// A load refused before any slot of the image ran, by words naming each of `named`.
void refused_before_any_slot(const char* image, std::initializer_list<std::string> named) {
    Switchboard bus;
    Kernel kernel(bus, fixture_admission());
    const KernelLifetimeCounts before = kernel_lifetime_counts();
    const LoadResult lr = kernel.load("needs", image);
    const KernelLifetimeCounts after = kernel_lifetime_counts();
    CHECK_FALSE(lr.ok);
    for (const std::string& word : named) {
        CAPTURE(word);
        CHECK(lr.error.find(word) != std::string::npos);
    }
    CAPTURE(lr.error);
    // Nothing made, nothing destroyed: no slot of its was called (each is null, and would crash).
    CHECK(after.instances_created == before.instances_created);
    CHECK(after.instances_destroyed == before.instances_destroyed);
    CHECK_FALSE(kernel.is_loaded("needs"));
    CHECK(bus.list_weaves().empty());
}

ZenStatus record_send(void* ctx, std::uint64_t, std::uint64_t, std::uint64_t, const std::uint8_t*,
                      std::size_t, std::uint64_t* attempt_out) {
    ++*static_cast<int*>(ctx);
    if (attempt_out != nullptr) {
        *attempt_out = 1;
    }
    return ZEN_OK;
}

} // namespace

TEST_SUITE("seam") {

TEST_CASE("a host reads a descriptor's slots as far as its size reaches, and a slot past it as "
          "NULL") {
    const ZenWeaveAbi* full = [] {
        static const ZenWeaveAbi abi = {.abi_version = ZEN_ABI_VERSION,
                                        .size = sizeof(ZenWeaveAbi),
                                        .host_size = sizeof(ZenHostApi),
                                        .manifest_version = kManifestVersion,
                                        .create = nullptr,
                                        .destroy = nullptr,
                                        .describe = nullptr,
                                        .snapshot = nullptr,
                                        .policy = nullptr,
                                        .revive = nullptr,
                                        .handle = nullptr,
                                        .claim_published = nullptr,
        .describe_creation = nullptr,
        .create_with = nullptr};
        return &abi;
    }();
    ZenWeaveAbi shorter = *full;
    // A slot this host knows, filled, which the descriptor's size says it does not hold.
    shorter.claim_published = [](void*, const std::uint8_t*, std::size_t) -> ZenStatus {
        return ZEN_OK;
    };
    shorter.size = static_cast<std::uint32_t>(detail::kDescriptorReachesHandle);
    ZenWeaveAbi read{};
    std::string why;
    REQUIRE_MESSAGE(detail::read_descriptor(&shorter, read, why), why);
    CHECK(read.claim_published == nullptr);
    CHECK(read.size == detail::kDescriptorReachesHandle);
    CHECK(read.host_size == sizeof(ZenHostApi));
    CHECK(read.manifest_version == kManifestVersion);

    // The canary: the same descriptor with its whole size is read whole.
    shorter.size = sizeof(ZenWeaveAbi);
    REQUIRE_MESSAGE(detail::read_descriptor(&shorter, read, why), why);
    CHECK(read.claim_published != nullptr);
}

TEST_CASE("a library calls no host door its host table's size does not reach") {
    int sends = 0;
    ZenHostApi host{};
    host.ctx = &sends;
    host.send = &record_send;
    // The canary: a table that reaches the door, and the send is the host's.
    host.size = sizeof(ZenHostApi);
    detail::HostApiBus reached(&host);
    CHECK(reached.send(WeaveId{7}, Message(knock(1))).valid());
    CHECK(sends == 1);
    // The same door, filled, in a table whose size ends before it: refused, and never called.
    host.size = static_cast<std::uint32_t>(offsetof(ZenHostApi, send));
    detail::HostApiBus short_of_it(&host);
    CHECK_FALSE(short_of_it.send(WeaveId{7}, Message(knock(2))).valid());
    CHECK(sends == 1);
}

TEST_CASE("a native weave reads the role a delivery was addressed to, apart from the role it "
          "was authored as") {
    Switchboard bus;
    const WeaveId door =
        bus.register_weave(std::make_unique<NativeProbe>(), Grant{}, std::string("door.a"));
    reads_the_addressed_role(bus, door);
}

TEST_CASE("a loaded weave reads the role a delivery was addressed to, as a native one does") {
    loaded_reads_the_addressed_role(ZEN_SO_ADDRESSED);
}

ZEN_BOXED_TWIN("a boxed weave reads the role a delivery was addressed to, as a native one does",
               loaded_reads_the_addressed_role, ZEN_BOX_ADDRESSED)

TEST_CASE("an image whose descriptor says it was built against smaller tables loads on this "
          "host, works, and is not read past its size") {
    // Its descriptor ends at `handle` and names a host table ending before `sense_offer`; the
    // slot past its size holds a door that ends the process if a host reads and calls it.
    loaded_reads_the_addressed_role(ZEN_SO_ADDRESSED_SMALLER);
}

TEST_CASE("an image needing what this host lacks is refused before any slot of it runs, by what "
          "it needs and what this host has") {
    SUBCASE("the next ABI version") {
        refused_before_any_slot(ZEN_SO_NEEDS_NEXT_VERSION,
                                {"needs abi_version " + std::to_string(ZEN_ABI_VERSION + 1u),
                                 "host supports " + std::to_string(ZEN_ABI_OLDEST)});
    }
    SUBCASE("a larger host table") {
        refused_before_any_slot(
            ZEN_SO_NEEDS_LARGER_HOST,
            {"host table of " + std::to_string(sizeof(ZenHostApi) + sizeof(void*)) + " bytes",
             "its table is " + std::to_string(sizeof(ZenHostApi)) + " bytes"});
    }
    SUBCASE("a manifest this host does not read, refused before create()") {
        refused_before_any_slot(
            ZEN_SO_NEEDS_UNREAD_MANIFEST,
            {"zen.Manifest version " + std::to_string(kManifestVersion - 1u),
             "it reads version " + std::to_string(kManifestVersion)});
    }
    SUBCASE("a descriptor too short to hold its handle slot") {
        refused_before_any_slot(
            ZEN_SO_NEEDS_SHORT_DESCRIPTOR,
            {std::to_string(offsetof(ZenWeaveAbi, handle)) + " bytes",
             "does not reach its handle slot (" +
                 std::to_string(detail::kDescriptorReachesHandle) + " bytes)"});
    }
}

TEST_CASE("an image compiled against ABI 12's kept header is refused at load, both versions "
          "named, before any slot of it runs") {
    refused_before_any_slot(ZEN_SO_ABI_V12_IMAGE,
                            {"abi_version 12", "host supports " + std::to_string(ZEN_ABI_OLDEST)});
}

TEST_CASE("an image compiled against ABI 12's kept header cannot replace a live participant, and "
          "the incumbent stands") {
    Switchboard bus;
    Kernel kernel(bus, fixture_admission());
    const LoadResult door = kernel.load("door", ZEN_SO_ADDRESSED, "door.a");
    REQUIRE_MESSAGE(door.ok, door.error);
    bus.send_to_role("door.a", Message(knock(1)));
    bus.drain_until_idle();
    REQUIRE(heard(bus, door.id).knocks == 1);

    const KernelLifetimeCounts before = kernel_lifetime_counts();
    const ReloadResult rr = kernel.reload_from("door", ZEN_SO_ABI_V12_IMAGE);
    const KernelLifetimeCounts after = kernel_lifetime_counts();
    CHECK_FALSE(rr.ok);
    CHECK_FALSE(rr.reloaded);
    CHECK(rr.error.find("abi_version 12") != std::string::npos);
    CHECK(rr.error.find("host supports " + std::to_string(ZEN_ABI_OLDEST)) != std::string::npos);
    CHECK(after.instances_created == before.instances_created);
    CHECK(after.instances_destroyed == before.instances_destroyed);

    // The incumbent is what it was: the same weave, its state, still hearing its role.
    CHECK(kernel.is_loaded("door"));
    CHECK(kernel.weave_id("door") == door.id);
    CHECK(kernel.status("door") == ArtifactStatus::Live);
    bus.send_to_role("door.a", Message(knock(2)));
    bus.drain_until_idle();
    const AddressedHeard h = heard(bus, door.id);
    CHECK(h.knocks == 2);
    CHECK(h.addressed == "door.a");
}

} // TEST_SUITE("seam")
