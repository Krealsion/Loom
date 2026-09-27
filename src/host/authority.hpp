// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_AUTHORITY_HPP
#define ZEN_HOST_AUTHORITY_HPP

// The person's standing decisions about what may run and what may speak: one file per install,
// written at the console or in an editor. `may_run` is admission, consulted before the code is
// opened, and mints a baseline frozen for the weave's life (GATE-05), so kept tiny; send and
// observe rules are delegated live authority, installed once it runs and revocable in place.
// So revoking speech takes effect on the next delivery, revoking the right to run at the next
// load. Rules are spelled as `loom::render_rule` prints them (zen/weaver/weaver.hpp).

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
    /// The build this decision was made about: `loom::file_content_id`; empty until the
    /// approved artifact first loads and its bytes are recorded. Recording it is part of
    /// admitting that load: unless `trust_rebuilds` is on, a build whose identity cannot be
    /// written is refused, since an unpinned rule cannot tell the next build from this one.
    std::string content_id;
    /// May its native code run in this process at all?
    bool may_run = false;
    /// What happens when the bytes change under a pinned rule. False (the default) asks
    /// again; true is the development posture, for a person rebuilding this artifact. It never
    /// widens anything: it decides only whether new bytes may run under authority already
    /// granted. A changed `CapabilityAsk` is compiled in, so it is changed bytes too, and
    /// leaving this off is how a person sees every change, that one included.
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

/// THE ONE SPELLING A STORED RULE HAS. An observe entry may be written with or without
/// its verb (`observe Tick v1` / `Tick v1`), and `render_rule` prints the first — so this
/// is what the file keeps, and two entries that mean one permission stop being two
/// strings that compare unequal.
std::string canonical_rule(const std::string& raw, bool observe);

/// Collapse repeated permissions, keeping the person's order (first occurrence wins). A
/// permission list is a set: a duplicate would survive one `authority revoke`, which would
/// report a revocation and leave the permission. Applied when a decision is written and when
/// a hand-edited file is read. Returns how many entries it removed.
std::size_t collapse_duplicates(AuthorityRule* rule);

/// A decision the admission policy could not make and is waiting on, recorded when a load is
/// refused for want of a rule or for changed bytes, so the console can show what to approve.
/// It carries no declared ask: this store refuses at `AdmissionStage::Open`, before the code
/// runs and a manifest exists, and not running unapproved code costs that.
struct PendingDecision {
    std::string artifact;
    std::string path;
    std::string content_id; ///< the build that was actually presented
    std::string pinned;     ///< the build the rule pinned, when there is one
    std::string why;        ///< the refusal, in the policy's own words
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

    /// Replace one decision and write the file now, not at shutdown: a host killed between an
    /// approval and its next boot comes back with the approval. Durable first: the change is
    /// applied to a candidate copy, the candidate is written, and only a successful write
    /// becomes the live policy, so a failed command changes nothing at all, and nothing runs
    /// under an approval the store does not hold.
    bool put(AuthorityRule rule, std::string* error);

    /// Drop one decision entirely and write the file — durable first, exactly as `put`.
    /// Does NOT touch a running weave: the caller is responsible for revoking its live
    /// authority too, and the console does both so a person's single "revoke" means both.
    bool forget(const std::string& artifact, std::string* error);

    /// The admission policy this store implements, which the Kernel asks. It writes the store
    /// only to remember: the first load under an unpinned rule pins its build, and a rebuild
    /// under `trust_rebuilds` pins again. A pin it cannot write never widens what runs (a
    /// first pin refuses unless `trust_rebuilds`). It asks `AdmissionRequest::build` at `Open`
    /// and refuses a build it cannot identify. It never blocks on a person: an undecided load
    /// is refused now, recorded in `pending()`, with the console command named in the refusal.
    AdmissionPolicy policy();

    const std::vector<PendingDecision>& pending() const noexcept { return pending_; }
    void clear_pending() noexcept { pending_.clear(); }

    /// What the policy did without asking, in its own words and order: which build was pinned
    /// to a fresh approval, and that an artifact came up on code the person has not seen,
    /// under authority granted earlier. A silent `trust_rebuilds` would hide what it does.
    const std::vector<std::string>& notes() const noexcept { return notes_; }

private:
    /// Serialize and replace the file from `rules` — never from `rules_`, so a caller can
    /// try a candidate before adopting it.
    bool write(const std::map<std::string, AuthorityRule>& rules, std::string* error) const;
    /// Apply a change to a candidate copy, write it, and adopt it only if the write
    /// landed. The one path `put` and `forget` share.
    bool commit(std::map<std::string, AuthorityRule> candidate, std::string* error);
    void note_pending(PendingDecision d);

    std::string path_;
    std::map<std::string, AuthorityRule> rules_;
    std::vector<PendingDecision> pending_;
    std::vector<std::string> notes_;
};

/// The baseline a policy-admitted artifact gets: the floor plus the right to answer a poke.
/// A baseline is frozen at admission (GATE-05), so everything a person might take back lives
/// in the delegated half; what remains is being inspectable, since a weave a person cannot
/// poke is one they cannot reason about.
Grant admitted_baseline();

} // namespace loom::host

#endif // ZEN_HOST_AUTHORITY_HPP
