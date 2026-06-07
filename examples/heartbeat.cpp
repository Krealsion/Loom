// The kernel is alive: two Shards on the Switchboard exchange a gated message
// and a reply, a publish reaches only its accepters, a malformed message is
// refused at the boundary, and a Shard dies and revives through native bytes —
// all witnessed by an observer. Public API only.

#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace zen;
using namespace zen::sb;

namespace {

std::shared_ptr<const Schema> ping_schema() {
    static const auto s = SchemaBuilder("Ping", 1).field("seq", Kind::Int).build();
    return s;
}
std::shared_ptr<const Schema> pong_schema() {
    static const auto s = SchemaBuilder("Pong", 1).field("seq", Kind::Int).build();
    return s;
}
std::shared_ptr<const Schema> counter_schema() {
    static const auto s = SchemaBuilder("Counter", 1).field("count", Kind::Int).build();
    return s;
}

Value ping_value(std::int64_t seq) {
    Value v(ping_schema());
    v.set("seq", Cell::integer(seq));
    return v;
}

// A tiny Shard: counts what it handles; optionally replies with a Pong.
class Node : public Shard {
public:
    explicit Node(std::string name, std::vector<std::shared_ptr<const Schema>> accept, bool reply)
        : name_(std::move(name)), accept_(std::move(accept)), reply_(reply) {}

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override { return accept_; }

    void handle(const Message& in, Switchboard& bus) override {
        ++count_;
        std::cout << "  " << name_ << " handled " << in.payload.schema().name() << "\n";
        if (reply_ && in.reply_to.valid()) {
            Value pong(pong_schema());
            pong.set("seq", *in.payload.get("seq")); // echo the seq back
            bus.send(in.reply_to, Message(std::move(pong)));
        }
    }

    Value snapshot() const override {
        Value v(counter_schema());
        v.set("count", Cell::integer(count_));
        return v;
    }
    Value policy() const override {
        Value v(lifecycle_policy_schema());
        v.set("max_reloads", Cell::integer(2));
        v.set("revive_from_last_good", Cell::boolean(true));
        return v;
    }
    void revive(const Value& state) override { count_ = state.get("count")->as_int(); }

private:
    std::string name_;
    std::vector<std::shared_ptr<const Schema>> accept_;
    bool reply_;
    std::int64_t count_ = 0;
};

} // namespace

int main() {
    Switchboard bus;
    bus.add_observer([](const BusEvent& e) {
        const char* kind = e.kind == EventKind::Delivered  ? "delivered"
                           : e.kind == EventKind::Refused  ? "refused"
                           : e.kind == EventKind::Died      ? "died"
                                                            : "revived";
        std::cout << "    [tap] " << kind << " " << e.schema_name;
        if (e.kind == EventKind::Refused) {
            std::cout << " (" << e.refusal.message() << ")";
        }
        std::cout << "\n";
    });

    ShardId responder = bus.register_shard(
        std::make_unique<Node>("responder", std::vector{ping_schema()}, /*reply=*/true));
    ShardId collector = bus.register_shard(
        std::make_unique<Node>("collector", std::vector{pong_schema()}, /*reply=*/false));

    std::cout << "directed Ping -> responder (reply_to collector):\n";
    bus.send(responder, Message(ping_value(7), ShardId{}, collector));
    bus.pump();

    std::cout << "a malformed Ping is refused at the door:\n";
    bus.send(responder, Message(Value(ping_schema()))); // 'seq' missing
    bus.pump();

    std::cout << "responder dies and revives from its own snapshot:\n";
    std::string saved = bus.snapshot_bytes(responder);
    bus.kill(responder);
    ReviveOutcome ro = bus.reload(responder, saved);
    std::cout << "  revived=" << std::boolalpha << ro.revived << "\n";

    return 0;
}
