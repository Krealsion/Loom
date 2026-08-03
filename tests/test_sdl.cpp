// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <doctest.h>

#include "../src/ui/sdl/sdl_ui.hpp"

#include <zen/ui/component.hpp>
#include <zen/ui/pixel.hpp>
#include <zen/ui/tree.hpp>

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// The SDL skin's smoke, run headless under SDL_VIDEODRIVER=dummy (set both here and by the
// ctest ENVIRONMENT property): the FULL pipeline — real SDL video (dummy), real TTF metrics,
// px_layout, command execution against a real (software) renderer — without a display. The
// projection LOGIC is pinned SDL-free in the ordinary suite (test_pixel.cpp); this proves the
// skin executes it and that raw-SDL-event -> semantic-InputEvent mapping behaves. What pixels
// LOOK like on a real screen stays Josh's visual verify (the established division).

using namespace loom;

namespace {

std::string font_path() {
    const char* env = std::getenv("ZEN_FONT");
    return env != nullptr ? env : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
}

std::unique_ptr<SdlUi> make_ui() {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1); // before SDL_Init; belt to ctest's braces
    std::string error;
    std::unique_ptr<SdlUi> ui = SdlUi::create("zen-sdl-tests", 640, 480, font_path(), 16, &error);
    REQUIRE_MESSAGE(ui != nullptr, error);
    return ui;
}

} // namespace

