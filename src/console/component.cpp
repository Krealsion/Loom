// The component vocabulary's mechanics: the lossless flatten/tree_of pair between the ONE
// tree's two representations (in-memory recursive Widget / flat wire node list), the stress
// canon, the design-time constructors, and the contract check. tree_of is the vocabulary's own
// decode layer AFTER the gate — the gate proves shape-conformance; THIS proves tree-ness, and
// it refuses with a reason instead of trusting (no silent-blank fate from the wire).

#include <zen/console/component.hpp>

#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loom {

// ---- flatten: recursive Widget -> flat pre-order node list ----

namespace {

UiNode node_of(const Widget& w) {
    UiNode n;
    n.kind = name_of(w.kind);
    n.region_id = w.region_id;
    n.title = w.title;
    n.content = w.content;
    n.prompt = w.prompt;
    n.value = w.value;
    n.hint = w.hint;
    n.items = w.items;
    n.selected_index = w.selected_index;
    n.activatable = w.activatable;
    n.editable = w.editable;
    n.reorderable = w.reorderable;
    n.focused = w.focused;
    n.weight = w.weight;
    n.overflow = name_of(w.overflow);
    n.from_field = w.from_field;
    n.route_to = w.route_to;
    n.slot_name = w.slot_name;
    n.slot_accepts = w.slot_accepts;
    return n; // children become indices, assigned by flatten_into
}

std::size_t flatten_into(const Widget& w, std::vector<UiNode>& out) {
    const std::size_t index = out.size();
    out.push_back(node_of(w));
    for (const Widget& child : w.children) {
        const std::size_t child_index = flatten_into(child, out);
        out[index].children.push_back(static_cast<std::int64_t>(child_index));
    }
    return index;
}

} // namespace

std::vector<UiNode> flatten(const Widget& root) {
    std::vector<UiNode> out;
    flatten_into(root, out);
    return out;
}

UiComponent make_component(std::string name, std::string contract_name,
                           std::uint32_t contract_version, const Widget& root) {
    UiComponent c;
    c.name = std::move(name);
    c.contract_name = std::move(contract_name);
    c.contract_version = static_cast<std::int64_t>(contract_version);
    c.nodes = flatten(root);
    return c;
}

// ---- tree_of: flat wire nodes -> the Widget tree, or a refusal with its reason ----

