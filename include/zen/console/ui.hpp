#ifndef ZEN_CONSOLE_UI_HPP
#define ZEN_CONSOLE_UI_HPP

// UI-as-data (Stage 3): the renderer-agnostic semantic widget tree. The console's own interface
// is emitted as a tree of INTENT and RELATIONSHIP — never absolute position or size. The SAME
// tree a terminal renderer resolves to box-characters and arrow-key focus, a GUI later resolves
// to rectangles and a mouse (like HTML: one semantic description, many renderers — a screen
// reader produces zero pixels from the same DOM). This is the structural support behind "the
// GUI inherits the engine": Widget's member set is closed and geometry-free (no x/y/width/height
// member exists to write), so layout can only ever happen in a renderer. A name-based
// compile-time fence below additionally blocks the common coordinate spellings from being
// re-added by accident — defense in depth, not unrepresentability (int x fails to build, int px
// compiles clean); see the note at the fence for its limits.
//
// The tree is built in the engine LIBRARY from the engine's public domain data (weaves, the
// reply buffer, the tap, the registry-derived guidance) — renderer-agnostic and fully testable
// with no terminal. There is ONE real renderer (the full-screen TUI in console_tui.cpp — the only
// place positions, sizes, and cells exist); a ~50-line test-only outline walk (here) consumes the
// same tree to PROVE it carries no medium — a renderer-agnosticism proof, not a 2nd production skin.

#include <zen/console/console.hpp>

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
#define ZEN_CONSOLE_HAS_MEMBER(NAME)                                                              \
    template <class T, class = void> struct has_##NAME : std::false_type {};                      \
    template <class T>                                                                             \
    struct has_##NAME<T, std::void_t<decltype(std::declval<T&>().NAME)>> : std::true_type {};
ZEN_CONSOLE_HAS_MEMBER(x)
ZEN_CONSOLE_HAS_MEMBER(y)
ZEN_CONSOLE_HAS_MEMBER(w)
ZEN_CONSOLE_HAS_MEMBER(h)
ZEN_CONSOLE_HAS_MEMBER(width)
ZEN_CONSOLE_HAS_MEMBER(height)
ZEN_CONSOLE_HAS_MEMBER(row)
ZEN_CONSOLE_HAS_MEMBER(col)
ZEN_CONSOLE_HAS_MEMBER(top)
ZEN_CONSOLE_HAS_MEMBER(left)
#undef ZEN_CONSOLE_HAS_MEMBER
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
// What the operator MEANS, not which key. The TUI maps raw keys onto these; a GUI maps
// clicks/keys onto the same set — so a future GUI inherits the input abstraction too. The
// raw-key -> Action mapping is the ONLY terminal-coupled code, and it lives in the TUI alone.
enum class Action : std::uint8_t {
    None,       ///< a true no-op: an unknown/unhandled key maps here (dispatched as nothing)
    FocusNext,  ///< move focus to the next focusable region
    FocusPrev,  ///< move focus to the previous focusable region
    SelectUp,   ///< move the selection up within the focused List
    SelectDown, ///< move the selection down within the focused List
    Activate,   ///< act on the focused selection (e.g. prefill the command with its weave/ref)
    Edit,       ///< type a character into the command field (carried in InputEvent::ch)
    Backspace,  ///< delete the last character of the command field
    Submit,     ///< submit the command field (compose via the ladder + gate-send)
    Cancel      ///< clear the in-progress command field
};

/// One input event: a semantic action, plus a character for Edit.
struct InputEvent {
    Action action = Action::None;
    char ch = 0;
};

/// Which region currently holds focus. Three focusable regions cycle under FocusNext/FocusPrev;
/// the tap log is read-only (not focusable).
enum class Focus : std::uint8_t { Weaves, Buffer, Compose };

/// The console's presentation/interaction state — what the ENGINE must not own: focus, the
/// in-progress command line, list selections, and the last compose result to surface. Passed to
/// emit_ui_tree so the engine's domain state stays pure.
struct UiState {
    Focus focus = Focus::Compose;       ///< start on the command field
    std::string partial_input;          ///< the in-progress compose command line
    int weave_cursor = 0;               ///< selection index in the Weaves list
    int buffer_cursor = 0;              ///< selection index in the Buffer list
    std::optional<Composed> pending;    ///< last ladder result to surface (NeedsInput / Error)
};

// ---- Engine-produced guidance + tree emission (renderer-agnostic; no terminal) ----

/// The next-choice guidance for a partial command line, derived from the registry + the partial
/// input — engine-produced, so it is renderer-agnostic (the TUI shows it inline; a GUI shows the
/// same string as a dropdown). Advances with input: empty -> "choose a weave"; a weave id -> its
/// shapes; a shape+version -> its fields. Reads only the engine's public domain data.
std::string guidance_for(const Console& engine, std::string_view partial_command);

/// Build the console's current UI tree from the engine's public domain data + the presentation
/// state: a root VStack of a Bus region (an HStack of a Weaves List and a Tap Log), a Buffer
/// List (m1, m2, ...), and a Compose Field (the partial command + the engine-produced guidance
/// hint). If `ui.pending` carries a NeedsInput/Error result, the tree gains a prompt region. The
/// tree is a fresh value each call (retained-mode is the renderer diffing successive trees).
Widget emit_ui_tree(const Console& engine, const UiState& ui);

// ---- The headless outline renderer (the second renderer — the bet's structural proof) ----

/// A minimal renderer: a tree-to-text-outline walk. Consumes the SAME Widget tree the TUI does,
/// producing a plain indented outline (no cells, no coordinates). Two renderers, one tree — the
/// proof that the tree bakes in no medium. Deliberately IGNORES `weight` and the overflow policy
/// (it lists every item) — showing those are per-renderer concerns, not tree content. Takes the
/// tree by const& and never mutates it.
std::string render_outline(const Widget& root);

// ---- The renderer-agnostic UI controller ----

/// Owns the presentation state (UiState), maps semantic Actions onto engine calls, and emits the
/// current widget tree. The TUI is THIS plus termios raw I/O plus a tree->cells layout; a
/// headless test drives THIS with scripted InputEvents and asserts on the emitted tree — so the
/// dataflow/interaction brain is proven with no terminal and the GUI inherits it whole.
class ConsoleUi {
public:
    explicit ConsoleUi(Console& engine) : engine_(engine) {}

    /// Apply one semantic action (the only mutation path). Submit composes via the ladder and
    /// pumps; the result is surfaced via the tree (Ready -> reply buffered; NeedsInput/Error ->
    /// a prompt region next emit).
    void dispatch(const InputEvent& ev);

    /// Emit the current widget tree (engine-built standing regions + any transient prompt).
    Widget tree() const;

    const UiState& state() const noexcept { return ui_; }
    Console& engine() noexcept { return engine_; }

private:
    void submit_command(); ///< parse the command line, run the ladder, surface the result

    Console& engine_;
    UiState ui_;
};

} // namespace loom

#endif // ZEN_CONSOLE_UI_HPP