TEST_SUITE("sdl") {

TEST_CASE("the full pipeline runs headless: dummy video, real TTF metrics, commands executed") {
    std::unique_ptr<SdlUi> ui = make_ui();

    // Real font metrics behave like metrics: wider text is wider, lines have height.
    const PxMetrics m = ui->metrics();
    CHECK(m.line_height > 0);
    CHECK(m.text_width("ab") > m.text_width("a"));
    CHECK(m.text_width("a") > 0);

    // The SAME tree shape the TUI draws, with the Unicode stress case the TUI cannot: laid
    // out, executed against the dummy renderer, presented — no crash, no SDL error state.
    Widget wrapped = text_widget(stress_text_unicode());
    wrapped.overflow = Overflow::Wrap;
    Widget rows = list("rows", "Rows", {"alpha", "beta"}, 0, /*activatable=*/true,
                       /*focused=*/true);
    const Widget tree =
        region("frame", "Preview", vstack("body", {rows, wrapped, open_slot("nav", "Route")}));
    SDL_ClearError();
    ui->draw(tree);
    CHECK(std::string_view(SDL_GetError()).empty());

    // The executed scene carried the Unicode run intact (the emoji's 4 bytes un-split) and
    // its clips balanced (the executor's stack discipline holds).
    const PxScene& scene = ui->scene();
    bool emoji = false;
    int pushes = 0;
    int pops = 0;
    for (const PxCmd& c : scene.cmds) {
        if (c.op == PxCmd::Op::Text && c.text.find("\xF0\x9F\xA7\xB6") != std::string::npos) {
            emoji = true;
        }
        pushes += c.op == PxCmd::Op::PushClip ? 1 : 0;
        pops += c.op == PxCmd::Op::PopClip ? 1 : 0;
    }
    CHECK(emoji);
    CHECK(pushes == pops);
    CHECK(pushes >= 1);
}

TEST_CASE("raw SDL events become semantic InputEvents; the controller never sees SDL") {
    // The mapping is pure over (event, scene) — synthesized events, no window needed.
    const PxMetrics m{10, 2, [](std::string_view s) {
                          return static_cast<int>(px_codepoint_count(s)) * 8;
                      }};
    Widget rows = list("rows", "Rows", {"alpha", "beta", "gamma"}, -1, /*activatable=*/true,
                       /*focused=*/false);
    const PxScene scene = px_layout(rows, PxRect{0, 0, 200, 60}, m);

    std::vector<InputEvent> out;

    // activatable -> click: a single left click on the second row SELECTS it by index.
    SDL_Event click{};
    click.type = SDL_MOUSEBUTTONDOWN;
    click.button.button = SDL_BUTTON_LEFT;
    click.button.clicks = 1;
    click.button.x = 5;
    click.button.y = 25;
    CHECK(sdl_map_event(click, scene, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].action == Action::SelectAt);
    CHECK(out[0].index == 1);

    // A double-click SELECTS then ACTIVATES — the pointer's Enter.
    out.clear();
    click.button.clicks = 2;
    CHECK(sdl_map_event(click, scene, out));
    REQUIRE(out.size() == 2);
    CHECK(out[0].action == Action::SelectAt);
    CHECK(out[1].action == Action::Activate);

    // A click on nothing maps to nothing (no phantom events).
    out.clear();
    click.button.x = 190;
    click.button.y = 3; // the title line: not a target
    CHECK(sdl_map_event(click, scene, out));
    CHECK(out.empty());

    // Keys mirror the TUI's bindings — the same semantic set, second medium.
    out.clear();
    SDL_Event key{};
    key.type = SDL_KEYDOWN;
    key.key.keysym.sym = SDLK_TAB;
    CHECK(sdl_map_event(key, scene, out));
    key.key.keysym.mod = KMOD_LSHIFT;
    CHECK(sdl_map_event(key, scene, out));
    key.key.keysym.mod = 0;
    key.key.keysym.sym = SDLK_RETURN;
    CHECK(sdl_map_event(key, scene, out));
    key.key.keysym.sym = SDLK_ESCAPE;
    CHECK(sdl_map_event(key, scene, out));
    REQUIRE(out.size() == 4);
    CHECK(out[0].action == Action::FocusNext);
    CHECK(out[1].action == Action::FocusPrev);
    CHECK(out[2].action == Action::Submit);
    CHECK(out[3].action == Action::Cancel);

    // editable -> typing: one TEXTINPUT with a two-byte UTF-8 codepoint fans out to one
    // byte-Edit per byte, in order — a std::string target reassembles it exactly.
    out.clear();
    SDL_Event text{};
    text.type = SDL_TEXTINPUT;
    text.text.text[0] = '\xC3';
    text.text.text[1] = '\xA9'; // U+00E9, "é"
    text.text.text[2] = '\0';
    CHECK(sdl_map_event(text, scene, out));
    REQUIRE(out.size() == 2);
    CHECK(out[0].action == Action::Edit);
    CHECK(out[1].action == Action::Edit);
    std::string reassembled;
    reassembled.push_back(out[0].ch);
    reassembled.push_back(out[1].ch);
    CHECK(reassembled == "\xC3\xA9");

    // The wheel walks the selection; quit is the one false return (the tui_map_key contract).
    out.clear();
    SDL_Event wheel{};
    wheel.type = SDL_MOUSEWHEEL;
    wheel.wheel.y = 1;
    CHECK(sdl_map_event(wheel, scene, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].action == Action::SelectUp);

    // Natural scrolling: SDL flips the sign and says so — the mapping honors the flag, so
    // user INTENT (scroll up) maps the same on both conventions.
    out.clear();
    wheel.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    CHECK(sdl_map_event(wheel, scene, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].action == Action::SelectDown);
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    CHECK_FALSE(sdl_map_event(quit, scene, out));
}

TEST_CASE("interaction stays intent-side: the tree that maps clicks is the tree the TUI walks with keys") {
    // The agnosticism pin at the input seam: ONE tree, and each medium maps the SAME declared
    // intent its own way — nothing on the node says click or key.
    Widget rows = list("rows", "Rows", {"a", "b"}, 0, /*activatable=*/true, /*focused=*/true);
    const std::string outline = render_outline(rows);
    CHECK(outline.find("(activatable)") != std::string::npos); // intent, medium-free
    CHECK(outline.find("click") == std::string::npos);
    CHECK(outline.find("key") == std::string::npos);
}

} // TEST_SUITE
