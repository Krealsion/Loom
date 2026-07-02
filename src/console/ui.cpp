// UI-as-data (Stage 3) implementation: the widget named-constructors, the engine-side guidance
// and tree emission (renderer-agnostic, built from the engine's public domain data), the
// headless outline renderer (the second renderer that proves the tree bakes in no medium), and
// the renderer-agnostic UI controller. NOTHING here knows about cells, coordinates, or termios —
// layout is the renderer's job alone.

#include <zen/console/ui.hpp>

#include <zen/console/input_lex.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace loom {

// ---- Named constructors (the only sanctioned Widget construction path) ----

Widget vstack(std::string region_id, std::vector<Widget> children) {
    Widget w;
    w.kind = WidgetKind::VStack;
    w.region_id = std::move(region_id);
    w.children = std::move(children);
    return w;
}

Widget hstack(std::string region_id, std::vector<Widget> children) {
    Widget w;
    w.kind = WidgetKind::HStack;
    w.region_id = std::move(region_id);
    w.children = std::move(children);
    return w;
}

Widget region(std::string region_id, std::string title, Widget child) {
    Widget w;
    w.kind = WidgetKind::Region;
    w.region_id = std::move(region_id);
    w.title = std::move(title);
    w.children.push_back(std::move(child));
    return w;
}

Widget list(std::string region_id, std::string title, std::vector<std::string> items,
            int selected_index, bool focusable, bool focused, Overflow overflow) {
    Widget w;
    w.kind = WidgetKind::List;
    w.region_id = std::move(region_id);
    w.title = std::move(title);
    w.items = std::move(items);
    w.selected_index = selected_index;
    w.focusable = focusable;
    w.focused = focused;
    w.overflow = overflow;
    return w;
}

Widget log_widget(std::string region_id, std::string title, std::vector<std::string> entries,
                  Overflow overflow) {
    Widget w;
    w.kind = WidgetKind::Log;
    w.region_id = std::move(region_id);
    w.title = std::move(title);
    w.items = std::move(entries);
    w.overflow = overflow;
    return w;
}

Widget text_widget(std::string content) {
    Widget w;
    w.kind = WidgetKind::Text;
    w.content = std::move(content);
    return w;
}

Widget field(std::string prompt, std::string value, std::string hint, bool focusable,
             bool focused) {
    Widget w;
    w.kind = WidgetKind::Field;
    w.prompt = std::move(prompt);
    w.value = std::move(value);
    w.hint = std::move(hint);
    w.focusable = focusable;
    w.focused = focused;
    return w;
}

namespace {

const char* kind_name(WidgetKind k) {
    switch (k) {
    case WidgetKind::VStack:
        return "VStack";
    case WidgetKind::HStack:
        return "HStack";
    case WidgetKind::Region:
        return "Region";
    case WidgetKind::List:
        return "List";
    case WidgetKind::Log:
        return "Log";
    case WidgetKind::Text:
        return "Text";
    case WidgetKind::Field:
        return "Field";
    } // no default: a new WidgetKind must be handled here (-Wswitch under -Werror)
    return "?";
}

// Walk the tree into an indented outline. DELIBERATELY ignores `weight` (it is a hint, not a
// size). The label per node carries the salient semantic content so a test can assert structure.
void outline_into(const Widget& node, int depth, std::string& out) {
    for (int i = 0; i < depth; ++i) {
        out += "  ";
    }
    out += kind_name(node.kind);
    if (node.focused) {
        out += "*"; // the focus marker — a flag, rendered as an annotation, never a position
    }
    switch (node.kind) {
    case WidgetKind::Region:
        out += " \"" + node.title + "\"";
        break;
    case WidgetKind::List:
    case WidgetKind::Log:
        out += " \"" + node.title + "\"";
        break;
    case WidgetKind::Text:
        out += " \"" + node.content + "\"";
        break;
    case WidgetKind::Field:
        out += " " + node.prompt + " value=\"" + node.value + "\" hint=\"" + node.hint + "\"";
        break;
    case WidgetKind::VStack:
    case WidgetKind::HStack:
        break;
    } // no default (exhaustive by -Wswitch)
    out += "\n";

    // List/Log items render as indented child lines (an index INTO items, ">" marks selection).
    if (node.kind == WidgetKind::List || node.kind == WidgetKind::Log) {
        for (std::size_t i = 0; i < node.items.size(); ++i) {
            for (int d = 0; d < depth + 1; ++d) {
                out += "  ";
            }
            const bool sel = node.selected_index >= 0 &&
                             static_cast<std::size_t>(node.selected_index) == i;
            out += sel ? "> " : "- ";
            out += node.items[i];
            out += "\n";
        }
    }
    for (const Widget& child : node.children) {
        outline_into(child, depth + 1, out);
    }
}

} // namespace

std::string render_outline(const Widget& root) {
    std::string out;
    outline_into(root, 0, out);
    return out;
}

