// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// CALLS ACROSS THE SEAM (MSG-14): a loaded weave calls as a native one does and provides as one
// does; a provider reloaded between two calls answers the second as its successor; a function
// that speaks through a host table it kept from a delivery is refused at every door; an image
// built against ABI 16's header loads and works without the door; and a box runs a provider's
// function in a store of its own, where it cannot change its weave.

#include <doctest.h>

#include "box_twin.hpp"
#include "switchboard_fixtures.hpp"
#include "weavelib/addressed_protocol.hpp"
#include "weavelib/call_protocol.hpp"

#include <zen/kernel/kernel.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace loom;
using namespace sbfx;
using namespace calling;

namespace {

/// A native provider of CallAdd to CallSum, answering `by` 7.
class NativeAdder final : public WeaveBase<NativeAdder, CallHeard, Accept<CallPeek>, Emit<>,
                                           Claims<>, Provides<CallAdd>> {
public:
    void on(const CallPeek&, Mail&) { ++state_.knocks; }
    static CallSum provide(const CallAdd& add) { return CallSum{add.a + add.b, 7}; }
};

/// A native caller whose handler does what the case hands it.
class NativeCaller final : public WeaveBase<NativeCaller, CallHeard, Accept<CallKnock>> {
public:
    explicit NativeCaller(std::function<void(Mail&)> act) : act_(std::move(act)) {}
    void on(const CallKnock&, Mail& mail) { act_(mail); }

private:
    std::function<void(Mail&)> act_;
};

CallHeard heard(Switchboard& sb, WeaveId id) {
    return from_value<CallHeard>(sb.weave(id)->snapshot());
}

/// Knock a loaded probe at `role` to call `callee` with CallAdd{a, b}, and read what it kept.
CallHeard knock(Switchboard& sb, const std::string& role, WeaveId id, const std::string& callee,
                std::int64_t a, std::int64_t b) {
    sb.send_to_role(role, Message(to_value(CallKnock{callee, a, b})));
    sb.drain_until_idle();
    return heard(sb, id);
}

/// One call from a native caller granted the rule, with what it held.
Called<CallSum> call_from_native(Switchboard& sb, const std::string& role, CallAdd add) {
    std::optional<Called<CallSum>> got;
    auto w = std::make_unique<NativeCaller>(
        [&](Mail& m) { got.emplace(m.call<CallSum>(role, add)); });
    NativeCaller* raw = w.get();
    const WeaveId c = sb.register_weave(std::move(w), Grant{}.allow_call(role, "CallAdd", 1));
    raw->zen_set_self(c);
    sb.send(c, Message(to_value(CallKnock{"", 0, 0})));
    sb.drain_until_idle();
    sb.unregister_weave(c);
    return std::move(*got);
}

/// A loaded or boxed caller calls a native provider, and is told each refusal in its own word.
void an_image_calls_a_native_provider(const char* path) {
    Switchboard sb;
    Kernel kernel(sb, fixture_admission());
    auto adder = std::make_unique<NativeAdder>();
    NativeAdder* raw = adder.get();
    const WeaveId provider = sb.register_weave(std::move(adder), Grant{}, "arith");
    raw->zen_set_self(provider);
    const LoadResult r =
        kernel.load("caller", path, "caller.a", Grant{}.allow_call("arith", "CallAdd", 1));
    REQUIRE_MESSAGE(r.ok, r.error);
    const std::size_t pending = sb.pending();
    CallHeard h = knock(sb, "caller.a", r.id, "arith", 20, 22);
    CHECK(h.refusal == static_cast<std::int64_t>(CallRefusal::None));
    CHECK(h.value == 42);
    CHECK(h.by == 7);
    CHECK(sb.pending() == pending);
    CHECK(heard(sb, provider).knocks == 0); // nothing was delivered to it
    h = knock(sb, "caller.a", r.id, "nobody", 1, 1);
    CHECK(h.refusal == static_cast<std::int64_t>(CallRefusal::NotAuthorized));
}

/// A native caller calls a loaded or boxed provider; its function's failure crosses as `Failed`
/// with its words, and the provider's own delivery is untouched.
void a_native_weave_calls_an_image(const char* path) {
    Switchboard sb;
    Kernel kernel(sb, fixture_admission());
    const LoadResult r = kernel.load("adder", path, "arith", Grant{});
    REQUIRE_MESSAGE(r.ok, r.error);
    const Called<CallSum> got = call_from_native(sb, "arith", CallAdd{2, 3});
    REQUIRE_MESSAGE(got, name_of(got.refusal()), " ", got.words());
    CHECK(got->value == 5);
    CHECK(got->by == 1);
    const Called<CallSum> failed = call_from_native(sb, "arith", CallAdd{kCallFails, 0});
    CHECK(failed.refusal() == CallRefusal::Failed);
    CHECK(failed.words().find("the probe was asked to fail") != std::string::npos);
    CHECK(heard(sb, r.id).knocks == 0);
    CHECK(sb.alive(r.id)); // a function that failed is no handler that failed
}

} // namespace

