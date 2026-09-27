// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_UI_COMPONENT_HPP
#define ZEN_UI_COMPONENT_HPP

// The UI component vocabulary: a component is a named, reusable piece of UI with typed open
// slots, built from the same `loom::Widget` tree the console renders (zen/ui/tree.hpp) and
// carried as ordinary gated Values (zen.ui.Node, zen.ui.Component, zen.ui.Presenter) through
// the registry, serializer and gate every message uses. Design-time placeholders default to
// stress values, so a preview tests its own layout. These are the shapes and their checks:
// nothing here binds live data, runs a presenter or navigates a route.

#include <zen/ui/tree.hpp>
#include <zen/kind.hpp>
#include <zen/schema.hpp>
#include <zen/weave/shape.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace loom {

// ---- The wire shapes: registered by hand, like the standard reply shapes, so their names carry
// ---- the "zen." prefix a macro-declared struct cannot have -----------------------------------

/// One tree node, flat on the wire: a schema cannot contain itself, so a component carries a
/// flat node list whose `children` are indices into it (node 0 is the root). Otherwise it is
/// field-for-field `loom::Widget`, with enums as their spellings and integers as Int, both
/// checked back by tree_of().
struct UiNode {
    std::string kind;                   ///< name_of(WidgetKind) spelling
    std::string region_id;
    std::string title;
    std::string content;                ///< Text body / design-time placeholder
    std::string prompt;
    std::string value;
    std::string hint;
    std::vector<std::string> items;     ///< List/Log rows / design-time placeholder rows
    std::int64_t selected_index = -1;   ///< an index INTO items, never a y (-1 = none)
    bool activatable = false;           ///< abstract interaction intent (never a key/gesture)
    bool editable = false;
    bool reorderable = false;
    bool focused = false;               ///< the focus MARKER (live trees carry it; schematics
                                        ///< conventionally default it)
    std::int64_t weight = 0;            ///< relative grow hint [0, 65535]; never a size
    std::string overflow;               ///< name_of(Overflow) spelling — a policy, never a size
    std::string from_field;             ///< data binding: contract field feeding this node
    std::string route_to;               ///< navigation intent: a view address ("" = none)
    std::string slot_name;              ///< Slot: the open hole's name
    std::string slot_accepts;           ///< Slot: "Component" | "Route" | a scalar Kind spelling
    std::vector<std::int64_t> children; ///< indices into the component's nodes (0 is the root)

    using ZenSelf = UiNode;
    static constexpr const char* zen_name = "zen.ui.Node";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(
            ZEN_FIELD(kind), ZEN_FIELD(region_id), ZEN_FIELD(title), ZEN_FIELD(content),
            ZEN_FIELD(prompt), ZEN_FIELD(value), ZEN_FIELD(hint), ZEN_FIELD(items),
            ZEN_FIELD(selected_index), ZEN_FIELD(activatable), ZEN_FIELD(editable),
            ZEN_FIELD(reorderable), ZEN_FIELD(focused), ZEN_FIELD(weight), ZEN_FIELD(overflow),
            ZEN_FIELD(from_field), ZEN_FIELD(route_to), ZEN_FIELD(slot_name),
            ZEN_FIELD(slot_accepts), ZEN_FIELD(children));
    }
};

// The wire twin is held to the SAME geometry fence as Widget (the traits are ui.hpp's): the
// vocabulary is "field-for-field the same", so a coordinate member must fail to build on both
// sides of the wire, not just the in-memory one. Same honest limits: name-based, one layer.
static_assert(!detail::has_x<UiNode>::value && !detail::has_y<UiNode>::value &&
                  !detail::has_w<UiNode>::value && !detail::has_h<UiNode>::value &&
                  !detail::has_width<UiNode>::value && !detail::has_height<UiNode>::value &&
                  !detail::has_row<UiNode>::value && !detail::has_col<UiNode>::value &&
                  !detail::has_top<UiNode>::value && !detail::has_left<UiNode>::value,
              "zen.ui.Node must not carry absolute geometry — position is the renderer's job "
              "(the wire form is the same intent-only vocabulary as Widget).");

/// A component: `name` is its identity and its address (a route points at it); the contract is
/// the shape it is built to consume, by (name, version) against the registry, which is what
/// makes its bindings checkable (check_bindings). An empty contract_name, with version 0,
/// consumes nothing. `nodes` is the flat tree, node 0 the root, pre-order as flatten() writes.
struct UiComponent {
    std::string name;                   ///< identity + route address
    std::string contract_name;          ///< the consumed shape's registered name ("" = none)
    std::int64_t contract_version = 0;  ///< the consumed shape's version
    std::vector<UiNode> nodes;          ///< the flat tree; nodes[0] is the root, pre-order

    using ZenSelf = UiComponent;
    static constexpr const char* zen_name = "zen.ui.Component";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(contract_name),
                               ZEN_FIELD(contract_version), ZEN_FIELD(nodes));
    }
};