// ---- Engine-produced guidance ----

std::string guidance_for(const Console& engine, std::string_view partial_command) {
    const std::vector<Token> toks = tokenize(std::string(partial_command));
    if (toks.empty()) {
        return "choose a weave: type its id, then <Shape> <version> [args]";
    }
    std::uint64_t id = 0;
    if (!parse_u64(toks[0].text, id)) {
        return "expected a weave id, got '" + toks[0].text + "'";
    }
    const std::vector<WeaveInfo> weaves = engine.weaves();
    const WeaveInfo* found = nullptr;
    for (const WeaveInfo& s : weaves) {
        if (s.id.value == id) {
            found = &s;
        }
    }
    if (found == nullptr) {
        return "no such weave: " + toks[0].text;
    }
    if (toks.size() == 1) {
        std::string out = "weave " + toks[0].text + " accepts:";
        for (const ShapeRef& a : found->accepts) {
            out += " " + a.name + " v" + std::to_string(a.version);
        }
        return out;
    }
    if (toks.size() == 2) {
        return "now the version: " + toks[1].text + " <version>";
    }
    std::uint64_t ver = 0;
    if (!parse_u64(toks[2].text, ver) || ver > 0xFFFFFFFFull) {
        return "expected a version number, got '" + toks[2].text + "'";
    }
    const std::optional<ShapeDesc> desc =
        engine.describe(toks[1].text, static_cast<std::uint32_t>(ver));
    if (!desc) {
        return "no such registered shape: " + toks[1].text + " v" + toks[2].text;
    }
    std::string out = desc->name + " v" + std::to_string(desc->version) + " fields:";
    for (const FieldDesc& f : desc->fields) {
        out += " " + f.name + ":" + f.type + (f.required ? "" : "?");
    }
    return out;
}

// ---- Tree emission ----

Widget emit_ui_tree(const Console& engine, const UiState& ui) {
    // Weaves list.
    std::vector<std::string> weave_items;
    for (const WeaveInfo& s : engine.weaves()) {
        std::string line = "weave " + std::to_string(s.id.value) + ":";
        for (const ShapeRef& a : s.accepts) {
            line += " " + a.name + " v" + std::to_string(a.version);
        }
        weave_items.push_back(std::move(line));
    }
    Widget weaves = list("weaves", "Weaves", std::move(weave_items),
                         ui.focus == Focus::Weaves ? ui.weave_cursor : -1,
                         /*focusable=*/true, /*focused=*/ui.focus == Focus::Weaves);

    // Tap log.
    std::vector<std::string> tap_items;
    for (const TapEvent& e : engine.tap()) {
        std::string line = e.kind + " " + e.schema + " " + std::to_string(e.sender.value) + "->" +
                           std::to_string(e.target.value);
        if (!e.refusal.empty()) {
            line += " [" + e.refusal + "]";
        }
        tap_items.push_back(std::move(line));
    }
    Widget taplog = log_widget("tap", "Tap", std::move(tap_items));

    Widget bus =
        region("bus", "Bus", hstack("bus-row", {std::move(weaves), std::move(taplog)}));

    // Buffer list (m1, m2, ...).
    std::vector<std::string> buf_items;
    for (std::size_t i = 1; i <= engine.buffer_size(); ++i) {
        std::optional<BufferEntry> b = engine.buffer_at(i);
        if (b) {
            buf_items.push_back(b->label + ": " + b->name + " v" + std::to_string(b->version));
        }
    }
    Widget buffer = list("buffer", "Buffer", std::move(buf_items),
                         ui.focus == Focus::Buffer ? ui.buffer_cursor : -1,
                         /*focusable=*/true, /*focused=*/ui.focus == Focus::Buffer);

    // Compose field: the partial command + the engine-produced next-choice guidance.
    Widget compose = field("compose>", ui.partial_input, guidance_for(engine, ui.partial_input),
                           /*focusable=*/true, /*focused=*/ui.focus == Focus::Compose);
    compose.region_id = "compose"; // identity (for focus/diff), not geometry

    std::vector<Widget> root_children;
    root_children.push_back(std::move(bus));
    root_children.push_back(std::move(buffer));
    root_children.push_back(std::move(compose));

    // Transient prompt region from the last ladder result (NeedsInput / Error).
    if (ui.pending) {
        const Composed& c = *ui.pending;
        if (c.status == Composed::Status::NeedsInput) {
            std::vector<std::string> open;
            for (const FieldDesc& f : c.open_fields) {
                open.push_back(f.name + " : " + f.type + (f.required ? " (required)" : " (optional)"));
            }
            for (const std::string& u : c.unplaced) {
                open.push_back("unplaced: " + u);
            }
            root_children.push_back(region(
                "prompt", "Needs input",
                list("prompt-fields", "open fields", std::move(open), -1, false, false)));
        } else if (c.status == Composed::Status::Error) {
            root_children.push_back(region("prompt", "Error", text_widget(c.error)));
        }
    }
    return vstack("root", std::move(root_children));
}

