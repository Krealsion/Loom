// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_UI_SDL_SDL_UI_HPP
#define ZEN_UI_SDL_SDL_UI_HPP

// The SDL2 skin — the ONLY place raw SDL events, windows, renderers, textures, and real fonts
// exist (the pixel analogue of the TUI rule that cells and raw key bytes exist only in
// tui_render.cpp). It consumes the IDENTICAL Widget tree the TUI consumes: the layout brain is
// zen/ui/pixel.hpp (SDL-free, suite-proven); this file merely EXECUTES those draw commands with
// SDL and translates raw SDL events into the same semantic InputEvents tui_map_key produces.
// The controller/engine never see an SDL_Event — input is messages, output is the tree.
//
// This is deliberately a THIN projection (a window, an event pump, a command executor, TTF
// text) — drawing knowledge borrowed from the Zengine reference (surface -> texture -> copy),
// none of its engine. Headless: under SDL_VIDEODRIVER=dummy the whole pipeline runs without a
// display, which is how the suite proves it; what pixels LOOK like on a real screen is Josh's
// visual verify (the established division).

#include <zen/ui/pixel.hpp>
#include <zen/ui/tree.hpp>

#include <SDL.h>
#include <SDL_ttf.h>

#include <memory>
#include <string>
#include <vector>

namespace loom {

/// Map one raw SDL event onto semantic InputEvents (0..n appended to `out` — e.g. a text-input
/// event fans out to one byte-Edit per UTF-8 byte, a double-click to SelectAt then Activate).
/// Returns false to signal quit (window close / SDL_QUIT), true otherwise — the exact contract
/// shape of tui_map_key. Interaction intent maps to the pointer medium HERE, never in the tree:
/// a click on an activatable row selects it (SelectAt), a double-click activates, the wheel
/// walks the selection, keys mirror the TUI's bindings. Pure over its inputs (hit-tests the
/// scene) — provable with synthesized events and no window at all.
bool sdl_map_event(const SDL_Event& e, const PxScene& scene, std::vector<InputEvent>& out);

/// The window + renderer + font, owning the last laid-out scene (input hit-tests it). create()
/// initializes SDL video + TTF on first use and reports failure as a reason, not a crash.
class SdlUi {
public:
    static std::unique_ptr<SdlUi> create(const std::string& title, int width, int height,
                                         const std::string& font_path, int point_size,
                                         std::string* error);
    ~SdlUi();

    SdlUi(const SdlUi&) = delete;
    SdlUi& operator=(const SdlUi&) = delete;

    /// TTF-backed metrics: real glyph advances for the pure layout to consume.
    PxMetrics metrics() const;

    /// Lay the tree out (px_layout, the suite-proven brain) and execute the commands: clear,
    /// fills, clipped runs, text via TTF surface -> texture -> copy, present.
    void draw(const Widget& root);

    /// The scene draw() last produced — what input events hit-test against.
    const PxScene& scene() const noexcept { return scene_; }

    /// Drain all pending SDL events into semantic InputEvents. False once quit was seen.
    bool pump(std::vector<InputEvent>& out);

private:
    SdlUi() = default;

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* font_ = nullptr;
    PxScene scene_;
};

} // namespace loom

#endif // ZEN_UI_SDL_SDL_UI_HPP
