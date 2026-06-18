#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/author.hpp>
#include <zen/gate.hpp>
#include <zen/kernel/control.hpp>
#include <zen/kernel/kernel.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace zen;
using namespace zen::sb;
using namespace sbfx;
namespace au = zen::author;

namespace {

bool tap_has_denied(const std::vector<TapRecord>& tap, ShardId target, const std::string& shape) {
    for (const TapRecord& r : tap) {
        if (r.kind == EventKind::Refused && r.reason == RefusalReason::CapabilityDenied &&
            r.target == target && r.schema == shape) {
            return true;
        }
    }
    return false;
}

// A Shard that, when poked with a Go, asks the kernel to load a library by
// message. Whether it succeeds depends entirely on its grant.
struct Go {
    std::int64_t n;
    ZEN_SHAPE(Go, 1, ZEN_FIELD(n));
};
struct LoaderState {
    std::int64_t n;
    ZEN_SHAPE(LoaderState, 1, ZEN_FIELD(n));
};
class Loader : public au::ShardBase<Loader, LoaderState, au::Accept<Go>,
                                    au::Emit<zen::kernel::LoadLibrary>> {
public:
    ShardId control{};
    std::string lib_name;
    std::string lib_path;
    void on(const Go&, au::Mail& mail) {
        mail.send(control, zen::kernel::LoadLibrary{lib_name, lib_path});
    }
};

} // namespace

TEST_SUITE("capabilities") {

TEST_CASE("a send the grant does not permit is denied before the gate, and observed") {
    Switchboard bus;
    Registered recorder = register_probe(bus, {pong_schema()});
    // The sender accepts Ping but is granted NOTHING; its handler tries to send Pong.
    Registered sender = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    sender.shard->on_handle = [rid = recorder.id](const Message&, Bus& bus_, ProbeShard&) {
        bus_.send(rid, Message(pong(99)));
    };

    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    const auto before = gate_invocations();
    bus.send(sender.id, Message(ping(1))); // host-injected root trigger
    bus.pump();
    const auto after = gate_invocations();

    // Only the trigger (Ping -> sender) was gated; the denied Pong never reached
    // the gate.
    CHECK(after == before + 1);
    CHECK(recorder.shard->handled_names.empty()); // Pong never delivered
    CHECK(tap_has_denied(tap, recorder.id, "Pong"));
}

TEST_CASE("an authorized send still goes through the one gate, and is delivered") {
    Switchboard bus;
    Registered recorder = register_probe(bus, {pong_schema()});
    Grant g;
    g.allow("Pong", 1, recorder.id); // exactly: may send Pong to the recorder
    Registered sender = register_probe(bus, {ping_schema()}, 2, true, g);
    sender.shard->on_handle = [rid = recorder.id](const Message&, Bus& bus_, ProbeShard&) {
        bus_.send(rid, Message(pong(7)));
    };

    const auto before = gate_invocations();
    bus.send(sender.id, Message(ping(1)));
    bus.pump();
    const auto after = gate_invocations();

    CHECK(after == before + 2); // Ping -> sender, then the authorized Pong -> recorder
    REQUIRE(recorder.shard->handled_names.size() == 1);
    CHECK(recorder.shard->handled_values[0] == 7);
}

TEST_CASE("publish only reaches the accepters the sender is permitted to send to") {
    Switchboard bus;
    Registered a = register_probe(bus, {pong_schema()});
    Registered b = register_probe(bus, {pong_schema()});
    // The publisher may send Pong only to `a`, though both accept it.
    Grant g;
    g.allow("Pong", 1, a.id);
    Registered pub = register_probe(bus, {ping_schema()}, 2, true, g);
    pub.shard->on_handle = [](const Message&, Bus& bus_, ProbeShard&) {
        bus_.publish(Message(pong(5)));
    };

    bus.send(pub.id, Message(ping(1)));
    bus.pump();

    CHECK(a.shard->handled_names.size() == 1); // permitted
    CHECK(b.shard->handled_names.empty());     // denied at delivery
}

TEST_CASE("the kernel door is driven by message only with the load capability") {
    using namespace zen::kernel;
    Switchboard bus;
    Kernel kernel(bus);
    ShardId control = mount_control(kernel, bus);

    // Holding the load capability, a loader drives the kernel by message.
    ShardId with_cap = au::mount_granted<Loader>(bus, load_capability(control));
    auto* lw = static_cast<Loader*>(bus.shard(with_cap));
    lw->control = control;
    lw->lib_name = "viamsg";
    lw->lib_path = ZEN_SO_SHARD;
    bus.send(with_cap, Message(au::to_value(Go{1}))); // host-injected root trigger
    bus.pump();
    CHECK(kernel.is_loaded("viamsg"));

    // Without it, the same send is denied at the control Shard's door; the kernel
    // never loads.
    ShardId no_cap = au::mount_granted<Loader>(bus, Grant::nothing());
    auto* ln = static_cast<Loader*>(bus.shard(no_cap));
    ln->control = control;
    ln->lib_name = "denied";
    ln->lib_path = ZEN_SO_SHARD;

    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });
    bus.send(no_cap, Message(au::to_value(Go{2})));
    bus.pump();

    CHECK_FALSE(kernel.is_loaded("denied"));
    CHECK(tap_has_denied(tap, control, "LoadLibrary"));
}

} // TEST_SUITE
