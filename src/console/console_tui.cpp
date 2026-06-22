// The full-screen TUI (Stage 3): the SECOND renderer of the console's widget tree — this one to
// terminal cells. It is the ONLY place in the console where positions, sizes, and cells exist:
// it LAYS OUT the renderer-agnostic Widget tree (intent + relationship) into a character grid,
// resolving each VStack/HStack/Region arrangement to rows/columns and each List/Log/Field within
// its computed area honoring the overflow policy. The tree, the controller, and the guidance all
// come from zen-console unchanged; a GUI later replaces this file alone, laying the SAME tree out
// to pixels. Hand-rolled ANSI + POSIX termios raw mode — no ncurses, no new dependency.

#include <zen/console/ui.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
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

// ---- Raw-mode guard: enter raw mode on construction, restore on destruction (normal scope exit
// or C++ exception unwinding) and via atexit (return-from-main / std::exit). NOTE: atexit does NOT
// run on abort()/_exit, so a sanitizer-fatal abort can still leave the tty raw — acceptable for a
// demo skin (run `reset` if so); a production renderer would add a SIGABRT/SIGSEGV handler. Gated
// behind isatty so a non-tty (piped/headless) run touches no terminal state. ----
class RawMode {
public:
    RawMode() {
        active_ = isatty(STDIN_FILENO) != 0;
        if (!active_) {
            return;
        }
        tcgetattr(STDIN_FILENO, &saved_);
        s_saved = saved_;
        s_have_saved = true;
        std::atexit(&RawMode::restore_atexit);
        struct termios raw = saved_;
        raw.c_lflag = raw.c_lflag & ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN] = static_cast<cc_t>(1);
        raw.c_cc[VTIME] = static_cast<cc_t>(0);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
    ~RawMode() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
        }
    }
    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;

private:
    static void restore_atexit() {
        if (s_have_saved) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_saved);
        }
    }
    bool active_ = false;
    struct termios saved_ {};
    static struct termios s_saved;
    static bool s_have_saved;
};
struct termios RawMode::s_saved {};
bool RawMode::s_have_saved = false;

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

void terminal_size(int& rows, int& cols) {
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        rows = static_cast<int>(ws.ws_row);
        cols = static_cast<int>(ws.ws_col);
    } else {
        rows = 24;
        cols = 80;
    }
}

// Map one raw key (already read) to a semantic InputEvent. Returns false to signal quit. This
// raw-key -> Action table is the ONLY terminal-coupled input code; the controller and engine
// never see a raw key.
bool map_key(char c, InputEvent& out) {
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
    if (u == 27) { // ESC: either an arrow sequence (ESC [ A/B/...) or a bare Cancel
        // Read the continuation with a brief timeout so a BARE ESC (the Cancel gesture) does not
        // block forever on the next byte (VMIN=1 makes reads blocking), and an unrelated next
        // keypress is not swallowed. Only meaningful on a tty (raw mode active).
        const bool tty = isatty(STDIN_FILENO) != 0;
        struct termios saved {};
        if (tty) {
            tcgetattr(STDIN_FILENO, &saved);
            struct termios timed = saved;
            timed.c_cc[VMIN] = static_cast<cc_t>(0);
            timed.c_cc[VTIME] = static_cast<cc_t>(1); // 0.1s grace for the escape sequence
            tcsetattr(STDIN_FILENO, TCSANOW, &timed);
        }
        char b1 = 0;
        char b2 = 0;
        const ssize_t r1 = read(STDIN_FILENO, &b1, 1);
        const ssize_t r2 = (r1 == 1 && b1 == '[') ? read(STDIN_FILENO, &b2, 1) : 0;
        if (tty) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved); // restore blocking VMIN=1
        }
        if (r1 != 1 || b1 != '[' || r2 != 1) {
            out = {Action::Cancel, 0}; // a bare ESC (or a timed-out / unknown sequence) = Cancel
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
        out = {Action::Edit, c};
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
    RawMode raw; // raw mode for the duration of main (restored on return / atexit)

    int rows = 24, cols = 80;
    terminal_size(rows, cols);
    draw(ui.tree(), rows, cols);

    char c = 0;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        InputEvent ev;
        if (!map_key(c, ev)) {
            break; // quit
        }
        ui.dispatch(ev);
        (void)engine.take_dirty(); // consume the per-region change flags (full repaint below)
        terminal_size(rows, cols);
        draw(ui.tree(), rows, cols);
    }

    std::cout << "\x1b[H\x1b[2J" << std::flush; // leave a clean screen
    return 0;
}
