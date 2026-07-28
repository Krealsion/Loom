#ifndef ZEN_SWITCHBOARD_MESSAGE_HPP
#define ZEN_SWITCHBOARD_MESSAGE_HPP

#include <zen/value.hpp>

#include <cstdint>
#include <utility>

namespace loom {

/// A stable handle to a registered Weave, assigned by the Switchboard. The
/// zero id is the null/invalid handle (e.g. "no reply_to", "from outside").
struct WeaveId {
    std::uint64_t value = 0;

    bool valid() const noexcept { return value != 0; }
    friend bool operator==(WeaveId a, WeaveId b) noexcept { return a.value == b.value; }
    friend bool operator!=(WeaveId a, WeaveId b) noexcept { return a.value != b.value; }
};

class Switchboard;

/// WHY A RECIPIENT MAY TRUST WHAT IT WAS JUST HANDED — a delivery fact, and the
/// third of three questions the system has always needed to keep apart:
///
///   SHAPE      "can this be represented and admitted?"   — answered by the gate
///   SENDER     "which weave emitted it?"                 — answered by the bus stamp
///   PROVENANCE "did Loom itself authorize this message's standing in a
///               lifecycle conversation?"                 — answered here
///
/// The three are related and NOT interchangeable, and conflating them is the
/// exact mistake this type exists to end. A perfectly-shaped `zen.Bequest` from
/// a weave that merely holds the grant for that shape passes the first two and
/// fails the third; before R2B-1 nothing could tell the difference, so a heir
/// claiming its inheritance by role had no way to know whether the answer came
/// from the steward it actually reached.
///
/// IT IS NOT A PAYLOAD FIELD, AND IT CANNOT BECOME ONE. There is no wire
/// representation: it is never serialized, never part of any schema, and every
/// ordinary enqueue path (`send`, `send_to_role`, `publish`, and their `_as`
/// forms) OVERWRITES it with nothing. So a weave that stores a delivered Message
/// and re-sends it — the copy-what-you-observed attack — sends an ordinary
/// message. Only the Switchboard's two attesting paths write a non-empty one.
class Provenance {
public:
    enum class Kind : std::uint8_t {
        None = 0,       ///< an ordinary message; it stands on shape and stamp alone
        Answer = 1,     ///< THE one authorized answer to a request this weave sent
        Activation = 2, ///< Loom attests a lifecycle commit for THIS incarnation
    };

    Provenance() = default;

    /// Describe the provenance the HOST computed for a delivery it is about to
    /// hand to its own handler.
    ///
    /// Deliberately public, and safe for a reason worth stating rather than
    /// hiding behind an access specifier: provenance has no wire representation
    /// and every enqueue path clears it, so constructing one can only describe a
    /// delivery the constructor is itself about to dispatch in its own process.
    /// It buys no reach. The one real caller is a weave library's own dispatch
    /// shim, translating the fact back across the C ABI (kernel/export.hpp) —
    /// and a library that lied here would be lying only to itself.
    static Provenance attested(Kind kind, std::int64_t sequence) noexcept {
        Provenance p;
        p.kind_ = kind;
        p.sequence_ = sequence;
        return p;
    }

    Kind kind() const noexcept { return kind_; }

    /// Is this delivery THE one authorized answer to a request this weave sent?
    bool answers_ask() const noexcept { return kind_ == Kind::Answer; }

    /// Does Loom attest a lifecycle commit for the incarnation being delivered to?
    bool lifecycle_activation() const noexcept { return kind_ == Kind::Activation; }

    /// The sequence Loom attested, for an activation. Compared by the consumer
    /// against the payload's own — an attestation for one sequence must not
    /// authenticate another.
    std::int64_t attested_sequence() const noexcept { return sequence_; }

private:
    Kind kind_ = Kind::None;
    std::int64_t sequence_ = 0;
};

/// The right to attach a lifecycle attestation — a capability OBJECT, not a
/// grant and not a payload flag.
///
/// Ordinary weave code cannot construct one: the default constructor is private
/// and the Switchboard is its only minter. A weave holds one because the HOST
/// handed it one at mount, exactly as the kernel's control door holds a Kernel
/// reference. Copying an authority you were given is ordinary (it is yours);
/// there is no path from "I know the shape" to "I hold the authority".
///
/// It does NOT widen the holder's grant. An attested send is still authorized
/// against the sender's ordinary grant at delivery, so the authority answers
/// "may this message carry Loom's word?" and never "may this weave send it?".
class LifecycleAuthority {
public:
    LifecycleAuthority(const LifecycleAuthority&) = default;
    LifecycleAuthority& operator=(const LifecycleAuthority&) = default;

private:
    friend class Switchboard;
    LifecycleAuthority() = default;
};

/// The routed envelope. The payload is a self-describing Value — its own schema
/// is its routing shape. Routing metadata rides alongside it: who sent it, an
/// optional reply address, an optional opaque correlation token so a handler can
/// reply by sending (synchronous await is a deliberate seam, not built here),
/// and — set only by Loom, never by a sender — the delivery's provenance.
struct Message {
    Value payload;
    WeaveId sender{};
    WeaveId reply_to{};
    std::uint64_t correlation = 0;
    /// Written by the Switchboard alone; cleared on every ordinary enqueue. A
    /// value set here by a sender never survives to a recipient.
    Provenance provenance{};

    explicit Message(Value payload_, WeaveId sender_ = {}, WeaveId reply_to_ = {},
                     std::uint64_t correlation_ = 0)
        : payload(std::move(payload_)), sender(sender_), reply_to(reply_to_),
          correlation(correlation_) {}
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_MESSAGE_HPP
