// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE REAL KERNEL PATH, DRIVEN BY A STRANGER.
//
// Not a custom test loader: loom::Kernel is the actual mechanism, so what is proven
// here is the mechanism a host really uses -- LoadLibrary/dlopen, the "zen_weave_abi"
// lookup by exact name, the descriptor handshake, a live delivery in BOTH directions
// across the C ABI, and unload.
//
// The reply is what makes this more than a load test. A weave that loads but whose
// outbound host callbacks are broken would still pass "it loaded"; only a Pong
// arriving back at a native collector proves the seam carries traffic both ways.

#include "witness_protocol.hpp"

#include <zen/gate.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/registry.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/terminal/composer.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %-6s %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) {
        ++failures;
    }
}

// A native collector: the reply has to land somewhere real.
struct Seen {
    std::int64_t count;
    std::int64_t last_seq;
    ZEN_SHAPE(Seen, 1, ZEN_FIELD(count), ZEN_FIELD(last_seq));
};

class Collector : public loom::WeaveBase<Collector, Seen, loom::Accept<witness::Pong>> {
public:
    void on(const witness::Pong& p, loom::Mail&) {
        ++state_.count;
        state_.last_seq = p.seq;
    }
};

// ---- the discoverer: a stranger asking a stranger what it accepts ------------
//
// It holds a WeaveId and an ordinary grant. It never sees the Switchboard, the
// Registry, the Kernel or a tap, and it never compiled against Ping, Nested or
// Inner -- everything it ends up knowing about them arrives in one reply.

struct Nudge {
    ZEN_SHAPE(Nudge, 1);
};

class Discoverer final : public loom::Weave {
public:
    std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const override {
        return {loom::schema_of<Nudge>(), loom::accepted_shapes_schema()};
    }
    // A participant's send is one made from INSIDE its own delivery: a host
    // calling bus.send() is a root send and takes no grant check at all, so an
    // authority claim driven that way would prove nothing. The nudge is how this
    // program hands the weave a turn of its own.
    void handle(const loom::Message& in, loom::Bus& bus) override {
        if (in.payload.schema().name() == "Nudge") {
            if (errand_) {
                errand_(bus);
            }
            return;
        }
        answer_ = in.payload;
    }
    loom::Value snapshot() const override { return loom::Value(loom::schema_of<Nudge>()); }
    loom::Value policy() const override {
        loom::Value v(loom::lifecycle_policy_schema());
        v.set("max_reloads", loom::Cell::integer(4));
        v.set("revive_from_last_good", loom::Cell::boolean(true));
        return v;
    }
    void revive(const loom::Value&) override {}

    void set_self(loom::WeaveId id) { self_ = id; }
    bool answered() const { return answer_.has_value(); }
    const loom::Value& answer() const { return *answer_; }

    void ask(loom::Switchboard& bus, loom::WeaveId target) {
        run(bus, [this, target](loom::Bus& b) {
            b.send(target,
                   loom::Message(loom::to_value(loom::DescribeAccepted{}), self_, self_, 1));
        });
    }
    void send_learned(loom::Switchboard& bus, loom::WeaveId target, loom::Value payload) {
        run(bus, [this, target, payload](loom::Bus& b) {
            b.send(target, loom::Message(payload, self_, self_, 2));
        });
    }

private:
    void run(loom::Switchboard& bus, std::function<void(loom::Bus&)> errand) {
        errand_ = std::move(errand);
        bus.send(self_, loom::Message(loom::to_value(Nudge{})));
        bus.pump();
        errand_ = nullptr;
    }
    std::function<void(loom::Bus&)> errand_;
    std::optional<loom::Value> answer_;
    loom::WeaveId self_{};
};

// The composer's two lookups over a vocabulary LEARNED from a reply.
class LearnedVocabulary final : public loom::ComposeSource {
public:
    explicit LearnedVocabulary(const loom::Registry& deps) : deps_(deps) {}
    std::shared_ptr<const loom::Schema> resolve_schema(std::string_view name,
                                                       std::uint32_t version) const override {
        return deps_.lookup(name, version);
    }
    std::optional<loom::Cell> resolve_ref(const loom::Ref&, std::string* error) const override {
        if (error != nullptr) {
            *error = "this consumer holds no received messages to reference";
        }
        return std::nullopt;
    }

private:
    const loom::Registry& deps_;
};

