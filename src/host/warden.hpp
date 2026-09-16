// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_WARDEN_HPP
#define ZEN_HOST_WARDEN_HPP

// THE HAND THAT INSTALLS WHAT A PERSON DECIDED.
//
// The supplied host keeps its standing decisions in a file (host/authority.hpp). This
// weave is the only thing that makes the running bus agree with that file.
//
// IT EXISTS BECAUSE THE INSTALL IS A DELIVERY-TIME ACT, not because the host wanted a
// middleman. `delegate_authority` is reachable only from a live participating Bus — a
// host's `main()` holds a Switchboard, which mints capabilities, but has no standing to
// spend one. So the host's administration is an ordinary participant, and the pleasant
// consequence is that every authority change is a message: it shows on the tap, in
// order, next to the traffic it affects. An invisible host-side mutation would have
// been simpler to write and impossible to watch.
//
// WHAT IT IS NOT. It is not a Weaver. `loom::Weaver` answers a SUBJECT'S request — one
// subject, one seat, one pending ask at a time, approve-or-refuse — and the supplied
// host's need is the other direction: an operator deciding unprompted, and a boot
// restoring decisions nobody is asking about right now. Mounting both would put two
// writers on one subject's delegated authority, which is the one thing GATE-05's single
// write path exists to prevent. A weave that wants to ASK for authority still wants a
// Weaver, and wiring one beside this is the natural next step — it is a different
// conversation, not a competing one.
//
// WHAT IT CANNOT DO, by construction:
//   - reach a subject the host never handed it a capability for. There is no message
//     that adds one; `govern()` is C++ the host calls, out of band.
//   - exceed what a person wrote. It reads the store and installs exactly that; the
//     payload carries no rules, so a well-formed forged message can at most re-apply
//     the file.
//   - be driven by anyone but the operator seat. Checked against the bus stamp, like
//     the Weaver checks its own.
//   - perform anything on a subject's behalf. It changes what a subject MAY say and
//     never says anything as one — which is the prompt's rule that an approval must not
//     silently act as a more powerful identity, kept by having no way to break it.

#include "authority.hpp"

#include <zen/switchboard/grant.hpp>
#include <zen/weave.hpp>
#include <zen/weaver/weaver.hpp> // render_authority: one spelling for rules, everywhere

#include <cstdint>
#include <map>
#include <tuple>
#include <string>
#include <utility>

namespace loom::host {

/// Make the bus agree with the store, for one artifact.
///
/// The payload names WHICH artifact and nothing else: the rules are in the file, and a
/// message that carried them too would be a second copy of the decision, able to
/// disagree with the first. "Sync" rather than "grant" is the honest verb — it installs
/// whatever the file now says, which for a forgotten artifact is nothing at all, and
/// that is how revoke works.
/// Hand-written registration blocks, not `ZEN_SHAPE`, for the reason the Manager's
/// command shapes are hand-written: the macro derives the wire name from the C++ type
/// name, and these need a PREFIX. `HostAuthoritySync` is a plausible enough name that an
/// application could one day declare its own — and a `(name, version)` is frozen
/// globally, so a collision is not a mix-up, it is a refused load for whoever arrives
/// second. `zen.host.` says whose these are: the supplied host's own vocabulary, not the
/// substrate's, which is why they are not plain `zen.`.
struct HostAuthoritySync {
    std::string artifact;
    using ZenSelf = HostAuthoritySync;
    static constexpr const char* zen_name = "zen.host.AuthoritySync";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(artifact)); }
};

/// Read back what the BUS will actually decide for one artifact — baseline and
/// delegated, separately, because the two have different lifetimes and a person
/// deciding what to revoke needs to know which half a permission is in.
///
/// A separate shape from the sync rather than a field on it, for the Weaver's reason:
/// reading an authority and changing one are different acts, and a shape that did both
/// depending on a value is a shape that can be mistyped into doing the other.
struct HostDescribeAuthority {
    std::string artifact;
    using ZenSelf = HostDescribeAuthority;
    static constexpr const char* zen_name = "zen.host.DescribeAuthority";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(artifact)); }
};

/// Counters, and deliberately nothing else — the Weaver's discipline, for the Weaver's
/// reason: a warden's persistable state is where a shadow permission database would
/// grow, so there is no rule, no subject and no record of what was installed here.
/// Both fields count things that happened; neither is consulted to decide anything.
struct WardenState {
    std::int64_t syncs = 0;
    std::int64_t installs = 0;
    ZEN_SHAPE(WardenState, 1, ZEN_FIELD(syncs), ZEN_FIELD(installs));
};

