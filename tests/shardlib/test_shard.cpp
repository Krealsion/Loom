// A real Shard, authored as a clean C++ zen::sb::Shard subclass and shipped as a
// .so with a single ZEN_EXPORT_SHARD line. No senses, no std::any — the same
// Shard one would compile in. Compile-time switches produce adversarial variants
// for the kernel's harness:
//   ZEN_SHARD_MALFORMED_SNAPSHOT  — emit a snapshot missing a required field
//   ZEN_SHARD_MALFORMED_MESSAGE   — emit a message missing a required field
//   ZEN_SHARD_STATE_V2            — bump the state schema version (reload mismatch)

#include <zen/kernel/export.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <memory>
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
#ifdef ZEN_SHARD_STATE_V2
    static const auto s = SchemaBuilder("Counter", 2)
                              .field("count", Kind::Int)
                              .field("note", Kind::Text, /*required=*/false)
                              .build();
#else
    static const auto s = SchemaBuilder("Counter", 1).field("count", Kind::Int).build();
#endif
    return s;
}

// Accepts Ping, replies Pong, and counts what it has handled as its state.
class TestShard : public Shard {
public:
    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return {ping_schema()};
    }

    void handle(const Message& in, Bus& bus) override {
        const std::int64_t seq = in.payload.get("seq")->as_int();
        ++count_;
#ifdef ZEN_SHARD_MALFORMED_MESSAGE
        (void)seq;
        bus.send(in.reply_to, Message(Value(pong_schema()))); // 'seq' deliberately absent
#else
        Value pong(pong_schema());
        pong.set("seq", Cell::integer(seq));
        bus.send(in.reply_to, Message(std::move(pong)));
#endif
    }

    Value snapshot() const override {
        Value v(counter_schema());
#ifndef ZEN_SHARD_MALFORMED_SNAPSHOT
        v.set("count", Cell::integer(count_));
#endif
        return v;
    }

    Value policy() const override {
        Value v(lifecycle_policy_schema());
        v.set("max_reloads", Cell::integer(8));
        v.set("revive_from_last_good", Cell::boolean(true));
        return v;
    }

    void revive(const Value& state) override { count_ = state.get("count")->as_int(); }

private:
    std::int64_t count_ = 0;
};

} // namespace

ZEN_EXPORT_SHARD(TestShard)