namespace {

std::string at(std::size_t i) { return "nodes[" + std::to_string(i) + "]"; }

// Rebuild the widget at nodes[index]. Returns nullopt after writing `error`. `visited` is the
// reached-exactly-once ledger: a revisit is a cycle or a shared child, both refused.
std::optional<Widget> build_node(const std::vector<UiNode>& nodes, std::size_t index, int depth,
                                 std::vector<bool>& visited, std::string& error) {
    if (depth > kMaxUiDepth) {
        error = at(index) + ": tree deeper than " + std::to_string(kMaxUiDepth) +
                " (the rebuild depth cap)";
        return std::nullopt;
    }
    if (visited[index]) {
        error = at(index) + ": reached twice — the nodes do not form a tree (a cycle, or one "
                            "node claimed as two parents' child)";
        return std::nullopt;
    }
    visited[index] = true;
    const UiNode& n = nodes[index];

    const std::optional<WidgetKind> kind = widget_kind_from(n.kind);
    if (!kind) {
        error = at(index) + ": unknown kind '" + n.kind + "'";
        return std::nullopt;
    }
    const std::optional<Overflow> overflow = overflow_from(n.overflow);
    if (!overflow) {
        error = at(index) + ": unknown overflow '" + n.overflow + "'";
        return std::nullopt;
    }
    if (n.selected_index < -1 || n.selected_index > INT_MAX) {
        error = at(index) + ": selected_index " + std::to_string(n.selected_index) +
                " out of range (an index into items, or -1 for none)";
        return std::nullopt;
    }
    if (n.weight < 0 || n.weight > 0xFFFF) {
        error = at(index) + ": weight " + std::to_string(n.weight) +
                " out of range (a relative grow hint in [0, 65535])";
        return std::nullopt;
    }

    // Per-kind CHILD ARITY is tree structure, so it is enforced here (unlike per-kind-unused
    // scalar fields, which both renderers consistently ignore and which round-trip verbatim).
    // Without this, a wire tree could put children where the vocabulary declares none — and the
    // renderers DIVERGE on that input (the TUI silently drops what the outline prints), which is
    // exactly the silent-fate class this decode layer exists to refuse. The honest constructors
    // cannot express these; the wire could.
    const std::size_t child_count = n.children.size();
    switch (*kind) {
    case WidgetKind::Region:
        if (child_count != 1) {
            error = at(index) + ": a Region wraps exactly one child (got " +
                    std::to_string(child_count) + ")";
            return std::nullopt;
        }
        break;
    case WidgetKind::List:
    case WidgetKind::Log:
    case WidgetKind::Text:
    case WidgetKind::Field:
        if (child_count != 0) {
            error = at(index) + ": a " + std::string(name_of(*kind)) +
                    " carries no children (got " + std::to_string(child_count) + ")";
            return std::nullopt;
        }
        break;
    case WidgetKind::VStack:
    case WidgetKind::HStack:
    case WidgetKind::Slot:
        break; // any arity: stacks arrange, a slot's children are its placeholder preview
    } // no default (exhaustive by -Wswitch under -Werror)

    Widget w;
    w.kind = *kind;
    w.region_id = n.region_id;
    w.title = n.title;
    w.content = n.content;
    w.prompt = n.prompt;
    w.value = n.value;
    w.hint = n.hint;
    w.items = n.items;
    w.selected_index = static_cast<int>(n.selected_index);
    w.activatable = n.activatable;
    w.editable = n.editable;
    w.reorderable = n.reorderable;
    w.focused = n.focused;
    w.weight = static_cast<std::uint16_t>(n.weight);
    w.overflow = *overflow;
    w.from_field = n.from_field;
    w.route_to = n.route_to;
    w.slot_name = n.slot_name;
    w.slot_accepts = n.slot_accepts;

    for (std::size_t ci = 0; ci < n.children.size(); ++ci) {
        const std::int64_t child = n.children[ci];
        if (child < 0 || static_cast<std::size_t>(child) >= nodes.size()) {
            error = at(index) + ".children[" + std::to_string(ci) + "]: index " +
                    std::to_string(child) + " out of range (" + std::to_string(nodes.size()) +
                    " nodes)";
            return std::nullopt;
        }
        std::optional<Widget> built =
            build_node(nodes, static_cast<std::size_t>(child), depth + 1, visited, error);
        if (!built) {
            return std::nullopt;
        }
        w.children.push_back(std::move(*built));
    }
    return w;
}

} // namespace

TreeResult tree_of(const UiComponent& component) {
    TreeResult result;
    if (component.contract_version < 0 || component.contract_version > 0xFFFFFFFFll) {
        result.error = "component '" + component.name + "': contract_version " +
                       std::to_string(component.contract_version) +
                       " out of range (a schema version is a u32) — not a resolvable contract";
        return result;
    }
    if (component.nodes.empty()) {
        result.error = "component '" + component.name + "' has no nodes (node 0 is the root)";
        return result;
    }
    std::vector<bool> visited(component.nodes.size(), false);
    std::string error;
    std::optional<Widget> root = build_node(component.nodes, 0, 0, visited, error);
    if (!root) {
        result.error = std::move(error);
        return result;
    }
    for (std::size_t i = 0; i < visited.size(); ++i) {
        if (!visited[i]) {
            result.error = at(i) + ": unreachable from the root — an orphan (or a detached "
                                   "cycle) is not part of the tree";
            return result;
        }
    }
    result.root = std::move(*root);
    return result;
}

