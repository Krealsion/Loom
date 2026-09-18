// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_SWITCHBOARD_FIXTURES_HPP
#define ZEN_TESTS_SWITCHBOARD_FIXTURES_HPP

// Dummy message schemas and an adversarial/cooperative probe Weave for the
// Switchboard tests. As always, these domain types live only in the tests; the
// bus and the kernel hard-code none of them.

#include <zen/kernel/admission.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sbfx {

using namespace loom;
using loom::Bus;
using loom::BusEvent;
using loom::EventKind;
using loom::Grant;
using loom::Message;
using loom::RefusalReason;
using loom::Weave;
using loom::WeaveId;
using loom::Switchboard;

// ---- the harness's admission posture, said once ---------------------------
//
// A TEST HARNESS IS A HOST. It holds the Switchboard, so it is inside the authority
// boundary by construction rather than by exemption (zen/host/grant_wiring.hpp says
// so about minting a GrantAuthority; the same sentence is true here). These suites
// test KERNEL MECHANICS — opening, the ABI, roles, reload, replacement — not the
// question of who should be trusted, and every artifact they load is this tree's own
// build output, produced by the same CMake run as the test binary.
//
// So the posture is stated once, here, instead of ninety times at the call sites: this
// host trusts what it loads. That is exactly the authority the Kernel used to mint
// invisibly for every library it could open (P-WORK-18) — the difference, and the whole
// point, is that a HOST now asks for it by name and a reader can grep for who did.
//
// A suite that wants to exercise a REAL policy — a refusal, a narrowed grant, a
// rebuild — installs its own instead. tests/test_admission.cpp does exactly that, and
// nothing here is in its way.
inline loom::AdmissionPolicy fixture_admission() {
    return loom::trust_every_artifact("Loom's own test harness: every artifact it loads "
                                      "is this build tree's output");
}

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

// ---- a flexible probe Weave ----------------------------------------------

// Accepts a configured schema set; records what it handles; runs an optional
// hook so a test can make it reply, flood, or sabotage from inside a handler.
// Its persistable state is a Counter{count}; its policy is the fixed grammar.
class ProbeWeave : public Weave {
public:
    using Hook = std::function<void(const Message&, Bus&, ProbeWeave&)>;

    explicit ProbeWeave(std::vector<std::shared_ptr<const Schema>> accept,
                        std::int64_t max_reloads = 2, bool revive_from_last_good = true)
        : accept_(std::move(accept)), max_reloads_(max_reloads),
          revive_from_last_good_(revive_from_last_good) {}

    // Public configuration / observation for tests.
    Hook on_handle;
    std::vector<std::string> handled_names;
    std::vector<std::int64_t> handled_values; // payload "seq"/"n" if present, else -1
    std::int64_t count = 0;
    int revive_calls = 0;
    /// A retained answer right, for hooks that defer. Move-only, so it lives here
    /// rather than being returned out of the hook.
    loom::DeferredAnswer pending{};
    /// The declared claim-set and emit-set a test wants this raw weave to carry
    /// (SENSE-04, and the schema-admission phase's emit registration). Set before
    /// registration; both are read through the `loom::Weave` virtuals exactly as
    /// the bus reads a WeaveBase's, so a probe can play a declared emitter.
    std::vector<std::shared_ptr<const Schema>> declared_claims;
    std::vector<std::shared_ptr<const Schema>> declared_emits;

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override { return accept_; }
    std::vector<std::shared_ptr<const Schema>> claimed_schemas() const override {
        return declared_claims;
    }
    std::vector<std::shared_ptr<const Schema>> emitted_schemas() const override {
        return declared_emits;
    }

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
        Value v(loom::lifecycle_policy_schema());
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
    WeaveId target;
    std::string schema;
    RefusalReason reason;
    ErrorKind error_kind;
    bool from_last_known_good;
};

inline TapRecord to_record(const BusEvent& e) {
    return TapRecord{e.kind,          e.target, e.schema_name, e.refusal.reason,
                     e.refusal.error.kind, e.from_last_known_good};
}

// Register a ProbeWeave and hand back both its id and a non-owning pointer (the
// bus owns the Weave; tests still want to read/configure the concrete object).
struct Registered {
    WeaveId id;
    ProbeWeave* weave;
};

// The probe is a trusted in-process test fixture; by default it is granted
// permissive send authority so existing routing/lifecycle tests are unaffected by
// capability gating. A capability test passes an explicit, restrictive grant.
inline Registered register_probe(Switchboard& bus,
                                 std::vector<std::shared_ptr<const Schema>> accept,
                                 std::int64_t max_reloads = 2,
                                 bool revive_from_last_good = true,
                                 Grant grant = Grant{}.allow_any()) {
    auto owned = std::make_unique<ProbeWeave>(std::move(accept), max_reloads, revive_from_last_good);
    ProbeWeave* raw = owned.get();
    WeaveId id = bus.register_weave(std::move(owned), std::move(grant));
    return {id, raw};
}

} // namespace sbfx

#endif // ZEN_TESTS_SWITCHBOARD_FIXTURES_HPP