std::set<std::string> wire_identities(const loom::Value& answer, const char* section) {
    std::set<std::string> out;
    const loom::Cell* list = answer.get(section);
    if (list == nullptr) {
        return out;
    }
    for (const loom::Cell& c : list->as_list()) {
        const loom::Value& d = *c.as_message();
        out.insert(d.get("name")->as_text() + " v" + std::to_string(d.get("version")->as_int()));
    }
    return out;
}

// Read a live weave's state back through the public path -- snapshot bytes, then the
// same gate every boundary uses. No back channel, and no raw pointer held across a
// bus that owns its participants.
template <class Shape>
Shape state_of(loom::Switchboard& bus, loom::WeaveId id, bool& ok) {
    const std::string bytes = bus.snapshot_bytes(id);
    loom::Unverified u = loom::parse(bytes);
    loom::Admission a = loom::admit(u, loom::schema_of<Shape>());
    ok = a.ok();
    if (!ok) {
        return Shape{};
    }
    return loom::from_value<Shape>(std::move(a).value());
}

} // namespace

int main() {
    std::puts("package witness: real Kernel load/exercise/unload");
    std::printf("  containment: %s\n", loom::Kernel::containment_note());

    loom::Switchboard bus;
    loom::Kernel kernel(bus);

    const loom::WeaveId collector_id = loom::mount<Collector>(bus);

    // ---- load, through the real Kernel ------------------------------------------
    const loom::LoadResult loaded = kernel.load("witness", ZEN_WITNESS_WEAVE);
    check(loaded.ok, "Kernel::load found and accepted the artifact" +
                         (loaded.ok ? std::string() : " -- " + loaded.error));
    if (!loaded.ok) {
        std::puts("package witness: kernel path FAILED");
        return 1;
    }

    // ---- a live delivery, and a reply back across the seam ------------------------
    bus.send(loaded.id,
             loom::Message(loom::to_value(witness::Ping{41}), loom::WeaveId{}, collector_id));
    bus.pump();

    bool seen_ok = false;
    const Seen seen = state_of<Seen>(bus, collector_id, seen_ok);
    check(seen_ok, "the collector's state passes the same gate every boundary uses");
    check(seen.count == 1, "the loaded weave handled a Ping and its reply reached a native weave");
    check(seen.last_seq == 41, "the Pong carried the payload the stranger's weave sent (seq 41)");

    // ---- its own state crossed the ABI too ---------------------------------------
    bool tally_ok = false;
    const witness::Tally tally = state_of<witness::Tally>(bus, loaded.id, tally_ok);
    check(tally_ok, "the loaded weave produced a gate-passing snapshot across the seam");
    check(tally.handled == 1, "the weave's own tally says handled == 1");
    check(tally.raw_total == 41,
          "a ZEN_HIDE field still round-trips as state (hiding governs access, not the wire)");
    check(tally.label == "stranger", "a std::string field crossed the C ABI intact");

    // ---- SELF-DESCRIPTION, asked of a real dynamically-loaded artifact -----------
    //
    // Everything from here runs through installed, participant-facing Loom only:
    // no Switchboard handed to the asker, no Registry from the host, no console,
    // no bridge, no tap. The host observes the outcome from outside, as a host.
    {
        auto d = std::make_unique<Discoverer>();
        Discoverer* discoverer = d.get();
        loom::Grant g; // ordinary and narrow: it may ASK, and nothing else
        g.allow_to_any("zen.DescribeAccepted", 1);
        const loom::WeaveId did = bus.register_weave(std::move(d), std::move(g));
        discoverer->set_self(did);

        discoverer->ask(bus, loaded.id);
        check(discoverer->answered(),
              "a stranger asked a loaded weave what it accepts, by ordinary message");
        if (discoverer->answered()) {
            const std::set<std::string> roots = wire_identities(discoverer->answer(), "accepted");
            const std::set<std::string> deps = wire_identities(discoverer->answer(), "referenced");
            check(roots.count("Ping v1") == 1, "the answer names the artifact's own accepted shape");
            check(roots.count("Nested v1") == 1, "...and its nested accepted shape");
            check(roots.count("zen.PokeDescribe v1") == 1,
                  "...and the substrate doors, truthfully, unfiltered");
            check(roots.count("zen.DescribeAccepted v1") == 1,
                  "...including the question that was asked");
            check(roots.count("Pong v1") == 0,
                  "an EMITTED shape is not an accepted one, and is absent");
            check(deps.count("Inner v1") == 1,
                  "the DEPENDENCY closure carries Inner, which the artifact does not accept");
            check(roots.count("Inner v1") == 0,
                  "...and Inner is not offered as a root: a dependency is not sendable");

            // Rebuild the vocabulary. This program never compiled against any of
            // it -- witness_protocol.hpp is included here only to CHECK the
            // result, never to produce it.
            loom::Registry learned;
            loom::decode_accepted_referenced(discoverer->answer(), learned);
            const std::vector<std::shared_ptr<const loom::Schema>> rebuilt =
                loom::decode_accepted_roots(discoverer->answer(), learned);
            for (const auto& r : rebuilt) {
                learned.register_schema(r);
            }
            const auto nested = learned.lookup("Nested", 1);
            check(nested != nullptr,
                  "a stranger reconstructed a NESTED accepted root from the reply alone");
            const bool structure =
                nested != nullptr && nested->fields().size() == 2 &&
                nested->fields()[0].type.kind == loom::Kind::Message &&
                nested->fields()[0].type.message != nullptr &&
                nested->fields()[0].type.message->name() == "Inner" &&
                nested->fields()[1].type.kind == loom::Kind::List &&
                nested->fields()[1].type.element != nullptr &&
                nested->fields()[1].type.element->kind == loom::Kind::Message;
            check(structure, "Message(Inner) and List<Message(Inner)> both survived the wire");
            check(nested != nullptr &&
                      nested->content_id() == loom::schema_of<witness::Nested>()->content_id(),
                  "the reconstruction is EXACT: the same content identity, not merely similar");

            // The MSG-0 handoff, end to end: describe -> compose -> assemble.
            LearnedVocabulary vocab(learned);
            const loom::ShapeDesc desc = loom::describe_schema(*learned.lookup("Ping", 1));
            check(desc.fields.size() == 1 && desc.fields[0].name == "seq" &&
                      desc.fields[0].type == "Int",
                  "a form can be generated from the learned schema (Ping{seq:Int})");
            const std::vector<loom::Arg> args{
                loom::Arg{std::string("seq"), loom::FieldValue{std::int64_t{99}}}};
            loom::Composition c = loom::compose_message(vocab, "Ping", 1, args);
            check(c.status == loom::Composition::Status::Ready,
                  "compose_message built a Ping from a schema this program never compiled");

            // KNOWING IS NOT PERMISSION. The grant above allows the question and
            // nothing else, so the composed message is refused at the gate --
            // read off the target's own state rather than asserted.
            if (c.status == loom::Composition::Status::Ready) {
                bool before_ok = false;
                bool after_ok = false;
                const witness::Tally before = state_of<witness::Tally>(bus, loaded.id, before_ok);
                discoverer->send_learned(bus, loaded.id, loom::assemble(c));
                const witness::Tally after = state_of<witness::Tally>(bus, loaded.id, after_ok);
                check(before_ok && after_ok && before.handled == after.handled,
                      "a DISCOVERED shape is still refused to an asker not granted to send it");
            }
        }
    }

    // ---- unload ------------------------------------------------------------------
    check(kernel.unload("witness"), "Kernel::unload released the artifact");
    check(!kernel.unload("witness"), "a second unload is honestly refused (nothing left to release)");

    if (failures != 0) {
        std::printf("package witness: kernel path FAILED (%d)\n", failures);
        return 1;
    }
    std::puts("package witness: kernel path PASSED");
    return 0;
}
