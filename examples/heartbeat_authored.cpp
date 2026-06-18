// The heartbeat, rewritten on the authoring layer. Compare with heartbeat.cpp:
// Ping/Pong/Counter are plain structs declared once, the Shards are typed
// handlers with derived snapshot/revive, and each is mounted in one call. No
// stringly-typed set(), no hand-built schema, no hand-written snapshot/revive.

#include <zen/author.hpp>
#include <zen/switchboard.hpp>

#include <cstdint>
#include <iostream>
#include <string>

using namespace zen;
using namespace zen::sb;
namespace au = zen::author;

namespace {

struct Ping {
    std::int64_t seq;
    ZEN_SHAPE(Ping, 1, ZEN_FIELD(seq));
};
struct Pong {
    std::int64_t seq;
    ZEN_SHAPE(Pong, 1, ZEN_FIELD(seq));
};
struct Count {
    std::int64_t handled;
    ZEN_SHAPE(Count, 1, ZEN_FIELD(handled));
};

class Responder : public au::ShardBase<Responder, Count, au::Accept<Ping>, au::Emit<Pong>> {
public:
    void on(const Ping& p, au::Mail& mail) {
        ++state_.handled;
        std::cout << "  responder handled Ping " << p.seq << "\n";
        mail.reply(Pong{p.seq}); // echo the seq back as a Pong
    }
};

class Collector : public au::ShardBase<Collector, Count, au::Accept<Pong>> {
public:
    void on(const Pong& p, au::Mail&) {
        ++state_.handled;
        std::cout << "  collector handled Pong " << p.seq << "\n";
    }
};

} // namespace

int main() {
    Switchboard bus;
    bus.add_observer([](const BusEvent& e) {
        const char* kind = e.kind == EventKind::Delivered ? "delivered"
                           : e.kind == EventKind::Refused ? "refused"
                           : e.kind == EventKind::Died     ? "died"
                                                           : "revived";
        std::cout << "    [tap] " << kind << " " << e.schema_name << "\n";
    });

    ShardId responder = au::mount<Responder>(bus);
    ShardId collector = au::mount<Collector>(bus);

    std::cout << "directed Ping -> responder (reply_to collector):\n";
    bus.send(responder, Message(au::to_value(Ping{7}), ShardId{}, collector));
    bus.pump();

    std::cout << "responder dies and revives from its own (derived) snapshot:\n";
    std::string saved = bus.snapshot_bytes(responder);
    bus.kill(responder);
    ReviveOutcome ro = bus.reload(responder, saved);
    std::cout << "  revived=" << std::boolalpha << ro.revived << "\n";
    return 0;
}
