// nucleus.cpp — a seed of Zen.
//
// The decision this resolves:
//   How does a message represent itself?
//
//   * Not a compile-time-only C++ struct. Then the in-engine console
//     cannot construct a message for a Shard whose type was never
//     compiled in — the door is shut to anything new.
//   * Not an opaque serialized blob. Then nothing can be *challenged*
//     at a boundary; the blob demands blind trust from its own future.
//
//   The synthesis is a value that CARRIES ITS OWN SHAPE: typed enough
//   to be challenged at any threshold, dynamic enough to be built at
//   runtime from a shape that was discovered, not compiled.
//
// One rule lives in this file:
//   Nothing crosses a boundary it cannot prove it belongs across.
//   The bus uses it. Persistence uses it. It is the same function.
//
// The kernel holds the grammar — never the answers.

#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace zen {

// ---- The grammar the kernel holds (and only the grammar) ------------------

struct Schema;
struct Value;

enum class Kind { Int, Float, Text, Bool, Message };

const char* name_of(Kind k) {
    switch (k) {
        case Kind::Int:     return "Int";
        case Kind::Float:   return "Float";
        case Kind::Text:    return "Text";
        case Kind::Bool:    return "Bool";
        case Kind::Message: return "Message";
    }
    return "?";
}

struct Field {
    std::string name;
    Kind kind;
    std::shared_ptr<const Schema> schema; // non-null iff kind == Message
    bool required = true;
};

struct Schema {
    std::string name;
    std::vector<Field> fields;
};

// A cell holds one field's data. A cell may itself hold a whole Value,
// so a policy can be a message, a message can carry a message, and the
// one boundary rule recurses to every depth without changing.
using Cell = std::variant<std::int64_t, double, std::string, bool,
                          std::shared_ptr<Value>>;

struct Value {
    std::shared_ptr<const Schema> schema; // what this value CLAIMS to be
    std::map<std::string, Cell> cells;
};

bool cell_matches(const Cell& c, const Field& f); // mutually recursive

// Is this value *genuinely* a well-formed instance of the given schema?
// Not "does it claim to be" — does it actually satisfy the shape.
bool conforms(const Value& v, const Schema& s, std::string& why) {
    for (const auto& f : s.fields) {
        auto it = v.cells.find(f.name);
        if (it == v.cells.end()) {
            if (f.required) { why = "missing required field '" + f.name + "'"; return false; }
            continue;
        }
        if (!cell_matches(it->second, f)) {
            why = "field '" + f.name + "' has the wrong type (expected " + name_of(f.kind) + ")";
            return false;
        }
    }
    return true;
}

bool cell_matches(const Cell& c, const Field& f) {
    switch (f.kind) {
        case Kind::Int:   return std::holds_alternative<std::int64_t>(c);
        case Kind::Float: return std::holds_alternative<double>(c);
        case Kind::Text:  return std::holds_alternative<std::string>(c);
        case Kind::Bool:  return std::holds_alternative<bool>(c);
        case Kind::Message: {
            if (!std::holds_alternative<std::shared_ptr<Value>>(c)) return false;
            const auto& nested = std::get<std::shared_ptr<Value>>(c);
            if (!nested || !nested->schema || !f.schema) return false;
            if (nested->schema->name != f.schema->name) return false;
            std::string ignored;
            return conforms(*nested, *f.schema, ignored);
        }
    }
    return false;
}

// ---- The threshold: the one rule, used at every boundary ------------------

struct Admission {
    bool ok = false;
    std::string reason;           // why refused, when !ok
    const Value* value = nullptr; // the thing let through, when ok
};

// "Are you a well-formed instance of what you claim — and is what you
//  claim what this door admits?"  Two questions, one gate.
Admission admit(const Value& v, const Schema& door) {
    if (!v.schema)
        return { false, "value carries no shape; it cannot say what it is", nullptr };
    if (v.schema->name != door.name)
        return { false, "value claims to be '" + v.schema->name +
                        "', this door admits only '" + door.name + "'", nullptr };
    std::string why;
    if (!conforms(v, door, why))
        return { false, "value claims '" + door.name + "' but " + why, nullptr };
    return { true, "", &v };
}

} // namespace zen

// ===========================================================================
//  Everything below is staging: builders, a printer, and three scenes that
//  make the moon visible. None of it is the kernel; the kernel is above.
// ===========================================================================

