// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_UI_PIXEL_HPP
#define ZEN_UI_PIXEL_HPP

// The pixel projection's layout: it resolves the intent-only widget tree (zen/ui/tree.hpp) into
// paint-ordered draw commands and pointer targets, owning no window, font library or display,
// so it is testable everywhere. Pixel geometry exists only here and in whatever executes the
// commands, as terminal cells exist only in the terminal renderer. A complete renderer adds a
// thin executor and an input mapper to the same semantic InputEvents; this repository ships the
// layout and no executor.

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

/// Injected text metrics: the line height and the pixel advance of a UTF-8 string, so layout is
/// deterministic under test and right under a real typeface. Widths are taken as additive across
/// codepoints (no kerning in wrap and truncate decisions). Tests inject a fixed and a
/// proportional width, since uniform widths make "fits the bound" and "counts the codepoints"
/// the same sentence.
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

/// Lay the tree out into `viewport`: paint-ordered draw commands and interactive targets. Pure:
/// the same tree and metrics give the same scene. Overflow is honored: Wrap breaks Text at
/// spaces, or inside a word at a codepoint boundary; Truncate ellipsizes at one; Grow draws at
/// natural size, clipped by the viewport; Scroll keeps a List's selection visible, else follows
/// the tail. List and Log rows are single lines, truncated. Weight splits stack space as the
/// terminal renderer does (64-bit sums).
PxScene px_layout(const Widget& root, PxRect viewport, const PxMetrics& m);

/// The TOPMOST (last-added) target containing the point, or nullptr — pointer hit-testing for
/// the input mapper.
const PxTarget* px_hit(const PxScene& scene, int x, int y) noexcept;

} // namespace loom

#endif // ZEN_UI_PIXEL_HPP
