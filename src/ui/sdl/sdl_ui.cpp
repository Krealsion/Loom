// The SDL2 skin's implementation: execute the pure layout's draw commands, translate raw SDL
// events to semantic InputEvents. Thin on purpose — every layout decision already happened in
// px_layout (suite-proven); everything here is mechanical execution and mapping.

#include "sdl_ui.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace loom {

namespace {

// The role palettes (dark theme). The command list is theme-free; colors are the executor's.
SDL_Color fill_color(PxRole role) {
    switch (role) {
    case PxRole::Background:
        return SDL_Color{24, 24, 34, 255};
    case PxRole::Selection:
        return SDL_Color{53, 64, 101, 255};
    case PxRole::Focus:
        return SDL_Color{45, 55, 95, 255};
    case PxRole::SlotMarker:
        return SDL_Color{64, 56, 28, 255};
    case PxRole::Title:
    case PxRole::Body:
    case PxRole::Hint:
        break; // text-only roles never Fill; a stray one lands on the background color
    }
    return SDL_Color{24, 24, 34, 255};
}

SDL_Color text_color(PxRole role) {
    switch (role) {
    case PxRole::Title:
        return SDL_Color{216, 216, 240, 255};
    case PxRole::Body:
        return SDL_Color{200, 200, 200, 255};
    case PxRole::Hint:
        return SDL_Color{136, 136, 160, 255};
    case PxRole::SlotMarker:
        return SDL_Color{224, 200, 120, 255};
    case PxRole::Selection:
    case PxRole::Focus:
    case PxRole::Background:
        break; // fill-only roles never carry Text; default to body grey if one ever does
    }
    return SDL_Color{200, 200, 200, 255};
}

SDL_Rect to_sdl(PxRect r) { return SDL_Rect{r.x, r.y, r.w, r.h}; }

} // namespace

std::unique_ptr<SdlUi> SdlUi::create(const std::string& title, int width, int height,
                                     const std::string& font_path, int point_size,
                                     std::string* error) {
    const auto fail = [&](const std::string& what) -> std::unique_ptr<SdlUi> {
        if (error != nullptr) {
            *error = what + ": " + SDL_GetError();
        }
        return nullptr;
    };
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_Init(SDL_INIT_VIDEO) != 0) {
        return fail("SDL_Init(VIDEO)");
    }
    if (TTF_WasInit() == 0 && TTF_Init() != 0) {
        return fail("TTF_Init");
    }
    std::unique_ptr<SdlUi> ui(new SdlUi());
    ui->window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN);
    if (ui->window_ == nullptr) {
        return fail("SDL_CreateWindow");
    }
    // Software renderer: honest everywhere (a WSL box or the dummy driver has no GPU story),
    // and pixels-per-frame at UI scale is trivial.
    ui->renderer_ = SDL_CreateRenderer(ui->window_, -1, SDL_RENDERER_SOFTWARE);
    if (ui->renderer_ == nullptr) {
        return fail("SDL_CreateRenderer");
    }
    ui->font_ = TTF_OpenFont(font_path.c_str(), point_size);
    if (ui->font_ == nullptr) {
        return fail("TTF_OpenFont('" + font_path + "')");
    }
    return ui;
}

SdlUi::~SdlUi() {
    if (font_ != nullptr) {
        TTF_CloseFont(font_);
    }
    if (renderer_ != nullptr) {
        SDL_DestroyRenderer(renderer_);
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
    }
}

PxMetrics SdlUi::metrics() const {
    PxMetrics m;
    m.line_height = TTF_FontLineSkip(font_);
    m.pad = 6;
    TTF_Font* font = font_;
    m.text_width = [font](std::string_view s) -> int {
        if (s.empty()) {
            return 0;
        }
        int w = 0;
        int h = 0;
        // TTF wants a NUL-terminated UTF-8 string; measurement failure counts as zero width
        // (the draw still draws whatever the rasterizer produces — never a crash).
        if (TTF_SizeUTF8(font, std::string(s).c_str(), &w, &h) != 0) {
            return 0;
        }
        return w;
    };
    return m;
}

