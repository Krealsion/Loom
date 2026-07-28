#ifndef ZEN_WEAVE_LIFECYCLE_HPP
#define ZEN_WEAVE_LIFECYCLE_HPP

// The lifecycle conversations a weave may choose to have. Two live here:
//
//   THE LETTER (zen.PrepareShutdown / zen.Bequest / zen.ClaimBequest) — cooperative
//   handoff: a weave writes a letter to its heir. Everything below the TIER note
//   down to bequeath_item/claim_item is the letter's.
//
//   ACTIVATION (zen.Activated) — the one narrow fact that a newly committed code
//   incarnation is live. It is not part of the letter and knows nothing about it;
//   the two share a header because they share a tier, not a mechanism.
//
// TIER. This header is the **Loomstd embryo**. The shapes here are not core law
// (nothing in the substrate requires them; a weave that ignores them is a clean
// non-event) and they are not domain vocabulary either. They are the small
// UNIVERSAL middle: a lifecycle conversation any weave, in any package, may
// choose to have. That is why they live in weave/ beside the standard reply
// shapes rather than in kernel/ with the machinery that happens to drive them —
// the Weave Manager is *a* consumer of this protocol, not its owner. When
// Loomstd becomes a real build artifact, this header moves there whole.
//
// WHAT IT IS FOR. Reload transplants state across the SAME shape. The letter
// converses across a DIFFERENT one: when a weave is replaced by a successor that
// is not shaped like it, nothing of what it knew can be transplanted — but it can
// still be *said*. So the predecessor chooses what to pass on, in its own
// vocabulary, as messages.
//
// MESSAGES ONLY — NO STATE BLOB. A deliberate deviation from the original
// {state, wake_messages[]} sketch. A state blob would be a second, shadow
// transplant path with none of reload's shape agreement — precisely the quiet
// growth of "reload" into "replace" the two ops exist to prevent. A weave that
// wants its state to carry says so **in its own vocabulary, as an item**. The
// letter is a conversation, not a transfer.
//
// TWO LAWS, both earned from 1a's own pins rather than assumed:
//
//   1. THE LETTER MUST NOT KNOW THE GAP. Nothing here may assume immediacy, wall
//      clock, or that the predecessor's WeaveId still means anything. The letter
//      waits; the heir asks when it wakes — a microsecond later or a month. That
//      is why delivery is PULL (the heir claims) and never push. Pull is also
//      what the grant model forces: pushing arbitrary domain shapes at an heir
//      would require the steward to hold shape grants unknowable at mount, and
//      `allow_any` on the steward is exactly the transitive reach its broker note
//      refuses.
//
//   2. THE LETTER DIES WITH ITS SENDER unless it is waited for. A gated message
//      is authorized by looking its sender up at DELIVERY time, so a letter still
//      queued when its author is unregistered is refused CapabilityDenied (the
//      1a in-flight pin). A graceful swap is therefore TWO-STAGE by construction:
//      ask, *receive the letter*, and only then unload. Fire-and-forget would
//      post the letter into the void.
//
// THE ITEMS ARE BYTES, AND THE GATE STAYS THE SOLE ADMITTER. A list cannot hold
// heterogeneous messages: a List's element is ONE TypeRef, and the gate pins a
// nested Message to a single schema by content_id (gate.cpp). So a letter whose
// items are arbitrary shapes cannot be a List<Message> — it is a List<Bytes>,
// each item serialized by the predecessor and **re-admitted through the real gate
// by the heir when it reads it** (see claim_item). The bytes are inert until they
// pass the one validator, exactly like every other untrusted input: the escape
// hatch buys heterogeneity without buying a second admission path.

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

/// The steward's well-known role. An heir wakes knowing nothing — not its
/// predecessor's id, not the steward's — so it claims by ROLE, the one address
/// that outlives its holder. (Role-first addressing, applied to the steward
/// itself: the Manager must survive the swaps it performs.)
inline constexpr const char* kManagerRole = "zen.manager";

/// A letter carries at most this many items. Bounded and PINNED, not hidden: a
/// predecessor writing its heir an unbounded letter would be a memory hole in
/// the steward, and "the last thing it did was flood the mailbox" is a poor
/// epitaph. Thirty-two is enough to say something and too few to hide in; a
/// predecessor with more to pass on should say less, more densely.
inline constexpr std::size_t kMaxBequestItems = 32;