/// What feeds a view, kept apart from it: a component never names its source, so the view
/// outlives the source, and the source's death is an event a renderer can show. The source is
/// addressed by role, never by a WeaveId. This is the declaration only; nothing runs it.
struct UiPresenter {
    std::string view;        ///< the component name (address) this presenter feeds
    std::string source_role; ///< the role whose values feed it (and whose death is an event)

    using ZenSelf = UiPresenter;
    static constexpr const char* zen_name = "zen.ui.Presenter";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(view), ZEN_FIELD(source_role)); }
};

// ---- The lossless pair between the two representations of the ONE tree ----

/// Flatten a Widget tree to the wire's node list, pre-order, `root` at nodes[0], children as
/// indices. Within kMaxUiDepth, tree_of() rebuilds the identical tree (Widget's ==); a deeper
/// tree flattens into a component tree_of() refuses.
std::vector<UiNode> flatten(const Widget& root);

/// Convenience: a component wrapping a flattened tree.
UiComponent make_component(std::string name, std::string contract_name,
                           std::uint32_t contract_version, const Widget& root);

/// The outcome of rebuilding a tree from the wire form: the root, or a refusal with its reason
/// (written for a stranger, naming the offending node). Never both.
struct TreeResult {
    std::optional<Widget> root;
    std::string error;
};

/// The deepest tree tree_of() rebuilds: the root is at depth 0 and the refusal is
/// `depth > kMaxUiDepth`, so the longest legal chain is kMaxUiDepth + 1 nodes. tree_of() walks
/// with an explicit stack, never native recursion, so this number decides which frames are
/// accepted and does not depend on a toolchain's stack frame size.
inline constexpr int kMaxUiDepth = 256;

/// Rebuild the Widget tree from a component's flat nodes. The gate proved the Values' shape;
/// this proves a tree: node 0 the root, every index in range and reached once (no cycle, share
/// or orphan), depth within kMaxUiDepth, known spellings, integers in range, and child arity
/// (a Region wraps one child; List, Log, Text and Field wrap none). It refuses with a reason.
/// Any valid layout is accepted, not only flatten()'s; unused per-kind scalars round-trip, and
/// selected_index is not checked against items (a cursor over an empty list is legitimate).
TreeResult tree_of(const UiComponent& component);

// ---- The stress canon: design-time placeholders default to the value that reveals a layout
// seam (a too-long text, the widest number, the empty list, a deep ladder), not a happy one.
// The defaults are ASCII so a byte-per-cell terminal renderer is stressed too; the Unicode
// case is the graphical renderers' extra value, exercised by the pixel projection.

/// A long paragraph plus one unbroken 64-char word: overflow + unbreakable-width stress.
std::string stress_text();
/// The graphical-renderer stress text: CJK width, emoji, a combining sequence, an RTL run, and
/// an unbroken mixed-script word — the cases a byte-per-cell terminal cannot draw. A pixel
/// renderer must not BREAK on these (no crash, no mid-codepoint split); correct BIDI/shaping
/// is the text stack's own affair, not what this pins.
std::string stress_text_unicode();
/// "-9223372036854775808" — the widest canonical Int spelling (sign + 19 digits).
std::string stress_number();
/// The empty list: does the layout survive nothing?
std::vector<std::string> stress_rows();
/// A deep alternating VStack/HStack ladder ending in a stress_text() leaf: nesting stress.
Widget stress_nested(int depth = 8);
/// The stress value for a scalar contract-field kind (Int/Float/Text/Bool). Bytes and
/// non-scalars have no display stress value — bindings to them are refused by check_bindings.
std::string stress_value_for(Kind k);

// ---- Design-time constructors (compose a schematic; placeholders are stress by default) ----

/// An open slot whose placeholder preview defaults to the stress case for what it accepts:
/// "Component" previews a deep nested ladder, a scalar Kind spelling previews that kind's
/// stress value as text, "Route" previews as the bare slot marker.
Widget open_slot(std::string slot_name, std::string accepts);
/// A Text node bound to a contract field, previewing that field-kind's stress value.
Widget bound_text(std::string region_id, std::string from_field, Kind field_kind);
/// A Field node bound to a contract field (editable by nature), previewing the stress value.
Widget bound_field(std::string prompt, std::string from_field, Kind field_kind);
/// A List node bound to a (list-kinded) contract field, previewing the EMPTY rows stress case.
Widget bound_list(std::string region_id, std::string title, std::string from_field);

// ---- The contract check ----

/// Check a component against its contract's schema, one problem string per offense (empty when
/// it fits): `from_field` names a contract field; Text and Field bind scalars, List and Log a
/// List of scalars, containers and Slots nothing; a Slot is named uniquely and accepts
/// "Component", "Route" or a scalar Kind spelling; `route_to` needs `activatable`. The caller
/// resolves the contract by its (name, version).
std::vector<std::string> check_bindings(const UiComponent& component, const Schema& contract);

} // namespace loom

#endif // ZEN_UI_COMPONENT_HPP