// ---- The stress canon ----

std::string stress_text() {
    // A long paragraph plus one unbroken 64-char word: enough to blow past any sane column
    // width (overflow stress) and to defeat word-wrap (unbreakable-width stress). ASCII only —
    // the TUI is byte-per-cell; the Unicode stress case arrives with a renderer that can draw it.
    return "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor "
           "incididunt ut labore et dolore magna aliqua, quis nostrud exercitation ullamco. "
           "Pneumonoultramicroscopicsilicovolcanoconiosis0123456789abcdefghij";
}

std::string stress_number() {
    return "-9223372036854775808"; // the widest canonical Int spelling: sign + 19 digits
}

std::vector<std::string> stress_rows() {
    return {}; // the zero-case: does the layout survive nothing?
}

Widget stress_nested(int depth) {
    Widget leaf = text_widget(stress_text());
    if (depth <= 0) {
        return leaf;
    }
    Widget current = std::move(leaf);
    for (int level = 0; level < depth; ++level) {
        std::vector<Widget> children;
        children.push_back(text_widget("nested level " + std::to_string(depth - level)));
        children.push_back(std::move(current));
        // Alternate stacking direction so the ladder stresses both axes.
        current = (level % 2 == 0) ? vstack("", std::move(children))
                                   : hstack("", std::move(children));
    }
    return current;
}

std::string stress_value_for(Kind k) {
    switch (k) {
    case Kind::Int:
        return stress_number();
    case Kind::Float:
        return "-1.7976931348623157e+308"; // the widest double, sign included
    case Kind::Text:
        return stress_text();
    case Kind::Bool:
        return "false";
    case Kind::Bytes:
    case Kind::Message:
    case Kind::List:
        // No display stress value exists: bindings to these are refused by check_bindings
        // (List binds to List/Log nodes whose stress case is stress_rows()).
        return "";
    }
    return "";
}

// ---- Design-time constructors ----

namespace {

// The scalar Kind named by a spelling, if it is one of the four displayable scalars.
std::optional<Kind> scalar_kind_from(std::string_view spelling) {
    for (Kind k : {Kind::Int, Kind::Float, Kind::Text, Kind::Bool}) {
        if (spelling == name_of(k)) {
            return k;
        }
    }
    return std::nullopt;
}

} // namespace

Widget open_slot(std::string slot_name, std::string accepts) {
    std::vector<Widget> placeholder;
    if (accepts == "Component") {
        placeholder.push_back(stress_nested()); // the deeply-nested stress case
    } else if (const std::optional<Kind> k = scalar_kind_from(accepts)) {
        placeholder.push_back(text_widget(stress_value_for(*k)));
    }
    // "Route" (and anything unknown, which check_bindings flags) previews as the bare marker.
    return slot(std::move(slot_name), std::move(accepts), std::move(placeholder));
}

Widget bound_text(std::string region_id, std::string from_field, Kind field_kind) {
    Widget w = text_widget(stress_value_for(field_kind));
    w.region_id = std::move(region_id);
    w.from_field = std::move(from_field);
    return w;
}

Widget bound_field(std::string prompt, std::string from_field, Kind field_kind) {
    Widget w = field(std::move(prompt), stress_value_for(field_kind), /*hint=*/"",
                     /*focused=*/false);
    w.from_field = std::move(from_field);
    return w;
}

Widget bound_list(std::string region_id, std::string title, std::string from_field) {
    Widget w = list(std::move(region_id), std::move(title), stress_rows(),
                    /*selected_index=*/-1, /*activatable=*/false, /*focused=*/false);
    w.from_field = std::move(from_field);
    return w;
}

// ---- The contract check ----

