#ifndef ZEN_UI_PIXEL_HPP
#define ZEN_UI_PIXEL_HPP

// The PIXEL projection's logic — one renderer's brain, kept SDL-free so it is provable
// everywhere the suite runs (no display, no SDL, no fonts). This is NOT part of the tree
// vocabulary: pixel-space geometry exists ONLY here and below (in the SDL skin that executes
// these commands), exactly as terminal cells exist only in the TUI's Grid. The tree stays
// intent-only; this layer RESOLVES intent into rectangles.
//
// The split mirrors the TUI's: tui_render.cpp = layout-to-cells + key mapping (the only place
// cells and raw keys exist); here = layout-to-draw-commands (the only place pixel rects exist),
// and the SDL skin (src/ui/sdl/) = the only place raw SDL events, windows, and textures exist.
// A renderer is: px_layout (this, pure) + a thin executor (SDL) + an input mapper (SDL events
// -> the same semantic InputEvents the TUI's tui_map_key produces).
//
// Text metrics are INJECTED (PxMetrics.text_width) so the layout is deterministic under test
// (a fixed per-codepoint width) and honest under SDL_ttf (real advances). Widths are treated
// as additive across codepoints — the thin renderer's stated simplification (kerning-free
// wrap/truncate decisions; the glyph rasterizer still draws whatever it draws).
//
// Overflow gets REAL meaning here (the hint the TUI ignores, this projection honors):
//   - a Text node with Wrap lays out as multiple lines, broken at spaces when possible and
//     hard-broken INSIDE a word only at a codepoint boundary (never mid-UTF-8-sequence);
//   - Truncate ellipsizes at a codepoint boundary;
//   - Grow draws at natural size (clipped only by the viewport);
//   - Scroll on a List/Log keeps the selection visible (else tail-follows), as in the TUI.
// List/Log rows are single-line by design (a row wider than the node truncates with an
// ellipsis; wrapped multi-line rows are a named refinement, not built).

#include <zen/ui/tree.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

/// A pixel-space rectangle. Exists only on the renderer side — never on the tree.
struct PxRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    friend bool operator==(const PxRect&, const PxRect&) = default;

    bool contains(int px, int py) const noexcept {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

/// The visual ROLE of a command — semantic, so the executor picks colors/styles and the
/// command list itself stays theme-free (a light and a dark executor run the same commands).
enum class PxRole : std::uint8_t {
    Background, ///< the viewport clear
    Title,      ///< Region/List/Log headings
    Body,       ///< content text: Text bodies, rows, Field prompt+value
    Hint,       ///< the Field's engine-produced guidance line
    Selection,  ///< the selected row's fill bar
    Focus,      ///< the focused node's title-line fill
    SlotMarker  ///< a Slot's open-hole marker line
};

/// One draw command. A single tagged value type (the house style — cf. Widget): per-op-unused
/// fields stay zeroed, the whole scene is comparable and assertable in tests.
struct PxCmd {
    enum class Op : std::uint8_t {
        Fill,     ///< fill `rect` with `role`'s color
        Text,     ///< draw `text` at (tx, ty) in `role`'s color
        PushClip, ///< clip subsequent commands to `rect` (executor keeps the stack)
        PopClip   ///< restore the previous clip
    };
    Op op = Op::Fill;
    PxRect rect;
    PxRole role = PxRole::Body;
    std::string text;
    int tx = 0; ///< Text: left edge of the run
    int ty = 0; ///< Text: TOP of the line (the executor draws the glyph box below this)

    friend bool operator==(const PxCmd&, const PxCmd&) = default;
};

/// An interactive target the layout discovered: where a pointer act lands, and on what. The
/// input mapper resolves a click through these into a semantic InputEvent (SelectAt/Activate/
/// ...) — the pointer analogue of the TUI's key map. `node` points into the caller's tree
/// (valid as long as the laid-out tree outlives the scene); item_index -1 = the node itself,
/// >= 0 = that row of a List/Log.
struct PxTarget {
    PxRect rect;
    const Widget* node = nullptr;
    int item_index = -1;
};

/// A laid-out frame: the draw commands in paint order + the interactive targets.
struct PxScene {
    std::vector<PxCmd> cmds;
    std::vector<PxTarget> targets;
};

/// Injected text metrics: the line height and the pixel advance of a UTF-8 string. Tests use a
/// fixed per-codepoint width; the SDL skin uses TTF advances.
struct PxMetrics {
    int line_height = 16;
    int pad = 4; ///< inner padding for selection bars / marker boxes
    std::function<int(std::string_view)> text_width;
};

/// Count the UTF-8 codepoints of `s` (an invalid byte counts as one codepoint — total, never
/// throwing; the boundary-safety the wrap/truncate logic needs, exposed for tests/metrics).
std::size_t px_codepoint_count(std::string_view s) noexcept;

/// Greedy word-wrap to `max_width`: break at spaces when possible; a word wider than the whole
/// width hard-breaks INSIDE the word — but only ever at a codepoint boundary (never splitting a
/// UTF-8 sequence). Breaking at a space consumes that space. A non-positive width yields the
/// text as one line (nothing sane to do — the clip bounds it).
std::vector<std::string> px_wrap(std::string_view text, int max_width, const PxMetrics& m);

/// Truncate to `max_width` with a trailing ellipsis, cutting only at a codepoint boundary.
/// Text that already fits is returned unchanged (no ellipsis).
std::string px_truncate(std::string_view text, int max_width, const PxMetrics& m);

/// Lay the tree out into `viewport`: intent + relationship -> paint-ordered draw commands and
/// interactive targets. Pure: same tree + same metrics = same scene (pinned in tests). The
/// weight grow-hint splits stack space exactly as the TUI resolves it (64-bit accumulation).
PxScene px_layout(const Widget& root, PxRect viewport, const PxMetrics& m);

/// The TOPMOST (last-added) target containing the point, or nullptr — pointer hit-testing for
/// the input mapper.
const PxTarget* px_hit(const PxScene& scene, int x, int y) noexcept;

} // namespace loom

#endif // ZEN_UI_PIXEL_HPP
