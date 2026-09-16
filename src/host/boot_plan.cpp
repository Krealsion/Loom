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

std::shared_ptr<const loom::Schema> boot_plan_schema() {
    static const auto s = loom::SchemaBuilder("zen.HostBootPlan", 1)
                              .list("boot", loom::type_message(boot_entry_schema()))
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