class HostWarden final
    : public WeaveBase<HostWarden, WardenState, Accept<HostAuthoritySync, HostDescribeAuthority>,
                       Emit<Result, Refused, AuthorityDescription>> {
public:
    /// `store` is the decisions; `seat` is the one weave whose word counts as the
    /// person's. Both come from the host at boot, out of band; no message changes
    /// either.
    HostWarden(const AuthorityStore& store, WeaveId seat) : store_(&store), seat_(seat) {}

    /// Hand the warden the capability to administer one loaded artifact. Called by the
    /// host right after a successful load, and by nothing else. A second call for the
    /// same name replaces the capability, which is what a reload or a replacement
    /// needs — the old one names a subject that may no longer exist.
    void govern(const std::string& artifact, GrantAuthority authority) {
        governed_.insert_or_assign(artifact, std::move(authority));
    }

    /// Forget an artifact's capability — after an unload, when there is nothing left
    /// to administer. Dropping it is not a revocation: an unloaded weave's authority
    /// went with it.
    void release(const std::string& artifact) { governed_.erase(artifact); }

    bool governs(const std::string& artifact) const { return governed_.count(artifact) != 0; }

    /// A warden does not reload: a revived one would come back governing nobody while
    /// the host still believed it governed everything. Same argument the Weaver makes.
    LifecyclePolicy policy_config() const { return LifecyclePolicy{0, true}; }

    void on(const HostAuthoritySync& s, Mail& mail) {
        ++state_.syncs;
        if (mail.sender() != seat_) {
            (void)mail.answer(Refused{"this host's authority is administered from the operator "
                                      "seat only"});
            return;
        }
        auto it = governed_.find(s.artifact);
        if (it == governed_.end()) {
            (void)mail.answer(
                Refused{"this host holds no authority over '" + s.artifact +
                        "': nothing by that name is loaded, so there is nothing to administer"});
            return;
        }
        // THE FILE IS THE REQUEST. A forgotten artifact yields `nothing()`, so removing
        // a decision and revoking it are the same act with the same code path — there
        // is no separate revoke to get wrong.
        LiveAuthority next;
        std::string why;
        if (const AuthorityRule* rule = store_->find(s.artifact)) {
            if (!to_live_authority(*rule, &next, &why)) {
                (void)mail.answer(Refused{why});
                return;
            }
        }
        const GrantChange change = mail.delegate_authority(it->second, std::move(next));
        if (change.outcome != GrantOutcome::Installed) {
            (void)mail.answer(Refused{std::string("could not administer '") + s.artifact +
                                      "': " + name_of(change.outcome)});
            return;
        }
        ++state_.installs;
        // Rendered from the SNAPSHOT the bus handed back, never from what the store
        // said — so this line cannot claim a permission the bus did not take. The
        // Weaver's rule, and its reason: if the two ever disagreed, this line would be
        // the lie.
        std::string report = "weave " + std::to_string(change.subject.value) + " may now say: ";
        const std::vector<std::string> rendered = render_authority(change.installed);
        if (rendered.empty()) {
            report += "nothing";
        }
        for (std::size_t i = 0; i < rendered.size(); ++i) {
            report += (i == 0 ? "" : "; ") + rendered[i];
        }
        (void)mail.answer(Result{report});
    }

    /// The read half, through the same capability and with the same scope. Reading an
    /// authority and issuing one are different acts, and only the second is gated — so
    /// this answers anyone who can reach the warden, not only the seat.
    void on(const HostDescribeAuthority& d, Mail& mail) {
        auto it = governed_.find(d.artifact);
        if (it == governed_.end()) {
            (void)mail.answer(Refused{"nothing is loaded under '" + d.artifact + "'"});
            return;
        }
        const AuthorityView view = mail.describe_authority(it->second);
        if (!view.available) {
            (void)mail.answer(
                Refused{"'" + d.artifact + "' is no longer a participant this host can describe"});
            return;
        }
        // Rendered from the snapshot, at the moment of the ask. EFFECTIVE authority is
        // the union of the two lists and is therefore read rather than sent: a third,
        // materialized list could fall out of date between here and the next delivery.
        (void)mail.answer(AuthorityDescription{static_cast<std::int64_t>(view.subject.value),
                                               render_authority(view.base),
                                               render_authority(view.delegated)});
    }

private:
    const AuthorityStore* store_;
    WeaveId seat_;
    /// Host-supplied wiring, not state: capabilities are not values and never cross
    /// the bus. (WeaveManager keeps its control door the same way.)
    std::map<std::string, GrantAuthority> governed_;
};

/// The grant a warden needs: its two answers to the seat, and the right to be poked.
/// Note what is absent — it may say nothing to anyone else, and holds no load
/// capability, no observation and no wildcard. Administering authority and having any
/// is the separation the Weaver's reference draws, kept here too.
inline Grant warden_capability(WeaveId seat) {
    Grant g;
    g.allow(Result::zen_name, Result::zen_version, seat);
    g.allow(Refused::zen_name, Refused::zen_version, seat);
    g.allow(AuthorityDescription::zen_name, AuthorityDescription::zen_version, seat);
    allow_poke_answers(g);
    return g;
}

} // namespace loom::host

#endif // ZEN_HOST_WARDEN_HPP
