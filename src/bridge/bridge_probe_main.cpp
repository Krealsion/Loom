// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// zen-bridge-probe: a NON-interactive remote operator that proves the operator-protocol end-to-end
// over a real socket — the Windows end of the crossing. It connects (TCP), discovers the bus, fetches
// a shape, composes + gate-sends a message, waits for the echoed reply to buffer, prints PASS, then
// disconnects (graceful). Run it against zen-bridge-host: locally (WSL<->WSL) for the inner loop, and
// from Windows (built via MinGW) against a WSL-hosted zen-bridge-host for the real cross-kernel proof.
//
// Usage: zen-bridge-probe [host=127.0.0.1] [port=7654] [message="hello from the remote operator"]

#include <zen/bridge/remote_console.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace loom;

bool spin_until(RemoteConsole& rc, const std::function<bool()>& done, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        rc.pump();
        if (done()) {
            return true;
        }
        if (rc.disconnected() || std::chrono::steady_clock::now() >= deadline) {
            return done();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const std::uint16_t port =
        argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : static_cast<std::uint16_t>(7654);
    const std::string message = argc > 3 ? argv[3] : "hello from the remote operator";

    std::string err;
    if (!loom::bridge_net_init(&err)) {
        std::fprintf(stderr, "probe: net init failed: %s\n", err.c_str());
        return 1;
    }
    const loom::socket_t sock = loom::bridge_connect_tcp(host, port, &err);
    if (sock == loom::kInvalidSocket) {
        std::fprintf(stderr, "probe: connect %s:%u failed: %s\n", host.c_str(), port, err.c_str());
        return 1;
    }

    loom::RemoteConsole rc(sock);
    if (!rc.connected()) {
        std::fprintf(stderr, "probe: handshake failed (no Welcome)\n");
        return 1;
    }
    std::printf("probe: connected to %s:%u; stamped operator id = %llu\n", host.c_str(), port,
                static_cast<unsigned long long>(rc.operator_id().value));

    // Discovery (a message the host answers): wait for the weave set to arrive.
    if (!spin_until(rc, [&] { return !rc.weaves().empty(); }, 3000)) {
        std::fprintf(stderr, "probe: FAIL — discovered no weaves\n");
        return 2;
    }
    const std::vector<loom::WeaveInfo> weaves = rc.weaves();
    std::printf("probe: discovered %zu weave(s)\n", weaves.size());
    if (weaves[0].accepts.empty()) {
        std::fprintf(stderr, "probe: FAIL — the target accepts no shapes\n");
        return 3;
    }
    const loom::WeaveId target = weaves[0].id;
    const std::string shape = weaves[0].accepts[0].name;
    const std::uint32_t version = weaves[0].accepts[0].version;

    // Describe (a message the host answers with the encoded schema): learn the field to fill.
    const std::optional<loom::ShapeDesc> desc = rc.describe(shape, version);
    if (!desc.has_value() || desc->fields.empty()) {
        std::fprintf(stderr, "probe: FAIL — could not describe %s v%u\n", shape.c_str(), version);
        return 4;
    }
    const std::string field = desc->fields[0].name;
    std::printf("probe: target weave %llu accepts %s v%u (field '%s')\n",
                static_cast<unsigned long long>(target.value), shape.c_str(), version, field.c_str());

    // Compose + gate-send (the ladder runs HERE; the assembled message crosses as a Send the host
    // re-admits + stamps). The send's fate returns as bus events (the tap + the buffered reply).
    loom::Arg arg;
    arg.name = field;
    arg.value = loom::FieldValue{message};
    const loom::Composed c = rc.compose(target, shape, version, {arg});
    if (c.status != loom::Composed::Status::Ready) {
        std::fprintf(stderr, "probe: FAIL — compose was not Ready (%s)\n",
                     c.status == loom::Composed::Status::Error ? c.error.c_str() : "NeedsInput");
        return 5;
    }

    // The reply buffers as m1 once the round-trip closes over the bus.
    if (!spin_until(rc, [&] { return rc.buffer_size() >= 1; }, 3000)) {
        std::fprintf(stderr, "probe: FAIL — no reply buffered\n");
        return 6;
    }
    const std::optional<loom::BufferEntry> m1 = rc.buffer_at(1);
    std::string echoed;
    if (m1.has_value()) {
        const loom::Cell* mc = m1->value.get(field);
        if (mc != nullptr) {
            echoed = mc->as_text();
        }
    }
    std::printf("probe: reply m1 = %s v%u { %s = \"%s\" }\n", m1->name.c_str(), m1->version,
                field.c_str(), echoed.c_str());

    // The tap streamed the bus events too.
    std::printf("probe: tap saw %zu event(s)\n", rc.tap().size());

    std::printf("probe: PASS — discovery + describe + gate-send + reply + tap crossed the wire; "
                "disconnecting.\n");
    return 0; // rc's destructor closes the socket: a graceful disconnect the host reaps as an event
}
