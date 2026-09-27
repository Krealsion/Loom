// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_WARDEN_HPP
#define ZEN_HOST_WARDEN_HPP

// The hand that installs what a person decided: the only thing that makes the running bus agree
// with the authority store (host/authority.hpp). A participant, because `delegate_authority` is
// spent from a live delivery, so an operator's change is a message on the tap. One write path
// (`install`), two entries: the seat's `zen.host.AuthoritySync`, and `adopt()` inside the load
// (`LifecycleAdoption`). Not a Weaver, which answers a subject's ask: it installs only what the
// file says, for subjects the host handed it, at the seat's word, and speaks as no subject.

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

/// Make the bus agree with the store, for one artifact. The payload names which artifact and
/// nothing else: the rules are in the file, and a second copy could disagree. "Sync", not
/// "grant": for a forgotten artifact it installs nothing, which is how revoke works.
/// Registered by hand, not `ZEN_SHAPE`, for the prefix: `zen.host.` marks the supplied host's
/// own vocabulary, so an application's `HostAuthoritySync` cannot collide with it.
struct HostAuthoritySync {
    std::string artifact;
    using ZenSelf = HostAuthoritySync;
    static constexpr const char* zen_name = "zen.host.AuthoritySync";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(artifact)); }
};

/// Read back what the bus will decide for one artifact, baseline and delegated apart, since a
/// person deciding what to revoke needs to know which half a permission is in. A separate
/// shape from the sync: reading and changing an authority are different acts.
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

/// WHAT ONE INSTALL DID, in words the caller can print or answer with.
///
/// `report` is rendered from the SNAPSHOT the bus handed back, never from what the store
/// said — so it cannot claim a permission the bus did not take. If the two ever disagreed,
/// this line would be the lie.
struct Installed {
    bool ok = false;
    WeaveId subject{};
    std::string report;
};

class HostWarden final
    : public WeaveBase<HostWarden, WardenState, Accept<HostAuthoritySync, HostDescribeAuthority>,
                       Emit<Result, Refused, AuthorityDescription>> {
public:
    /// `store` is the decisions; `seat` is the one weave whose word counts as the
    /// person's. Both come from the host at boot, out of band; no message changes
    /// either.
    HostWarden(const AuthorityStore& store, WeaveId seat) : store_(&store), seat_(seat) {}

    /// Hand the warden the capability to administer one loaded artifact. A second call
    /// for the same name replaces the capability, which is what a reload or a
    /// replacement needs — the old one names a subject that may no longer exist.
    void govern(const std::string& artifact, GrantAuthority authority) {
        governed_.insert_or_assign(artifact, std::move(authority));
    }

    /// Put a freshly committed incarnation under administration and make the bus agree with
    /// the file, in one act, inside the delivery that committed it (`LifecycleAdoption`), the
    /// only moment before the weave's own `zen.Activated`. `mail` is the door's, and that is
    /// not a second writer: `delegate_authority` is authorized by the capability (GATE-05), and
    /// the write path is the same function the seat's `AuthoritySync` runs.
    Installed adopt(Mail& mail, const std::string& artifact, GrantAuthority authority) {
        govern(artifact, std::move(authority));
        return install(mail, artifact);
    }

    /// Forget an artifact's capability — after an unload, when there is nothing left
    /// to administer. Dropping it is not a revocation: an unloaded weave's authority
    /// went with it, and the person's standing decision in the file is untouched.
    void release(const std::string& artifact) { governed_.erase(artifact); }

    bool governs(const std::string& artifact) const { return governed_.count(artifact) != 0; }

    /// The subject this warden currently administers under `artifact`, or an invalid id.
    /// A FACT ABOUT THE CAPABILITY, never about a payload — it is what the host prints
    /// when it says which weave it just put under administration.
    WeaveId subject_of(const std::string& artifact) const {
        auto it = governed_.find(artifact);
        return it == governed_.end() ? WeaveId{} : it->second.subject();
    }

    /// The artifact name this warden administers `subject` under, or empty -- the same fact read
    /// the other way, for a host door that must know WHOSE standing decision bounds a request a
    /// participant made (the session door's run registration). Never a payload's claim.
    std::string artifact_of(WeaveId subject) const {
        if (!subject.valid()) {
            return {};
        }
        for (const auto& [artifact, authority] : governed_) {
            if (authority.subject() == subject) {
                return artifact;
            }
        }
        return {};
    }

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
        const Installed done = install(mail, s.artifact);
        if (done.ok) {
            (void)mail.answer(Result{done.report});
        } else {
            (void)mail.answer(Refused{done.report});
        }
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
    /// THE ONE WRITE PATH, and the reason there is only one. Two entry points reach it —
    /// the operator's `AuthoritySync` and the host's adoption of a freshly loaded
    /// artifact — and if each installed authority its own way, "what did I approve" would
    /// have two answers that could drift. Everything policy-shaped is here; the entry
    /// points decide only who may ask.
    Installed install(Mail& mail, const std::string& artifact) {
        auto it = governed_.find(artifact);
        if (it == governed_.end()) {
            return {false, WeaveId{},
                    "this host holds no authority over '" + artifact +
                        "': nothing by that name is loaded, so there is nothing to administer"};
        }
        // THE FILE IS THE REQUEST. A forgotten artifact yields `nothing()`, so removing
        // a decision and revoking it are the same act with the same code path — there
        // is no separate revoke to get wrong.
        LiveAuthority next;
        std::string why;
        if (const AuthorityRule* rule = store_->find(artifact)) {
            if (!to_live_authority(*rule, &next, &why)) {
                return {false, it->second.subject(), why};
            }
        }
        const GrantChange change = mail.delegate_authority(it->second, std::move(next));
        if (change.outcome != GrantOutcome::Installed) {
            return {false, change.subject,
                    std::string("could not administer '") + artifact + "': " +
                        name_of(change.outcome)};
        }
        ++state_.installs;
        std::string report = "weave " + std::to_string(change.subject.value) + " may now say: ";
        const std::vector<std::string> rendered = render_authority(change.installed);
        if (rendered.empty()) {
            report += "nothing";
        }
        for (std::size_t i = 0; i < rendered.size(); ++i) {
            report += (i == 0 ? "" : "; ") + rendered[i];
        }
        return {true, change.subject, report};
    }

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
