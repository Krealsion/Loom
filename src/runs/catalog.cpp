// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include "catalog.hpp"

#include "../detail/json.hpp"
#include "../detail/sha256.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace loom::runs {

namespace {

using loom::detail::JsonValue;
using JT = JsonValue::Type;

std::string hex(const std::array<std::uint8_t, 32>& d) {
    static const char* const digits = "0123456789abcdef";
    std::string out(64, '0');
    for (std::size_t i = 0; i < d.size(); ++i) {
        out[i * 2] = digits[(d[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[d[i] & 0xF];
    }
    return out;
}

bool read_text(const std::filesystem::path& p, std::string* out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

/// One key of an object the file must declare exactly: an unknown key is a problem.
bool only_keys(const JsonValue& obj, std::initializer_list<const char*> allowed, const std::string& where,
               std::string* why) {
    for (const auto& [key, value] : obj.members) {
        (void)value;
        bool known = false;
        for (const char* a : allowed) {
            known = known || key == a;
        }
        if (!known) {
            *why = where + ": '" + key + "' is not a key this file defines";
            return false;
        }
    }
    return true;
}

bool text_of(const JsonValue& obj, const char* key, bool required, std::string* out,
             const std::string& where, std::string* why) {
    const JsonValue* v = obj.find(key);
    if (v == nullptr) {
        if (required) {
            *why = where + ": '" + key + "' is required";
            return false;
        }
        return true;
    }
    if (v->type != JT::String) {
        *why = where + ": '" + key + "' must be a string";
        return false;
    }
    *out = v->text;
    return true;
}

bool texts_of(const JsonValue& obj, const char* key, std::vector<std::string>* out,
              const std::string& where, std::string* why) {
    const JsonValue* v = obj.find(key);
    if (v == nullptr) {
        return true;
    }
    if (v->type != JT::Array) {
        *why = where + ": '" + key + "' must be a list of strings";
        return false;
    }
    for (const JsonValue& e : v->items) {
        if (e.type != JT::String) {
            *why = where + ": every entry of '" + key + "' must be a string";
            return false;
        }
        out->push_back(e.text);
    }
    return true;
}

/// A JSON literal re-spelled as text: what `default_json` holds.
std::string literal(const JsonValue& v) {
    switch (v.type) {
    case JT::String: {
        std::string out;
        loom::detail::json_quote(v.text, out);
        return out;
    }
    case JT::Number:
        return v.text;
    case JT::Bool:
        return v.boolean ? "true" : "false";
    default:
        return "null";
    }
}

bool integral(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    std::size_t i = token[0] == '-' ? 1 : 0;
    if (i >= token.size()) {
        return false;
    }
    for (; i < token.size(); ++i) {
        if (token[i] < '0' || token[i] > '9') {
            return false;
        }
    }
    return true;
}

bool fits(const JsonValue& v, const std::string& type) {
    if (type == "text") {
        return v.type == JT::String;
    }
    if (type == "bool") {
        return v.type == JT::Bool;
    }
    if (type == "int") {
        return v.type == JT::Number && integral(v.text);
    }
    if (type == "number") {
        return v.type == JT::Number;
    }
    return false;
}

bool read_tool(const JsonValue& t, const std::string& where, ToolSpec* tool, std::string* why) {
    if (t.type != JT::Object) {
        *why = where + ": a tool is an object";
        return false;
    }
    if (!only_keys(t, {"name", "script", "summary", "description", "inputs", "outputs", "requires",
                       "asks", "vocabulary", "terms", "example", "recovery"},
                   where, why) ||
        !text_of(t, "name", true, &tool->name, where, why) ||
        !text_of(t, "script", true, &tool->script, where, why) ||
        !text_of(t, "summary", true, &tool->summary, where, why) ||
        !text_of(t, "description", false, &tool->description, where, why) ||
        !text_of(t, "example", false, &tool->example, where, why) ||
        !text_of(t, "recovery", false, &tool->recovery, where, why) ||
        !texts_of(t, "requires", &tool->requires_, where, why) ||
        !texts_of(t, "asks", &tool->asks, where, why) ||
        !texts_of(t, "vocabulary", &tool->vocabulary, where, why) ||
        !texts_of(t, "terms", &tool->terms, where, why)) {
        return false;
    }
    const std::string here = where + " tool '" + tool->name + "'";
    if (tool->name.empty() || tool->name.find('/') != std::string::npos) {
        *why = where + ": a tool's name is non-empty and has no '/'";
        return false;
    }
    if (tool->script.find("..") != std::string::npos ||
        std::filesystem::path(tool->script).is_absolute()) {
        *why = here + ": its script is a path inside the package";
        return false;
    }
    if (const JsonValue* inputs = t.find("inputs")) {
        if (inputs->type != JT::Array) {
            *why = here + ": 'inputs' is a list";
            return false;
        }
        for (const JsonValue& i : inputs->items) {
            InputSpec in;
            if (i.type != JT::Object ||
                !only_keys(i, {"name", "type", "required", "default", "help"}, here, why) ||
                !text_of(i, "name", true, &in.name, here, why) ||
                !text_of(i, "type", true, &in.type, here, why) ||
                !text_of(i, "help", false, &in.help, here, why)) {
                if (why->empty()) {
                    *why = here + ": an input is an object";
                }
                return false;
            }
            if (in.type != "text" && in.type != "int" && in.type != "bool" && in.type != "number") {
                *why = here + ": input '" + in.name + "' has type '" + in.type +
                       "'; the types are text, int, bool and number";
                return false;
            }
            if (const JsonValue* r = i.find("required")) {
                if (r->type != JT::Bool) {
                    *why = here + ": input '" + in.name + "': 'required' is true or false";
                    return false;
                }
                in.required = r->boolean;
            }
            if (const JsonValue* d = i.find("default")) {
                if (!fits(*d, in.type)) {
                    *why = here + ": input '" + in.name + "' has a default that is not " + in.type;
                    return false;
                }
                in.has_default = true;
                in.default_json = literal(*d);
            }
            tool->inputs.push_back(std::move(in));
        }
    }
    if (const JsonValue* outputs = t.find("outputs")) {
        if (outputs->type != JT::Array) {
            *why = here + ": 'outputs' is a list";
            return false;
        }
        for (const JsonValue& o : outputs->items) {
            OutputSpec out;
            if (o.type != JT::Object || !only_keys(o, {"name", "help"}, here, why) ||
                !text_of(o, "name", true, &out.name, here, why) ||
                !text_of(o, "help", false, &out.help, here, why)) {
                if (why->empty()) {
                    *why = here + ": an output is an object";
                }
                return false;
            }
            tool->outputs.push_back(std::move(out));
        }
    }
    return true;
}

bool read_manifest(const std::filesystem::path& dir, Package* p, std::string* why) {
    const std::filesystem::path file = dir / "loom-tool.json";
    std::string text;
    if (!read_text(file, &text)) {
        *why = "no readable loom-tool.json in " + dir.string();
        return false;
    }
    const loom::detail::JsonParse parsed = loom::detail::parse_json(text, 16);
    if (!parsed.ok) {
        *why = file.string() + ": not valid JSON: " + parsed.error;
        return false;
    }
    const JsonValue& root = parsed.value;
    const std::string where = file.string();
    if (root.type != JT::Object) {
        *why = where + ": the manifest is one object";
        return false;
    }
    if (!only_keys(root, {"package", "version", "summary", "uses", "tools"}, where, why) ||
        !text_of(root, "package", true, &p->name, where, why) ||
        !text_of(root, "version", false, &p->version, where, why) ||
        !text_of(root, "summary", false, &p->summary, where, why) ||
        !texts_of(root, "uses", &p->uses, where, why)) {
        return false;
    }
    if (p->name.empty() || p->name.find('/') != std::string::npos) {
        *why = where + ": a package's name is non-empty and has no '/'";
        return false;
    }
    if (p->uses.size() > kMaxUses) {
        *why = where + ": 'uses' names at most " + std::to_string(kMaxUses) + " packages";
        return false;
    }
    for (std::size_t i = 0; i < p->uses.size(); ++i) {
        const std::string& u = p->uses[i];
        if (u.empty() || u.find('/') != std::string::npos || u == p->name ||
            std::find(p->uses.begin(), p->uses.begin() + static_cast<std::ptrdiff_t>(i), u) !=
                p->uses.begin() + static_cast<std::ptrdiff_t>(i)) {
            *why = where + ": 'uses' names other packages of the catalog by name, each once";
            return false;
        }
    }
    const JsonValue* tools = root.find("tools");
    if (tools == nullptr || tools->type != JT::Array) {
        *why = where + ": 'tools' is a list";
        return false;
    }
    if (tools->items.size() > kMaxToolsPerPackage) {
        *why = where + ": more than " + std::to_string(kMaxToolsPerPackage) + " tools";
        return false;
    }
    for (const JsonValue& t : tools->items) {
        ToolSpec tool;
        if (!read_tool(t, where, &tool, why)) {
            return false;
        }
        p->tools.push_back(std::move(tool));
    }
    return true;
}

/// The files a package's digest and snapshot cover, sorted by relative path.
bool package_files(const std::filesystem::path& dir,
                   std::vector<std::pair<std::string, std::filesystem::path>>* files,
                   std::string* why) {
    std::error_code ec;
    std::uintmax_t bytes = 0;
    std::filesystem::recursive_directory_iterator it(dir, ec);
    if (ec) {
        *why = "cannot read " + dir.string() + ": " + ec.message();
        return false;
    }
    for (const std::filesystem::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            *why = "cannot read " + dir.string() + ": " + ec.message();
            return false;
        }
        const std::filesystem::directory_entry& e = *it;
        const std::string leaf = e.path().filename().string();
        if (e.is_symlink(ec)) {
            *why = e.path().string() + " is a symbolic link; a package holds its own files";
            return false;
        }
        if (e.is_directory(ec)) {
            if (leaf == "__pycache__") {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!e.is_regular_file(ec) ||
            (leaf.size() > 4 && leaf.compare(leaf.size() - 4, 4, ".pyc") == 0)) {
            continue;
        }
        bytes += e.file_size(ec);
        if (files->size() >= kMaxPackageFiles || bytes > kMaxPackageBytes) {
            *why = dir.string() + " holds more than " + std::to_string(kMaxPackageFiles) +
                   " files or " + std::to_string(kMaxPackageBytes) + " bytes";
            return false;
        }
        files->emplace_back(std::filesystem::relative(e.path(), dir, ec).generic_string(), e.path());
    }
    std::sort(files->begin(), files->end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return true;
}

} // namespace

const Package* Catalog::find(const std::string& id, const ToolSpec** tool) const {
    *tool = nullptr;
    const std::size_t slash = id.find('/');
    if (slash == std::string::npos) {
        return nullptr;
    }
    const std::string pkg = id.substr(0, slash);
    const std::string name = id.substr(slash + 1);
    for (const Package& p : packages) {
        if (p.name != pkg || !p.problem.empty()) {
            continue;
        }
        for (const ToolSpec& t : p.tools) {
            if (t.name == name) {
                *tool = &t;
                return &p;
            }
        }
    }
    return nullptr;
}

const Package* Catalog::package(const std::string& name) const {
    for (const Package& p : packages) {
        if (p.name == name && p.problem.empty()) {
            return &p;
        }
    }
    return nullptr;
}

bool uses_of(const Catalog& c, const Package& p, std::vector<const Package*>* used,
             std::string* why) {
    used->clear();
    for (const std::string& name : p.uses) {
        const Package* u = c.package(name);
        if (u == nullptr) {
            *why = "package '" + p.name + "' uses '" + name + "', which the catalog " + c.file +
                   " does not list (or could not read)";
            return false;
        }
        if (!u->uses.empty()) {
            *why = "package '" + p.name + "' uses '" + name + "', which uses others itself; a " +
                   "package builds on packages that use nothing";
            return false;
        }
        used->push_back(u);
    }
    return true;
}

std::string file_sha256(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    loom::detail::Sha256 h;
    std::array<char, 1 << 16> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n > 0) {
            h.update(buf.data(), static_cast<std::size_t>(n));
        }
    }
    if (in.bad()) {
        return {};
    }
    return hex(h.digest());
}

std::string package_revision(const std::filesystem::path& dir, std::string* why) {
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    if (!package_files(dir, &files, why)) {
        return {};
    }
    loom::detail::Sha256 h;
    for (const auto& [rel, full] : files) {
        const std::string digest = file_sha256(full);
        if (digest.empty()) {
            *why = "cannot read " + full.string();
            return {};
        }
        h.update(rel);
        h.update(std::string_view("\0", 1));
        h.update(digest);
        h.update("\n");
    }
    return hex(h.digest());
}

bool snapshot_package(const std::filesystem::path& from, const std::filesystem::path& to,
                      std::string* why) {
    std::error_code ec;
    if (std::filesystem::exists(to, ec)) {
        *why = to.string() + " already exists; a snapshot is never written over another";
        return false;
    }
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    if (!package_files(from, &files, why)) {
        return false;
    }
    for (const auto& [rel, full] : files) {
        const std::filesystem::path dest = to / std::filesystem::path(rel);
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec || !std::filesystem::copy_file(full, dest, std::filesystem::copy_options::none, ec) || ec) {
            *why = "cannot copy " + full.string() + " into the run's snapshot: " +
                   (ec ? ec.message() : std::string("copy failed"));
            return false;
        }
    }
    return true;
}

bool approved(const Package& p, const std::string& revision, std::string* why) {
    if (p.approve == "any-revision") {
        return true;
    }
    if (p.approve.empty()) {
        *why = "package '" + p.name + "' is not approved to run; add \"approve\": \"" + revision +
               "\" (this revision) or \"any-revision\" (every edit) to its entry in the catalog";
        return false;
    }
    if (p.approve != revision) {
        *why = "package '" + p.name + "' changed since it was approved (approved " +
               p.approve.substr(0, 12) + ", now " + revision.substr(0, 12) +
               "); approve this revision in the catalog, or \"any-revision\" while you edit it";
        return false;
    }
    return true;
}

std::string check_inputs(const ToolSpec& tool, const std::string& json, std::string* why) {
    const std::string text = json.empty() ? std::string("{}") : json;
    if (text.size() > 16 * 1024) {
        *why = "a run's inputs are at most 16 KiB of JSON";
        return {};
    }
    const loom::detail::JsonParse parsed = loom::detail::parse_json(text, 8);
    if (!parsed.ok || parsed.value.type != JT::Object) {
        *why = "a run's inputs are one JSON object" +
               (parsed.ok ? std::string() : std::string(": ") + parsed.error);
        return {};
    }
    for (const auto& [key, value] : parsed.value.members) {
        (void)value;
        bool known = false;
        for (const InputSpec& in : tool.inputs) {
            known = known || in.name == key;
        }
        if (!known) {
            std::string names;
            for (const InputSpec& in : tool.inputs) {
                names += (names.empty() ? "" : ", ") + in.name;
            }
            *why = "'" + key + "' is not an input of " + tool.name + " (its inputs: " +
                   (names.empty() ? std::string("none") : names) + ")";
            return {};
        }
    }
    std::string out = "{";
    bool first = true;
    for (const InputSpec& in : tool.inputs) {
        const JsonValue* v = parsed.value.find(in.name);
        std::string spelled;
        if (v != nullptr) {
            if (!fits(*v, in.type)) {
                *why = "input '" + in.name + "' must be " + in.type;
                return {};
            }
            spelled = literal(*v);
        } else if (in.has_default) {
            spelled = in.default_json;
        } else if (in.required) {
            *why = "input '" + in.name + "' is required (" + in.help + ")";
            return {};
        } else {
            continue;
        }
        out += first ? "" : ",";
        first = false;
        loom::detail::json_quote(in.name, out);
        out += ":" + spelled;
    }
    out += "}";
    return out;
}

Catalog read_catalog(const std::string& file) {
    Catalog c;
    std::error_code ec;
    c.file = std::filesystem::absolute(file, ec).string();
    std::string text;
    if (!read_text(c.file, &text)) {
        c.problems.push_back("no catalog: " + c.file + " does not exist yet (it names the tool "
                             "packages this session knows, and which may run)");
        return c;
    }
    c.present = true;
    const loom::detail::JsonParse parsed = loom::detail::parse_json(text, 16);
    std::string why;
    if (!parsed.ok || parsed.value.type != JT::Object) {
        c.problems.push_back(c.file + ": not a JSON object" +
                             (parsed.ok ? std::string() : ": " + parsed.error));
        return c;
    }
    const JsonValue& root = parsed.value;
    if (!only_keys(root, {"python", "runtime", "packages"}, c.file, &why) ||
        !text_of(root, "python", false, &c.python, c.file, &why) ||
        !text_of(root, "runtime", false, &c.runtime, c.file, &why)) {
        c.problems.push_back(why);
        return c;
    }
    const std::filesystem::path base = std::filesystem::path(c.file).parent_path();
    if (!c.runtime.empty() && std::filesystem::path(c.runtime).is_relative()) {
        c.runtime = (base / c.runtime).string();
    }
    const JsonValue* packages = root.find("packages");
    if (packages == nullptr) {
        return c;
    }
    if (packages->type != JT::Array) {
        c.problems.push_back(c.file + ": 'packages' is a list");
        return c;
    }
    for (const JsonValue& entry : packages->items) {
        if (c.packages.size() >= kMaxPackages) {
            c.problems.push_back(c.file + ": more than " + std::to_string(kMaxPackages) +
                                 " packages; the rest were not read");
            break;
        }
        Package p;
        why.clear();
        if (entry.type != JT::Object ||
            !only_keys(entry, {"path", "approve"}, c.file, &why) ||
            !text_of(entry, "path", true, &p.declared, c.file, &why) ||
            !text_of(entry, "approve", false, &p.approve, c.file, &why)) {
            c.problems.push_back(why.empty() ? c.file + ": a package entry is an object" : why);
            continue;
        }
        if (!p.approve.empty() && p.approve != "any-revision" &&
            (p.approve.size() != 64 ||
             p.approve.find_first_not_of("0123456789abcdef") != std::string::npos)) {
            c.problems.push_back(c.file + ": package '" + p.declared + "': \"approve\" is " +
                                 "\"any-revision\" or a 64-hex revision");
            p.approve.clear();
        }
        std::filesystem::path dir(p.declared);
        if (dir.is_relative()) {
            dir = base / dir;
        }
        p.dir = std::filesystem::weakly_canonical(dir, ec).string();
        if (!read_manifest(p.dir, &p, &why)) {
            p.problem = why;
            c.problems.push_back(why);
        } else {
            p.revision = package_revision(p.dir, &why);
            if (p.revision.empty()) {
                p.problem = why;
                c.problems.push_back(why);
            }
        }
        c.packages.push_back(std::move(p));
    }
    return c;
}

} // namespace loom::runs
