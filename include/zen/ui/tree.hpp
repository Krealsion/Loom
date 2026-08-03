// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_UI_TREE_HPP
#define ZEN_UI_TREE_HPP

// The renderer-agnostic semantic widget tree — the Loom's UI vocabulary. A UI is emitted as a
// tree of INTENT and RELATIONSHIP — never absolute position or size. The SAME tree a terminal
// renderer resolves to box-characters and arrow-key focus, a graphical renderer resolves to
// rectangles and a mouse (like HTML: one semantic description, many renderers — a screen reader
// produces zero pixels from the same DOM). Widget's member set is closed and geometry-free (no
// x/y/width/height member exists to write), so layout can only ever happen in a renderer. A
// name-based compile-time fence below additionally blocks the common coordinate spellings from
// being re-added by accident — defense in depth, not unrepresentability (int x fails to build,
// int px compiles clean); see the note at the fence for its limits.
//
// Lifted out of the console (Phase B): the vocabulary lives here, in its own target (zen-ui),
// with NO console dependency — the console is one consumer (it emits its own interface as this
// tree; see zen/console/ui.hpp), the TUI renderer another, the SDL2 renderer a third. The
// ~50-line outline walk (render_outline) is the standing renderer-agnosticism proof: it
// consumes the same tree and shows it carries meaning, not medium.

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace loom {

/// The semantic widget kinds. Arrangement: VStack/HStack (children in a vertical/horizontal
/// RELATIONSHIP — not a grid of cells) and Region (a titled area wrapping one child). Content:
/// List (an ordered set of selectable lines), Log (an append-oriented sequence), Text (static
/// text), Field (an input affordance carrying an engine-produced guidance hint). Composition:
/// Slot (a typed OPEN HOLE a component leaves to be filled later — see component.hpp; its
/// children are the design-time placeholder preview).
enum class WidgetKind : std::uint8_t { VStack, HStack, Region, List, Log, Text, Field, Slot };

/// Stable spelling of a widget kind — the vocabulary's public contract (it is the wire spelling
/// a serialized component carries; see component.hpp). Do not rename these.
const char* name_of(WidgetKind k) noexcept;
/// Parse a stable spelling back to its kind; nullopt for an unknown spelling (the decode path
/// REFUSES unknown kinds — there is no silent-blank fate from the wire).
std::optional<WidgetKind> widget_kind_from(std::string_view spelling) noexcept;

/// Cross-cutting overflow POLICY (how content exceeding its area behaves), resolved per-renderer
/// in that medium's own units (rows for the terminal, pixels for a GUI). It is a policy, NEVER a
/// size.
enum class Overflow : std::uint8_t { Grow, Scroll, Wrap, Truncate };

/// Stable spelling of an overflow policy (the wire spelling; see component.hpp).
const char* name_of(Overflow o) noexcept;
/// Parse a stable spelling back to its policy; nullopt for an unknown spelling.
std::optional<Overflow> overflow_from(std::string_view spelling) noexcept;

/// One node of the widget tree. A single value type: copyable and DEEPLY comparable (defaulted
/// ==), so the whole tree is one value — trivially asserted in tests and diffable by region for
/// retained-mode partial redraw. CONSTRUCT ONLY via the named constructors below, so per-kind-
/// unused fields are zero-initialized consistently (otherwise two logically-identical nodes
/// could differ on leftover junk and pollute the == proof and the dirty-by-diff backstop).
///
/// THE BET, MADE STRUCTURAL: there is NO x/y/width/height/row/col member here — a closed,
/// geometry-free member set, with no positional field to write (a name-based fence below also makes
/// adding one of those names fail to build; defense in depth, not unrepresentability). A renderer
/// alone decides position. `weight` is a RELATIVE grow hint (0 = natural), never an ABSOLUTE size —
/// the outline renderer ignores it (proving it not tree content); the TUI resolves it as relative
/// cells.
struct Widget {
    WidgetKind kind = WidgetKind::Text;
    std::string region_id;          ///< stable key for dirty/diff (empty = pure decoration)
    std::string title;              ///< Region / List / Log heading
    std::string content;            ///< Text body (in a schematic: the design-time placeholder)
    std::string prompt;             ///< Field label
    std::string value;              ///< Field current (in-progress) value
    std::string hint;               ///< Field engine-produced next-choice guidance
    std::vector<std::string> items; ///< List rows / Log lines (pre-rendered domain text;
                                    ///< in a schematic: the design-time placeholder rows)
    int selected_index = -1;        ///< List selection: an index INTO items, never a y (-1 = none)

    // Abstract interaction INTENT — what the operator may DO here, never which key/gesture does
    // it (the renderer maps its medium onto these: the TUI keys, a GUI clicks/drags). These
    // replaced `focusable`: no renderer ever read it, and focus-eligibility is derivable
    // (a node you can act on is a node you can focus) — the intent is the load-bearing fact.
    bool activatable = false;       ///< the node can be acted on (Activate on its selection)
    bool editable = false;          ///< the node accepts text editing (a Field's nature)
    bool reorderable = false;       ///< the node's items may be reordered (declared intent;
                                    ///< no built renderer consumes it yet — the Builder composes it)

    bool focused = false;           ///< the focus MARKER (a flag, never a coordinate)
    std::uint16_t weight = 0;       ///< relative grow hint (0 = natural); never an absolute size
    Overflow overflow = Overflow::Grow;

    // Component-vocabulary fields (see component.hpp; all default-empty on live console trees).
    std::string from_field;         ///< data binding: the contract field feeding this node's
                                    ///< content/items ("" = static). Declared, not yet resolved —
                                    ///< live binding is a later phase; design-time shows placeholder.
    std::string route_to;           ///< navigation intent: the view address an Activate should
                                    ///< route to ("" = none). The routing RUNTIME is a later phase.
    std::string slot_name;          ///< Slot: the open hole's name ("" on non-slots)
    std::string slot_accepts;       ///< Slot: what may fill it — "Component", "Route", or a
                                    ///< scalar Kind spelling ("Int"/"Float"/"Text"/"Bool")

    std::vector<Widget> children;   ///< child relationship (by value — the tree is one value);
                                    ///< on a Slot: the design-time placeholder preview

    friend bool operator==(const Widget&, const Widget&) = default;
};

// ---- The bet, enforced at compile time (one honest layer — a name-based convention) ----
// A member-detection trait per common geometry name: has_<name><T> is true iff T has a member
// reachable as `t.<name>`. We static_assert NONE exist on Widget, so adding e.g. `int x;` fails
// to COMPILE. This catches only the enumerated names (it would miss a coordinate named `px`), so
// it is one layer, paired with code review and the equality-comparable guard — NOT airtight, and
// not sold as such. The strongest guarantee remains structural: no such member exists to write.
namespace detail {
#define ZEN_UI_HAS_MEMBER(NAME)                                                                    \
    template <class T, class = void> struct has_##NAME : std::false_type {};                      \
    template <class T>                                                                             \
    struct has_##NAME<T, std::void_t<decltype(std::declval<T&>().NAME)>> : std::true_type {};
ZEN_UI_HAS_MEMBER(x)
ZEN_UI_HAS_MEMBER(y)
ZEN_UI_HAS_MEMBER(w)
ZEN_UI_HAS_MEMBER(h)
ZEN_UI_HAS_MEMBER(width)
ZEN_UI_HAS_MEMBER(height)
ZEN_UI_HAS_MEMBER(row)
ZEN_UI_HAS_MEMBER(col)
ZEN_UI_HAS_MEMBER(top)
ZEN_UI_HAS_MEMBER(left)
#undef ZEN_UI_HAS_MEMBER
} // namespace detail

static_assert(!detail::has_x<Widget>::value && !detail::has_y<Widget>::value &&
                  !detail::has_w<Widget>::value && !detail::has_h<Widget>::value &&
                  !detail::has_width<Widget>::value && !detail::has_height<Widget>::value &&
                  !detail::has_row<Widget>::value && !detail::has_col<Widget>::value &&
                  !detail::has_top<Widget>::value && !detail::has_left<Widget>::value,
              "Widget must not carry absolute geometry — position is the renderer's job (the bet "
              "of Stage 3: intent and relationship, never coordinates).");
static_assert(std::equality_comparable<Widget>,
              "Widget must stay value-comparable — the headless structural-equality proof and the "
              "dirty-by-diff backstop rely on operator==.");

// ---- Named constructors: the ONLY sanctioned way to build a Widget ----
// They zero-initialize every per-kind-unused field consistently, so two logically-identical
// trees compare equal. Do not raw-aggregate-init a Widget at a call site.
Widget vstack(std::string region_id, std::vector<Widget> children);
Widget hstack(std::string region_id, std::vector<Widget> children);
Widget region(std::string region_id, std::string title, Widget child);
Widget list(std::string region_id, std::string title, std::vector<std::string> items,
            int selected_index, bool activatable, bool focused,
            Overflow overflow = Overflow::Scroll);
Widget log_widget(std::string region_id, std::string title, std::vector<std::string> entries,
                  Overflow overflow = Overflow::Scroll);
Widget text_widget(std::string content);
/// A Field is an input affordance by nature: it is always `editable` (a non-editable value
/// display is a Text). Interaction intent is still DECLARED on the node — the constructor is
/// where the declaration happens.
Widget field(std::string prompt, std::string value, std::string hint, bool focused);
/// A typed open hole (see component.hpp for the design-time constructor that fills its
/// placeholder preview with stress values). `accepts` declares what may fill it.
Widget slot(std::string slot_name, std::string accepts, std::vector<Widget> placeholder);

// ---- Input: renderer-agnostic semantic actions (symmetric with the output tree) ----
// What the operator MEANS, not which key or gesture. The TUI maps raw keys onto these; the SDL
// renderer maps clicks/keys onto the same set — so every frontend inherits the input
// abstraction. The raw-key -> Action map lives in the TUI alone; the raw-SDL-event -> Action
// map lives in the SDL renderer alone.
enum class Action : std::uint8_t {
    None,       ///< a true no-op: an unknown/unhandled key maps here (dispatched as nothing)
    FocusNext,  ///< move focus to the next focusable region
    FocusPrev,  ///< move focus to the previous focusable region
    SelectUp,   ///< move the selection up within the focused List
    SelectDown, ///< move the selection down within the focused List
    SelectAt,   ///< select a SPECIFIC row (InputEvent::index) — the pointer medium's basic
                ///< selection act (a click names a row; keys walk). Shared vocabulary: a
                ///< terminal with mouse reporting could emit it too.
    Activate,   ///< act on the focused selection (e.g. prefill the command with its weave/ref)
    Edit,       ///< type a character into the command field (carried in InputEvent::ch)
    Backspace,  ///< delete the last character of the command field
    Submit,     ///< submit the command field (compose via the ladder + gate-send)
    Cancel      ///< clear the in-progress command field
};

/// One input event: a semantic action, plus a character for Edit and a row index for SelectAt.
/// (Edit is byte-oriented: multi-byte UTF-8 input arrives as one Edit per byte, appended in
/// order — a std::string target reassembles the sequence; codepoint-aware editing is a named
/// seam for the phase that needs it.)
struct InputEvent {
    Action action = Action::None;
    char ch = 0;
    int index = -1; ///< SelectAt: the row to select (an index into items, never a coordinate)
};

// ---- The headless outline renderer (the renderer-agnosticism proof) ----

/// A minimal renderer: a tree-to-text-outline walk. Consumes the SAME Widget tree the real
/// renderers do, producing a plain indented outline (no cells, no pixels, no coordinates).
/// It prints the tree's MEANING — kinds, titles, content, interaction intent, bindings, routes,
/// slots — and deliberately IGNORES `weight` and the overflow policy (hints a renderer resolves,
/// not tree content). Takes the tree by const& and never mutates it.
std::string render_outline(const Widget& root);

} // namespace loom

#endif // ZEN_UI_TREE_HPP
