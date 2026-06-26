// The shared TUI renderer: it LAYS OUT the renderer-agnostic Widget tree (intent + relationship)
// into a character grid — the only place positions, sizes, and cells exist — and maps raw key bytes
// to semantic Actions. Both the in-process and remote consoles reuse this unchanged; a GUI later
// replaces this file alone, laying the SAME tree out to pixels. Hand-rolled ANSI, no ncurses, no new
// dependency. All terminal control lives behind the TerminalBackend seam.

#include "tui_render.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace loom {

namespace {

// ---- A character grid: the medium-specific surface. THIS is where cells live. ----
struct Grid {
    int rows = 0;
    int cols = 0;
    std::vector<std::string> cells; // rows lines, each cols chars

    Grid(int r, int c) : rows(r), cols(c), cells(static_cast<std::size_t>(r > 0 ? r : 0)) {
        for (std::string& line : cells) {
            line.assign(static_cast<std::size_t>(c > 0 ? c : 0), ' ');
        }
    }
    // Write text at (row, col), clipped to the grid and to `limit` columns.
    void put(int row, int col, const std::string& text, int limit) {
        if (row < 0 || row >= rows) {
            return;
        }
        std::string& line = cells[static_cast<std::size_t>(row)];
        int c = col;
        for (char ch : text) {
            if (c >= cols || c >= col + limit) {
                break;
            }
            if (c >= 0) {
                line[static_cast<std::size_t>(c)] = ch;
            }
            ++c;
        }
    }
};

struct Rect {
    int top, left, height, width;
};

// Split a length into n parts proportional to weights (0 => natural/equal share). Guards the
// all-zero (total==0) case so we never divide by zero.
std::vector<int> split(int total, const std::vector<std::uint16_t>& weights) {
    const std::size_t n = weights.size();
    std::vector<int> out(n, 0);
    if (n == 0 || total <= 0) {
        return out;
    }
    std::uint32_t wsum = 0;
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
        const int part = static_cast<int>(static_cast<std::uint32_t>(total) * weights[i] / wsum);
        out[i] = part;
        used += part;
    }
    out[n - 1] += total - used; // give rounding slack to the last part
    return out;
}

void layout(const Widget& node, Rect area, Grid& grid);

// Render a List/Log's items within `area`, honoring Scroll (show the tail when overflowing) and
// marking the selected row. The focus marker is on the title (drawn by the caller).
void layout_lines(const Widget& node, Rect area, Grid& grid) {
    const int capacity = area.height;
    if (capacity <= 0) {
        return;
    }
    const int count = static_cast<int>(node.items.size());
    int first = 0;
    if (count > capacity) {
        if (node.overflow == Overflow::Scroll) {
            if (node.selected_index >= 0) {
                first = node.selected_index - capacity + 1;
                if (first < 0) {
                    first = 0;
                }
            } else {
                first = count - capacity;
            }
        }
    }
    for (int i = 0; i < capacity && first + i < count; ++i) {
        const int idx = first + i;
        const bool sel = node.selected_index == idx;
        std::string line = (sel ? "> " : "  ") + node.items[static_cast<std::size_t>(idx)];
        grid.put(area.top + i, area.left, line, area.width);
    }
}

