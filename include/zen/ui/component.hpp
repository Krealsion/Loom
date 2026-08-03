// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_UI_COMPONENT_HPP
#define ZEN_UI_COMPONENT_HPP

// The UI-Builder component vocabulary (Phase A — the shapes only; no renderer, no Builder UI,
// no live binding, no routing/presenter runtime).
//
// A COMPONENT is a schematic with typed slots: a reusable piece of UI, incomplete BY DESIGN. It
// declares the data shape it consumes (its CONTRACT), pulls fields from that shape (a node's
// `from_field` binding), and leaves typed open holes (Slot nodes) to be filled later — by data,
// by a route, by a child component. Design-time display uses PLACEHOLDER data, and the default
// placeholders are deliberately adversarial stress-values (see the stress canon below): a
// component self-stress-tests its layout the moment it is previewed.
//
// A COMPONENT IS DATA. It serializes as a gated Value like any message in Zen — same schemas,
// same registry, same serialize/parse/admit, no second format. That is what makes "a schematic
// shared is a toy others can play with" literal: save it, send it, load it, and the SAME gate
// that guards every boundary validates it.
//
// ONE TREE, NOT TWO. The nodes here ARE the console's widget tree (loom::Widget, ui.hpp) — this
// header adds the component concept AROUND that tree and a wire form OF it; it does not invent a
// second vocabulary. The TUI keeps rendering the same tree — the standing renderer-agnosticism
// proof.
//
// THE WIRE FORM IS FLAT. A schema cannot reference itself (published schemas are immutable, so
// a "node containing a list of nodes" is unbuildable), and nested-Message decoding is depth-
// capped — so the tree crosses the wire as a FLAT list of zen.ui.Node values whose `children`
// are indices into that list (node 0 is the root). flatten() and tree_of() are the lossless
// pair between the two representations of the ONE vocabulary — lossless FOR TREES WITHIN
// kMaxUiDepth (flatten of a deeper tree produces a component tree_of refuses; that bound is
// pinned, not hidden) — with the round-trip pinned by Widget's structural operator==. flatten's
// output is pre-order and is the canonical layout; tree_of deliberately accepts any layout that
// forms a valid tree (rebuild is deterministic and both renderers see the identical result), so
// ordering is a canonical-authoring convention, not a decode requirement.
//
// HONEST LAYERING (load-bearing): the gate validates SHAPE-conformance — kinds, required
// fields, nesting — but it cannot see TREE-ness. Whether the flat node list is actually a tree
// (indices in range, every node reachable exactly once, no cycles, sane enum spellings and
// numeric ranges) is the vocabulary's own decode check, done by tree_of(), which REFUSES with a
// reason instead of trusting. Passing the gate does NOT mean tree_of() will accept; tests forge
// hostile Values to pin exactly that seam. Per-kind-unused fields (e.g. a `title` on a Text
// node) are NOT refused: the wire carries the full flat struct and round-trips it verbatim —
// canonical authoring zeroes them (the named constructors do); a strict canonicality check is a
// named seam, not built until a consumer needs it.
//
// THE VIEW / PRESENTER SPLIT. A component is pure DISPLAY data: it declares what it consumes
// (the contract) but never names what FEEDS it. The feeding is a separate value — zen.ui.
// Presenter binds a source (by role) to a view (by name/address). Because the two are separable
// data, a crashed program-weave can never take the UI down with it: the display tree keeps
// existing, the presenter's source dying is just an EVENT the UI can render gracefully (or open
// into the poke-weave debugger). The presenter RUNTIME is a later phase; the split exists now.
//
// ROUTES. A component's `name` IS its address. A node's `route_to` declares navigation intent
// ("activating this goes to that view"); a Slot with accepts="Route" is an unbound navigation
// target. The routing RUNTIME (actually navigating) is a later phase — only addressability
// lives in the vocabulary.
//
// Every field below survived the least-complete-information razor: it stays only if removing it
// leaves a renderer unable to project or the Builder unable to compose. Sediment was cut —
// starting with Widget::focusable, which no renderer ever read (focus-eligibility is derivable
// from interaction intent).

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

// ---- The wire shapes (hand-written registration blocks, like the standard reply shapes, so
// ---- the names carry the substrate's "zen." prefix — a maker's macro-declared struct cannot
// ---- produce a dotted name, which is exactly what keeps "zen.ui.*" the Loom's to speak) ----

/// One tree node, flat on the wire. Field-for-field the SAME vocabulary as loom::Widget
/// (ui.hpp) with three mechanical differences: enums travel as their stable spellings (so a
/// serialized component READS as intent — "List", "Scroll" — and an unknown spelling is refused
/// on decode, never a silent blank), integers travel as Int (range-checked back by tree_of),
/// and `children` are indices into the component's node list instead of nested values.
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

