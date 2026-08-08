// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The component vocabulary's mechanics: the lossless flatten/tree_of pair between the ONE
// tree's two representations (in-memory recursive Widget / flat wire node list), the stress
// canon, the design-time constructors, and the contract check. tree_of is the vocabulary's own
// decode layer AFTER the gate — the gate proves shape-conformance; THIS proves tree-ness, and
// it refuses with a reason instead of trusting (no silent-blank fate from the wire).

#include <zen/ui/component.hpp>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <optional>
#include <set>
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

// Rebuild the ONE widget at nodes[index] INTO `out` — its own fields, none of its children.
// Returns false after writing `error`, leaving `out` meaningless. `visited` is the
// reached-exactly-once ledger: a revisit is a cycle or a shared child, both refused.
//
// `depth` is this node's own depth (the root's is 0) and it is checked FIRST, so an over-deep
// node is refused before it is read, let alone materialized. Nothing here recurses; walking to
// the children is build_tree's job, below.
//
// It fills a caller-owned Widget rather than returning one so that the rebuilt node is
// constructed exactly where it will live, in its own traversal frame. Returning by value cost
// two extra moves of a 19-field Widget per node, which measured ~1.8x on the whole decode.
bool build_node(const std::vector<UiNode>& nodes, std::size_t index, int depth,
                std::vector<bool>& visited, std::string& error, Widget& out) {
    if (depth > kMaxUiDepth) {
        error = at(index) + ": tree deeper than " + std::to_string(kMaxUiDepth) +
                " (the rebuild depth cap)";
        return false;
    }
    if (visited[index]) {
        error = at(index) + ": reached twice — the nodes do not form a tree (a cycle, or one "
                            "node claimed as two parents' child)";
        return false;
    }
    visited[index] = true;
    const UiNode& n = nodes[index];

    const std::optional<WidgetKind> kind = widget_kind_from(n.kind);
    if (!kind) {
        error = at(index) + ": unknown kind '" + n.kind + "'";
        return false;
    }
    const std::optional<Overflow> overflow = overflow_from(n.overflow);
    if (!overflow) {
        error = at(index) + ": unknown overflow '" + n.overflow + "'";
        return false;
    }
    if (n.selected_index < -1 || n.selected_index > INT_MAX) {
        error = at(index) + ": selected_index " + std::to_string(n.selected_index) +
                " out of range (an index into items, or -1 for none)";
        return false;
    }
    if (n.weight < 0 || n.weight > 0xFFFF) {
        error = at(index) + ": weight " + std::to_string(n.weight) +
                " out of range (a relative grow hint in [0, 65535])";
        return false;
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
            return false;
        }
        break;
    case WidgetKind::List:
    case WidgetKind::Log:
    case WidgetKind::Text:
    case WidgetKind::Field:
        if (child_count != 0) {
            error = at(index) + ": a " + std::string(name_of(*kind)) +
                    " carries no children (got " + std::to_string(child_count) + ")";
            return false;
        }
        break;
    case WidgetKind::VStack:
    case WidgetKind::HStack:
    case WidgetKind::Slot:
        break; // any arity: stacks arrange, a slot's children are its placeholder preview
    } // no default (exhaustive by -Wswitch under -Werror)

    out.kind = *kind;
    out.region_id = n.region_id;
    out.title = n.title;
    out.content = n.content;
    out.prompt = n.prompt;
    out.value = n.value;
    out.hint = n.hint;
    out.items = n.items;
    out.selected_index = static_cast<int>(n.selected_index);
    out.activatable = n.activatable;
    out.editable = n.editable;
    out.reorderable = n.reorderable;
    out.focused = n.focused;
    out.weight = static_cast<std::uint16_t>(n.weight);
    out.overflow = *overflow;
    out.from_field = n.from_field;
    out.route_to = n.route_to;
    out.slot_name = n.slot_name;
    out.slot_accepts = n.slot_accepts;
    out.children.clear(); // every field is written, so `out` need not arrive empty

    return true; // children are attached by build_tree as each subtree completes
}

// One entry per ANCESTOR of the node currently being rebuilt — the walk's state, written down
// instead of left implicit in native call frames.
struct Frame {
    std::size_t index;          ///< the node this frame is rebuilding
    int depth;                  ///< its depth (the root's is 0)
    std::size_t next_child = 0; ///< cursor into nodes[index].children; [0, next_child) attached
    Widget widget;              ///< the widget being rebuilt, built in place by build_node

    Frame(std::size_t i, int d) : index(i), depth(d) {}
};