void layout(const Widget& node, Rect area, Grid& grid) {
    if (area.height <= 0 || area.width <= 0) {
        return;
    }
    switch (node.kind) {
    case WidgetKind::VStack: {
        std::vector<std::uint16_t> weights;
        weights.reserve(node.children.size());
        for (const Widget& c : node.children) {
            weights.push_back(c.weight);
        }
        const std::vector<int> heights = split(area.height, weights);
        int row = area.top;
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            layout(node.children[i], Rect{row, area.left, heights[i], area.width}, grid);
            row += heights[i];
        }
        break;
    }
    case WidgetKind::HStack: {
        std::vector<std::uint16_t> weights;
        weights.reserve(node.children.size());
        for (const Widget& c : node.children) {
            weights.push_back(c.weight);
        }
        const std::vector<int> widths = split(area.width, weights);
        int col = area.left;
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            layout(node.children[i], Rect{area.top, col, area.height, widths[i]}, grid);
            col += widths[i];
        }
        break;
    }
    case WidgetKind::Region: {
        std::string heading = "[ " + node.title + " ]";
        grid.put(area.top, area.left, heading, area.width);
        if (!node.children.empty()) {
            layout(node.children[0], Rect{area.top + 1, area.left, area.height - 1, area.width},
                   grid);
        }
        break;
    }
    case WidgetKind::List:
    case WidgetKind::Log: {
        std::string heading = node.title;
        if (node.focused) {
            heading += " *"; // the focus marker (a flag rendered as an annotation, not a position)
        }
        grid.put(area.top, area.left, heading, area.width);
        layout_lines(node, Rect{area.top + 1, area.left, area.height - 1, area.width}, grid);
        break;
    }
    case WidgetKind::Text:
        grid.put(area.top, area.left, node.content, area.width);
        break;
    case WidgetKind::Field: {
        std::string line = node.prompt + " " + node.value;
        if (node.focused) {
            line += "_"; // a crude cursor on the focused field
        }
        grid.put(area.top, area.left, line, area.width);
        if (area.height > 1 && !node.hint.empty()) {
            grid.put(area.top + 1, area.left, "  (" + node.hint + ")", area.width);
        }
        break;
    }
    } // no default (exhaustive by -Wswitch under -Werror)
}

} // namespace

void tui_draw(const Widget& root, int rows, int cols, TerminalBackend& term) {
    Grid grid(rows, cols);
    // Reserve the top row for a banner; lay the tree out beneath it.
    // ASCII-only (no non-ASCII bytes anywhere the renderer emits): a Unicode em-dash here would be
    // decoded by the Windows console code page as mojibake (the "GCo"/"ΓÇö" bytes).
    grid.put(0, 0, "zen console | Tab: focus  arrows: select  Enter: send  Ctrl-X: quit", cols);
    layout(root, Rect{1, 0, rows - 1, cols}, grid);

    std::string frame = "\x1b[H\x1b[2J"; // home + clear
    for (int r = 0; r < rows; ++r) {
        frame += grid.cells[static_cast<std::size_t>(r)];
        if (r + 1 < rows) {
            frame += "\r\n";
        }
    }
    term.write(frame); // output behind the seam: a local backend writes stdout, a socket the wire
    term.flush();
}

bool tui_map_key(int c, loom::TerminalBackend& term, InputEvent& out) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u == 24) { // Ctrl-X
        return false;
    }
    if (u == '\t') {
        out = {Action::FocusNext, 0};
        return true;
    }
    if (u == '\r' || u == '\n') {
        out = {Action::Submit, 0};
        return true;
    }
    if (u == 127 || u == 8) {
        out = {Action::Backspace, 0};
        return true;
    }
    if (u == 27) { // ESC: either an escape sequence (ESC [ A/B/...) or a bare Cancel
        const int b1 = term.read_byte_timeout(100);
        if (b1 != '[') {
            out = {Action::Cancel, 0}; // a bare ESC (or a timed-out / unknown sequence) = Cancel
            return true;
        }
        const int b2 = term.read_byte_timeout(100);
        if (b2 < 0) {
            out = {Action::Cancel, 0}; // a partial "ESC [" whose final byte timed out = Cancel
            return true;
        }
        switch (b2) {
        case 'A':
            out = {Action::SelectUp, 0};
            break;
        case 'B':
            out = {Action::SelectDown, 0};
            break;
        case 'Z': // Shift-Tab
            out = {Action::FocusPrev, 0};
            break;
        default:
            out = {Action::Activate, 0};
            break;
        }
        return true;
    }
    if (u >= 32 && u < 127) {
        out = {Action::Edit, static_cast<char>(c)};
        return true;
    }
    out = {Action::FocusNext, 0}; // benign no-op-ish for unknown control bytes
    return true;
}

} // namespace loom
