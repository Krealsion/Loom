// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_STANDARD_SHAPES_HPP
#define ZEN_WEAVE_STANDARD_SHAPES_HPP

// The standard reply shapes: one vocabulary for the answers every protocol sends. A protocol
// uses these for contentless and simple replies, and keeps a bespoke reply only where it
// carries structure whose absence would leave the reader confused rather than less informed.
//
//   zen.Ack      no fields: "done". The reply's correlation says what was done.
//   zen.Refused  one field: "no, and here is why", self-contained.
//   zen.Result   the payload, as text: "here is what you asked for".
//
// Not Error or Value: loom::Error is the gate's admission fault and loom::Value the value type,
// while a zen.Refused is a deliberate answer by policy. These are ordinary registered, gated
// shapes, registered by hand so their wire names carry the "zen." prefix (a maker's own `Ack`
// is "Ack").
//
// Any granted participant can send these, so a weave that accepts one matches each arrival
// against its own outstanding requests, by correlation and by bus-stamped sender, and treats an
// unsolicited reply as data at best, on every reply shape it accepts. Loom ships that check
// twice: loom::AskBook (ask_book.hpp) for the participant that asked, and loom::relay
// (relay.hpp) for a middleman relaying somebody else's answer. A producer declares the reply
// shapes it sends in Emit<...> like any other.

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>
#include <tuple>

namespace loom {

/// Contentless success: "done." The correlation carries what was done.
struct Ack {
    using ZenSelf = Ack;
    static constexpr const char* zen_name = "zen.Ack";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// A refusal with its reason: "no, and here is why". An answer, never silence; the reason is
/// written for a stranger.
struct Refused {
    std::string reason;
    using ZenSelf = Refused;
    static constexpr const char* zen_name = "zen.Refused";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(reason)); }
};

/// A result carrying its payload as text: "here is what you asked for". A reply whose payload is
/// not text stays bespoke.
struct Result {
    std::string value;
    using ZenSelf = Result;
    static constexpr const char* zen_name = "zen.Result";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(value)); }
};

} // namespace loom

#endif // ZEN_WEAVE_STANDARD_SHAPES_HPP
