// The pixel projection's layout: intent + relationship -> paint-ordered draw commands. Pure
// (injected metrics, no SDL, no display) so the projection LOGIC is provable in the ordinary
// suite on every platform; the SDL skin merely executes the commands. Mirrors the TUI's layout
// decisions where the semantics are shared (weight split, scroll-keeps-selection-visible) and
// gives the overflow hints the TUI ignores their REAL meaning (wrap, truncate).

#include <zen/ui/pixel.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loom {

// ---- UTF-8 iteration (boundary safety for wrap/truncate) ----

namespace {

// Length of the UTF-8 sequence starting at `first` (1 for an invalid/stray byte — total, never
// throwing; a malformed byte is carried through as one unit, never split further).
std::size_t cp_len(unsigned char first) noexcept {
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
    return 1; // stray continuation / invalid lead: one opaque unit
}

// The codepoint (as a byte span) starting at s[i], clamped to the string end.
std::string_view cp_at(std::string_view s, std::size_t i) noexcept {
    const std::size_t n = cp_len(static_cast<unsigned char>(s[i]));
    const std::size_t take = (i + n <= s.size()) ? n : s.size() - i;
    return s.substr(i, take);
}

constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // U+2026, spelled in bytes on purpose

} // namespace

std::size_t px_codepoint_count(std::string_view s) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size(); i += cp_at(s, i).size()) {
        ++count;
    }
    return count;
}

std::vector<std::string> px_wrap(std::string_view text, int max_width, const PxMetrics& m) {
    std::vector<std::string> lines;
    if (max_width <= 0) {
        lines.emplace_back(text); // nothing sane to do — one line, the clip bounds it
        return lines;
    }
    std::string line;
    int line_w = 0;
    std::size_t break_pos = std::string::npos; // byte pos IN `line` of the last space
    int break_w = 0;                           // line width up to (excluding) that space

    for (std::size_t i = 0; i < text.size();) {
        const std::string_view cp = cp_at(text, i);
        const int w = m.text_width(cp);
        if (!line.empty() && line_w + w > max_width) {
            if (cp == " ") {
                // The overflowing codepoint IS a space: the line is complete as it stands —
                // break HERE and consume the space (never back-break the line's last word).
                lines.push_back(std::move(line));
                line.clear();
                line_w = 0;
                break_pos = std::string::npos;
                break_w = 0;
                i += cp.size();
                continue;
            }
            if (break_pos != std::string::npos) {
                // Soft-break at the last space: it is consumed by the break.
                std::string rest = line.substr(break_pos + 1);
                line.resize(break_pos);
                lines.push_back(std::move(line));
                line = std::move(rest);
                line_w -= break_w + m.text_width(" ");
                break_pos = std::string::npos;
                break_w = 0;
                // Re-test the same codepoint against the shortened line (no advance).
                continue;
            }
            // Hard-break inside an unbroken word — at this codepoint boundary.
            lines.push_back(std::move(line));
            line.clear();
            line_w = 0;
        }
        if (cp == " " && !line.empty()) {
            break_pos = line.size();
            break_w = line_w;
        }
        line.append(cp);
        line_w += w;
        i += cp.size();
    }
    lines.push_back(std::move(line));
    return lines;
}

std::string px_truncate(std::string_view text, int max_width, const PxMetrics& m) {
    if (max_width <= 0) {
        return std::string(kEllipsis);
    }
    if (m.text_width(text) <= max_width) {
        return std::string(text);
    }
    const int ell_w = m.text_width(kEllipsis);
    std::string out;
    int out_w = 0;
    for (std::size_t i = 0; i < text.size();) {
        const std::string_view cp = cp_at(text, i);
        const int w = m.text_width(cp);
        if (out_w + w + ell_w > max_width) {
            break;
        }
        out.append(cp);
        out_w += w;
        i += cp.size();
    }
    out += kEllipsis;
    return out;
}

// ---- Layout ----

namespace {

// Split a length into parts proportional to weights (0 => natural/equal share). The same
// resolution the TUI uses, 64-bit accumulators for the same reason (a wire-legal wide+heavy
// stack must not wrap the sum).
std::vector<int> px_split(int total, const std::vector<std::uint16_t>& weights) {
    const std::size_t n = weights.size();
    std::vector<int> out(n, 0);
    if (n == 0 || total <= 0) {
        return out;
    }
    std::uint64_t wsum = 0;
    for (std::uint16_t w : weights) {
        wsum += w;
    }
    if (wsum == 0) {
        const int base = total / static_cast<int>(n);
        int rem = total - base * static_cast<int>(n);
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = base + (static_cast<int>(i) < rem ? 1 : 0);
        }
        return out;
    }
    int used = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int part =
            static_cast<int>(static_cast<std::uint64_t>(total) * weights[i] / wsum);
        out[i] = part;
        used += part;
    }
    out[n - 1] += total - used;
    return out;
}