TEST_SUITE("call_loaded") {

TEST_CASE("a loaded weave calls a native provider inside its handler") {
    an_image_calls_a_native_provider(ZEN_SO_CALL_PROBE);
}

TEST_CASE("a native weave calls a loaded provider's function, of its argument alone") {
    a_native_weave_calls_an_image(ZEN_SO_CALL_PROBE);
}

TEST_CASE("a loaded provider reloaded between two calls answers the second as its successor") {
    Switchboard sb;
    Kernel kernel(sb, fixture_admission());
    const LoadResult r = kernel.load("adder", ZEN_SO_CALL_PROBE, "arith", Grant{});
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(call_from_native(sb, "arith", CallAdd{1, 1})->by == 1);
    const ReloadResult reloaded = kernel.reload_from("adder", ZEN_SO_CALL_PROBE_V2);
    REQUIRE_MESSAGE(reloaded.reloaded, reloaded.error);
    const Called<CallSum> second = call_from_native(sb, "arith", CallAdd{1, 1});
    REQUIRE(second);
    CHECK(second->by == 2);
    // A successor that provides nothing provides nothing: the functions belong to the code.
    const ReloadResult bare = kernel.reload_from("adder", ZEN_SO_CALL_CALLER);
    REQUIRE_MESSAGE(bare.reloaded, bare.error);
    CHECK(call_from_native(sb, "arith", CallAdd{1, 1}).refusal() == CallRefusal::NotProvided);
}

TEST_CASE("a function that speaks through a host table it kept from a delivery is refused at "
          "every door") {
    Switchboard sb;
    Kernel kernel(sb, fixture_admission());
    const LoadResult r = kernel.load("forger", ZEN_SO_CALL_FORGE, "calls.forge",
                                     Grant{}.allow_any().allow_call("calls.forge", "CallAdd", 1));
    REQUIRE_MESSAGE(r.ok, r.error);
    // One delivery, whose table the image keeps.
    sb.send_to_role("calls.forge", Message(to_value(CallKnock{"", 0, 0})));
    sb.drain_until_idle();
    REQUIRE(heard(sb, r.id).knocks == 1);
    const std::size_t pending = sb.pending();
    const Called<CallSum> got = call_from_native(sb, "calls.forge", CallAdd{1, 2});
    REQUIRE_MESSAGE(got, name_of(got.refusal()), " ", got.words());
    CHECK(got->value == ZEN_ERR_REFUSED); // its send, refused
    CHECK(got->by == ZEN_ERR_REFUSED);    // its call, refused
    CHECK(sb.pending() == pending);       // and nothing was queued
}

TEST_CASE("a loaded image's function runs in the image's own memory: pure is asked of native "
          "code") {
    Switchboard sb;
    Kernel kernel(sb, fixture_admission());
    const LoadResult r = kernel.load("adder", ZEN_SO_CALL_PROBE, "arith", Grant{});
    REQUIRE_MESSAGE(r.ok, r.error);
    REQUIRE(call_from_native(sb, "arith", CallAdd{1, 1}));
    sb.send_to_role("arith", Message(to_value(CallPeek{1})));
    sb.drain_until_idle();
    CHECK(heard(sb, r.id).provided_here == 1); // nothing here stops a native function's write
}

TEST_CASE("an image built against ABI 16's header loads and works here, without the door") {
    Switchboard sb;
    Kernel kernel(sb, fixture_admission());
    const LoadResult door = kernel.load("door", ZEN_SO_ABI_V16_IMAGE, "door.a");
    REQUIRE_MESSAGE(door.ok, door.error);
    sb.send_to_role("door.a", Message(to_value(addressed::AddressedKnock{1})));
    sb.drain_until_idle();
    const auto knocked = from_value<addressed::AddressedHeard>(sb.weave(door.id)->snapshot());
    CHECK(knocked.knocks == 1);
    CHECK(knocked.addressed == "door.a");
    // A caller built against it reaches no call door: its table ends before one.
    auto adder = std::make_unique<NativeAdder>();
    NativeAdder* raw = adder.get();
    raw->zen_set_self(sb.register_weave(std::move(adder), Grant{}, "arith"));
    const LoadResult caller = kernel.load("caller", ZEN_SO_ABI_V16_CALLER, "caller.a",
                                          Grant{}.allow_call("arith", "CallAdd", 1));
    REQUIRE_MESSAGE(caller.ok, caller.error);
    const CallHeard h = knock(sb, "caller.a", caller.id, "arith", 1, 1);
    CHECK(h.knocks == 1);
    CHECK(h.refusal == static_cast<std::int64_t>(CallRefusal::NotOffered));
}

} // TEST_SUITE("call_loaded")

ZEN_BOXED_TWIN("a boxed weave calls a native provider inside its handler",
               an_image_calls_a_native_provider, ZEN_BOX_CALL_PROBE)
ZEN_BOXED_TWIN("a native weave calls a boxed provider's function, of its argument alone",
               a_native_weave_calls_an_image, ZEN_BOX_CALL_PROBE)

#if defined(LOOM_ENABLE_WASM)
TEST_CASE("a boxed provider's function runs in a store of its own and cannot change its weave" *
          doctest::test_suite("box")) {
    Switchboard sb;
    Kernel kernel(sb, fixture_admission());
    const LoadResult r = kernel.load("adder", ZEN_BOX_CALL_PROBE, "arith", Grant{});
    REQUIRE_MESSAGE(r.ok, r.error);
    REQUIRE(call_from_native(sb, "arith", CallAdd{1, 1}));
    REQUIRE(call_from_native(sb, "arith", CallAdd{2, 2}));
    sb.send_to_role("arith", Message(to_value(CallPeek{1})));
    sb.drain_until_idle();
    // Its writes went to the store made for each call and dropped after it: imposed in a box.
    CHECK(heard(sb, r.id).provided_here == 0);
    CHECK(heard(sb, r.id).knocks == 0);
}
#endif
