// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// zen-bridge-host: a minimal WSL-side host that stands up a bus with one demo weave (a Greeter that
// echoes Greet), listens for remote operators on 127.0.0.1:<port>, and runs the BridgeServer's
// event-driven multiplexer. This is the WSL end of the real Windows->WSL crossing: a Windows console
// (zen-console-remote) or the probe connects across the boundary and drives THIS bus.
//
// Honest containment, stated where it is imposed: the security boundary is the REACHABILITY of this
// socket — a party that can reach 127.0.0.1:<port> holds operator power, exactly as a local operator
// at this host does. Securing that reachability is a DEPLOYMENT responsibility (don't expose it to an
// untrusted network); the bridge does NOT authenticate connectors. Threat tier: abuse, not escape.

#include <zen/bridge/server.hpp>
#include <zen/switchboard.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

using namespace loom;

std::shared_ptr<const loom::Schema> greet_schema() {
    static const auto s = loom::SchemaBuilder("Greet", 1).field("msg", loom::Kind::Text).build();
    return s;
}

// Echoes the `msg` of any Greet back to the sender's reply target.
class Greeter final : public loom::Weave {
public:
    std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const override {
        return {greet_schema()};
    }
    void handle(const loom::Message& in, loom::Bus& bus) override {
        std::printf("Greeter: got %s\n", in.payload.get("msg")->as_text().c_str());
        loom::Value v(greet_schema());
        const loom::Cell* m = in.payload.get("msg");
        v.set("msg", loom::Cell::text(m != nullptr ? m->as_text() : std::string()));
        bus.send(in.reply_to, loom::Message(std::move(v)));
    }
    loom::Value snapshot() const override {
        loom::Value v(state_schema());
        v.set("n", loom::Cell::integer(0));
        return v;
    }
    loom::Value policy() const override {
        loom::Value v(loom::lifecycle_policy_schema());
        v.set("max_reloads", loom::Cell::integer(0));
        v.set("revive_from_last_good", loom::Cell::boolean(true));
        return v;
    }
    void revive(const loom::Value&) override {}

private:
    static std::shared_ptr<const loom::Schema> state_schema() {
        static const auto s =
            loom::SchemaBuilder("GreeterState", 1).field("n", loom::Kind::Int).build();
        return s;
    }
};

} // namespace

int main(int argc, char** argv) {
    std::string err;
    if (!loom::bridge_net_init(&err)) {
        std::fprintf(stderr, "zen-bridge-host: net init failed: %s\n", err.c_str());
        return 1;
    }
    const std::uint16_t want_port =
        argc > 1 ? static_cast<std::uint16_t>(std::atoi(argv[1])) : static_cast<std::uint16_t>(7654);

    loom::Switchboard bus;
    bus.register_weave(std::make_unique<Greeter>(), loom::Grant{}.allow_any());

    const loom::socket_t listener = loom::bridge_listen_tcp(want_port, &err);
    if (listener == loom::kInvalidSocket) {
        std::fprintf(stderr, "zen-bridge-host: listen on 127.0.0.1:%u failed: %s\n", want_port,
                     err.c_str());
        return 1;
    }
    const std::uint16_t port = loom::bridge_socket_port(listener);
    std::printf("zen-bridge-host: listening on 127.0.0.1:%u (one demo weave: Greeter accepting "
                "Greet v1)\n",
                port);
    std::printf("zen-bridge-host: containment — reachability of this socket IS operator authority "
                "(a deployment responsibility; the bridge does not authenticate; abuse-tier)\n");
    std::fflush(stdout);

    loom::BridgeServer server(bus, listener);
    server.run(50); // serve forever (Ctrl-C / kill to stop)
    return 0;
}
