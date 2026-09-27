// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_MESSAGE_HPP
#define ZEN_SWITCHBOARD_MESSAGE_HPP

#include <zen/value.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace loom {

/// A stable handle to a registered Weave, assigned by the Switchboard. Zero is the invalid
/// handle: no reply address, or from outside.
struct WeaveId {
    std::uint64_t value = 0;

    bool valid() const noexcept { return value != 0; }
    friend bool operator==(WeaveId a, WeaveId b) noexcept { return a.value == b.value; }
    friend bool operator!=(WeaveId a, WeaveId b) noexcept { return a.value != b.value; }
};

class Switchboard;

/// The private identity of one Loom: an empty type that exists once per Switchboard and that
/// only the Switchboard constructs. Held by shared ownership: the board holds the one
/// `shared_ptr`, each authority a `weak_ptr`, so when a board dies every authority it issued
/// expires, even if a later board is allocated at the same address.
class LoomIdentity {
private:
    friend class Switchboard;
    LoomIdentity() = default;
};

/// Why a recipient may trust what it was handed: a delivery fact, apart from the other two
/// questions about a message:
///
///   shape       can it be represented and admitted?          the gate
///   sender      which weave sent it?                         the bus's stamp
///   provenance  what standing does Loom give it?             this
///
/// A well-shaped `zen.Bequest` from a weave that merely holds the grant for it passes the first
/// two and not the third. Provenance has two independent axes: `Kind` (none, an answer, an
/// activation, a dispatch refusal) and the authored office (empty for personal speech), so a
/// delivery may carry both, though no public door produces that combination.
/// MSG-07, ANS-01; docs/laws/messaging-laws.md
///
/// Not a payload field: never serialized, in no schema, and overwritten with nothing by every
/// ordinary enqueue (`send`, `send_to_role`, `publish` and their `_as` forms), so a stored and
/// re-sent Message is ordinary. Only the Switchboard's attesting doors write one.
class Provenance {
public:
    enum class Kind : std::uint8_t {
        None = 0,       ///< an ordinary message; it stands on shape and stamp alone
        Answer = 1,     ///< the one authorized answer to a request this weave sent
        Activation = 2, ///< Loom attests a lifecycle commit for this incarnation
        DispatchRefusal = 3, ///< Loom refused an ordinary send before handler entry
    };

    Provenance() = default;

    /// Describe the provenance the host computed for a delivery it is about to hand to its own
    /// handler. Public because it buys nothing: provenance has no wire form and every enqueue
    /// clears it. Its caller is a weave library's dispatch shim translating the fact back across
    /// the C ABI (kernel/export.hpp).
    static Provenance attested(Kind kind, std::int64_t sequence) noexcept {
        Provenance p;
        p.kind_ = kind;
        p.sequence_ = sequence;
        return p;
    }

    /// Attach the authored-office fact, the second axis, to any Kind. Public for the same
    /// reason as `attested`: only the Switchboard's verified doors and a library's dispatch shim
    /// write a value that reaches a recipient.
    Provenance with_authored_role(std::string role) && {
        authored_role_ = std::move(role);
        return std::move(*this);
    }

    Kind kind() const noexcept { return kind_; }

    /// Is this delivery the one authorized answer to a request this weave sent?
    bool answers_ask() const noexcept { return kind_ == Kind::Answer; }

    /// Loom attests refusal of dispatch, never an application answer.
    bool dispatch_refused() const noexcept { return kind_ == Kind::DispatchRefusal; }

    /// Does Loom attest a lifecycle commit for the incarnation being delivered to?
    bool lifecycle_activation() const noexcept { return kind_ == Kind::Activation; }

    /// The sequence Loom attested, for an activation, to compare with the payload's own.
    std::int64_t attested_sequence() const noexcept { return sequence_; }

    /// The office this delivery was deliberately authored as, verified when it was authored;
    /// empty for personal speech, which no bindable role can equal. A fact about the statement,
    /// never about now: current membership is `Switchboard::role_holder`'s question.
    std::string_view authored_role() const noexcept { return authored_role_; }

    /// Was this delivery deliberately authored as `role`? False for empty
    /// `role`, so "no office" can never satisfy a membership question.
    bool authored_from_role(std::string_view role) const noexcept {
        return !role.empty() && authored_role_ == role;
    }

private:
    Kind kind_ = Kind::None;
    std::int64_t sequence_ = 0;
    /// The second axis. Empty = spoken personally (the overwhelmingly common
    /// case, and the default every ordinary enqueue restores).
    std::string authored_role_{};
};

