// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include <zen/ui/component.hpp>
#include <zen/ui/pixel.hpp>
#include <zen/ui/tree.hpp>

#include <climits>
#include <string>
#include <string_view>
#include <vector>

// The pixel projection's LOGIC, proven with no display, no window and no font library:
// injected metrics make the layout deterministic, so tree -> draw-commands is pinned like any
// pure function. Two metrics are injected on purpose — a fixed per-codepoint width, and a
// deliberately proportional one, because under uniform widths "fits the bound" and "counts the
// codepoints" are the same sentence and a real typeface makes them different sentences.
// Executing the commands belongs to a presentation; everything that can be proven without one
// IS proven here, on every platform the suite runs.

using namespace loom;

namespace {

// Fixed metrics: every codepoint 8px wide, lines 10px. Deterministic and unicode-aware (a
// 4-byte emoji is ONE codepoint = 8px), which is exactly what the wrap/truncate logic must
// respect.
PxMetrics fixed_metrics() {
    PxMetrics m;
    m.line_height = 10;
    m.pad = 2;
    m.text_width = [](std::string_view s) {
        return static_cast<int>(px_codepoint_count(s)) * 8;
    };
    return m;
}

// Length of the UTF-8 sequence starting at `first` (1 for a stray/invalid byte) — the
// walker the proportional metric below needs to charge per codepoint rather than per byte.
std::size_t lead_byte_len(unsigned char first) {
    if ((first & 0x80u) == 0x00u) {
        return 1;
    }
    if ((first & 0xE0u) == 0xC0u) {
        return 2;
    }
    if ((first & 0xF0u) == 0xE0u) {
        return 3;
    }
    if ((first & 0xF8u) == 0xF0u) {
        return 4;
    }
    return 1;
}

// The advance of ONE codepoint under a deliberately PROPORTIONAL metric: every value below
// differs from the others, the space and the ellipsis glyph included, and none is a multiple
// of a common width. This is the shape a real typeface has — and it is the shape under which
// "sum the advances" and "count the codepoints" stop agreeing, so a layout that quietly
// counted would land somewhere else.
int proportional_advance(std::string_view cp) {
    if (cp == "\xE2\x80\xA6") {
        return 15; // the ellipsis: its own glyph, wider than the run it stands in for
    }
    if (cp.size() != 1) {
        return 12; // any other multi-byte codepoint
    }
    switch (cp.front()) {
    case 'W':
        return 20;
    case 'm':
        return 18;
    case ' ':
        return 6;
    case 'l':
        return 5;
    case 'i':
        return 4;
    default:
        return 10;
    }
}

// Proportional metrics: same injected seam, non-uniform answers. `pad` is 0 so that WIDTH is
// the only variable these cases introduce.
PxMetrics proportional_metrics() {
    PxMetrics m;
    m.line_height = 10;
    m.pad = 0;
    m.text_width = [](std::string_view s) {
        int total = 0;
        for (std::size_t i = 0; i < s.size();) {
            const std::size_t n = lead_byte_len(static_cast<unsigned char>(s[i]));
            const std::size_t take = (i + n <= s.size()) ? n : s.size() - i;
            total += proportional_advance(s.substr(i, take));
            i += take;
        }
        return total;
    };
    return m;
}

// A UTF-8 continuation byte (0b10xxxxxx) — a line/fragment must never START with one, or a
// codepoint was split.
bool starts_mid_codepoint(std::string_view s) {
    return !s.empty() && (static_cast<unsigned char>(s.front()) & 0xC0u) == 0x80u;
}

const PxCmd* find_text(const PxScene& scene, std::string_view needle) {
    for (const PxCmd& c : scene.cmds) {
        if (c.op == PxCmd::Op::Text && c.text.find(needle) != std::string::npos) {
            return &c;
        }
    }
    return nullptr;
}

int count_op(const PxScene& scene, PxCmd::Op op) {
    int n = 0;
    for (const PxCmd& c : scene.cmds) {
        if (c.op == op) {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_SUITE("pixel") {

TEST_CASE("wrap is greedy, space-preferring, and never splits a codepoint") {
    const PxMetrics m = fixed_metrics();

    // 10 codepoints per line at width 80. "alpha beta gamma" breaks at the spaces.
    const std::vector<std::string> lines = px_wrap("alpha beta gamma", 80, m);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "alpha beta"); // exactly 10 cps — fits; the breaking space is consumed
    CHECK(lines[1] == "gamma");

    // A word wider than the whole width hard-breaks INSIDE the word — at codepoint boundaries.
    const std::vector<std::string> hard = px_wrap("abcdefghijklmnopqrst", 64, m); // 8 cps/line
    REQUIRE(hard.size() == 3);
    CHECK(hard[0] == "abcdefgh");
    CHECK(hard[1] == "ijklmnop");
    CHECK(hard[2] == "qrst");

    // The Unicode gauntlet: every produced line fits, none starts mid-codepoint, and the
    // emoji survives intact somewhere (4 bytes, never split).
    const std::string uni = stress_text_unicode();
    const std::vector<std::string> ulines = px_wrap(uni, 80, m);
    REQUIRE(ulines.size() > 1); // it genuinely wrapped
    std::string reassembled;
    for (const std::string& line : ulines) {
        CHECK_FALSE(starts_mid_codepoint(line));
        CHECK(m.text_width(line) <= 80);
        reassembled += line;
    }
    // Nothing but the consumed break-spaces may differ: byte content survives in order.
    std::string original_no_spaces;
    for (char ch : uni) {
        if (ch != ' ') {
            original_no_spaces.push_back(ch);
        }
    }
    std::string reassembled_no_spaces;
    for (char ch : reassembled) {
        if (ch != ' ') {
            reassembled_no_spaces.push_back(ch);
        }
    }
    CHECK(reassembled_no_spaces == original_no_spaces);
    CHECK(reassembled.find("\xF0\x9F\xA7\xB6") != std::string::npos); // the yarn emoji, whole
}

TEST_CASE("truncate ellipsizes at a codepoint boundary and leaves fitting text alone") {
    const PxMetrics m = fixed_metrics();
    CHECK(px_truncate("short", 80, m) == "short"); // fits: unchanged, no ellipsis

    // Width 40 = 5 codepoints; the ellipsis is one, so 4 content cps survive.
    CHECK(px_truncate("abcdefghij", 40, m) == "abcd\xE2\x80\xA6");

    // Cutting inside the CJK run must land BETWEEN codepoints (each CJK cp is 3 bytes).
    const std::string cut = px_truncate(stress_text_unicode(), 200, m); // 25 cps
    CHECK_FALSE(starts_mid_codepoint(cut));
    CHECK(cut.size() >= 3);
    CHECK(cut.substr(cut.size() - 3) == "\xE2\x80\xA6"); // ends with the ellipsis
    // Everything before the ellipsis is a clean prefix of the original bytes.
    CHECK(stress_text_unicode().rfind(cut.substr(0, cut.size() - 3), 0) == 0);
}

TEST_CASE("the same tree the TUI draws lays out to pixel draw-commands (one tree, two media)") {
    // The exact node set the console emits — a projection adds no node of its own.
    Widget rows = list("rows", "Rows", {"alpha", "beta", "gamma"}, 1, /*activatable=*/true,
                       /*focused=*/true);
    const Widget root =
        region("frame", "Preview", vstack("body", {rows, field("say>", "hi", "type", false)}));

    const PxMetrics m = fixed_metrics();
    const PxScene scene = px_layout(root, PxRect{0, 0, 400, 100}, m);

    // Paint order: background clear first, then content.
    REQUIRE_FALSE(scene.cmds.empty());
    CHECK(scene.cmds[0].op == PxCmd::Op::Fill);
    CHECK(scene.cmds[0].role == PxRole::Background);
    CHECK(scene.cmds[0].rect == PxRect{0, 0, 400, 100});

    // The region title on the first line; the list title on the next (region eats one line).
    const PxCmd* title = find_text(scene, "Preview");
    REQUIRE(title != nullptr);
    CHECK(title->ty == 0);
    CHECK(title->role == PxRole::Title);
    const PxCmd* list_title = find_text(scene, "Rows");
    REQUIRE(list_title != nullptr);
    CHECK(list_title->ty == 10);

    // The focused list gets a Focus fill on its title line; the selected row a Selection bar
    // painted BEFORE its text (fill-then-text order is what makes the bar a background).
    bool saw_focus_fill = false;
    bool selection_before_beta = false;
    bool saw_selection = false;
    for (const PxCmd& c : scene.cmds) {
        if (c.op == PxCmd::Op::Fill && c.role == PxRole::Focus) {
            saw_focus_fill = true;
        }
        if (c.op == PxCmd::Op::Fill && c.role == PxRole::Selection) {
            saw_selection = true;
        }
        if (c.op == PxCmd::Op::Text && c.text == "beta" && saw_selection) {
            selection_before_beta = true;
        }
    }
    CHECK(saw_focus_fill);
    CHECK(selection_before_beta);

    // Rows are clipped (a push/pop pair per list) and balanced overall.
    CHECK(count_op(scene, PxCmd::Op::PushClip) == count_op(scene, PxCmd::Op::PopClip));
    CHECK(count_op(scene, PxCmd::Op::PushClip) >= 1);

    // Interaction intent became pointer TARGETS: one per visible row of the activatable list,
    // plus the editable field's line. Row 1 ("beta") is selected; its target carries index 1.
    REQUIRE(scene.targets.size() >= 4); // 3 rows + the field
    bool row1 = false;
    bool field_target = false;
    for (const PxTarget& t : scene.targets) {
        if (t.item_index == 1 && t.node != nullptr && t.node->kind == WidgetKind::List) {
            row1 = true;
        }
        if (t.item_index == -1 && t.node != nullptr && t.node->kind == WidgetKind::Field) {
            field_target = true;
        }
    }
    CHECK(row1);
    CHECK(field_target);
}

TEST_CASE("overflow gets REAL meaning: wrap makes lines, truncate makes an ellipsis, grow stays whole") {
    const PxMetrics m = fixed_metrics();
    Widget wrapped = text_widget("alpha beta gamma delta epsilon");
    wrapped.overflow = Overflow::Wrap;
    Widget truncated = text_widget("alpha beta gamma delta epsilon");
    truncated.overflow = Overflow::Truncate;
    Widget grown = text_widget("alpha beta gamma delta epsilon");
    grown.overflow = Overflow::Grow;

    // Width 100 - 2*pad = 96 px = 12 codepoints of content space.
    const PxScene w_scene = px_layout(wrapped, PxRect{0, 0, 100, 100}, m);
    int text_cmds = 0;
    for (const PxCmd& c : w_scene.cmds) {
        if (c.op == PxCmd::Op::Text) {
            ++text_cmds;
            CHECK(m.text_width(c.text) <= 96);
        }
    }
    CHECK(text_cmds > 1); // it genuinely wrapped into multiple lines

    const PxScene t_scene = px_layout(truncated, PxRect{0, 0, 100, 100}, m);
    const PxCmd* t_cmd = find_text(t_scene, "\xE2\x80\xA6");
    REQUIRE(t_cmd != nullptr); // one ellipsized line
    CHECK(m.text_width(t_cmd->text) <= 96);

    const PxScene g_scene = px_layout(grown, PxRect{0, 0, 100, 100}, m);
    const PxCmd* g_cmd = find_text(g_scene, "epsilon");
    REQUIRE(g_cmd != nullptr); // the whole run, unclipped — natural size
    CHECK(g_cmd->text == "alpha beta gamma delta epsilon");

    // THE SAME TREES render in the TUI too (which ignores the hint) — one tree, two media,
    // one honoring the policy, one not: exactly how a hint should behave.
    CHECK(render_outline(wrapped) == render_outline(truncated));
}

TEST_CASE("a slot projects as a marker plus its placeholder preview, stacked beneath") {
    const PxMetrics m = fixed_metrics();
    const Widget hole = open_slot("actions", "Int"); // placeholder: the Int stress value
    const PxScene scene = px_layout(hole, PxRect{0, 0, 800, 60}, m);

    const PxCmd* marker = find_text(scene, "slot actions: accepts Int");
    REQUIRE(marker != nullptr);
    CHECK(marker->role == PxRole::SlotMarker);
    CHECK(marker->ty == 0);
    const PxCmd* preview = find_text(scene, "-9223372036854775808");
    REQUIRE(preview != nullptr); // the stress preview, below the marker
    CHECK(preview->ty >= 10);
}

TEST_CASE("hit-testing resolves a point to the row the pointer names (topmost wins)") {
    const PxMetrics m = fixed_metrics();
    Widget rows = list("rows", "Rows", {"alpha", "beta", "gamma"}, -1, /*activatable=*/true,
                       /*focused=*/false);
    const PxScene scene = px_layout(rows, PxRect{0, 0, 200, 60}, m);

    // Title line at y 0..9; rows begin at y=10, one per 10px line.
    const PxTarget* hit = px_hit(scene, 5, 25); // second row's band
    REQUIRE(hit != nullptr);
    CHECK(hit->item_index == 1);
    REQUIRE(hit->node != nullptr);
    CHECK(hit->node->region_id == "rows");
    CHECK(px_hit(scene, 5, 5) == nullptr);      // the title is not a target
    CHECK(px_hit(scene, 500, 500) == nullptr);  // outside everything
}

TEST_CASE("weight splits pixel space exactly as it splits cells (the shared hint, resolved per medium)") {
    const PxMetrics m = fixed_metrics();
    Widget a = text_widget("a");
    a.weight = 1;
    Widget b = text_widget("b");
    b.weight = 3;
    const Widget root = vstack("v", {a, b});
    const PxScene scene = px_layout(root, PxRect{0, 0, 100, 80}, m);

    // 1:3 of 80px -> a at y=0 (band 0..19), b at y=20 (band 20..79).
    const PxCmd* ta = find_text(scene, "a");
    const PxCmd* tb = find_text(scene, "b");
    REQUIRE(ta != nullptr);
    REQUIRE(tb != nullptr);
    CHECK(ta->ty == 0);
    CHECK(tb->ty == 20);
}

TEST_CASE("degenerate areas stay defined: a squeezed list with a huge legal cursor, a node narrower than the ellipsis") {
    const PxMetrics m = fixed_metrics();

    // A list whose row area fits ZERO rows (15px total - 10px title = 5px < one line), with
    // selected_index at the wire-legal ceiling. The capacity guard must refuse to compute the
    // scroll window (selected_index - 0 + 1 was signed-overflow UB before the guard); under
    // the sanitized build UBSan enforces this pin, headlessly.
    Widget squeezed = list("sq", "Sq", {"a", "b", "c"}, 0, /*activatable=*/true,
                           /*focused=*/false);
    squeezed.selected_index = INT_MAX;
    const PxScene scene = px_layout(squeezed, PxRect{0, 0, 100, 15}, m);
    CHECK(find_text(scene, "Sq") != nullptr); // the title still draws
    CHECK(find_text(scene, "a") == nullptr);  // no row fits, no row drawn
    CHECK(scene.targets.empty());             // and nothing untargetable to click

    // A truncate node narrower than the ellipsis glyph itself: the string logic cannot fit
    // the bound, so the CLIP is the backstop — the text command sits inside a clip pair that
    // bounds it to the node's own rect (nothing paints into a neighbor).
    Widget narrow = text_widget("hello world");
    narrow.overflow = Overflow::Truncate;
    const PxRect tiny{0, 0, 10, 100};
    const PxScene t = px_layout(narrow, tiny, m);
    bool clipped_to_node = false;
    bool inside = false;
    for (const PxCmd& c : t.cmds) {
        if (c.op == PxCmd::Op::PushClip && c.rect == tiny) {
            clipped_to_node = true;
        } else if (c.op == PxCmd::Op::Text && clipped_to_node) {
            inside = true; // the (over-wide) ellipsis run is bounded by the node's clip
        } else if (c.op == PxCmd::Op::PopClip) {
            clipped_to_node = false;
        }
    }
    CHECK(inside);
    CHECK(count_op(t, PxCmd::Op::PushClip) == count_op(t, PxCmd::Op::PopClip));

    // A huge wrapped text in a SHORT node emits only the visible lines (+1 partial), never the
    // whole flood — the clip would hide the rest, but an executor still pays per command, and
    // a wire-legal giant content must not become millions of draws.
    std::string huge;
    for (int i = 0; i < 2000; ++i) {
        huge += "word ";
    }
    Widget flood = text_widget(huge);
    flood.overflow = Overflow::Wrap;
    const PxScene f = px_layout(flood, PxRect{0, 0, 100, 30}, m); // 3 lines fit (+1 partial)
    CHECK(count_op(f, PxCmd::Op::Text) <= 4);
    CHECK(count_op(f, PxCmd::Op::Text) >= 3); // and the visible ones ARE there

    // Hostile metrics degrade, never divide-by-zero: line_height 0 is clamped to 1px.
    PxMetrics degenerate = fixed_metrics();
    degenerate.line_height = 0;
    const PxScene d = px_layout(squeezed, PxRect{0, 0, 100, 15}, degenerate);
    CHECK_FALSE(d.cmds.empty()); // laid out (15 one-px lines), no UB — the clamp held
}

// ---- The same brain under a PROPORTIONAL metric --------------------------------------
//
// Everything above injects a uniform width, under which "this line is 80px" and "this line is
// ten codepoints" are the same sentence. A real typeface makes them different sentences, and
// the layout must be reading the first one. These cases feed the same injected seam a metric
// where text_width("W") != text_width("i") != text_width(" ") != text_width("…"), and pin what
// the layout ANSWERS — not that the injected function returned two different numbers.

TEST_CASE("proportional metrics: wrap sums real advances, so one bound holds different counts") {
    const PxMetrics prop = proportional_metrics();

    // "WWWWW iiiii" at 80px. Four W's already fill the bound (4 * 20), so the fifth
    // hard-breaks with no space seen yet; the tail then carries W + space + five i's
    // (20 + 6 + 5 * 4 = 46) and never needs a second break.
    const std::vector<std::string> wide_first = px_wrap("WWWWW iiiii", 80, prop);
    REQUIRE(wide_first.size() == 2);
    CHECK(wide_first[0] == "WWWW");
    CHECK(wide_first[1] == "W iiiii");

    // The SAME string at the SAME bound under the uniform metric breaks somewhere else: ten
    // codepoints per line, so the split lands at the space. The metric decided the answer.
    const std::vector<std::string> uniform = px_wrap("WWWWW iiiii", 80, fixed_metrics());
    REQUIRE(uniform.size() == 2);
    CHECK(uniform[0] == "WWWWW");
    CHECK(uniform[1] == "iiiii");
    CHECK(wide_first != uniform);

    // Lines bounded by the same width hold DIFFERENT numbers of codepoints — the whole
    // difference a proportional typeface makes — and every one of them still fits.
    CHECK(px_codepoint_count(wide_first[0]) == 4);
    CHECK(px_codepoint_count(wide_first[1]) == 7);
    for (const std::string& line : wide_first) {
        CHECK(prop.text_width(line) <= 80);
    }

    // The soft break carries the remainder's REAL width across, the consumed space included:
    // "iiiiiiiiii WWWW" breaks back to the space with one W (20px) already on the new line,
    // so all four W's (80px exactly) still fit it. Charge the wrong width for the space that
    // the break consumed and the tail splits a second time.
    const std::vector<std::string> narrow_first = px_wrap("iiiiiiiiii WWWW", 80, prop);
    REQUIRE(narrow_first.size() == 2);
    CHECK(narrow_first[0] == "iiiiiiiiii");
    CHECK(narrow_first[1] == "WWWW");
    CHECK(prop.text_width(narrow_first[1]) == 80);
}

TEST_CASE("proportional metrics: truncate reserves the ellipsis's own advance, not a cell") {
    const PxMetrics prop = proportional_metrics();
    CHECK(prop.text_width("\xE2\x80\xA6") == 15); // the reserved glyph is its own width

    // Wide glyphs: three W's (60) + the ellipsis (15) = 75; a fourth would be 95.
    CHECK(px_truncate("WWWWWWWW", 80, prop) == "WWW\xE2\x80\xA6");
    // Narrow glyphs, SAME bound: sixteen i's (64) + the ellipsis (15) = 79.
    CHECK(px_truncate(std::string(30, 'i'), 80, prop) == std::string(16, 'i') + "\xE2\x80\xA6");

    // Same bound, different surviving counts — and both results measure inside the bound,
    // which is the invariant a uniform metric can satisfy by accident.
    const std::string wide = px_truncate("WWWWWWWW", 80, prop);
    const std::string narrow = px_truncate(std::string(30, 'i'), 80, prop);
    CHECK(px_codepoint_count(wide) == 4);
    CHECK(px_codepoint_count(narrow) == 17);
    CHECK(prop.text_width(wide) <= 80);
    CHECK(prop.text_width(narrow) <= 80);

    // "It fits" is measured by advance too: twenty narrow glyphs are 80px and survive whole,
    // while five wide ones are 100px and do not.
    CHECK(px_truncate(std::string(20, 'i'), 80, prop) == std::string(20, 'i'));
    CHECK(px_truncate("WWWWW", 80, prop) != "WWWWW");

    // Codepoint boundaries hold under the non-uniform metric exactly as under the uniform one.
    const std::string cut = px_truncate(stress_text_unicode(), 80, prop);
    CHECK_FALSE(starts_mid_codepoint(cut));
    REQUIRE(cut.size() >= 3);
    CHECK(cut.substr(cut.size() - 3) == "\xE2\x80\xA6");
}

TEST_CASE("proportional metrics reach the whole layout: rows, fields and wrapped text") {
    const PxMetrics prop = proportional_metrics(); // pad 0: the rect's width IS the text bound

    // Two rows of the SAME pixel width, one wide-glyph and one narrow. The layout cuts each by
    // what it measures, so eight wide glyphs lose five and twenty narrow ones lose none.
    Widget rows = list("rows", "Rows", {"WWWWWWWW", std::string(20, 'i')}, -1,
                       /*activatable=*/true, /*focused=*/false);
    const PxScene scene = px_layout(rows, PxRect{0, 0, 80, 40}, prop);
    const PxCmd* wide_row = find_text(scene, "WWW");
    const PxCmd* narrow_row = find_text(scene, "iii");
    REQUIRE(wide_row != nullptr);
    REQUIRE(narrow_row != nullptr);
    CHECK(wide_row->text == "WWW\xE2\x80\xA6");
    CHECK(narrow_row->text == std::string(20, 'i'));
    CHECK(px_codepoint_count(wide_row->text) < px_codepoint_count(narrow_row->text));

    // A Field measures its whole run — prompt, separator and value — the same way.
    const Widget entry = field("mmm>", "WWWWWW", "", /*focused=*/false);
    const PxScene f = px_layout(entry, PxRect{0, 0, 80, 10}, prop);
    const PxCmd* run = find_text(f, "mmm>");
    REQUIRE(run != nullptr);
    CHECK(run->text == "mmm>\xE2\x80\xA6"); // 3 * 18 + 10 + 15 = 79; the space would be 85
    CHECK(prop.text_width(run->text) <= 80);

    // And wrap, through the layout rather than the primitive: the node emits the lines the
    // advances produce, so the draw COUNT follows the metric too.
    Widget wrapped = text_widget("WWWWW iiiii");
    wrapped.overflow = Overflow::Wrap;
    const PxScene w = px_layout(wrapped, PxRect{0, 0, 80, 40}, prop);
    CHECK(count_op(w, PxCmd::Op::Text) == 2);
    CHECK(find_text(w, "WWWW") != nullptr);
    CHECK(find_text(w, "W iiiii") != nullptr);
}

TEST_CASE("the pure layout is deterministic: same tree, same metrics, same scene") {
    const PxMetrics m = fixed_metrics();
    const Widget tree = region("r", "R", vstack("v", {text_widget("x"), open_slot("s", "Route")}));
    const PxScene one = px_layout(tree, PxRect{0, 0, 300, 90}, m);
    const PxScene two = px_layout(tree, PxRect{0, 0, 300, 90}, m);
    CHECK(one.cmds == two.cmds);
    REQUIRE(one.targets.size() == two.targets.size());
    for (std::size_t i = 0; i < one.targets.size(); ++i) {
        CHECK(one.targets[i].rect == two.targets[i].rect);
        CHECK(one.targets[i].item_index == two.targets[i].item_index);
    }
}

} // TEST_SUITE