// --- tiny builders so the scenes read cleanly ------------------------------
static std::shared_ptr<zen::Schema> schema(std::string name, std::vector<zen::Field> fields) {
    auto s = std::make_shared<zen::Schema>();
    s->name = std::move(name);
    s->fields = std::move(fields);
    return s;
}
static zen::Field f(std::string n, zen::Kind k) { return { std::move(n), k, nullptr, true }; }

static zen::Cell ci(std::int64_t x) { return x; }
static zen::Cell cf(double x)       { return x; }
static zen::Cell ct(std::string x)  { return x; }
static zen::Cell cb(bool x)         { return x; }

// --- a recursive printer so values can be seen -----------------------------
static void print_value(const zen::Value& v, int indent) {
    std::string pad(indent, ' ');
    std::cout << pad << (v.schema ? v.schema->name : "<shapeless>") << " {\n";
    for (const auto& [key, c] : v.cells) {
        std::cout << pad << "  " << key << " = ";
        std::visit([&](auto&& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::shared_ptr<zen::Value>>) {
                std::cout << "\n";
                if (x) print_value(*x, indent + 4);
                else   std::cout << pad << "    <null>\n";
            } else if constexpr (std::is_same_v<T, bool>) {
                std::cout << (x ? "true" : "false") << "\n";
            } else if constexpr (std::is_same_v<T, std::string>) {
                std::cout << '"' << x << "\"\n";
            } else {
                std::cout << x << "\n";
            }
        }, c);
    }
    std::cout << pad << "}\n";
}

static void report(const zen::Admission& a) {
    if (a.ok) std::cout << "    -> ADMITTED\n";
    else      std::cout << "    -> REFUSED: " << a.reason << "\n";
}

// The in-engine console. It has NO compiled knowledge of any message type.
// Handed a shape at runtime, it walks the shape and fills each field from a
// source. This is precisely the act a static-struct-only design cannot do.
static zen::Value construct_blind(std::shared_ptr<const zen::Schema> shape,
                                  const std::function<zen::Cell(const zen::Field&)>& source) {
    zen::Value v;
    v.schema = shape;
    std::cout << "    console: I was never told what a '" << shape->name
              << "' is. Reading its shape.\n";
    for (const auto& fld : shape->fields) {
        std::cout << "    console:   field '" << fld.name << "' : "
                  << zen::name_of(fld.kind) << "\n";
        v.cells[fld.name] = source(fld);
    }
    return v;
}

// --- a Shard that declares how it wishes to return -------------------------
struct Shard {
    std::string name;
    std::shared_ptr<const zen::Schema> state_schema; // the lock the self chose
    zen::Value policy;                                // self-declared; kernel-checked
    zen::Value last_known_good;                       // the safe self to revert to
    std::int64_t reloads_used = 0;
};

static void revive(Shard& s, const zen::Value& candidate, const zen::Schema& policy_door) {
    // The kernel first checks the policy is itself a well-formed policy.
    // It does not understand what the policy *means*. It only checks shape.
    auto pol = zen::admit(s.policy, policy_door);
    if (!pol.ok) { std::cout << "    policy itself refused: " << pol.reason << "\n"; return; }

    const bool revive_lkg = std::get<bool>(s.policy.cells.at("revive_from_last_good"));
    const auto max        = std::get<std::int64_t>(s.policy.cells.at("max_reloads"));

    if (s.reloads_used >= max) {
        std::cout << "    " << s.name << " has spent its reloads. It stays gone.\n";
        return;
    }

    std::cout << "    a candidate state arrives; the threshold challenges it:\n";
    auto a = zen::admit(candidate, *s.state_schema); // the self-set lock
    if (a.ok) {
        std::cout << "    -> ADMITTED. " << s.name << " wakes as:\n";
        print_value(candidate, 6);
        s.last_known_good = candidate;
        s.reloads_used++;
    } else {
        std::cout << "    -> REFUSED: " << a.reason << "\n";
        if (revive_lkg) {
            std::cout << "    the self said: revive from last good. " << s.name << " wakes as:\n";
            print_value(s.last_known_good, 6);
            s.reloads_used++;
        } else {
            std::cout << "    no fallback was permitted. " << s.name << " stays gone.\n";
        }
    }
}