std::vector<std::string> check_bindings(const UiComponent& component, const Schema& contract) {
    std::vector<std::string> problems;
    const std::string contract_label =
        contract.name() + " v" + std::to_string(contract.version());

    std::vector<std::string> seen_slot_names;
    for (std::size_t i = 0; i < component.nodes.size(); ++i) {
        const UiNode& n = component.nodes[i];
        const std::optional<WidgetKind> kind = widget_kind_from(n.kind);
        if (!kind) {
            problems.push_back(at(i) + ": unknown kind '" + n.kind + "'");
            continue;
        }

        if (*kind == WidgetKind::Slot) {
            if (n.slot_accepts != "Component" && n.slot_accepts != "Route" &&
                !scalar_kind_from(n.slot_accepts)) {
                problems.push_back(at(i) + ": slot '" + n.slot_name + "' accepts '" +
                                   n.slot_accepts +
                                   "' — not \"Component\", \"Route\", or a scalar kind "
                                   "(Int/Float/Text/Bool)");
            }
            // A slot is filled BY NAME later — a nameless or ambiguous hole cannot be filled.
            if (n.slot_name.empty()) {
                problems.push_back(at(i) + ": a slot with no name cannot be filled");
            } else {
                for (const std::string& seen : seen_slot_names) {
                    if (seen == n.slot_name) {
                        problems.push_back(at(i) + ": duplicate slot name '" + n.slot_name +
                                           "' — filling by name would be ambiguous");
                        break;
                    }
                }
                seen_slot_names.push_back(n.slot_name);
            }
        }

        // Navigation intent fires on activation; a route on a node that can never Activate is
        // a dead declaration the composer should hear about now, not at runtime.
        if (!n.route_to.empty() && !n.activatable) {
            problems.push_back(at(i) + ": route_to '" + n.route_to +
                               "' on a node that is not activatable — the route can never fire");
        }

        if (n.from_field.empty()) {
            continue; // static content — nothing to check against the contract
        }
        const Field* f = contract.find(n.from_field);
        if (f == nullptr) {
            problems.push_back(at(i) + ": from_field '" + n.from_field + "' is not a field of " +
                               contract_label);
            continue;
        }
        switch (*kind) {
        case WidgetKind::Text:
        case WidgetKind::Field:
            if (f->type.kind != Kind::Int && f->type.kind != Kind::Float &&
                f->type.kind != Kind::Text && f->type.kind != Kind::Bool) {
                problems.push_back(at(i) + ": '" + n.from_field + "' is a " +
                                   name_of(f->type.kind) + " field of " + contract_label +
                                   " — a " + n.kind + " node displays a scalar "
                                   "(Int/Float/Text/Bool)");
            }
            break;
        case WidgetKind::List:
        case WidgetKind::Log:
            if (f->type.kind != Kind::List) {
                problems.push_back(at(i) + ": '" + n.from_field + "' is a " +
                                   name_of(f->type.kind) + " field of " + contract_label +
                                   " — a " + n.kind + " node binds a List field");
            } else if (f->type.element == nullptr ||
                       (f->type.element->kind != Kind::Int &&
                        f->type.element->kind != Kind::Float &&
                        f->type.element->kind != Kind::Text &&
                        f->type.element->kind != Kind::Bool)) {
                // A row is displayed as text: a list of non-displayable elements (Bytes,
                // nested messages, lists of lists) has no row form to bind.
                problems.push_back(
                    at(i) + ": '" + n.from_field + "' is a List of " +
                    std::string(f->type.element ? name_of(f->type.element->kind) : "?") +
                    " in " + contract_label + " — rows display scalars (Int/Float/Text/Bool)");
            }
            break;
        case WidgetKind::VStack:
        case WidgetKind::HStack:
        case WidgetKind::Region:
        case WidgetKind::Slot:
            problems.push_back(at(i) + ": a " + n.kind +
                               " node cannot bind data (from_field '" + n.from_field + "')");
            break;
        } // no default (exhaustive by -Wswitch under -Werror)
    }
    return problems;
}

} // namespace loom
