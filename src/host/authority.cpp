// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include "authority.hpp"

#include "config_file.hpp"

#include <zen/content_id.hpp>
#include <zen/schema.hpp>
#include <zen/serialize.hpp>
#include <zen/value.hpp>
#include <zen/weave/poke.hpp>

#include <cstdint>
#include <optional>
#include <utility>

namespace loom::host {

namespace {

std::shared_ptr<const loom::Schema> rule_schema() {
    static const auto s = loom::SchemaBuilder("zen.HostAuthorityRule", 1)
                              .field("artifact", loom::Kind::Text)
                              .field("content_id", loom::Kind::Text, /*required=*/false)
                              .field("may_run", loom::Kind::Bool)
                              .field("trust_rebuilds", loom::Kind::Bool, /*required=*/false)
                              .list("send", loom::type_of(loom::Kind::Text))
                              .list("observe", loom::type_of(loom::Kind::Text))
                              .field("note", loom::Kind::Text, /*required=*/false)
                              .build();
    return s;
}

std::shared_ptr<const loom::Schema> store_schema() {
    static const auto s = loom::SchemaBuilder("zen.HostAuthority", 1)
                              .list("rules", loom::type_message(rule_schema()))
                              .build();
    return s;
}

std::string text_or(const loom::Value& v, const char* field, const char* fallback) {
    const loom::Cell* c = v.get(field);
    return (c == nullptr) ? std::string(fallback) : c->as_text();
}

bool bool_or(const loom::Value& v, const char* field, bool fallback) {
    const loom::Cell* c = v.get(field);
    return (c == nullptr) ? fallback : c->as_bool();
}

/// The first eight hex characters of a content id, for a line a person reads. The
/// whole id is in the file; a message that printed all 32 would push the sentence that
/// matters off the end of the line.
std::string brief(const std::string& content_id) {
    return content_id.empty() ? std::string("(unknown)") : content_id.substr(0, 8);
}

// ---- the rule grammar, which is `render_rule` read backwards ----------------

bool split_shape(const std::string& text, std::string* name, std::uint32_t* version,
                 std::string* error) {
    const std::size_t sp = text.find_last_of(' ');
    if (sp == std::string::npos || sp + 2 >= text.size() || text[sp + 1] != 'v') {
        *error = "'" + text + "' is not a shape: expected a name, a space, then vN (e.g. 'Greet v1')";
        return false;
    }
    *name = text.substr(0, sp);
    const std::string digits = text.substr(sp + 2);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
        *error = "'" + text + "' has no version: expected vN after the shape name";
        return false;
    }
    try {
        *version = static_cast<std::uint32_t>(std::stoul(digits));
    } catch (const std::exception&) {
        *error = "'" + text + "' has an unreadable version";
        return false;
    }
    return name->empty() ? (*error = "a rule needs a shape name", false) : true;
}

std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) {
        return {};
    }
    const std::size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

} // namespace

bool apply_rule(const std::string& raw, LiveAuthority* into, std::string* error) {
    const std::string text = trim(raw);
    if (text.empty()) {
        *error = "an empty rule permits nothing and is probably a mistake";
        return false;
    }
    // Observe first, because its verb is a prefix and a send rule never starts with it.
    if (text.rfind("observe ", 0) == 0) {
        const std::string what = trim(text.substr(8));
        if (what == "any shape") {
            into->allow_observe_any();
            return true;
        }
        std::string name;
        std::uint32_t version = 0;
        if (!split_shape(what, &name, &version, error)) {
            return false;
        }
        into->allow_observe(name, version);
        return true;
    }

    const std::size_t arrow = text.find("->");
    if (arrow == std::string::npos) {
        *error = "'" + text +
                 "' has no destination: a send rule is '<Shape> vN -> any target', "
                 "'<Shape> vN -> role <office>', or 'any shape -> any target'";
        return false;
    }
    const std::string lhs = trim(text.substr(0, arrow));
    const std::string rhs = trim(text.substr(arrow + 2));

    if (rhs.rfind("weave #", 0) == 0) {
        // Refused rather than parsed, on purpose. A WeaveId is minted per run, so a
        // file naming one would silently mean a different participant tomorrow — or
        // nobody. Roles exist for exactly this.
        *error = "'" + text +
                 "' names a WeaveId, which is minted fresh every run; a remembered rule must "
                 "name a role ('-> role <office>') or any target";
        return false;
    }

    const bool any_shape = (lhs == "any shape");
    std::string name;
    std::uint32_t version = 0;
    if (!any_shape && !split_shape(lhs, &name, &version, error)) {
        return false;
    }

    if (rhs == "any target") {
        if (any_shape) {
            into->allow_any(); // broad authority, and it took a person writing it out
        } else {
            into->allow_to_any(name, version);
        }
        return true;
    }
    if (rhs.rfind("role ", 0) == 0) {
        const std::string office = trim(rhs.substr(5));
        if (office.empty()) {
            *error = "'" + text + "' names no office after 'role'";
            return false;
        }
        if (any_shape) {
            // LiveAuthority has no any-shape-to-a-role rule, and inventing one here
            // would be inventing authority the bus cannot express. Say so plainly.
            *error = "'" + text +
                     "' cannot be expressed: 'any shape' goes only to 'any target'; name the "
                     "shapes you mean, or grant 'any shape -> any target' deliberately";
            return false;
        }
        into->allow_to_role(name, version, office);
        return true;
    }
    *error = "'" + text + "' has an unreadable destination '" + rhs +
             "': expected 'any target' or 'role <office>'";
    return false;
}