/// The right to attach a lifecycle attestation: a capability object, not a grant and not a
/// payload flag. Only trusted host infrastructure can hold one: the mint is private and
/// non-static on the Switchboard, which a weave never holds (it gets a `Bus&`), and the one
/// expression that reaches it is in a host-wiring header no weave-authoring header includes
/// (`zen/host/lifecycle_wiring.hpp`). So three things stay distinct:
///
///   the shape            ordinary code may represent `zen.Activated`
///   a grant to emit it   ordinary code may be permitted to send it
///   this authority       only Loom infrastructure may attest it as a lifecycle fact
///
/// It widens no grant: an attested send (`announce_lifecycle`) is still authorized against the
/// sender's grant. The one exception is the activation an admission delivers, which is Loom's
/// own act rather than a send: no grant is consulted, and the coordinator's id is stamped as
/// who admitted.
/// PR-08, LIFE-05; docs/laws/replacement-laws.md
///
/// It names the Loom that issued it, which is the only Loom that accepts it. Anyone may own a
/// second Switchboard and mint from it; that authority is real, and worthless here.
class LifecycleAuthority {
public:
    LifecycleAuthority(const LifecycleAuthority&) = default;
    LifecycleAuthority& operator=(const LifecycleAuthority&) = default;

private:
    friend class Switchboard;
    explicit LifecycleAuthority(std::weak_ptr<const LoomIdentity> issuer)
        : issuer_(std::move(issuer)) {}

    /// Weak, so the authority does not keep its board alive; it lasts as long as the Loom that
    /// issued it, and an expired one is refused.
    std::weak_ptr<const LoomIdentity> issuer_;
};

/// The right to answer later: the same one answer, retained (ANS-02). For a responder whose
/// answer depends on messages it has not yet received. An answer may outlive the handler, but
/// never the conversation or the incarnation that earned it.
/// ANS-02; docs/laws/answer-authority-laws.md
///
/// Not a payload field, not serializable, not a future. Spending it needs a live `Mail` of the
/// exact incarnation that earned it, and the reply is still governed by that weave's grant.
/// Move-only, so a second answer to one question does not compile. A default-constructed one
/// is invalid (`valid()`), and spending it fails visibly.
class DeferredAnswer {
public:
    DeferredAnswer() = default;
    DeferredAnswer(const DeferredAnswer&) = delete;
    DeferredAnswer& operator=(const DeferredAnswer&) = delete;
    DeferredAnswer(DeferredAnswer&& other) noexcept
        : issuer_(std::move(other.issuer_)), token_(other.token_) {
        other.token_ = 0; // moved-from is invalid: one right, one holder
    }
    DeferredAnswer& operator=(DeferredAnswer&& other) noexcept {
        if (this != &other) {
            issuer_ = std::move(other.issuer_);
            token_ = other.token_;
            other.token_ = 0;
        }
        return *this;
    }

    /// Does this name a conversation? False for a default one and one moved from. It does not
    /// promise the conversation is still answerable; only the bus says that, when it is spent.
    bool valid() const noexcept { return token_ != 0; }

    /// The bus-private index this names. Public and inert: every spend is checked against the
    /// record's requester, respondent, both incarnations, correlation and issuing board. A Bus
    /// on the far side of the C ABI has to hand it to the host.
    std::uint64_t opaque_token() const noexcept { return token_; }

    /// Which Loom issued it; empty for one held inside a dynamic library (see
    /// `from_host_token`).
    std::weak_ptr<const LoomIdentity> issuer() const noexcept { return issuer_; }

    /// Rebuild a capability a dynamic library was handed across the C ABI. It has no issuer:
    /// a loaded weave presents a token only through the host context of the delivery that gave
    /// it one, which is its own board. Every spend is checked against the record, so a made-up
    /// token reaches at most a conversation the library already owns.
    static DeferredAnswer from_host_token(std::uint64_t token) noexcept {
        DeferredAnswer d;
        d.token_ = token;
        return d;
    }

private:
    friend class Switchboard;
    DeferredAnswer(std::weak_ptr<const LoomIdentity> issuer, std::uint64_t token)
        : issuer_(std::move(issuer)), token_(token) {}

    /// Board-relative, as LifecycleAuthority is.
    std::weak_ptr<const LoomIdentity> issuer_;
    /// An index into bus-private state, not a secret: every spend is checked against the
    /// record, so an invented number reaches at most a conversation the caller already owns.
    std::uint64_t token_ = 0;
};

/// The routed envelope: a self-describing Value, whose schema is its routing shape, with its
/// sender, an optional reply address, an optional correlation, and the provenance only Loom
/// sets. There is no synchronous await; a handler replies by sending.
struct Message {
    Value payload;
    WeaveId sender{};
    WeaveId reply_to{};
    std::uint64_t correlation = 0;
    /// Written by the Switchboard alone and cleared by every ordinary enqueue, so a sender's
    /// value never reaches a recipient.
    Provenance provenance{};

    explicit Message(Value payload_, WeaveId sender_ = {}, WeaveId reply_to_ = {},
                     std::uint64_t correlation_ = 0)
        : payload(std::move(payload_)), sender(sender_), reply_to(reply_to_),
          correlation(correlation_) {}
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_MESSAGE_HPP