/// A component: a schematic with typed slots. `name` is its identity AND its address (routes
/// point at it). The contract — the shape this component is built to consume — is part of that
/// identity: a row-for-shape-X knows X's fields, which is what makes its bindings checkable
/// (see check_bindings). contract_name "" (with version 0) means "consumes nothing" (a static
/// component — a title card, a button panel). The tree is the flat node list; node 0 is the
/// root. Agreement on the contract is by (name, version) against the registry, consistent with
/// how every schema in Zen is resolved; pinning the contract's content-id into the component is
/// the migration/identity layer's business — a named seam, not built here.
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

/// The OTHER half of the view/presenter split: the declaration of what FEEDS a view. Separate
/// data on purpose — a component never names its source, so display survives the source. The
/// source is addressed by ROLE (the persistable participant-addressing the powerbox already
/// speaks), never by a session-scoped WeaveId. Two fields is the complete image: remove `view`
/// and you cannot tell which view is fed; remove `source_role` and you cannot tell what feeds
/// it. The subscription/update/crash-handling RUNTIME is a later phase.
struct UiPresenter {
    std::string view;        ///< the component name (address) this presenter feeds
    std::string source_role; ///< the role whose values feed it (and whose death is an event)

    using ZenSelf = UiPresenter;
    static constexpr const char* zen_name = "zen.ui.Presenter";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(view), ZEN_FIELD(source_role)); }
};

// ---- The lossless pair between the two representations of the ONE tree ----

/// Flatten a Widget tree to the wire's flat node list (pre-order; the returned nodes[0] is
/// `root`; children become indices). Lossless: tree_of() rebuilds the identical tree (pinned by
/// Widget's structural ==).
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

/// Maximum tree depth tree_of() will rebuild. A UI deeper than this is pathological; the cap
/// keeps hostile deep-chain components from exhausting the C++ stack (the decode-side analogue
/// of the binary codec's own nesting cap).
inline constexpr int kMaxUiDepth = 256;

/// Rebuild the Widget tree from a component's flat nodes — the vocabulary's own decode check,
/// one honest layer AFTER the gate. The gate proved shape-conformance; this proves TREE-ness:
/// node 0 is the root, every child index is in range, every node is reached exactly once (no
/// cycles, no sharing, no orphans), depth <= kMaxUiDepth, kind/overflow spellings are known,
/// integer fields are in range (selected_index >= -1, weight in [0, 65535], contract_version a
/// u32), and per-kind CHILD ARITY holds (a Region wraps exactly one child; List/Log/Text/Field
/// carry none — child structure is where the renderers would otherwise silently diverge, so it
/// is tree structure, refused here; per-kind-unused SCALAR fields stay lenient and round-trip
/// verbatim). Anything else is refused with a reason — never a silent blank. NOTE:
/// selected_index is deliberately NOT checked against items.size() — a live tree legitimately
/// carries a cursor over an empty list (renderers treat it as no-selection).
TreeResult tree_of(const UiComponent& component);

// ---- The stress canon (adversarial placeholder defaults) ----
//
// Design-time previews default to the value that REVEALS the seam, not the happy value: a
// too-long text (width blowout / wrap), the widest number, the empty list (zero-case layout),
// a deep ladder (nesting). The DEFAULTS stay ASCII on purpose — the TUI is a byte-per-cell
// renderer and the default placeholder must stress layout in EVERY projection, not mojibake
// one of them. The Unicode case is the graphical renderers' ADDITIONAL stress value, provided
// here (the canon is shared vocabulary) and exercised by the pixel projection.

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

// ---- The contract check (what makes a typed slot TYPED) ----

/// Check a component's bindings and slot/intent declarations against its contract's actual
/// schema: every `from_field` must name a contract field; Text/Field nodes display scalars
/// (Int/Float/Text/Bool); List/Log nodes bind List fields whose ELEMENTS are displayable
/// scalars (rows are text — a List of Bytes/Message has no row form); container/Slot nodes
/// cannot bind data; a Slot's `accepts` must be "Component", "Route", or a scalar Kind
/// spelling; a Slot must be named, uniquely (slots are filled BY NAME later — nameless or
/// duplicate holes cannot be filled unambiguously); and `route_to` requires `activatable`
/// (navigation fires on activation — a route that can never fire is a dead declaration).
/// Returns one problem string per offense (empty = the component fits its contract). The
/// caller resolves the contract schema (registry lookup by the declared (name, version)).
std::vector<std::string> check_bindings(const UiComponent& component, const Schema& contract);

} // namespace loom

#endif // ZEN_UI_COMPONENT_HPP
