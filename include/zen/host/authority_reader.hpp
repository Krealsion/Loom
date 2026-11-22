// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_AUTHORITY_READER_HPP
#define ZEN_HOST_AUTHORITY_READER_HPP

// The authority reader: a door that tells a granted asker what one participant may do now, as a
// fact, and hands it nothing. A weave that judges an act at its own door by the actor's live
// authority (a guest's capture doors, a carried reference's input actor) asks here instead of
// holding a capability over that actor:
//
//   zen.AuthorityAsked{subject, shape, version, role, door}
//       -> zen.AuthorityAnswered{subject, present, permits}
//
// `present` says whether the subject is a live participant now; `permits` whether its effective
// live authority (its baseline and what was delegated to it since, GATE-05) allows sending
// `shape` at `version` to `role`, at `door` when one is named. An empty `shape` asks presence
// alone. The answer is two facts at the moment of the ask, never a handle: nothing in it can be
// spent, installed or kept to read again later, and a later act is judged again by the bus.
//
// Host wiring, like the control door: it is constructed with the Switchboard it reads, which no
// message can hand a weave, and it mints for each ask a describe-only authority that installs
// nothing (`host_describe_only_authority`) and drops it before answering. Who may ask is the
// asker's grant to send `zen.AuthorityAsked` here; a weave not granted it is `CapabilityDenied`
// at its send, and learns nothing. docs/reference/capabilities.md#reading-an-actors-authority

#include <zen/host/grant_wiring.hpp>
#include <zen/switchboard/door.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>

namespace loom::host {

/// Ask what `subject` may do now: whether it may send `shape` at `version` to `role`, at
/// `door` when one is named. An empty `shape` asks only whether `subject` is present.
struct AuthorityAsked {
    std::int64_t subject = 0;
    std::string shape;
    std::int64_t version = 0;
    std::string role;
    std::string door; ///< a door at `role` (`zen/switchboard/door.hpp`), or empty for none
    using ZenSelf = AuthorityAsked;
    static constexpr const char* zen_name = "zen.AuthorityAsked";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(subject), ZEN_FIELD(shape), ZEN_FIELD(version),
                               ZEN_FIELD(role), ZEN_FIELD(door));
    }
};

/// The answer: the subject asked about, whether it is a live participant now, and whether its
/// effective live authority permits the act asked. `permits` is false for a subject not present,
/// and for an ask of presence alone.
struct AuthorityAnswered {
    std::int64_t subject = 0;
    bool present = false;
    bool permits = false;
    using ZenSelf = AuthorityAnswered;
    static constexpr const char* zen_name = "zen.AuthorityAnswered";
    static constexpr std::uint32_t zen_version = 1;
    template <int ZenLazy = 0>
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(subject), ZEN_FIELD(present), ZEN_FIELD(permits));
    }
};

/// Counters, and nothing else: a reader that kept what it read would be a second, stale copy of
/// every participant's authority.
struct AuthorityReaderState {
    std::int64_t asked = 0;
    ZEN_SHAPE(AuthorityReaderState, 1, ZEN_FIELD(asked));
};

/// The door. Mount it with `authority_reader_grant()`, beside the weaves that ask it.
class AuthorityReader final
    : public WeaveBase<AuthorityReader, AuthorityReaderState, Accept<AuthorityAsked>,
                       Emit<AuthorityAnswered, Refused>> {
public:
    explicit AuthorityReader(Switchboard& bus) : bus_(&bus) {}

    /// A door holding host wiring does not reload: a revived one would hold no Switchboard.
    LifecyclePolicy policy_config() const { return LifecyclePolicy{0, true}; }

    void on(const AuthorityAsked& asked, Mail& mail) {
        ++state_.asked;
        std::optional<Door> door;
        if (!asked.door.empty()) {
            door = Door::parse(asked.door);
            if (!door) {
                answer(mail, Refused{"'" + asked.door + "' is not a door"});
                return;
            }
        }
        if (asked.version < 0 || asked.version > static_cast<std::int64_t>(UINT32_MAX)) {
            answer(mail, Refused{"version " + std::to_string(asked.version) +
                                 " is no shape's version"});
            return;
        }
        AuthorityAnswered out;
        out.subject = asked.subject;
        const WeaveId subject{static_cast<std::uint64_t>(asked.subject < 0 ? 0 : asked.subject)};
        if (subject.valid() && bus_->alive(subject)) {
            const AuthorityView live =
                mail.describe_authority(host_describe_only_authority(*bus_, subject));
            out.present = live.available;
            if (live.available && !asked.shape.empty()) {
                const auto version = static_cast<std::uint32_t>(asked.version);
                out.permits = door ? live.permits_role(asked.shape, version, asked.role, *door)
                                   : live.permits_role(asked.shape, version, asked.role);
            }
        }
        answer(mail, out);
    }

private:
    /// To the asker through this delivery's answer right, or, where `reply_to` names another
    /// weave, an ordinary send there under the request's correlation: the control door's rule.
    template <class Answer>
    void answer(Mail& mail, const Answer& a) {
        const WeaveId to = mail.reply_to().valid() ? mail.reply_to() : mail.sender();
        if (!to.valid()) {
            return;
        }
        if (to == mail.sender()) {
            (void)mail.answer(a);
            return;
        }
        mail.send(to, a, mail.correlation());
    }

    Switchboard* bus_;
};

/// The grant the reader needs: its answer and its refusal to whoever asked, and the poke answers.
/// It sends nothing else, and holds no administration: it reads, and mints only describe-only
/// authority, which installs nothing.
inline Grant authority_reader_grant() {
    Grant g;
    g.allow_to_any(AuthorityAnswered::zen_name, AuthorityAnswered::zen_version);
    g.allow_to_any(Refused::zen_name, Refused::zen_version);
    allow_poke_answers(g);
    return g;
}

} // namespace loom::host

#endif // ZEN_HOST_AUTHORITY_READER_HPP
