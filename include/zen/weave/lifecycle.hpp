// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_WEAVE_LIFECYCLE_HPP
#define ZEN_WEAVE_LIFECYCLE_HPP

// The lifecycle conversations a weave may choose to have: the letter (zen.PrepareShutdown,
// zen.Bequest, zen.ClaimBequest), a cooperative handoff to an heir, and activation
// (zen.Activated), the fact that a new code incarnation is live. Neither is required: a weave
// that ignores them is unaffected. They are protocol any weave may use, so they live beside the
// standard reply shapes; the Weave Manager is one consumer.
// docs/reference/lifecycle.md#graceful-swap--the-other-ceremony-and-when-to-prefer-it
//
// Reload transplants state across the same shape; the letter speaks across a different one. The
// predecessor chooses what to pass on, in its own vocabulary, as messages, and there is no state
// blob, which would be a second transplant path with none of reload's shape agreement.
//
// The letter may not assume immediacy, a clock, or that the predecessor's id still means
// anything: the heir claims it when it wakes (pull, never push, which would need the steward to
// hold grants for shapes unknown at mount). And a letter still queued when its author is
// removed is refused `SenderLifeEnded` (MSG-03), so a graceful swap asks, receives the letter,
// and only then unloads.
//
// Items are serialized bytes: a List holds one element type, and a letter's items are of any
// shape. The heir re-admits each through the gate when it reads it (claim_item), so the bytes
// stay inert until they pass the one validator.

#include <zen/gate.hpp>
#include <zen/serialize.hpp>
#include <zen/weave/shape.hpp>
#include <zen/weave/standard_shapes.hpp> // a claim is answered Bequest | Refused

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace loom {

/// The steward's role. An heir knows neither its predecessor's id nor the steward's, so it
/// claims by role, the address that outlives its holder.
inline constexpr const char* kManagerRole = "zen.manager";

/// The most items a letter carries, so a predecessor cannot flood the steward.
inline constexpr std::size_t kMaxBequestItems = 32;

/// "You are being replaced; say what you want your heir to know." The correlation carries the
/// conversation, so there are no fields.
struct PrepareShutdown {
    using ZenSelf = PrepareShutdown;
    static constexpr const char* zen_name = "zen.PrepareShutdown";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// The letter. `role` names the succession it belongs to, descriptively; the steward files it
/// under its own record of what it asked, never under a payload field.
struct Bequest {
    std::string role;
    std::vector<Bytes> items; ///< each item: one serialized message, gate-checked on read
    using ZenSelf = Bequest;
    static constexpr const char* zen_name = "zen.Bequest";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(role), ZEN_FIELD(items)); }
};

/// "Did anyone leave me anything?": the heir's question, asked when it wakes.
struct ClaimBequest {
    std::string role;
    using ZenSelf = ClaimBequest;
    static constexpr const char* zen_name = "zen.ClaimBequest";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(role)); }
};

// ---- activation (LIFE-01, LIFE-02) ------------------------------------------
// `zen.Activated` means exactly: "the lifecycle operator that sent this has committed a new code
// incarnation at this address." Not that the weave is healthy, ready, a role holder, that
// state was kept, that a predecessor existed, or that anything should start. Do not grow it.
//
// A weave participates by listing it in its accept-set; a sender asks first and otherwise stays
// silent, so a non-participant hears nothing. Its identity is the bus-stamped sender plus
// `sequence`, monotonic within that sender's revived lineage and unique nowhere else. Any weave
// granted the shape can send one, so a consumer checks the stamped sender against the operator
// it trusts, or requires Loom's attestation (`Mail::lifecycle_attested`), and treats a sequence
// no newer than that sender's last as a duplicate. No role field and no cause field.
//
// The kernel's control door (kernel/control.hpp) sends it on a successful LoadLibrary or
// ReloadLibrary; weaves mounted natively with mount<T>() are not activated.
struct Activated {
    /// Positive, newer than the previous activation from the same revived lineage, and never
    /// reused by it, because the sender refuses the lifecycle operation when it cannot say so;
    /// at the last representable value the operation is refused and nothing is sent (LIFE-03;
    /// ControlState and activation_block in kernel/control.hpp).
    std::int64_t sequence;

    using ZenSelf = Activated;
    static constexpr const char* zen_name = "zen.Activated";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(sequence)); }
};

/// Write one built Value into a letter item, as canonical bytes.
inline Bytes bequeath_item_value(const Value& v) {
    const std::string encoded = loom::serialize(v);
    return Bytes(encoded.begin(), encoded.end());
}

/// The same, for a ZEN_SHAPE struct.
template <class T>
Bytes bequeath_item(const T& msg) {
    return bequeath_item_value(to_value(msg));
}

/// Read one letter item back as `T`, or nothing. The bytes are parsed and admitted against T's
/// schema before any field is read, so a malformed, truncated or differently shaped item is a
/// clean nullopt. A letter is untrusted input, and this is its one door.
template <class T>
std::optional<T> claim_item(const Bytes& item) {
    const std::string_view bytes(reinterpret_cast<const char*>(item.data()), item.size());
    loom::Unverified candidate = loom::parse(bytes);
    loom::Admission admitted = loom::admit(candidate, schema_of<T>());
    if (!admitted.ok()) {
        return std::nullopt;
    }
    return from_value<T>(admitted.value());
}

} // namespace loom

#endif // ZEN_WEAVE_LIFECYCLE_HPP
