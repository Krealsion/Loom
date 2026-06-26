#ifndef ZEN_CONSOLE_TUI_RENDER_HPP
#define ZEN_CONSOLE_TUI_RENDER_HPP

// The TUI renderer + key mapping, shared by the in-process console (console_tui.cpp) and the remote
// console (console_remote.cpp). It lays the renderer-agnostic Widget tree out to terminal cells and
// maps raw key bytes to semantic Actions — the ONLY place positions, cells, and raw keys exist. Both
// frontends reuse it unchanged; they differ ONLY in transport (a local Switchboard vs a socket) and
// in their I/O loop (a synchronous read vs an event-driven multiplexer over {input, socket}). This is
// "remote is just another backend" made literal at the renderer level.

#include "terminal.hpp"

#include <zen/console/ui.hpp>

namespace loom {

/// Lay `root` out into a `rows`x`cols` character grid (a banner on the top row) and write the frame
/// through the terminal backend (output behind the seam). The SAME tree a GUI later lays out to pixels.
void tui_draw(const Widget& root, int rows, int cols, TerminalBackend& term);

/// Map one raw byte (already read; 0..255) to a semantic InputEvent. Returns false to signal quit
/// (Ctrl-X). Escape continuations are read through the backend's timed read, so this is
/// platform-agnostic — the ONLY terminal-coupled input code; the controller/engine never see a key.
bool tui_map_key(int c, TerminalBackend& term, InputEvent& out);

} // namespace loom

#endif // ZEN_CONSOLE_TUI_RENDER_HPP
