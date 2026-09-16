// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_AUTHORITY_HPP
#define ZEN_HOST_AUTHORITY_HPP

// THE PERSON'S STANDING DECISIONS ABOUT WHAT MAY RUN AND WHAT MAY SPEAK.
//
// One file, per install, that a person writes at the console and can also read and
// edit in an editor. It is the source of both halves of the supplied host's authority,
// and the halves are deliberately different mechanisms because they have different
// lifetimes (GATE-05):
//
//   may_run      -> ADMISSION. Consulted by the Kernel's admission policy before the
//                   artifact's code is opened. Its yes mints a BASELINE, which is
//                   frozen for that weave's whole life and cannot be revoked in place.
//                   So the baseline is kept deliberately tiny: the floor plus the
//                   right to answer a poke, which is what makes a loaded weave
//                   inspectable without making it able to say anything.
//
//   send/observe -> DELEGATED LIVE AUTHORITY. Installed after the weave is running, by
//                   a holder of a host-minted GrantAuthority, and REPLACEABLE while it
//                   runs. That is why the approvals a person remembers land here and
//                   not in the baseline: the prompt's requirement is that a restored
//                   permission stay revocable, and only this half can be.
//
// So "revoke" means two different, both-honest things, and the console says which:
// revoking speech takes effect on the next delivery; revoking the right to RUN takes
// effect at the next load, and the honest way to end a running weave's baseline is to
// unload it.
//
// THE RULE SPELLING IS THE WEAVER'S. A rule in this file is written exactly as
// `loom::render_rule` prints one (`zen/weaver/weaver.hpp`) — `Greet v1 -> any target`,
// `Tick v1 -> role clock`, `any shape -> any target`, `observe Tick v1`. One spelling
// for what a person reads and what a person writes; a second grammar here would be a
// second answer to "what did I approve".

#include <zen/kernel/admission.hpp>
#include <zen/switchboard/grant.hpp>

#include <map>
#include <string>
#include <vector>

namespace loom::host {

/// One artifact's standing decision. Keyed by the artifact NAME — the host's word for
/// it — because that is the name a person types and the name a boot plan uses. The
/// build is pinned separately, by content, so "which artifact" and "which build of it"
/// stay two questions.
struct AuthorityRule {
    std::string artifact;
    /// The build this decision was made about: `loom::file_content_id`. Empty means
    /// "not pinned yet" — the state a rule is in between a person approving an
    /// artifact and that artifact first loading, when its bytes get recorded.
    std::string content_id;
    /// May its native code run in this process at all?
    bool may_run = false;
    /// What happens when the bytes change under a pinned rule. False (the default)
    /// re-asks: a different build is a different thing and the person decides again.
    /// True is the DEVELOPMENT posture — the person is rebuilding this artifact
    /// themselves and does not want to re-approve every compile.
    ///
    /// It never widens anything. The speech rules below are unchanged by a rebuild;
    /// all this decides is whether new bytes may run under the authority the person
    /// already granted. That distinction is the whole reason it can be a default-off
    /// per-artifact switch rather than a global one.
    bool trust_rebuilds = false;
    std::vector<std::string> send;    ///< rendered send rules (see the header note)
    std::vector<std::string> observe; ///< rendered observe rules
    std::string note;                 ///< the person's own words about why
};

/// Turn a rendered rule into real authority. Returns false and sets `*error` on
/// anything it cannot spell — including, deliberately, `weave #N`: a WeaveId is minted
/// per run, so a file that named one would mean something different on every boot.
bool apply_rule(const std::string& text, LiveAuthority* into, std::string* error);

/// Every send/observe rule of one decision, as live authority. Stops at the first rule
/// it cannot parse, so a typo is reported rather than silently dropping a permission
/// the person believes they granted.
bool to_live_authority(const AuthorityRule& rule, LiveAuthority* out, std::string* error);

/// A decision the admission policy could not make and is waiting on. Recorded when a
/// load is refused for want of a rule (or because the bytes changed), so the console
/// can show a person exactly what to approve, with the facts in front of them.
struct PendingDecision {
    std::string artifact;
    std::string path;
    std::string content_id;   ///< the build that was actually presented
    std::string pinned;       ///< the build the rule pinned, when there is one
    std::string why;          ///< the refusal, in the policy's own words
    /// The manifest's ask, if the artifact got far enough to publish one. Advice,
    /// shown to the person, never consulted — the same rule that holds everywhere else.
    bool declared_present = false;
    CapabilityAsk declared{};
};

/// The persisted store, and the admission policy over it.
class AuthorityStore {
public:
    /// Point the store at a file and read it. A missing file is an empty store, which
    /// admits nothing — the honest starting state, and the one a person then fills in
    /// from the console. A file that exists but is malformed is an error: somebody
    /// wrote it on purpose and is owed the parse failure.
    bool open(const std::string& path, std::string* error);

    const std::string& path() const noexcept { return path_; }

    const AuthorityRule* find(const std::string& artifact) const;
    std::vector<AuthorityRule> rules() const;

    /// Replace one decision and write the file. The write is where "remember this"
    /// actually happens, so it is not deferred to shutdown: a host that is killed
    /// between an approval and its next boot must come back with the approval.
    bool put(AuthorityRule rule, std::string* error);

    /// Drop one decision entirely and write the file. Does NOT touch a running
    /// weave — the caller is responsible for revoking its live authority too, and
    /// the console does both so a person's single "revoke" means both.
    bool forget(const std::string& artifact, std::string* error);

    /// THE ADMISSION POLICY THIS STORE IMPLEMENTS — the thing the Kernel asks.
    ///
    /// It may WRITE the store, and that is the remembering: the first successful load
    /// under an unpinned rule records which build it was, and a rebuild admitted under
    /// `trust_rebuilds` re-pins. Nothing else in it mutates anything.
    ///
    /// It never blocks on a person. A decision it cannot make is a refusal now, with
    /// the facts recorded in `pending()` and the console command to resolve it named
    /// in the refusal itself — so the asker hears a real answer about the request it
    /// made, and the person has everything they need to answer it properly.
    AdmissionPolicy policy();

    const std::vector<PendingDecision>& pending() const noexcept { return pending_; }
    void clear_pending() noexcept { pending_.clear(); }

    /// WHAT THE POLICY DID WITHOUT ASKING, in its own words, in the order it did it.
    ///
    /// An admission that needed no decision is still a decision the policy made, and
    /// two of them are things a person should be able to see afterwards: which build
    /// got pinned to a fresh approval, and — the one that matters — that an artifact
    /// they are developing came up on code they have not seen before, under authority
    /// they granted earlier. A `trust_rebuilds` that was silent would be a switch that
    /// hides exactly what it is doing.
    const std::vector<std::string>& notes() const noexcept { return notes_; }

private:
    bool write(std::string* error) const;
    void note_pending(PendingDecision d);

    std::string path_;
    std::map<std::string, AuthorityRule> rules_;
    std::vector<PendingDecision> pending_;
    std::vector<std::string> notes_;
};

/// THE BASELINE A POLICY-ADMITTED ARTIFACT GETS, and the argument for its size.
///
/// The floor plus the right to answer a poke, and nothing else. A baseline is frozen
/// at admission and can never be narrowed (GATE-05), so everything a person might one
/// day want to take back has to live in the delegated half instead. What is left is
/// the one permission it would be perverse to make revocable: being inspectable. A
/// weave a person cannot poke is a weave they cannot reason about, and they approved
/// loading it.
Grant admitted_baseline();

} // namespace loom::host

#endif // ZEN_HOST_AUTHORITY_HPP