// ---- The controller ----

namespace {

Focus next_focus(Focus f) {
    switch (f) {
    case Focus::Weaves:
        return Focus::Buffer;
    case Focus::Buffer:
        return Focus::Compose;
    case Focus::Compose:
        return Focus::Weaves;
    }
    return Focus::Compose;
}

Focus prev_focus(Focus f) {
    switch (f) {
    case Focus::Weaves:
        return Focus::Compose;
    case Focus::Buffer:
        return Focus::Weaves;
    case Focus::Compose:
        return Focus::Buffer;
    }
    return Focus::Compose;
}

} // namespace

void ConsoleUi::submit_command() {
    const std::vector<Token> toks = tokenize(ui_.partial_input);
    const auto error = [&](std::string msg) {
        Composed c;
        c.status = Composed::Status::Error;
        c.error = std::move(msg);
        ui_.pending = std::move(c);
    };
    if (toks.size() < 3) {
        error("incomplete command: need <id> <Shape> <version> [args]");
        return;
    }
    std::uint64_t id = 0;
    std::uint64_t ver = 0;
    if (!parse_u64(toks[0].text, id) || !parse_u64(toks[2].text, ver) || ver > 0xFFFFFFFFull) {
        error("bad weave id or version");
        return;
    }
    std::vector<Arg> args;
    for (std::size_t i = 3; i < toks.size(); ++i) {
        args.push_back(lex_arg(toks[i]));
    }
    Composed c =
        engine_.compose(loom::WeaveId{id}, toks[1].text, static_cast<std::uint32_t>(ver), args);
    const bool ready = c.status == Composed::Status::Ready;
    ui_.pending = std::move(c);
    if (ready) {
        engine_.pump();         // deliver the send and buffer the reply (record_tap marks dirty)
        ui_.partial_input.clear(); // the command was consumed
    }
}

void ConsoleUi::dispatch(const InputEvent& ev) {
    switch (ev.action) {
    case Action::None:
        break; // a true no-op — an unknown control byte changes nothing
    case Action::FocusNext:
        ui_.focus = next_focus(ui_.focus);
        break;
    case Action::FocusPrev:
        ui_.focus = prev_focus(ui_.focus);
        break;
    case Action::SelectUp:
        if (ui_.focus == Focus::Weaves && ui_.weave_cursor > 0) {
            --ui_.weave_cursor;
        } else if (ui_.focus == Focus::Buffer && ui_.buffer_cursor > 0) {
            --ui_.buffer_cursor;
        }
        break;
    case Action::SelectDown:
        // Clamp against the live item count here — the controller is the ONLY writer of UiState
        // (a renderer takes the tree by const& and cannot clamp), so an out-of-range cursor would
        // otherwise make the selection silently vanish on the next emit.
        if (ui_.focus == Focus::Weaves) {
            const int n = static_cast<int>(engine_.weaves().size());
            if (ui_.weave_cursor + 1 < n) {
                ++ui_.weave_cursor;
            }
        } else if (ui_.focus == Focus::Buffer) {
            const int n = static_cast<int>(engine_.buffer_size());
            if (ui_.buffer_cursor + 1 < n) {
                ++ui_.buffer_cursor;
            }
        }
        break;
    case Action::Activate:
        if (ui_.focus == Focus::Weaves) {
            const std::vector<WeaveInfo> weaves = engine_.weaves();
            if (ui_.weave_cursor >= 0 &&
                static_cast<std::size_t>(ui_.weave_cursor) < weaves.size()) {
                ui_.partial_input =
                    std::to_string(weaves[static_cast<std::size_t>(ui_.weave_cursor)].id.value) + " ";
                ui_.focus = Focus::Compose; // move to the command line, pre-filled
            }
        } else if (ui_.focus == Focus::Buffer) {
            if (ui_.buffer_cursor >= 0 &&
                static_cast<std::size_t>(ui_.buffer_cursor) < engine_.buffer_size()) {
                std::optional<BufferEntry> b =
                    engine_.buffer_at(static_cast<std::size_t>(ui_.buffer_cursor) + 1);
                if (b) {
                    ui_.partial_input += "$" + b->label + "."; // begin a reference wire
                    ui_.focus = Focus::Compose;
                }
            }
        }
        break;
    case Action::Edit:
        if (ev.ch != 0) {
            ui_.partial_input.push_back(ev.ch);
        }
        break;
    case Action::Backspace:
        if (!ui_.partial_input.empty()) {
            ui_.partial_input.pop_back();
        }
        break;
    case Action::Submit:
        submit_command();
        break;
    case Action::Cancel:
        ui_.partial_input.clear();
        ui_.pending.reset();
        break;
    } // no default (exhaustive by -Wswitch)
}

Widget ConsoleUi::tree() const { return emit_ui_tree(engine_, ui_); }

} // namespace loom
