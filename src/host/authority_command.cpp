// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/host/authority_command.hpp>

#include <zen/schema.hpp>
#include <zen/switchboard/grant.hpp>
#include <zen/weave/standard_shapes.hpp>
#include <zen/weaver/vocabulary.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace loom::host {

namespace {

/// A warden's answer as one line: a refusal as a refusal, with its reason, since the reason is
/// the whole value of it; a result as its text.
std::string answer_line(const Value& reply) {
    const std::string& name = reply.schema().name();
    const Cell* value = reply.get("value");
    const Cell* reason = reply.get("reason");
    if (name == Refused::zen_name && reason != nullptr) {
        return "refused: " + reason->as_text();
    }
    if (value != nullptr && value->kind() == Kind::Text) {
        return value->as_text();
    }
    if (name == Ack::zen_name) {
        return "ok";
    }
    return name + " v" + std::to_string(reply.schema().version());
}

/// Make the bus agree with the store for one artifact, through the warden: the only route by
/// which delegated authority changes at a person's request, and it is a message, so it shows on
/// the bus's record like any other.
std::string sync_line(const HostWarden& warden, AuthorityDesk& desk, const std::string& artifact) {
    if (!warden.governs(artifact)) {
        return "nothing loaded under '" + artifact + "' to administer";
    }
    const WardenAnswer a = desk.sync(artifact);
    return a.reply ? answer_line(*a.reply) : a.trouble;
}

/// Is `text` still permitted by what `rule` leaves behind? Asked of the AUTHORITY, not
/// of the list of strings, so `revoke Spin v1 -> role probe` under a standing
/// `any shape -> any target` says the honest thing instead of implying the permission is
/// gone. The two lists are different vocabularies for the same wall.
bool still_permitted(const AuthorityRule& rule, const std::string& text) {
    loom::LiveAuthority remaining;
    loom::LiveAuthority revoked;
    std::string ignored;
    if (!apply_rule(text, &revoked, &ignored)) {
        return false;
    }
    if (!to_live_authority(rule, &remaining, &ignored)) {
        return false;
    }
    return remaining.contains(revoked);
}

} // namespace

