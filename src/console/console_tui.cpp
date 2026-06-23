// The full-screen TUI (Stage 3): the SECOND renderer of the console's widget tree — this one to
// terminal cells. It is the ONLY place in the console where positions, sizes, and cells exist:
// it LAYS OUT the renderer-agnostic Widget tree (intent + relationship) into a character grid,
// resolving each VStack/HStack/Region arrangement to rows/columns and each List/Log/Field within
// its computed area honoring the overflow policy. The tree, the controller, and the guidance all
// come from zen-console unchanged; a GUI later replaces this file alone, laying the SAME tree out
// to pixels. Hand-rolled ANSI, no ncurses, no new dependency.
//
// This file is PLATFORM-HEADER-FREE: all terminal control (raw mode, window size, byte reads, the
// ESC-disambiguation timeout) lives behind the TerminalBackend seam (terminal.hpp), implemented per
// platform in terminal_posix.cpp / terminal_windows.cpp. make_terminal() is the only per-platform
// symbol. That single seam is the hook the WSL remote console (a socket backend) plugs into next.

#include "terminal.hpp"

#include <zen/console/ui.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace zen::console;

// ---- A demo responder so a standalone TUI has something to drive ----
std::shared_ptr<const zen::Schema> greet_schema() {
    static const auto s = zen::SchemaBuilder("Greet", 1).field("msg", zen::Kind::Text).build();
    return s;
}
class Greeter final : public zen::sb::Shard {
public:
    std::vector<std::shared_ptr<const zen::Schema>> accepted_schemas() const override {
        return {greet_schema()};
    }
    void handle(const zen::sb::Message& in, zen::sb::Bus& bus) override {
        zen::Value v(greet_schema());
        v.set("msg", zen::Cell::text(in.payload.get("msg")->as_text()));
        bus.send(in.reply_to, zen::sb::Message(std::move(v)));
    }
    zen::Value snapshot() const override {
        zen::Value v(state_schema());
        v.set("n", zen::Cell::integer(0));
        return v;
    }
    zen::Value policy() const override {
        zen::Value v(zen::sb::lifecycle_policy_schema());
        v.set("max_reloads", zen::Cell::integer(0));
        v.set("revive_from_last_good", zen::Cell::boolean(true));
        return v;
    }
    void revive(const zen::Value&) override {}

private:
    static std::shared_ptr<const zen::Schema> state_schema() {
        static const auto s = zen::SchemaBuilder("GreeterState", 1).field("n", zen::Kind::Int).build();
        return s;
    }
};

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
        int written = 0;
        for (char ch : text) {
            if (c >= cols || c >= col + limit) {
                break;
            }
            if (c >= 0) {
                line[static_cast<std::size_t>(c)] = ch;
            }
            ++c;
            ++written;
        }
        (void)written;
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
        // Equal shares; distribute the remainder to the leading parts.
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
            // Keep the selection visible; otherwise show the tail.
            if (node.selected_index >= 0) {
                first = node.selected_index - capacity + 1;
                if (first < 0) {
                    first = 0;
                }
            } else {
                first = count - capacity;
            }
        }
        // Truncate/Wrap/Grow all just clip to capacity here (a richer renderer would wrap).
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

void draw(const Widget& root, int rows, int cols) {
    Grid grid(rows, cols);
    // Reserve the top row for a banner; lay the tree out beneath it.
    grid.put(0, 0, "zen console (stage 3) — Tab: focus  arrows: select  Enter: send  Ctrl-X: quit",
             cols);
    layout(root, Rect{1, 0, rows - 1, cols}, grid);

    std::string frame = "\x1b[H\x1b[2J"; // home + clear
    for (int r = 0; r < rows; ++r) {
        frame += grid.cells[static_cast<std::size_t>(r)];
        if (r + 1 < rows) {
            frame += "\r\n";
        }
    }
    std::cout << frame << std::flush;
}

// Map one raw byte (already read) to a semantic InputEvent. Returns false to signal quit. This
// raw-byte -> Action table is the ONLY terminal-coupled input code; the controller and engine
// never see a raw key. Escape continuations are read through the backend's timed read, so this is
// platform-agnostic. `c` is a byte value (0..255); the caller guarantees c >= 0.
bool map_key(int c, zen::tui::TerminalBackend& term, InputEvent& out) {
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
        // Read the continuation with a brief grace so a BARE ESC (the Cancel gesture) does not
        // block, and an unrelated next keypress is not swallowed. The platform-specific timed read
        // lives behind the seam (POSIX VTIME / Windows WaitForSingleObject).
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

} // namespace

int main() {
    zen::sb::Switchboard bus;
    ConsoleEngine engine(bus);
    bus.register_shard(std::make_unique<Greeter>(), zen::sb::Grant{}.allow_any());

    ConsoleUi ui(engine);
    std::unique_ptr<zen::tui::TerminalBackend> term = zen::tui::make_terminal(); // enters raw mode

    int rows = 24, cols = 80;
    if (!term->size(rows, cols)) {
        rows = 24;
        cols = 80;
    }
    draw(ui.tree(), rows, cols);

    for (int c = term->read_byte(); c >= 0; c = term->read_byte()) {
        InputEvent ev;
        if (!map_key(c, *term, ev)) {
            break; // quit (Ctrl-X)
        }
        ui.dispatch(ev);
        (void)engine.take_dirty(); // consume the per-region change flags (full repaint below)
        if (!term->size(rows, cols)) {
            rows = 24;
            cols = 80;
        }
        draw(ui.tree(), rows, cols);
    }

    std::cout << "\x1b[H\x1b[2J" << std::flush; // leave a clean screen
    return 0;
} // term's dtor restores cooked mode here
