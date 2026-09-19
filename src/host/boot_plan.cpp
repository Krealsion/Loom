// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include "boot_plan.hpp"

#include "config_file.hpp"

#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <optional>
#include <utility>

namespace loom::host {

namespace {

// THE PLAN CROSSES THE SAME GATE AS EVERYTHING ELSE. It is a file a person edits, so
// it is exactly the sort of input that is hand-typed, half-finished and occasionally
// wrong — which is why it is a declared schema admitted through `admit()` rather than
// a hand-rolled reader that would have to remember to check every field itself.
std::shared_ptr<const loom::Schema> boot_entry_schema() {
    static const auto s = loom::SchemaBuilder("zen.HostBootEntry", 1)
                              .field("name", loom::Kind::Text)
                              .field("path", loom::Kind::Text)
                              .field("role", loom::Kind::Text, /*required=*/false)
                              .field("enabled", loom::Kind::Bool, /*required=*/false)
                              .field("on_failure", loom::Kind::Text, /*required=*/false)
                              .build();
    return s;
}

std::shared_ptr<const loom::Schema> link_entry_schema() {
    static const auto s = loom::SchemaBuilder("zen.HostLinkEntry", 1)
                              .field("name", loom::Kind::Text)
                              .field("connect", loom::Kind::Text)
                              .field("identity", loom::Kind::Text, /*required=*/false)
                              .field("credential", loom::Kind::Text, /*required=*/false)
                              .build();
    return s;
}

std::shared_ptr<const loom::Schema> history_retain_schema() {
    static const auto s = loom::SchemaBuilder("zen.HostHistoryRetain", 1)
                              .field("shape", loom::Kind::Text)
                              .field("last_n", loom::Kind::Int, /*required=*/false)
                              .field("in_recent", loom::Kind::Bool, /*required=*/false)
                              .field("retain_payload", loom::Kind::Bool, /*required=*/false)
                              .build();
    return s;
}

std::shared_ptr<const loom::Schema> history_keep_schema() {
    static const auto s = loom::SchemaBuilder("zen.HostHistoryKeep", 1)
                              .field("shape", loom::Kind::Text)
                              .field("cap", loom::Kind::Int, /*required=*/false)
                              .build();
    return s;
}

std::shared_ptr<const loom::Schema> history_schema() {
    static const auto s = loom::SchemaBuilder("zen.HostHistory", 1)
                              .field("log", loom::Kind::Text, /*required=*/false)
                              .field("recent", loom::Kind::Int, /*required=*/false)
                              .field("payload_budget", loom::Kind::Int, /*required=*/false)
                              .field("keep_refusals", loom::Kind::Bool, /*required=*/false)
                              .list("retain", loom::type_message(history_retain_schema()),
                                    /*required=*/false)
                              .list("keep", loom::type_message(history_keep_schema()),
                                    /*required=*/false)
                              .build();
    return s;
}

// VERSION 2 ADDS TWO OPTIONAL SECTIONS AND CHANGES NOTHING A v1 FILE SAID: `links` (other
// hosts this one connects to at boot) and `history` (what this host remembers and keeps). A
// file with only `boot` still reads, because the host admits the person's object against the
// schema it knows by name and supplies the version itself.
std::shared_ptr<const loom::Schema> boot_plan_schema() {
    static const auto s = loom::SchemaBuilder("zen.HostBootPlan", 2)
                              .list("boot", loom::type_message(boot_entry_schema()))
                              .list("links", loom::type_message(link_entry_schema()),
                                    /*required=*/false)
                              .message("history", history_schema(), /*required=*/false)
                              .build();
    return s;
}

std::int64_t int_or(const loom::Value& v, const char* field, std::int64_t fallback) {
    const loom::Cell* c = v.get(field);
    return (c == nullptr) ? fallback : c->as_int();
}

std::string text_or(const loom::Value& v, const char* field, const char* fallback) {
    const loom::Cell* c = v.get(field);
    return (c == nullptr) ? std::string(fallback) : c->as_text();
}

bool bool_or(const loom::Value& v, const char* field, bool fallback) {
    const loom::Cell* c = v.get(field);
    return (c == nullptr) ? fallback : c->as_bool();
}

} // namespace

const char* name_of(BootState s) noexcept {
    switch (s) {
    case BootState::Pending:
        return "pending";
    case BootState::Skipped:
        return "skipped";
    case BootState::Started:
        return "started";
    case BootState::Refused:
        return "refused";
    case BootState::Failed:
        return "failed";
    }
    return "?";
}

bool read_boot_plan(const std::string& path, BootPlan* out, std::string* error) {
    out->entries.clear();
    out->source.clear();
    bool missing = false;
    std::optional<loom::Value> v = read_gated_file(path, boot_plan_schema(), &missing, error);
    if (missing) {
        return true; // no plan yet: an empty boot, not a failure. See the header.
    }
    if (!v) {
        *error = "boot plan " + *error;
        return false;
    }
    for (const loom::Cell& c : v->get("boot")->as_list()) {
        const loom::Value& e = *c.as_message();
        BootEntry entry;
        entry.name = e.get("name")->as_text();
        entry.path = e.get("path")->as_text();
        entry.role = text_or(e, "role", "");
        entry.enabled = bool_or(e, "enabled", true);
        const std::string on_fail = text_or(e, "on_failure", "continue");
        if (on_fail == "stop") {
            entry.on_failure = OnFailure::Stop;
        } else if (on_fail == "continue") {
            entry.on_failure = OnFailure::Continue;
        } else {
            // A typo here changes what a boot does, so it is a refusal and not a
            // shrug. Naming both accepted values in the message is the whole fix.
            *error = "boot plan '" + path + "': entry '" + entry.name + "' has on_failure '" +
                     on_fail + "'; it must be 'continue' or 'stop'";
            return false;
        }
        if (entry.name.empty() || entry.path.empty()) {
            *error = "boot plan '" + path + "': every entry needs a non-empty name and path";
            return false;
        }
        out->entries.push_back(std::move(entry));
    }
    if (const loom::Cell* links = v->get("links")) {
        for (const loom::Cell& c : links->as_list()) {
            const loom::Value& e = *c.as_message();
            LinkEntry link;
            link.name = e.get("name")->as_text();
            link.connect = e.get("connect")->as_text();
            link.identity = text_or(e, "identity", "");
            link.credential = text_or(e, "credential", "");
            if (link.name.empty() || link.connect.empty()) {
                *error = "boot plan '" + path + "': every link needs a non-empty name and connect";
                return false;
            }
            for (const LinkEntry& other : out->links) {
                if (other.name == link.name) {
                    *error = "boot plan '" + path + "': two links are named '" + link.name +
                             "', and a name is one office";
                    return false;
                }
            }
            out->links.push_back(std::move(link));
        }
    }
    if (const loom::Cell* h = v->get("history")) {
        const loom::Value& hv = *h->as_message();
        out->history.log = text_or(hv, "log", "");
        out->history.recent = int_or(hv, "recent", 0);
        out->history.payload_budget = int_or(hv, "payload_budget", 0);
        out->history.keep_refusals = bool_or(hv, "keep_refusals", false);
        if (out->history.recent < 0 || out->history.payload_budget < 0) {
            *error = "boot plan '" + path + "': history.recent and history.payload_budget are "
                     "sizes and cannot be negative";
            return false;
        }
        if (const loom::Cell* retain = hv.get("retain")) {
            for (const loom::Cell& c : retain->as_list()) {
                const loom::Value& r = *c.as_message();
                HistoryRetain rule;
                rule.shape = r.get("shape")->as_text();
                rule.last_n = int_or(r, "last_n", 1);
                rule.in_recent = bool_or(r, "in_recent", true);
                rule.retain_payload = bool_or(r, "retain_payload", true);
                if (rule.shape.empty() || rule.last_n < 0) {
                    *error = "boot plan '" + path + "': a history.retain row needs a shape and "
                             "a non-negative last_n";
                    return false;
                }
                out->history.retain.push_back(std::move(rule));
            }
        }
        if (const loom::Cell* keep = hv.get("keep")) {
            for (const loom::Cell& c : keep->as_list()) {
                const loom::Value& k = *c.as_message();
                HistoryKeep rule;
                rule.shape = k.get("shape")->as_text();
                rule.cap = int_or(k, "cap", 0);
                if (rule.shape.empty() || rule.cap < 0) {
                    *error = "boot plan '" + path + "': a history.keep row needs a shape and a "
                             "non-negative cap";
                    return false;
                }
                out->history.keep.push_back(std::move(rule));
            }
        }
    }
    out->source = path;
    return true;
}

std::size_t BootReport::count(BootState s) const {
    std::size_t n = 0;
    for (const BootOutcome& r : rows) {
        if (r.state == s) {
            ++n;
        }
    }
    return n;
}

bool BootReport::complete() const {
    for (const BootOutcome& r : rows) {
        if (r.state != BootState::Started && r.state != BootState::Skipped) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> BootReport::render() const {
    std::vector<std::string> out;
    if (rows.empty()) {
        out.push_back("  boot plan: nothing requested");
        return out;
    }
    for (const BootOutcome& r : rows) {
        std::string line = "  ";
        line += name_of(r.state);
        line.resize(11, ' ');
        line += r.entry.name;
        if (!r.entry.role.empty()) {
            line += " @" + r.entry.role;
        }
        if (r.state == BootState::Started) {
            line += "  (weave " + std::to_string(r.weave) + ")";
        }
        if (!r.detail.empty()) {
            line += "\n              " + r.detail;
        }
        out.push_back(std::move(line));
    }
    // The summary states what a person does next, which is why refused and failed are
    // counted apart: a refusal is answered at the console with an authority decision,
    // a failure is answered in the build or the filesystem.
    std::string tail = "  " + std::to_string(count(BootState::Started)) + " started";
    if (count(BootState::Refused) != 0) {
        tail += ", " + std::to_string(count(BootState::Refused)) + " refused by policy";
    }
    if (count(BootState::Failed) != 0) {
        tail += ", " + std::to_string(count(BootState::Failed)) + " failed";
    }
    if (count(BootState::Pending) != 0) {
        tail += ", " + std::to_string(count(BootState::Pending)) + " never attempted";
    }
    if (count(BootState::Skipped) != 0) {
        tail += ", " + std::to_string(count(BootState::Skipped)) + " skipped";
    }
    tail += complete() ? "  -- boot COMPLETE" : "  -- boot INCOMPLETE";
    out.push_back(std::move(tail));
    return out;
}

} // namespace loom::host