bool to_live_authority(const AuthorityRule& rule, LiveAuthority* out, std::string* error) {
    for (const std::string& r : rule.send) {
        if (!apply_rule(r, out, error)) {
            *error = "artifact '" + rule.artifact + "': " + *error;
            return false;
        }
    }
    for (const std::string& r : rule.observe) {
        // An observe list entry may be written with or without the verb, because a
        // person listing what something may READ has already said which list it is.
        const std::string text = (trim(r).rfind("observe ", 0) == 0) ? r : ("observe " + trim(r));
        if (!apply_rule(text, out, error)) {
            *error = "artifact '" + rule.artifact + "': " + *error;
            return false;
        }
    }
    return true;
}

Grant admitted_baseline() {
    Grant g;
    allow_poke_answers(g);
    return g;
}

// ---- the store --------------------------------------------------------------

bool AuthorityStore::open(const std::string& path, std::string* error) {
    path_ = path;
    rules_.clear();
    bool missing = false;
    std::optional<loom::Value> doc = read_gated_file(path, store_schema(), &missing, error);
    if (missing) {
        return true; // no decisions yet: an empty store, which admits nothing
    }
    if (!doc) {
        *error = "authority store " + *error;
        return false;
    }
    for (const loom::Cell& c : doc->get("rules")->as_list()) {
        const loom::Value& v = *c.as_message();
        AuthorityRule r;
        r.artifact = v.get("artifact")->as_text();
        r.content_id = text_or(v, "content_id", "");
        r.may_run = v.get("may_run")->as_bool();
        r.trust_rebuilds = bool_or(v, "trust_rebuilds", false);
        for (const loom::Cell& s : v.get("send")->as_list()) {
            r.send.push_back(s.as_text());
        }
        for (const loom::Cell& s : v.get("observe")->as_list()) {
            r.observe.push_back(s.as_text());
        }
        r.note = text_or(v, "note", "");
        // EVERY RULE IS PARSED AT LOAD, not at first use. A person who hand-edits this
        // file learns about a typo when the host starts, in a sentence naming the rule
        // — not silently at some later delivery, as a permission that never worked.
        LiveAuthority probe;
        std::string why;
        if (!to_live_authority(r, &probe, &why)) {
            *error = "authority store '" + path + "': " + why;
            return false;
        }
        rules_[r.artifact] = std::move(r);
    }
    return true;
}

const AuthorityRule* AuthorityStore::find(const std::string& artifact) const {
    auto it = rules_.find(artifact);
    return it == rules_.end() ? nullptr : &it->second;
}

std::vector<AuthorityRule> AuthorityStore::rules() const {
    std::vector<AuthorityRule> out;
    out.reserve(rules_.size());
    for (const auto& [name, r] : rules_) {
        out.push_back(r);
    }
    return out;
}

bool AuthorityStore::put(AuthorityRule rule, std::string* error) {
    LiveAuthority probe;
    if (!to_live_authority(rule, &probe, error)) {
        return false; // never write a decision the host could not carry out
    }
    const std::string key = rule.artifact;
    rules_[key] = std::move(rule);
    return write(error);
}

bool AuthorityStore::forget(const std::string& artifact, std::string* error) {
    if (rules_.erase(artifact) == 0) {
        *error = "no standing decision for '" + artifact + "'";
        return false;
    }
    return write(error);
}