// Rebuild the tree rooted at node 0, or refuse. ITERATIVE ON PURPOSE (MSVC-1).
//
// WHY THE WALK IS EXPLICIT. kMaxUiDepth is a bound this decoder claims over UNTRUSTED input,
// and the recursive form it replaces could not keep that promise: a deliberate semantic bound
// is not a real bound if an implementation resource fails first for input the bound claims to
// admit or refuse safely. It did fail first. Measured on MSVC Debug (19.50, /Od, 1 MB stack),
// the recursion died of STATUS_STACK_OVERFLOW at chain depth 240 against a cap that ACCEPTS
// 257 — so every frame in the 240..257 window killed the host process instead of being either
// rebuilt or refused, and the case named "a hostile deep chain is bounded by the depth cap,
// not the C++ stack" was simply false there. GCC's thinner frames hid it: MinGW walked 4000
// deep and refused exactly on the cap. The cap was never wrong; it was being enforced by
// whichever compiler's frame size ran out first. Lowering it would only have re-calibrated it
// against one more toolchain. This makes the native stack stop participating at all.
//
// THE WALK IS THE SAME WALK. Pre-order; children left to right; a child's whole subtree
// finished before the next child's index is even looked at. Every refusal therefore still
// fires on the same node, in the same order, with the same words as the recursion did — the
// order is observable (a malformed tree usually has more than one thing wrong with it), so it
// is preserved deliberately rather than incidentally.
//
// AND IT IS BOUNDED, TWICE OVER. At most kMaxUiDepth + 1 == 257 frames ever hold a rebuilt
// node, however deep the input CLAIMS to be — a 200,000-node chain costs the same 257 as a
// 258-node one, and gets the same sentence back. One more frame than that exists for exactly as
// long as it takes the cap to refuse the node that overflowed it: a frame is claimed before
// build_node is asked about it, so the ceiling is 258, and the reserve below says 258 for that
// reason. The stack also cannot outgrow the node count, so one allocation covers a whole decode
// and no frame is ever moved by a regrow. Width costs no frames at all: siblings are rebuilt
// one at a time and each finished subtree moves straight into its parent, so the live widgets
// are exactly the ones the recursion held (each node is materialized at most once, per
// `visited`).
//
// Each node is built IN PLACE in its own frame and handed to its parent in a single move. That
// is not incidental tidiness: the obvious form -- build_node returning a Widget, moved into a
// Frame, moved into the vector -- cost five moves of a 19-field Widget per node against the
// recursion's two, and measured ~1.8x slower on the whole decode under MSVC Release. This form
// costs one.
std::optional<Widget> build_tree(const std::vector<UiNode>& nodes, std::vector<bool>& visited,
                                 std::string& error) {
    std::vector<Frame> stack;
    stack.reserve(std::min(nodes.size(), static_cast<std::size_t>(kMaxUiDepth) + 2));

    stack.emplace_back(/*index=*/0, /*depth=*/0);
    if (!build_node(nodes, 0, 0, visited, error, stack.back().widget)) {
        return std::nullopt;
    }

    for (;;) {
        Frame& top = stack.back();
        const UiNode& n = nodes[top.index];

        if (top.next_child < n.children.size()) {
            const std::size_t ci = top.next_child++;
            const std::int64_t child = n.children[ci];
            if (child < 0 || static_cast<std::size_t>(child) >= nodes.size()) {
                error = at(top.index) + ".children[" + std::to_string(ci) + "]: index " +
                        std::to_string(child) + " out of range (" + std::to_string(nodes.size()) +
                        " nodes)";
                return std::nullopt;
            }
            // A frame's depth passed the cap, so it is <= kMaxUiDepth and this cannot overflow.
            const int child_depth = top.depth + 1;
            const std::size_t child_index = static_cast<std::size_t>(child);
            // `top` and `n` are not used again below: the reserve above rules out a regrow, but
            // nothing here leans on that.
            stack.emplace_back(child_index, child_depth);
            if (!build_node(nodes, child_index, child_depth, visited, error,
                            stack.back().widget)) {
                return std::nullopt;
            }
            continue;
        }

        // Every child of this node is attached, so the subtree is whole: hand it to the parent,
        // or return it if this frame was the root.
        if (stack.size() == 1) {
            return std::move(top.widget);
        }
        stack[stack.size() - 2].widget.children.push_back(std::move(top.widget));
        stack.pop_back();
    }
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
    std::optional<Widget> root = build_tree(component.nodes, visited, error);
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
    // the DEFAULT placeholder must stress layout in every projection (the TUI is byte-per-cell);
    // the Unicode case is the graphical renderers' additional value, below.
    return "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor "
           "incididunt ut labore et dolore magna aliqua, quis nostrud exercitation ullamco. "
           "Pneumonoultramicroscopicsilicovolcanoconiosis0123456789abcdefghij";
}

std::string stress_text_unicode() {
    // The wide-glyph / multi-byte gauntlet, spelled as explicit UTF-8 byte escapes so the
    // source file stays pure ASCII (no editor/codepage can silently reshape it):
    //   - CJK run (wide glyphs):        织机の糸を編む (E7 BB 87 ...)
    //   - emoji (4-byte sequences):     🧶🧵 (F0 9F A7 B6, F0 9F A7 B5)
    //   - combining sequence:           cafe + U+0301 (combining acute -> "café")
    //   - RTL run (bidi input):         مرحبا (Arabic "marhaba")
    //   - an unbroken mixed-script word to defeat wrap the way the ASCII canon's word does.
    // A pixel renderer must not break on any of it: no crash, no mid-codepoint split. What the
    // glyphs LOOK like (shaping, bidi order) is the text stack's affair, not this pin's.
    return "Unicode stress: "
           "\xE7\xBB\x87\xE6\x9C\xBA\xE3\x81\xAE\xE7\xB3\xB8\xE3\x82\x92\xE7\xB7\xA8\xE3\x82\x80 "
           "\xF0\x9F\xA7\xB6\xF0\x9F\xA7\xB5 "
           "caf\x65\xCC\x81 "
           "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 "
           "Zen\xE7\xB9\x94\xF0\x9F\xA7\xB6loom\xE3\x81\xAE\xE7\xB3\xB8weave0123456789";
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

    std::set<std::string> seen_slot_names; // log-time membership: a hostile many-slot
                                           // component must not turn the checker quadratic
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
            } else if (!seen_slot_names.insert(n.slot_name).second) {
                problems.push_back(at(i) + ": duplicate slot name '" + n.slot_name +
                                   "' — filling by name would be ambiguous");
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
