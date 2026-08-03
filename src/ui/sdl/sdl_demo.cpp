// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The SDL2 demo: Josh's visual-verify surface. Renders a PLACEHOLDER-DATA schematic (Phase A's
// stress defaults + the Unicode case) — no live weaves, no binding, no Builder (those are later
// phases). Input demonstrates input-as-messages end to end: raw SDL events become semantic
// InputEvents (sdl_map_event), and a small DEMO-ONLY shim below applies them to the local tree
// so clicks/keys visibly do something. The shim is not a controller — the real controllers
// (ConsoleUi today, the Builder later) consume the same InputEvents; an SDL console frontend
// (ConsoleUi over the engine with this renderer) is a named seam, deliberately not this demo.

#include "sdl_ui.hpp"

#include <zen/ui/component.hpp>
#include <zen/ui/tree.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

// The preview: the Phase A task-card schematic shapes, plus the two overflow proofs (the SAME
// Unicode stress text wrapped and truncated) and an activatable, routed list to click.
loom::Widget preview_tree() {
    using namespace loom;
    std::vector<Widget> body;
    body.push_back(bound_text("t-title", "title", Kind::Text));
    body.push_back(bound_field("count>", "count", Kind::Int));
    Widget rows = list("rows", "Rows (click to select, double-click to activate)",
                       {"alpha", "beta", "gamma", "delta"}, 0, /*activatable=*/true,
                       /*focused=*/true);
    rows.route_to = "detail-view";
    body.push_back(std::move(rows));
    Widget wrapped = text_widget(stress_text_unicode());
    wrapped.overflow = Overflow::Wrap; // REAL wrap — the hint the TUI ignores, honored here
    wrapped.weight = 3;
    body.push_back(std::move(wrapped));
    Widget truncated = text_widget(stress_text_unicode());
    truncated.overflow = Overflow::Truncate; // REAL truncate — ellipsized at a codepoint
    body.push_back(std::move(truncated));
    body.push_back(open_slot("actions", "Component"));
    body.push_back(open_slot("nav", "Route"));
    return region("preview", "Component Preview (stress placeholders)",
                  vstack("preview-body", std::move(body)));
}

// DEMO-ONLY shim: make the semantic events visibly do something to the local tree. The real
// consumers of these events are the controllers (ConsoleUi / the Builder) — this just proves
// the wiring in a window.
void apply_demo(loom::Widget& root, const loom::InputEvent& ev) {
    using namespace loom;
    Widget* rows = nullptr;
    Widget* fld = nullptr;
    for (Widget& w : root.children[0].children) { // region -> vstack -> children
        if (w.kind == WidgetKind::List && w.activatable) {
            rows = &w;
        }
        if (w.kind == WidgetKind::Field) {
            fld = &w;
        }
    }
    switch (ev.action) {
    case Action::SelectAt:
        if (rows != nullptr && ev.index >= 0 &&
            ev.index < static_cast<int>(rows->items.size())) {
            rows->selected_index = ev.index;
        }
        break;
    case Action::SelectUp:
        if (rows != nullptr && rows->selected_index > 0) {
            --rows->selected_index;
        }
        break;
    case Action::SelectDown:
        if (rows != nullptr &&
            rows->selected_index + 1 < static_cast<int>(rows->items.size())) {
            ++rows->selected_index;
        }
        break;
    case Action::Activate:
        if (rows != nullptr && rows->selected_index >= 0) {
            rows->title = "Rows (activated: " +
                          rows->items[static_cast<std::size_t>(rows->selected_index)] +
                          " -> " + rows->route_to + ")";
        }
        break;
    case Action::Edit:
        if (fld != nullptr && ev.ch != 0) {
            fld->value.push_back(ev.ch);
        }
        break;
    case Action::Backspace:
        if (fld != nullptr && !fld->value.empty()) {
            fld->value.pop_back();
        }
        break;
    case Action::Cancel:
        if (fld != nullptr) {
            fld->value.clear();
        }
        break;
    case Action::None:
    case Action::FocusNext:
    case Action::FocusPrev:
    case Action::Submit:
        break; // focus/submit belong to real controllers; the demo has one focused list
    } // no default (exhaustive by -Wswitch under -Werror)
}

} // namespace

int main() {
    const char* env_font = std::getenv("ZEN_FONT");
    const std::string font_path =
        env_font != nullptr ? env_font : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

    std::string error;
    std::unique_ptr<loom::SdlUi> ui =
        loom::SdlUi::create("zen ui preview", 1024, 720, font_path, 16, &error);
    if (ui == nullptr) {
        std::cerr << "zen-ui-sdl-demo: " << error << "\n"
                  << "(set ZEN_FONT to a .ttf path if the default font is missing)\n";
        return 1;
    }

    loom::Widget tree = preview_tree();
    ui->draw(tree);

    bool alive = true;
    while (alive) {
        std::vector<loom::InputEvent> events;
        alive = ui->pump(events);
        for (const loom::InputEvent& ev : events) {
            apply_demo(tree, ev);
        }
        ui->draw(tree);
        SDL_Delay(16); // ~60 fps is plenty for a preview window
    }
    return 0;
}