bool AuthorityStore::write(std::string* error) const {
    if (path_.empty()) {
        return true; // in-memory only (no --authority given)
    }
    loom::Value v(store_schema());
    std::vector<loom::Cell> entries;
    entries.reserve(rules_.size());
    for (const auto& [name, r] : rules_) {
        loom::Value e(rule_schema());
        e.set("artifact", loom::Cell::text(r.artifact));
        e.set("content_id", loom::Cell::text(r.content_id));
        e.set("may_run", loom::Cell::boolean(r.may_run));
        e.set("trust_rebuilds", loom::Cell::boolean(r.trust_rebuilds));
        std::vector<loom::Cell> send;
        for (const std::string& s : r.send) {
            send.push_back(loom::Cell::text(s));
        }
        e.set("send", loom::Cell::list(std::move(send)));
        std::vector<loom::Cell> obs;
        for (const std::string& s : r.observe) {
            obs.push_back(loom::Cell::text(s));
        }
        e.set("observe", loom::Cell::list(std::move(obs)));
        e.set("note", loom::Cell::text(r.note));
        entries.push_back(loom::Cell::message(std::move(e)));
    }
    v.set("rules", loom::Cell::list(std::move(entries)));
    return write_gated_file(path_, v, error);
}

void AuthorityStore::note_pending(PendingDecision d) {
    for (PendingDecision& p : pending_) {
        if (p.artifact == d.artifact) {
            p = std::move(d); // latest only: one artifact, one outstanding question
            return;
        }
    }
    pending_.push_back(std::move(d));
}

AdmissionPolicy AuthorityStore::policy() {
    // Captures `this`. The store outlives the Kernel in the host's main — declared
    // before it, destroyed after it — which is the ordinary C++ way of saying that a
    // policy may not outlive what it consults.
    return [this](const AdmissionRequest& req) -> AdmissionVerdict {
        if (req.stage == AdmissionStage::Speak) {
            // The bytes were already approved at `open`. What is decided here is only
            // the baseline, and the baseline is the same tiny thing for everything the
            // policy admits: everything a person can revoke lives in the delegated
            // half instead (see admitted_baseline()).
            return AdmissionVerdict::admit(admitted_baseline(),
                                           "baseline: inspectable, and silent until granted");
        }

        const AuthorityRule* rule = find(req.name);
        if (rule == nullptr || !rule->may_run) {
            PendingDecision d;
            d.artifact = req.name;
            d.path = req.path;
            d.content_id = req.content_id;
            d.why = (rule == nullptr)
                        ? "no standing decision for '" + req.name + "'"
                        : "'" + req.name + "' is denied by a standing decision";
            note_pending(d);
            return AdmissionVerdict::refuse(
                d.why + " (build " + brief(req.content_id) + ", " + req.path +
                "); approve it at the console with:  authority trust " + req.name);
        }

        if (req.content_id.empty()) {
            return AdmissionVerdict::refuse(
                "'" + req.name + "' could not be identified: its file at " + req.path +
                " could not be read, so there is no build to check against the decision");
        }

        if (rule->content_id.empty()) {
            // First load under a fresh approval: pin what actually turned up. The
            // person approved an artifact; this records which build that was, so the
            // NEXT different build is a question rather than a silent substitution.
            AuthorityRule pinned = *rule;
            pinned.content_id = req.content_id;
            std::string why;
            (void)put(std::move(pinned), &why); // a failed write loses the pin, not the decision
            notes_.push_back(req.name + ": approved; pinned build " + brief(req.content_id));
            return AdmissionVerdict::admit(Grant{}, notes_.back());
        }

        if (rule->content_id == req.content_id) {
            return AdmissionVerdict::admit(Grant{}, "approved build " + brief(req.content_id));
        }

        if (rule->trust_rebuilds) {
            // THE OLD ID IS COPIED OUT FIRST. `rule` points into `rules_`, and `put`
            // replaces that entry — so reading `rule->content_id` afterwards reports
            // the NEW build as the old one, and the note said "84f68879 -> 84f68879".
            // Found by reading the note it printed.
            const std::string was = rule->content_id;
            AuthorityRule repinned = *rule;
            repinned.content_id = req.content_id;
            std::string why;
            (void)put(std::move(repinned), &why);
            // Said out loud rather than passed over. "Nothing changed" and "you are
            // running code you have not seen before, under authority you granted
            // earlier" are different facts and a person should be able to tell — so
            // this lands in notes(), which the host prints after every boot.
            notes_.push_back(req.name + ": REBUILT since you approved it (" + brief(was) + " -> " +
                             brief(req.content_id) + "), admitted because trust_rebuilds is on");
            return AdmissionVerdict::admit(Grant{}, notes_.back());
        }

        PendingDecision d;
        d.artifact = req.name;
        d.path = req.path;
        d.content_id = req.content_id;
        d.pinned = rule->content_id;
        d.why = "the file changed since it was approved";
        note_pending(d);
        return AdmissionVerdict::refuse(
            "'" + req.name + "' changed since it was approved (approved " + brief(rule->content_id) +
            ", now " + brief(req.content_id) +
            "); its speech authority is unchanged, but the code is not the code that was "
            "approved. Re-approve this build with:  authority trust " + req.name +
            "    or, if you are developing it:  authority trust " + req.name + " --rebuilds");
    };
}

} // namespace loom::host
