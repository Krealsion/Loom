// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE STRANGER'S CROSSING -- a host outside Loom's build tree that serves a listener under an
// admission policy of its own, and a client outside it that connects, is refused, then admitted
// and answered, reaching both halves only through `find_package(loom)`.
//
// The two-host crossing exported `loom::bridge` on the strength of a second HOST -- a Loom host
// that links to a running Workshop -- and this closes the same debt the history witness closed
// for the history pair: the build tree can satisfy a target the export set never published, and
// the difference only shows up in somebody else's project. It links `loom::bridge` and nothing
// that is not exported: no console, no UI, no remote console. If the admission seam, the client
// or the link envelope stop being reachable that way, this fails to configure or to compile.

#include <zen/bridge/client.hpp>
#include <zen/bridge/link.hpp>
#include <zen/bridge/server.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/standard_shapes.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void ok(bool condition, const char* what) {
    std::printf("  %-5s %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

struct Echo {
    std::string msg;
    ZEN_SHAPE(Echo, 1, ZEN_FIELD(msg));
};
struct Tally {
    std::int64_t heard;
    ZEN_SHAPE(Tally, 1, ZEN_FIELD(heard));
};

/// An ordinary participant that ANSWERS -- `mail.answer`, so the crossing carries Loom's word.
class Answerer final
    : public loom::WeaveBase<Answerer, Tally, loom::Accept<Echo>, loom::Emit<loom::Result>> {
public:
    void on(const Echo& e, loom::Mail& mail) {
        ++state_.heard;
        (void)mail.answer(loom::Result{"stranger says " + e.msg});
    }
};

bool until(const std::function<bool()>& done, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return done();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

int main() {
    std::printf("stranger bridge witness (the two-host crossing, through the package)\n");
    std::string err;
    ok(loom::bridge_net_init(&err), "a stranger can initialise the crossing's network");

    loom::Switchboard bus;
    const loom::WeaveId answerer =
        bus.register_weave(std::make_unique<Answerer>(), loom::Grant{}.allow_any(), "echo");
    (void)answerer;
    const loom::socket_t listener = loom::bridge_listen_tcp(0, &err);
    ok(listener != loom::kInvalidSocket, "a stranger can open a loopback listener");
    const std::uint16_t port = loom::bridge_socket_port(listener);

    // THE POLICY IS THE STRANGER'S: one credential admits, as a name the stranger chose, with
    // one shape to one office and no observation. Everything else is refused in words.
    loom::BridgeServer server(bus, listener, [](const loom::ConnectionRequest& r) {
        if (r.credential != "let-me-in") {
            return loom::ConnectionVerdict::refuse("the stranger admits one credential");
        }
        loom::ConnectionAdmitted a;
        a.grant.allow_to_role(Echo::zen_name, Echo::zen_version, "echo");
        a.established_name = "guest-of-" + r.claimed_name;
        return loom::ConnectionVerdict::admit(std::move(a));
    });

    // 1. REFUSED, in words, and nothing of it remains.
    {
        const loom::socket_t s = loom::bridge_connect_tcp("127.0.0.1", port, &err);
        ok(s != loom::kInvalidSocket, "a stranger's client can connect");
        loom::BridgeClient client(s);
        ok(client.hello("agent", "wrong"), "...and say hello");
        std::vector<loom::BridgeEvent> events;
        (void)until(
            [&] {
                server.step();
                client.poll(events);
                return client.denied();
            },
            3000);
        ok(client.denied() && client.denial() == "the stranger admits one credential",
           "a wrong credential is refused with the policy's own sentence");
        (void)until(
            [&] {
                server.step();
                return server.connection_count() == 0;
            },
            3000);
        ok(server.connection_count() == 0 && server.refused_count() == 1,
           "a refused connection leaves no session behind");
    }

    // 2. ADMITTED under the policy's name and grant; asked; answered with Loom's attestation.
    {
        const loom::socket_t s = loom::bridge_connect_tcp("127.0.0.1", port, &err);
        loom::BridgeClient client(s);
        ok(client.hello("agent", "let-me-in"), "a right credential says hello");
        std::vector<loom::BridgeEvent> events;
        (void)until(
            [&] {
                server.step();
                client.poll(events);
                return client.admitted();
            },
            3000);
        ok(client.admitted(), "...and is admitted");
        ok(client.established_name() == "guest-of-agent",
           "the established name is the policy's word, not the claim");
        client.send_to_role("echo", 9, loom::serialize(loom::to_value(Echo{"hi"})));
        const loom::BridgeEvent* delivered = nullptr;
        (void)until(
            [&] {
                server.step();
                client.poll(events);
                for (const loom::BridgeEvent& e : events) {
                    if (e.kind == loom::BridgeEvent::Kind::Delivered) {
                        delivered = &e;
                    }
                }
                return delivered != nullptr;
            },
            3000);
        ok(delivered != nullptr, "an office-addressed ask is answered across the crossing");
        if (delivered != nullptr) {
            ok(delivered->correlation == 9 && delivered->answers_ask,
               "...under its correlation, with Loom's attestation that it IS the answer");
            ok(loom::parse(delivered->payload).claimed_name() == loom::Result::zen_name,
               "...carrying the answerer's own shape");
        }
        // The link envelope is reachable too: a stranger's weave can compose one.
        const loom::link::Ask ask = loom::link::ask_role("echo", Echo{"across"});
        ok(ask.role == "echo" && !ask.payload.empty(), "the link envelope composes from a shape");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