/// "You are being replaced. Say what you want your heir to know." Zero fields:
/// the correlation carries the conversation, and there is nothing else to say
/// that the recipient does not already know about itself.
struct PrepareShutdown {
    using ZenSelf = PrepareShutdown;
    static constexpr const char* zen_name = "zen.PrepareShutdown";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// The letter itself. `role` is the succession it belongs to — descriptive, for
/// the heir and for anyone poking the steward's mail; it is NOT the authority
/// for filing it (the steward keys by its own record of what it asked about,
/// never by a field on the payload — the same routing-metadata-not-payload
/// discipline the relay uses for askers).
struct Bequest {
    std::string role;
    std::vector<Bytes> items; ///< each item: one serialized message, gate-checked on read
    using ZenSelf = Bequest;
    static constexpr const char* zen_name = "zen.Bequest";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(role), ZEN_FIELD(items)); }
};

/// "Did anyone leave me anything?" The heir's question, asked whenever it wakes.
struct ClaimBequest {
    std::string role;
    using ZenSelf = ClaimBequest;
    static constexpr const char* zen_name = "zen.ClaimBequest";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(role)); }
};

// ---- activation (R2A-1) -----------------------------------------------------
//
// THE WHOLE FACT, and nothing beside it. `zen.Activated` means exactly:
//
//     "The lifecycle operator that sent this message has successfully committed a
//      new code incarnation at this address."
//
// It does NOT mean the weave is healthy, ready for domain traffic, holder of a
// role, that state was preserved, that a predecessor existed, that a replacement
// was graceful, that external resources are available, that it should start a
// loop, that it should repeat prior work, or that the system is ready. Those are
// larger claims; none of them is smuggled in here. The power of this shape is how
// little it claims — do not grow it to carry tomorrow's lifecycle.
//
// PARTICIPATION IS DECLARED, NOT ATTEMPTED. A weave opts in by listing this shape
// in its accepted schemas; a sender asks first and stays silent otherwise. A
// non-participant hears nothing and produces no refusal — the optional-
// participation floor, exactly as with the letter's PrepareShutdown.
//
// IDENTITY IS **bus-stamped sender + sequence**. `sequence` is monotonic within
// the revived lineage of the sender that emitted it; it is NOT claimed unique
// across hosts, control weaves, or histories, and a naked number is not an
// identity. The stamped sender stays load-bearing.
//
// THE CONSUMER'S OBLIGATION follows from that, and it is the consumer's, not the
// substrate's: this is an ordinary registered shape, so any weave granted it can
// emit one — exactly as any weave granted `zen.Ack` can emit an Ack. A consumer
// that acts on an activation must therefore check the BUS-STAMPED SENDER (the one
// field a sender cannot forge) against the operator it actually trusts, and treat
// a sequence that is not newer than the last one from that sender as a duplicate.
// Trusting a bare `sequence` because it looked plausible is the same mistake as
// trusting an unsolicited standard reply, and has the same answer.
//
// NO ROLE FIELD: a loaded weave may hold none, a weave already knows which
// message it received, and role ownership is separate live composition truth —
// a payload field would invite trusting metadata over the bus and the live role
// map. NO CAUSE FIELD: "load"/"reload"/"swap"/"recovery" is vocabulary no proven
// consumer needs yet; the only proven fact is that a new incarnation committed.
//
// WHO SENDS IT, TODAY: the kernel's control door (kernel/control.hpp), on a
// successful LoadLibrary or ReloadLibrary — the ordinary participant sitting on
// the kernel operation, reachable by the default Manager and by any explicitly
// authorized alternate operator alike. Host-native mount<T>() weaves are NOT
// covered. What a participant should DO on activation is its own business and no
// part of this shape (Zengine's Timer is the first intended consumer — R2A-2).
struct Activated {
    /// Positive, and newer than the previous activation from the same revived
    /// lineage. Never reused by that lineage.
    std::int64_t sequence;

    using ZenSelf = Activated;
    static constexpr const char* zen_name = "zen.Activated";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(sequence)); }
};

/// Write one already-built Value into a letter item. The predecessor's side of
/// the escape hatch: canonical bytes out.
inline Bytes bequeath_item_value(const Value& v) {
    const std::string encoded = loom::serialize(v);
    return Bytes(encoded.begin(), encoded.end());
}

/// The same, for a ZEN_SHAPE struct.
template <class T>
Bytes bequeath_item(const T& msg) {
    return bequeath_item_value(to_value(msg));
}

/// Read one letter item back as `T`, or nothing. THE GATE IS THE TRUTH HERE:
/// the bytes are parsed and admitted against T's own schema before a single
/// field is touched, so a letter item that is malformed, truncated, or simply a
/// different shape than the heir hoped for is a clean nullopt — never a
/// misread. An heir reading a letter is reading untrusted input, and this is
/// the one door it comes through.
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
