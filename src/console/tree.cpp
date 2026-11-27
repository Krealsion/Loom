// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The console's tree mechanics: the widget named-constructors, the stable spellings, and the
// headless outline renderer (the renderer-agnosticism proof). NOTHING here knows about cells,
// coordinates or termios: layout is the renderer's job alone.

#include <zen/console/tree.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
            int selected_index, bool activatable, bool focused, Overflow overflow) {
    Widget w;
    w.kind = WidgetKind::List;
    w.region_id = std::move(region_id);
    w.title = std::move(title);
    w.items = std::move(items);
    w.selected_index = selected_index;
    w.activatable = activatable;
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

Widget field(std::string prompt, std::string value, std::string hint, bool focused) {
    Widget w;
    w.kind = WidgetKind::Field;
    w.prompt = std::move(prompt);
    w.value = std::move(value);
    w.hint = std::move(hint);
    w.editable = true; // a Field IS an input affordance — the intent is declared, always
    w.focused = focused;
    return w;
}

// ---- Stable spellings, as the outline prints them ----

const char* name_of(WidgetKind k) noexcept {
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

// ---- The headless outline renderer ----

namespace {

// Walk the tree into an indented outline. DELIBERATELY ignores `weight` and `overflow` (hints a
// renderer resolves, not tree content) — but DOES print interaction intent: that is the tree's
// MEANING, so the outline proves it is content, not medium.
// The label per node carries the salient semantic content so a test can assert structure.
void outline_into(const Widget& node, int depth, std::string& out) {
    for (int i = 0; i < depth; ++i) {
        out += "  ";
    }
    out += name_of(node.kind);
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

    // Interaction intent reads as part of the outline: "a list, of rows, activatable".
    if (node.activatable) {
        out += " (activatable)";
    }
    if (node.editable) {
        out += " (editable)";
    }
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

} // namespace loom