struct Layouter {
    const PxMetrics& m;
    PxScene& scene;

    void text_cmd(int tx, int ty, std::string text, PxRole role) {
        PxCmd c;
        c.op = PxCmd::Op::Text;
        c.role = role;
        c.text = std::move(text);
        c.tx = tx;
        c.ty = ty;
        scene.cmds.push_back(std::move(c));
    }
    void fill_cmd(PxRect r, PxRole role) {
        PxCmd c;
        c.op = PxCmd::Op::Fill;
        c.rect = r;
        c.role = role;
        scene.cmds.push_back(std::move(c));
    }
    void push_clip(PxRect r) {
        PxCmd c;
        c.op = PxCmd::Op::PushClip;
        c.rect = r;
        scene.cmds.push_back(std::move(c));
    }
    void pop_clip() {
        PxCmd c;
        c.op = PxCmd::Op::PopClip;
        scene.cmds.push_back(std::move(c));
    }

    void stack_children(const Widget& node, PxRect area, bool vertical) {
        std::vector<std::uint16_t> weights;
        weights.reserve(node.children.size());
        for (const Widget& c : node.children) {
            weights.push_back(c.weight);
        }
        const std::vector<int> parts = px_split(vertical ? area.h : area.w, weights);
        int pos = vertical ? area.y : area.x;
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            if (vertical) {
                layout(node.children[i], PxRect{area.x, pos, area.w, parts[i]});
            } else {
                layout(node.children[i], PxRect{pos, area.y, parts[i], area.h});
            }
            pos += parts[i];
        }
    }

    // Rows of a List/Log: the scroll window mirrors the TUI (Scroll keeps the selection
    // visible, else tail-follows; every other policy shows the head); each row is single-line,
    // ellipsized to the row width (multi-line rows are a named refinement, not built).
    void rows(const Widget& node, PxRect area) {
        if (area.h <= 0 || area.w <= 0) {
            return;
        }
        const int capacity = area.h / m.line_height;
        if (capacity <= 0) {
            // Not even one row fits. Guard CAPACITY, not just the area (the TUI's discipline):
            // the scroll calc below would otherwise compute selected_index - 0 + 1, and a
            // wire-legal selected_index of INT_MAX makes that signed overflow — UB from data.
            return;
        }
        const int count = static_cast<int>(node.items.size());
        int first = 0;
        if (count > capacity && node.overflow == Overflow::Scroll) {
            if (node.selected_index >= 0) {
                first = node.selected_index - capacity + 1;
                if (first < 0) {
                    first = 0;
                }
            } else {
                first = count - capacity;
            }
        }
        push_clip(area);
        for (int k = 0; k < capacity && first + k < count; ++k) {
            const int idx = first + k;
            const PxRect row{area.x, area.y + k * m.line_height, area.w, m.line_height};
            if (node.selected_index == idx) {
                fill_cmd(row, PxRole::Selection);
            }
            text_cmd(row.x + m.pad, row.y,
                     px_truncate(node.items[static_cast<std::size_t>(idx)],
                                 row.w - 2 * m.pad, m),
                     PxRole::Body);
            if (node.activatable) {
                scene.targets.push_back(PxTarget{row, &node, idx});
            }
        }
        pop_clip();
    }

    void layout(const Widget& node, PxRect area) {
        if (area.h <= 0 || area.w <= 0) {
            return;
        }
        switch (node.kind) {
        case WidgetKind::VStack:
            stack_children(node, area, /*vertical=*/true);
            break;
        case WidgetKind::HStack:
            stack_children(node, area, /*vertical=*/false);
            break;
        case WidgetKind::Region: {
            text_cmd(area.x + m.pad, area.y, node.title, PxRole::Title);
            if (!node.children.empty()) {
                layout(node.children[0], PxRect{area.x, area.y + m.line_height, area.w,
                                                area.h - m.line_height});
            }
            break;
        }
        case WidgetKind::List:
        case WidgetKind::Log: {
            const PxRect title_line{area.x, area.y, area.w, m.line_height};
            if (node.focused) {
                fill_cmd(title_line, PxRole::Focus); // the focus MARKER, resolved to pixels
            }
            text_cmd(area.x + m.pad, area.y, node.title, PxRole::Title);
            rows(node, PxRect{area.x, area.y + m.line_height, area.w,
                              area.h - m.line_height});
            break;
        }
        case WidgetKind::Text: {
            const int inner = area.w - 2 * m.pad;
            switch (node.overflow) {
            case Overflow::Wrap: {
                // REAL wrap — the hint the TUI ignores, this projection honors.
                const std::vector<std::string> lines = px_wrap(node.content, inner, m);
                push_clip(area);
                for (std::size_t k = 0; k < lines.size(); ++k) {
                    text_cmd(area.x + m.pad,
                             area.y + static_cast<int>(k) * m.line_height, lines[k],
                             PxRole::Body);
                }
                pop_clip();
                break;
            }
            case Overflow::Truncate:
                // REAL truncate: one ellipsized line, cut at a codepoint boundary. Clipped as
                // a backstop: when the area is narrower than the ellipsis glyph itself, the
                // string logic cannot fit the bound — the clip guarantees nothing paints
                // outside the node regardless.
                push_clip(area);
                text_cmd(area.x + m.pad, area.y, px_truncate(node.content, inner, m),
                         PxRole::Body);
                pop_clip();
                break;
            case Overflow::Grow:
            case Overflow::Scroll:
                // Natural size: one line, bounded only by the viewport/window.
                text_cmd(area.x + m.pad, area.y, node.content, PxRole::Body);
                break;
            } // no default (exhaustive by -Wswitch under -Werror)
            if (node.activatable) {
                scene.targets.push_back(PxTarget{area, &node, -1});
            }
            break;
        }
        case WidgetKind::Field: {
            const PxRect line{area.x, area.y, area.w, m.line_height};
            push_clip(area); // the same nothing-paints-outside-the-node backstop as Truncate
            if (node.focused) {
                fill_cmd(line, PxRole::Focus);
            }
            std::string run = node.prompt + " " + node.value;
            if (node.focused) {
                run += "_"; // the crude cursor, same presentation the TUI chose
            }
            text_cmd(area.x + m.pad, area.y, px_truncate(run, area.w - 2 * m.pad, m),
                     PxRole::Body);
            if (area.h >= 2 * m.line_height && !node.hint.empty()) {
                text_cmd(area.x + m.pad, area.y + m.line_height,
                         px_truncate("(" + node.hint + ")", area.w - 2 * m.pad, m),
                         PxRole::Hint);
            }
            pop_clip();
            scene.targets.push_back(PxTarget{line, &node, -1}); // a Field is editable by nature
            break;
        }
        case WidgetKind::Slot: {
            const PxRect marker{area.x, area.y, area.w, m.line_height};
            push_clip(marker); // marker text bounded to its own line, whatever the width
            fill_cmd(marker, PxRole::SlotMarker);
            text_cmd(area.x + m.pad, area.y,
                     px_truncate("slot " + node.slot_name + ": accepts " + node.slot_accepts,
                                 area.w - 2 * m.pad, m),
                     PxRole::SlotMarker);
            pop_clip();
            // The design-time placeholder preview stacks beneath the marker (as in the TUI).
            const PxRect below{area.x, area.y + m.line_height, area.w,
                               area.h - m.line_height};
            std::vector<std::uint16_t> weights;
            weights.reserve(node.children.size());
            for (const Widget& c : node.children) {
                weights.push_back(c.weight);
            }
            const std::vector<int> parts = px_split(below.h, weights);
            int y = below.y;
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                layout(node.children[i], PxRect{below.x, y, below.w, parts[i]});
                y += parts[i];
            }
            break;
        }
        } // no default (exhaustive by -Wswitch under -Werror)
    }
};

} // namespace

PxScene px_layout(const Widget& root, PxRect viewport, const PxMetrics& m) {
    PxScene scene;
    Layouter l{m, scene};
    l.fill_cmd(viewport, PxRole::Background);
    l.layout(root, viewport);
    return scene;
}

const PxTarget* px_hit(const PxScene& scene, int x, int y) noexcept {
    for (std::size_t i = scene.targets.size(); i > 0; --i) {
        const PxTarget& t = scene.targets[i - 1];
        if (t.rect.contains(x, y)) {
            return &t;
        }
    }
    return nullptr;
}

} // namespace loom
