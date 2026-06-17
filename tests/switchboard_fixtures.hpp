#ifndef ZEN_TESTS_SWITCHBOARD_FIXTURES_HPP
#define ZEN_TESTS_SWITCHBOARD_FIXTURES_HPP

// Dummy message schemas and an adversarial/cooperative probe Shard for the
// Switchboard tests. As always, these domain types live only in the tests; the
// bus and the kernel hard-code none of them.

#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sbfx {

using namespace zen;
using zen::sb::Bus;
using zen::sb::BusEvent;
using zen::sb::EventKind;
using zen::sb::Message;
using zen::sb::RefusalReason;
using zen::sb::Shard;
using zen::sb::ShardId;
using zen::sb::Switchboard;

// ---- message schemas ------------------------------------------------------

inline std::shared_ptr<const Schema> ping_schema() {
    static const auto s = SchemaBuilder("Ping", 1).field("seq", Kind::Int).build();
    return s;
}
inline std::shared_ptr<const Schema> pong_schema() {
    static const auto s = SchemaBuilder("Pong", 1).field("seq", Kind::Int).build();
    return s;
}
inline std::shared_ptr<const Schema> greet_schema() {
    static const auto s = SchemaBuilder("Greet", 1).field("msg", Kind::Text).build();
    return s;
}
inline std::shared_ptr<const Schema> tick_schema() {
    static const auto s = SchemaBuilder("Tick", 1).field("n", Kind::Int).build();
    return s;
}
inline std::shared_ptr<const Schema> counter_schema() {
    static const auto s = SchemaBuilder("Counter", 1).field("count", Kind::Int).build();
    return s;
}

inline Value ping(std::int64_t seq) {
    Value v(ping_schema());
    v.set("seq", Cell::integer(seq));
    return v;
}
inline Value malformed_ping() { return Value(ping_schema()); } // 'seq' deliberately absent
inline Value pong(std::int64_t seq) {
    Value v(pong_schema());
    v.set("seq", Cell::integer(seq));
    return v;
}
inline Value greet(std::string msg) {
    Value v(greet_schema());
    v.set("msg", Cell::text(std::move(msg)));
    return v;
}
inline Value tick(std::int64_t n) {
    Value v(tick_schema());
    v.set("n", Cell::integer(n));
    return v;
}

// ---- a flexible probe Shard ----------------------------------------------

// Accepts a configured schema set; records what it handles; runs an optional
// hook so a test can make it reply, flood, or sabotage from inside a handler.
// Its persistable state is a Counter{count}; its policy is the fixed grammar.
class ProbeShard : public Shard {
public:
    using Hook = std::function<void(const Message&, Bus&, ProbeShard&)>;

    explicit ProbeShard(std::vector<std::shared_ptr<const Schema>> accept,
                        std::int64_t max_reloads = 2, bool revive_from_last_good = true)
        : accept_(std::move(accept)), max_reloads_(max_reloads),
          revive_from_last_good_(revive_from_last_good) {}

    // Public configuration / observation for tests.
    Hook on_handle;
    std::vector<std::string> handled_names;
    std::vector<std::int64_t> handled_values; // payload "seq"/"n" if present, else -1
    std::int64_t count = 0;
    int revive_calls = 0;

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override { return accept_; }

    void handle(const Message& in, Bus& bus) override {
        handled_names.push_back(in.payload.schema().name());
        std::int64_t v = -1;
        if (const Cell* seq = in.payload.get("seq")) {
            v = seq->as_int();
        } else if (const Cell* n = in.payload.get("n")) {
            v = n->as_int();
        }
        handled_values.push_back(v);
        ++count;
        if (on_handle) {
            on_handle(in, bus, *this);
        }
    }

    Value snapshot() const override {
        Value v(counter_schema());
        v.set("count", Cell::integer(count));
        return v;
    }

    Value policy() const override {
        Value v(zen::sb::lifecycle_policy_schema());
        v.set("max_reloads", Cell::integer(max_reloads_));
        v.set("revive_from_last_good", Cell::boolean(revive_from_last_good_));
        return v;
    }

    void revive(const Value& state) override {
        count = state.get("count")->as_int();
        ++revive_calls;
    }

private:
    std::vector<std::shared_ptr<const Schema>> accept_;
    std::int64_t max_reloads_;
    bool revive_from_last_good_;
};

// A compact, copyable record of a bus event (BusEvent carries a payload pointer
// valid only during the callback, so taps copy out the durable fields).
struct TapRecord {
    EventKind kind;
    ShardId target;
    std::string schema;
    RefusalReason reason;
    ErrorKind error_kind;
    bool from_last_known_good;
};

inline TapRecord to_record(const BusEvent& e) {
    return TapRecord{e.kind,          e.target, e.schema_name, e.refusal.reason,
                     e.refusal.error.kind, e.from_last_known_good};
}

// Register a ProbeShard and hand back both its id and a non-owning pointer (the
// bus owns the Shard; tests still want to read/configure the concrete object).
struct Registered {
    ShardId id;
    ProbeShard* shard;
};

inline Registered register_probe(Switchboard& bus,
                                 std::vector<std::shared_ptr<const Schema>> accept,
                                 std::int64_t max_reloads = 2,
                                 bool revive_from_last_good = true) {
    auto owned = std::make_unique<ProbeShard>(std::move(accept), max_reloads, revive_from_last_good);
    ProbeShard* raw = owned.get();
    ShardId id = bus.register_shard(std::move(owned));
    return {id, raw};
}

} // namespace sbfx

#endif // ZEN_TESTS_SWITCHBOARD_FIXTURES_HPP
