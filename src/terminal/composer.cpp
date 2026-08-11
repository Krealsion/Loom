// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/terminal/composer.hpp>

#include <zen/kind.hpp>

#include <cstddef>
#include <set>
#include <type_traits>
#include <utility>

namespace loom {

namespace {

std::string type_string(const loom::TypeRef& t) {
    switch (t.kind) {
    case loom::Kind::List:
        return std::string("List<") + type_string(*t.element) + ">";
    case loom::Kind::Message:
        return std::string("Message(") + t.message->name() + " v" +
               std::to_string(t.message->version()) + ")";
    default:
        return loom::name_of(t.kind);
    }
}

// A normalized argument: a literal (FieldValue) or a resolved reference (a scalar Cell),
// carrying the arg's kind for type-directed matching.
struct NArg {
    std::optional<std::string> name;              // set iff named (`field=…`)
    loom::Kind kind;                              // the arg's own type
    std::variant<FieldValue, loom::Cell> payload; // literal, or resolved-reference cell
};

loom::Kind literal_kind(const FieldValue& v) {
    return std::visit(
        [](auto&& x) -> loom::Kind {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                return loom::Kind::Int;
            } else if constexpr (std::is_same_v<T, double>) {
                return loom::Kind::Float;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return loom::Kind::Text;
            } else if constexpr (std::is_same_v<T, bool>) {
                return loom::Kind::Bool;
            } else {
                return loom::Kind::Bytes;
            }
        },
        v);
}

// Place arg `a` into field `f`: a literal coerces among numerics (Int→Float ok), exact
// otherwise; a reference matches its resolved kind EXACTLY (no coercion — predictable
// wiring). Returns the cell to assign, or nullopt if the arg does not fit the field.
std::optional<loom::Cell> place(const loom::Field& f, const NArg& a) {
    if (const loom::Cell* refcell = std::get_if<loom::Cell>(&a.payload)) {
        if (refcell->kind() == f.type.kind) {
            return *refcell;
        }
        return std::nullopt;
    }
    const FieldValue& v = std::get<FieldValue>(a.payload);
    switch (f.type.kind) {
    case loom::Kind::Int:
        if (const auto* p = std::get_if<std::int64_t>(&v)) {
            return loom::Cell::integer(*p);
        }
        break;
    case loom::Kind::Float:
        if (const auto* pd = std::get_if<double>(&v)) {
            return loom::Cell::real(*pd);
        }
        if (const auto* pi = std::get_if<std::int64_t>(&v)) {
            return loom::Cell::real(static_cast<double>(*pi)); // numeric widening Int→Float
        }
        break;
    case loom::Kind::Text:
        if (const auto* p = std::get_if<std::string>(&v)) {
            return loom::Cell::text(*p);
        }
        break;
    case loom::Kind::Bool:
        if (const auto* p = std::get_if<bool>(&v)) {
            return loom::Cell::boolean(*p);
        }
        break;
    case loom::Kind::Bytes:
        if (const auto* p = std::get_if<loom::Bytes>(&v)) {
            return loom::Cell::bytes(*p);
        }
        break;
    default:
        break; // Message/List are not composable from a flat argument list
    }
    return std::nullopt;
}

// Render an unplaced arg for the NeedsInput prompt (domain data; the frontend formats).
std::string render_narg(const NArg& a) {
    if (const loom::Cell* c = std::get_if<loom::Cell>(&a.payload)) {
        switch (c->kind()) {
        case loom::Kind::Int:
            return std::to_string(c->as_int());
        case loom::Kind::Float:
            return std::to_string(c->as_float());
        case loom::Kind::Text:
            return c->as_text();
        case loom::Kind::Bool:
            return c->as_bool() ? "true" : "false";
        default:
            return std::string("(") + loom::name_of(c->kind()) + ")";
        }
    }
    return std::visit(
        [](auto&& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) {
                return std::to_string(x);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return x;
            } else if constexpr (std::is_same_v<T, bool>) {
                return x ? "true" : "false";
            } else {
                return "(bytes)";
            }
        },
        std::get<FieldValue>(a.payload));
}

} // namespace

ShapeDesc describe_schema(const loom::Schema& schema) {
    ShapeDesc d;
    d.name = schema.name();
    d.version = schema.version();
    for (const loom::Field& f : schema.fields()) {
        d.fields.push_back({f.name, type_string(f.type), f.required});
    }
    return d;
}