void SdlUi::draw(const Widget& root) {
    int w = 0;
    int h = 0;
    SDL_GetRendererOutputSize(renderer_, &w, &h);
    scene_ = px_layout(root, PxRect{0, 0, w, h}, metrics());

    std::vector<SDL_Rect> clip_stack;
    for (const PxCmd& cmd : scene_.cmds) {
        switch (cmd.op) {
        case PxCmd::Op::Fill: {
            const SDL_Color c = fill_color(cmd.role);
            SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
            const SDL_Rect r = to_sdl(cmd.rect);
            SDL_RenderFillRect(renderer_, &r);
            break;
        }
        case PxCmd::Op::Text: {
            if (cmd.text.empty()) {
                break;
            }
            // The Zengine-borrowed path, with the leaks fixed: render -> texture -> copy ->
            // destroy both. Blended = antialiased alpha; the surface size is the glyph box.
            SDL_Surface* surface =
                TTF_RenderUTF8_Blended(font_, cmd.text.c_str(), text_color(cmd.role));
            if (surface == nullptr) {
                break; // an unrenderable run must never take the frame down
            }
            SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
            if (texture != nullptr) {
                const SDL_Rect dst{cmd.tx, cmd.ty, surface->w, surface->h};
                SDL_RenderCopy(renderer_, texture, nullptr, &dst);
                SDL_DestroyTexture(texture);
            }
            SDL_FreeSurface(surface);
            break;
        }
        case PxCmd::Op::PushClip: {
            clip_stack.push_back(to_sdl(cmd.rect));
            SDL_RenderSetClipRect(renderer_, &clip_stack.back());
            break;
        }
        case PxCmd::Op::PopClip: {
            if (!clip_stack.empty()) {
                clip_stack.pop_back();
            }
            SDL_RenderSetClipRect(renderer_,
                                  clip_stack.empty() ? nullptr : &clip_stack.back());
            break;
        }
        } // no default (exhaustive by -Wswitch under -Werror)
    }
    SDL_RenderPresent(renderer_);
}

bool SdlUi::pump(std::vector<InputEvent>& out) {
    bool alive = true;
    SDL_Event e;
    while (SDL_PollEvent(&e) != 0) {
        if (!sdl_map_event(e, scene_, out)) {
            alive = false;
        }
    }
    return alive;
}

bool sdl_map_event(const SDL_Event& e, const PxScene& scene, std::vector<InputEvent>& out) {
    switch (e.type) {
    case SDL_QUIT:
        return false;
    case SDL_MOUSEBUTTONDOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) {
            break;
        }
        // activatable -> click: the abstract intent, mapped to THIS medium here (the TUI maps
        // the same intent to arrow keys + Enter). A click on a row SELECTS it by index — the
        // pointer names a row where keys can only walk; a double-click ACTIVATES the selection.
        const PxTarget* hit = px_hit(scene, e.button.x, e.button.y);
        if (hit == nullptr) {
            break;
        }
        if (hit->item_index >= 0) {
            InputEvent ev;
            ev.action = Action::SelectAt;
            ev.index = hit->item_index;
            out.push_back(ev);
        }
        if (e.button.clicks >= 2 && hit->node != nullptr && hit->node->activatable) {
            out.push_back(InputEvent{Action::Activate, 0, -1});
        }
        break;
    }
    case SDL_MOUSEWHEEL: {
        // Honor natural-scrolling: SDL flips the sign and TELLS us (direction == FLIPPED);
        // ignoring it inverts SelectUp/Down on trackpad/libinput setups.
        const Sint32 y =
            e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
        if (y > 0) {
            out.push_back(InputEvent{Action::SelectUp, 0, -1});
        } else if (y < 0) {
            out.push_back(InputEvent{Action::SelectDown, 0, -1});
        }
        break;
    }
    case SDL_KEYDOWN:
        switch (e.key.keysym.sym) {
        case SDLK_TAB:
            out.push_back(InputEvent{(e.key.keysym.mod & KMOD_SHIFT) != 0 ? Action::FocusPrev
                                                                          : Action::FocusNext,
                                     0, -1});
            break;
        case SDLK_UP:
            out.push_back(InputEvent{Action::SelectUp, 0, -1});
            break;
        case SDLK_DOWN:
            out.push_back(InputEvent{Action::SelectDown, 0, -1});
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            out.push_back(InputEvent{Action::Submit, 0, -1});
            break;
        case SDLK_BACKSPACE:
            out.push_back(InputEvent{Action::Backspace, 0, -1});
            break;
        case SDLK_ESCAPE:
            out.push_back(InputEvent{Action::Cancel, 0, -1});
            break;
        default:
            break; // printable input arrives as SDL_TEXTINPUT, not keydowns
        }
        break;
    case SDL_TEXTINPUT: {
        // editable -> typing: one byte-Edit per UTF-8 byte, in order — a std::string target
        // reassembles the sequence exactly (the same byte-oriented Edit the TUI produces).
        for (const char* p = e.text.text; *p != '\0'; ++p) {
            out.push_back(InputEvent{Action::Edit, *p, -1});
        }
        break;
    }
    default:
        break; // everything else is a true no-op (window juggling, focus churn, ...)
    }
    return true;
}

} // namespace loom