// ===========================================================================
int main() {
    using zen::Kind;

    // -- A. A message that knows what it is ---------------------------------
    std::cout << "A. A message that knows what it is.\n\n";
    auto Move = schema("Move", { f("dx", Kind::Float), f("dy", Kind::Float) });

    zen::Value step; step.schema = Move;
    step.cells["dx"] = cf(1.0); step.cells["dy"] = cf(-2.0);
    std::cout << "  a well-formed Move, sent across the bus:\n";
    report(zen::admit(step, *Move));

    zen::Value broken; broken.schema = Move;
    broken.cells["dx"] = cf(1.0); // dy never set
    std::cout << "  a Move that lost a field:\n";
    report(zen::admit(broken, *Move));

    auto Spawn = schema("Spawn", { f("kind", Kind::Text) });
    zen::Value liar; liar.schema = Spawn; liar.cells["kind"] = ct("dragon");
    std::cout << "  a Spawn, offered to a door that admits only Move:\n";
    report(zen::admit(liar, *Move));

    // -- B. The console builds a message for a Shard it has never seen ------
    std::cout << "\nB. The console builds a message for a Shard it has never seen.\n\n";
    // Imagine this shape arrived from a DLL loaded one second ago. Nothing
    // in this binary was compiled to know it exists.
    auto SetColor = schema("SetColor", {
        f("r", Kind::Int), f("g", Kind::Int), f("b", Kind::Int), f("named", Kind::Bool)
    });
    auto source = [](const zen::Field& fld) -> zen::Cell {
        switch (fld.kind) {                  // in the engine: the user, typing
            case Kind::Int:   return ci(128);
            case Kind::Bool:  return cb(true);
            case Kind::Float: return cf(0.0);
            case Kind::Text:  return ct("");
            default:          return ci(0);
        }
    };
    zen::Value built = construct_blind(SetColor, source);
    std::cout << "  the console produced:\n";
    print_value(built, 2);
    std::cout << "  offered to the SetColor Shard's threshold:\n";
    report(zen::admit(built, *SetColor));

    // -- C. The self sets the lock its own return must open -----------------
    std::cout << "\nC. The self sets the lock its own return must open.\n\n";

    // The kernel has NO compiled ReloadPolicy type. A policy is just a value
    // against a shape the kernel was handed. Here a Shard declares one.
    auto PlayerState  = schema("PlayerState", { f("hp", Kind::Int), f("name", Kind::Text) });
    auto ReloadPolicy = schema("ReloadPolicy", {
        f("max_reloads", Kind::Int),
        f("revive_from_last_good", Kind::Bool)
    });

    Shard ami;
    ami.name = "Ami";
    ami.state_schema = PlayerState;            // the self chooses its own lock
    ami.policy.schema = ReloadPolicy;
    ami.policy.cells["max_reloads"] = ci(2);
    ami.policy.cells["revive_from_last_good"] = cb(true);
    ami.last_known_good.schema = PlayerState;
    ami.last_known_good.cells["hp"] = ci(20);
    ami.last_known_good.cells["name"] = ct("Ami");

    std::cout << "  the kernel validates the policy the self declared:\n";
    report(zen::admit(ami.policy, *ReloadPolicy));

    std::cout << "\n  death 1 — a clean save returns:\n";
    zen::Value good; good.schema = PlayerState;
    good.cells["hp"] = ci(30); good.cells["name"] = ct("Ami");
    revive(ami, good, *ReloadPolicy);

    std::cout << "\n  death 2 — a corrupted save tries to return:\n";
    zen::Value corrupt; corrupt.schema = PlayerState;     // it still CLAIMS to be PlayerState
    corrupt.cells["hp"] = ct("?!");                        // but hp is text, not a number
    corrupt.cells["name"] = ct("Ami");
    revive(ami, corrupt, *ReloadPolicy);                  // kernel cannot fix it — only refuse it

    // The grammar is open: a wholly different policy shape the kernel never
    // anticipated runs through the identical gate.
    std::cout << "\n  a different self declares a different kind of policy entirely:\n";
    auto EphemeralPolicy = schema("EphemeralPolicy", { f("wipe_on_reload", Kind::Bool) });
    zen::Value eph; eph.schema = EphemeralPolicy;
    eph.cells["wipe_on_reload"] = cb(true);
    std::cout << "  the same threshold, never changed, judges it:\n";
    report(zen::admit(eph, *EphemeralPolicy));

    std::cout << "\nThe kernel above understood the meaning of none of this.\n"
                 "It only ever asked one question, at every door.\n";
    return 0;
}