Composition compose_message(const ComposeSource& source, std::string_view name,
                            std::uint32_t version, const std::vector<Arg>& args) {
    Composition result;
    // On an error the schema is left EXACTLY as far as resolution got: null when the shape itself
    // was not known, set when it was and an argument was wrong. That is the difference between
    // "your terminal has never heard of this" and "you filled it in wrong", and a caller that
    // could not tell them apart would send a person to the wrong place.
    const auto error = [&](const std::string& m) {
        result.status = Composition::Status::Error;
        result.error = m;
        result.cells.clear();
        return result;
    };
    std::shared_ptr<const loom::Schema> schema = source.resolve_schema(name, version);
    if (!schema) {
        return error("no such registered shape: " + std::string(name) + " v" +
                     std::to_string(version));
    }
    result.schema = schema;

    // Normalize: resolve references to scalar Cells up front (a bad reference is a hard error).
    std::vector<NArg> nargs;
    nargs.reserve(args.size());
    for (const Arg& a : args) {
        if (const Ref* ref = std::get_if<Ref>(&a.value)) {
            std::string rerr;
            std::optional<loom::Cell> cell = source.resolve_ref(*ref, &rerr);
            if (!cell) {
                return error(rerr);
            }
            const loom::Kind k = cell->kind();
            nargs.push_back(NArg{a.name, k, std::move(*cell)});
        } else {
            const FieldValue& lit = std::get<FieldValue>(a.value);
            nargs.push_back(NArg{a.name, literal_kind(lit), lit});
        }
    }

    std::map<std::string, loom::Cell> assigned;

    // Rung 1: named wins.
    std::vector<const NArg*> bare;
    for (const NArg& a : nargs) {
        if (a.name) {
            const loom::Field* f = schema->find(*a.name);
            if (f == nullptr) {
                return error("shape " + schema->name() + " has no field '" + *a.name + "'");
            }
            if (assigned.count(*a.name) != 0) {
                return error("field '" + *a.name + "' assigned more than once");
            }
            std::optional<loom::Cell> cell = place(*f, a);
            if (!cell) {
                return error("field '" + *a.name + "' (" + loom::name_of(f->type.kind) +
                             ") cannot take this value");
            }
            assigned.insert_or_assign(*a.name, std::move(*cell));
        } else {
            bare.push_back(&a);
        }
    }

    // Open fields, in declaration order.
    std::vector<const loom::Field*> open;
    for (const loom::Field& f : schema->fields()) {
        if (assigned.count(f.name) == 0) {
            open.push_back(&f);
        }
    }

    // Rungs 2/3: place the bare args — positional, else type-directed, else prompt.
    if (!bare.empty()) {
        bool placed = false;

        // Rung 2: positional (bare[i] → open[i]); accept only if EVERY one fits, else fall.
        if (bare.size() <= open.size()) {
            std::vector<loom::Cell> cells;
            bool all = true;
            for (std::size_t i = 0; i < bare.size(); ++i) {
                std::optional<loom::Cell> c = place(*open[i], *bare[i]);
                if (!c) {
                    all = false;
                    break;
                }
                cells.push_back(std::move(*c));
            }
            if (all) {
                for (std::size_t i = 0; i < bare.size(); ++i) {
                    assigned.insert_or_assign(open[i]->name, std::move(cells[i]));
                }
                open.erase(open.begin(), open.begin() + static_cast<std::ptrdiff_t>(bare.size()));
                placed = true;
            }
        }

        // Rung 3: type-directed (each bare arg → its UNIQUE fitting open field, all distinct).
        if (!placed) {
            std::vector<std::size_t> chosen(bare.size(), 0);
            bool ok = true;
            for (std::size_t i = 0; i < bare.size() && ok; ++i) {
                std::size_t match = 0;
                int count = 0;
                for (std::size_t j = 0; j < open.size(); ++j) {
                    if (place(*open[j], *bare[i]).has_value()) {
                        match = j;
                        ++count;
                    }
                }
                if (count != 1) {
                    ok = false; // no match, or several → ambiguous
                    break;
                }
                chosen[i] = match;
            }
            std::set<std::size_t> matched;
            if (ok) {
                for (std::size_t c : chosen) {
                    if (!matched.insert(c).second) {
                        ok = false; // two args claim the same field → ambiguous
                        break;
                    }
                }
            }
            if (ok) {
                for (std::size_t i = 0; i < bare.size(); ++i) {
                    const loom::Field* f = open[chosen[i]];
                    assigned.insert_or_assign(f->name, std::move(*place(*f, *bare[i])));
                }
                std::vector<const loom::Field*> remaining;
                for (std::size_t j = 0; j < open.size(); ++j) {
                    if (matched.count(j) == 0) {
                        remaining.push_back(open[j]);
                    }
                }
                open.swap(remaining);
                placed = true;
            }
        }

        if (!placed) {
            // Rung 4: genuine ambiguity → prompt (never guess, never mis-send).
            result.status = Composition::Status::NeedsInput;
            for (const loom::Field* f : open) {
                result.open_fields.push_back({f->name, type_string(f->type), f->required});
            }
            for (const NArg* a : bare) {
                result.unplaced.push_back(render_narg(*a));
            }
            return result;
        }
    }

    // A still-open REQUIRED field → prompt rather than knowingly compose incomplete (the gate
    // would refuse it anyway, but prompting is the friendlier floor).
    bool any_required_open = false;
    for (const loom::Field* f : open) {
        if (f->required) {
            any_required_open = true;
        }
    }
    if (any_required_open) {
        result.status = Composition::Status::NeedsInput;
        for (const loom::Field* f : open) {
            result.open_fields.push_back({f->name, type_string(f->type), f->required});
        }
        return result; // unplaced empty: every arg was placed; only required fields remain
    }

    result.status = Composition::Status::Ready;
    result.cells = std::move(assigned);
    return result;
}

loom::Value assemble(const Composition& composition) {
    // construct_blind walks the schema's declared fields and fills each from the cells the ladder
    // placed; an unset field is left ABSENT, to be caught at the gate (the send-time backstop).
    return loom::construct_blind(composition.schema,
                                 [&](const loom::Field& f) -> std::optional<loom::Cell> {
                                     auto it = composition.cells.find(f.name);
                                     if (it == composition.cells.end()) {
                                         return std::nullopt;
                                     }
                                     return it->second;
                                 });
}

} // namespace loom