void run_authority_command(AuthorityStore& store, const HostWarden& warden, AuthorityDesk& desk,
                           const std::vector<std::string>& words, std::ostream& out) {
    const std::string sub = words.size() > 1 ? words[1] : "";

    if (sub.empty()) {
        const std::vector<AuthorityRule> all = store.rules();
        if (all.empty()) {
            out << "  no standing decisions yet. Nothing may run.\n"
                   "  approve something with:  authority trust <name>\n";
            return;
        }
        for (const AuthorityRule& r : all) {
            out << "  " << (r.may_run ? "run " : "DENY") << "  " << r.artifact;
            if (!r.content_id.empty()) {
                out << "  build " << r.content_id.substr(0, 8);
            }
            if (r.trust_rebuilds) {
                out << "  (+rebuilds)";
            }
            out << "  " << r.send.size() << " send, " << r.observe.size() << " observe\n";
        }
        return;
    }

    if (sub == "pending") {
        if (store.pending().empty()) {
            out << "  nothing is waiting on you.\n";
            return;
        }
        for (const PendingDecision& p : store.pending()) {
            out << "  " << p.artifact << "  " << p.path << '\n';
            out << "    " << p.why << '\n';
            out << "    build presented: " << (p.content_id.empty() ? "(unreadable)"
                                                                    : p.content_id)
                << '\n';
            if (!p.pinned.empty()) {
                out << "    build approved:  " << p.pinned << '\n';
            }
        }
        return;
    }

    if (words.size() < 3) {
        out << "  usage: authority show|trust|deny|allow|revoke|forget <name> [...]\n";
        return;
    }
    const std::string name = words[2];
    const AuthorityRule* existing = store.find(name);
    std::string error;

    if (sub == "show") {
        if (existing == nullptr) {
            out << "  no standing decision for '" << name << "'\n";
        } else {
            out << "  " << name << (existing->may_run ? ": may run" : ": may NOT run") << '\n';
            out << "    build pinned: "
                << (existing->content_id.empty() ? "(not yet)" : existing->content_id)
                << (existing->trust_rebuilds ? "   (any rebuild is accepted)" : "") << '\n';
            for (const std::string& r : existing->send) {
                out << "    may say:  " << r << '\n';
            }
            for (const std::string& r : existing->observe) {
                out << "    may read: " << r << '\n';
            }
            if (!existing->note.empty()) {
                out << "    note: " << existing->note << '\n';
            }
        }
        // AND WHAT THE BUS ACTUALLY THINKS, which is the line that matters: asked of
        // the live authority through the warden, so a file that disagrees with the
        // running system shows up here instead of being believed. Asked as a MESSAGE,
        // by the operator seat, exactly as anything else here is — and settled on this
        // host's own conversation, so it is this artifact's authority and not whatever
        // description happened to arrive most recently.
        const WardenAnswer a = desk.describe(name);
        if (!a.reply) {
            out << "    (could not ask: " << a.trouble << ")\n";
            return;
        }
        if (a.reply->schema().name() != loom::AuthorityDescription::zen_name) {
            out << "    " << answer_line(*a.reply) << '\n';
            return;
        }
        out << "    LIVE, weave " << a.reply->get("subject")->as_int() << ":\n";
        const auto print_list = [&out](const loom::Cell* list, const char* label) {
            if (list == nullptr || list->as_list().empty()) {
                out << "      " << label << ": nothing\n";
                return;
            }
            for (const loom::Cell& c : list->as_list()) {
                out << "      " << label << ": " << c.as_text() << '\n';
            }
        };
        print_list(a.reply->get("base"), "baseline (frozen at admission)");
        print_list(a.reply->get("delegated"), "delegated (revocable)");
        return;
    }

    AuthorityRule rule = (existing != nullptr) ? *existing : AuthorityRule{};
    rule.artifact = name;

    if (sub == "trust") {
        rule.may_run = true;
        for (std::size_t i = 3; i < words.size(); ++i) {
            if (words[i] == "--rebuilds") {
                rule.trust_rebuilds = true;
            } else if (words[i] == "--ask-again") {
                // The way back: without it, turning `--rebuilds` off would mean hand-editing
                // the file, a poor answer for the one switch that lets new code run unasked.
                rule.trust_rebuilds = false;
            } else {
                out << "  unknown option '" << words[i] << "'\n";
                return;
            }
        }
        // Re-pin from whatever the policy last saw refused, if anything, so "trust"
        // after a changed-file refusal approves THE BUILD THAT WAS REFUSED and not some
        // other one. Otherwise clear the pin and let the next load record it.
        rule.content_id.clear();
        for (const PendingDecision& p : store.pending()) {
            if (p.artifact == name && !p.content_id.empty()) {
                rule.content_id = p.content_id;
            }
        }
        const bool trusts_rebuilds = rule.trust_rebuilds;
        if (!store.put(std::move(rule), &error)) {
            // NOTHING CHANGED, AND THAT IS WHY THE SENTENCE CAN SAY SO. The store writes
            // the file before it changes what this host permits, so a failed command is
            // a command that did not happen — rather than an approval good for this
            // session that the next boot has never heard of.
            out << "  " << error << "\n  '" << name
                << "' is unchanged: it still may " << (existing != nullptr && existing->may_run
                                                           ? "run"
                                                           : "NOT run")
                << ", here and after a restart.\n";
            return;
        }
        // BOTH HALVES OF WHAT WAS JUST DECIDED, because a person who typed one word is
        // owed the other: this grants the right to RUN and nothing else, and whether a
        // later rebuild will ask again is exactly what the flag changed.
        out << "  '" << name << "' may run"
            << (trusts_rebuilds ? ", and so will any rebuild of it"
                                : "; a rebuild of it will ask again")
            << ". It may still say nothing until you allow something.\n";
        return;
    }

    if (sub == "deny") {
        rule.may_run = false;
        if (!store.put(std::move(rule), &error)) {
            out << "  " << error << "\n  '" << name << "' is unchanged.\n";
            return;
        }
        out << "  '" << name << "' may not run. A copy already loaded keeps running — "
               "end it with 'stop " << name << "'.\n";
        return;
    }

    if (sub == "allow") {
        if (words.size() < 4) {
            out << "  usage: authority allow <name> <rule>\n"
                   "  rules read exactly as 'authority show' prints them:\n"
                   "    Greet v1 -> any target     Tick v1 -> role clock\n"
                   "    any shape -> any target    observe Tick v1\n";
            return;
        }
        std::string text;
        for (std::size_t i = 3; i < words.size(); ++i) {
            text += (i == 3 ? "" : " ") + words[i];
        }
        loom::LiveAuthority probe;
        if (!apply_rule(text, &probe, &error)) {
            out << "  " << error << '\n';
            return;
        }
        const bool observe = text.rfind("observe", 0) == 0;
        const std::string canonical = canonical_rule(text, observe);
        // Already granted is said, not stored twice: a second copy would survive one
        // `revoke`, which would report a revocation and leave the permission installed, and a
        // person retrying a command must not make their own decision unrevokable.
        std::vector<std::string>& into = observe ? rule.observe : rule.send;
        for (const std::string& r : into) {
            if (canonical_rule(r, observe) == canonical) {
                out << "  '" << name << "' is already allowed '" << canonical
                    << "' — nothing to add. One 'authority revoke " << name << " "
                    << canonical << "' takes it back.\n";
                return;
            }
        }
        into.push_back(canonical);
        if (!store.put(std::move(rule), &error)) {
            out << "  " << error << "\n  nothing was allowed; '" << name
                << "' permits exactly what it permitted before.\n";
            return;
        }
        out << "  remembered. " << sync_line(warden, desk, name) << '\n';
        return;
    }

    if (sub == "revoke") {
        if (existing == nullptr) {
            out << "  no standing decision for '" << name << "'\n";
            return;
        }
        std::string text;
        const bool all = words.size() == 3;
        if (all) {
            rule.send.clear();
            rule.observe.clear();
        } else {
            for (std::size_t i = 3; i < words.size(); ++i) {
                text += (i == 3 ? "" : " ") + words[i];
            }
            // Every copy, not the first: the store collapses duplicates, but a file edited by
            // hand between two runs may still hold two, and a revoke that leaves one behind
            // leaves the permission.
            std::size_t removed = 0;
            const auto drop = [&removed](std::vector<std::string>& v, const std::string& want,
                                         bool observe) {
                for (auto it = v.begin(); it != v.end();) {
                    if (canonical_rule(*it, observe) == want) {
                        it = v.erase(it);
                        ++removed;
                    } else {
                        ++it;
                    }
                }
            };
            drop(rule.send, canonical_rule(text, /*observe=*/false), false);
            drop(rule.observe, canonical_rule(text, /*observe=*/true), true);
            if (removed == 0) {
                out << "  '" << text << "' is not one of its rules (see 'authority show "
                    << name << "')\n";
                return;
            }
        }
        const AuthorityRule after = rule; // for the honest effective-authority line
        if (!store.put(std::move(rule), &error)) {
            out << "  " << error << "\n  nothing was revoked; '" << name
                << "' permits exactly what it permitted before.\n";
            return;
        }
        // BOTH HALVES, from one word. The file is the memory and the bus is the effect;
        // a revoke that changed only one of them would be a promise half kept.
        out << "  revoked, for this run and the next. " << sync_line(warden, desk, name) << '\n';
        // AND THE HONEST REMAINDER. A narrow rule taken back under a broad one that is
        // still standing has changed the list and changed nothing about what the weave
        // may do, and a person reading "revoked" deserves to be told which they got.
        if (!all && still_permitted(after, text)) {
            out << "  NOTE: '" << text << "' is STILL PERMITTED by another rule you have "
                   "granted '" << name << "'. See 'authority show " << name << "'.\n";
        }
        return;
    }

    if (sub == "forget") {
        if (!store.forget(name, &error)) {
            out << "  " << error << '\n';
            return;
        }
        out << "  forgotten. " << sync_line(warden, desk, name) << '\n';
        return;
    }

    out << "  unknown: authority " << sub << " (try 'help')\n";
}

const char* authority_help() {
    return R"(  authority                     your standing decisions
  authority show <name>         one decision, and what the bus actually permits now
  authority pending             what the policy refused and is waiting on
  authority trust <name> [--rebuilds|--ask-again]
                                it may run, pinned to this build; --rebuilds accepts any
                                rebuild of it, --ask-again turns that back off
  authority deny <name>         it may not run
  authority allow <name> <rule> it may say one more thing, e.g.  Greet v1 -> any target
  authority revoke <name> [rule]   take one back, or all of them; live and remembered
  authority forget <name>       drop the decision entirely
)";
}

} // namespace loom::host
