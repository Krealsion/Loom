#include <zen/gate.hpp>

#include "detail/gate_internal.hpp"

#include <atomic>
#include <stdexcept>
#include <string>
#include <utility>

namespace zen {

// ---- Error / Admission ----------------------------------------------------

const char* name_of(ErrorKind k) noexcept {
    switch (k) {
    case ErrorKind::None:
        return "None";
    case ErrorKind::SchemaMismatch:
        return "SchemaMismatch";
    case ErrorKind::UnknownSchema:
        return "UnknownSchema";
    case ErrorKind::MissingField:
        return "MissingField";
    case ErrorKind::TypeMismatch:
        return "TypeMismatch";
    case ErrorKind::NullMessage:
        return "NullMessage";
    case ErrorKind::MalformedBytes:
        return "MalformedBytes";
    case ErrorKind::MalformedField:
        return "MalformedField";
    case ErrorKind::UnknownField:
        return "UnknownField";
    }
    return "?";
}

std::string Error::message() const {
    std::string loc = path.empty() ? std::string("<value>") : path;
    std::string s = loc + ": " + name_of(kind);
    if (!expected.empty() || !actual.empty()) {
        s += " (expected " + expected + ", got " + actual + ")";
    }
    if (!detail.empty()) {
        s += " — " + detail;
    }
    return s;
}

Admission Admission::accept(Value v) {
    Admission a;
    a.value_ = std::move(v);
    return a;
}

Admission Admission::reject(Error e) {
    Admission a;
    a.errors_.push_back(std::move(e));
    return a;
}

Admission Admission::reject(std::vector<Error> errors) {
    Admission a;
    if (errors.empty()) {
        errors.push_back(Error{ErrorKind::None, "", "", "", "refused without detail"});
    }
    a.errors_ = std::move(errors);
    return a;
}

const Value& Admission::value() const& {
    if (!value_) {
        throw std::logic_error("Admission::value() called on a refused admission");
    }
    return *value_;
}

Value Admission::value()&& {
    if (!value_) {
        throw std::logic_error("Admission::value() called on a refused admission");
    }
    return std::move(*value_);
}

const Error& Admission::first_error() const {
    if (errors_.empty()) {
        throw std::logic_error("Admission::first_error() called on an accepted admission");
    }
    return errors_.front();
}

// ---- The single validator -------------------------------------------------

namespace {

std::atomic<std::uint64_t> g_gate_calls{0};

std::string version_label(const Schema& s) {
    return s.name() + " v" + std::to_string(s.version());
}

std::string type_label(const TypeRef& t) {
    switch (t.kind) {
    case Kind::Message:
        return "Message(" + version_label(*t.message) + ")";
    case Kind::List:
        return "List<" + type_label(*t.element) + ">";
    default:
        return name_of(t.kind);
    }
}

struct Ctx {
    std::vector<Error>& errs;
    bool collect_all;
    bool stopped = false;

    void emit(Error e) {
        errs.push_back(std::move(e));
        if (!collect_all) {
            stopped = true;
        }
    }
};

void validate_cell(const Cell& c, const TypeRef& t, Ctx& ctx, const std::string& path);

void validate_value(const Value& v, const Schema& door, Ctx& ctx, const std::string& base) {
    for (const Field& f : door.fields()) {
        if (ctx.stopped) {
            return;
        }
        std::string fpath = base.empty() ? f.name : base + "." + f.name;
        const Cell* c = v.get(f.name);
        if (c == nullptr) {
            if (f.required) {
                ctx.emit(Error{ErrorKind::MissingField, fpath, type_label(f.type), "absent", ""});
            }
            continue;
        }
        validate_cell(*c, f.type, ctx, fpath);
    }
}

void validate_cell(const Cell& c, const TypeRef& t, Ctx& ctx, const std::string& path) {
    if (ctx.stopped) {
        return;
    }
    switch (t.kind) {
    case Kind::Int:
    case Kind::Float:
    case Kind::Text:
    case Kind::Bool:
    case Kind::Bytes:
        if (c.kind() != t.kind) {
            ctx.emit(Error{ErrorKind::TypeMismatch, path, name_of(t.kind), name_of(c.kind()), ""});
        }
        return;
    case Kind::Message: {
        if (c.kind() != Kind::Message) {
            ctx.emit(Error{ErrorKind::TypeMismatch, path, type_label(t), name_of(c.kind()), ""});
            return;
        }
        const std::shared_ptr<Value>& nested = c.as_message();
        if (!nested) {
            ctx.emit(Error{ErrorKind::NullMessage, path, type_label(t), "null", ""});
            return;
        }
        if (nested->schema().content_id() != t.message->content_id()) {
            ctx.emit(Error{ErrorKind::SchemaMismatch, path, type_label(t),
                           version_label(nested->schema()), "nested value claims a different shape"});
            return;
        }
        validate_value(*nested, *t.message, ctx, path);
        return;
    }
    case Kind::List: {
        if (c.kind() != Kind::List) {
            ctx.emit(Error{ErrorKind::TypeMismatch, path, type_label(t), name_of(c.kind()), ""});
            return;
        }
        const Cell::Array& arr = c.as_list();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (ctx.stopped) {
                return;
            }
            validate_cell(arr[i], *t.element, ctx, path + "[" + std::to_string(i) + "]");
        }
        return;
    }
    }
}

} // namespace

namespace detail {

std::vector<Error> validate_into(const Value& claimant, const Schema& door, bool collect_all) {
    g_gate_calls.fetch_add(1, std::memory_order_relaxed);

    std::vector<Error> errs;
    Ctx ctx{errs, collect_all};

    // Question 1: identity. Does the claimant claim the shape this door admits?
    if (claimant.schema().content_id() != door.content_id()) {
        errs.push_back(Error{ErrorKind::SchemaMismatch, "", version_label(door),
                             version_label(claimant.schema()),
                             "claimed shape does not match this door"});
        // A different shape cannot be structurally compared against this door;
        // even in Full mode there is nothing meaningful to add.
        return errs;
    }

    // Question 2: structure. Is it genuinely a well-formed instance?
    validate_value(claimant, door, ctx, "");
    return errs;
}

} // namespace detail

// ---- Public gate ----------------------------------------------------------

Admission admit(Value claimant, const Schema& door, Report report) {
    std::vector<Error> errs =
        detail::validate_into(claimant, door, report == Report::Full);
    if (errs.empty()) {
        return Admission::accept(std::move(claimant));
    }
    return Admission::reject(std::move(errs));
}

std::vector<Error> diagnose(const Value& claimant, const Schema& door) {
    return detail::validate_into(claimant, door, /*collect_all=*/true);
}

std::uint64_t gate_invocations() noexcept { return g_gate_calls.load(std::memory_order_relaxed); }

} // namespace zen
