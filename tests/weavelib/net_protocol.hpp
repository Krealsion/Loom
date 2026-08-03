// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_NET_PROTOCOL_HPP
#define ZEN_TESTS_NET_PROTOCOL_HPP

// The NetworkBroker wire protocol (role "net") + the test-control shape that drives a
// net-client mod. The network analog of storage's key-value: a request-response. `host`
// is an IPv4 string — raw TCP, no DNS, no dependency. Shared by the broker .so, the
// client .so, and the policy test (same content-id across the .so boundary, per the gate).

#include <zen/weave/shape.hpp>
#include <zen/value.hpp> // loom::Bytes

#include <cstdint>
#include <string>

namespace net {

struct NetRequest {
    std::string host; // IPv4 dotted-quad (no DNS)
    std::int64_t port;
    loom::Bytes payload;
    ZEN_SHAPE(NetRequest, 1, ZEN_FIELD(host), ZEN_FIELD(port), ZEN_FIELD(payload));
};
struct NetResponse {
    bool ok; // false = refused by the broker's allow-list, or the connection failed
    loom::Bytes data;
    ZEN_SHAPE(NetResponse, 1, ZEN_FIELD(ok), ZEN_FIELD(data));
};

// Test-control: the test points the net-client mod at a destination.
struct DoNet {
    std::string host;
    std::int64_t port;
    ZEN_SHAPE(DoNet, 1, ZEN_FIELD(host), ZEN_FIELD(port));
};

} // namespace net

#endif // ZEN_TESTS_NET_PROTOCOL_HPP
