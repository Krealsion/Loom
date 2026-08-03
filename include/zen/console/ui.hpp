// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_CONSOLE_UI_HPP
#define ZEN_CONSOLE_UI_HPP

// UI-as-data (Stage 3), the CONSOLE side: the console emits its OWN interface as the Loom's
// semantic widget tree (zen/ui/tree.hpp — the vocabulary lives there, in its own target; the
// console is one consumer of it, not its owner). This header holds what is genuinely the
// console's: its presentation state (UiState — focus, the in-progress command line, cursors),
// the engine-produced guidance, the tree emission from the engine's public domain data, and the
// renderer-agnostic ConsoleUi controller that maps semantic Actions onto engine calls.
//
// The tree is built in the engine LIBRARY from the engine's public domain data (weaves, the
// reply buffer, the tap, the registry-derived guidance) — renderer-agnostic and fully testable
// with no terminal. The TUI (console_tui.cpp) and the SDL renderer are skins over the SAME
// tree; the test-only outline walk (render_outline, in zen/ui) proves the tree carries no
// medium.

#include <zen/console/console.hpp>
#include <zen/ui/tree.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace loom {

/// Which region currently holds focus. Three interactive regions cycle under FocusNext/
/// FocusPrev; the tap log is read-only (not focus-eligible).
enum class Focus : std::uint8_t { Weaves, Buffer, Compose };

/// The console's presentation/interaction state — what the ENGINE must not own: focus, the
/// in-progress command line, list selections, and the last compose result to surface. Passed to
/// emit_ui_tree so the engine's domain state stays pure.
struct UiState {
    Focus focus = Focus::Compose;       ///< start on the command field
    std::string partial_input;          ///< the in-progress compose command line
    int weave_cursor = 0;               ///< selection index in the Weaves list
    int buffer_cursor = 0;              ///< selection index in the Buffer list
    std::optional<Composed> pending;    ///< last ladder result to surface (NeedsInput / Error)
};

// ---- Engine-produced guidance + tree emission (renderer-agnostic; no terminal) ----

/// The next-choice guidance for a partial command line, derived from the registry + the partial
/// input — engine-produced, so it is renderer-agnostic (the TUI shows it inline; a GUI shows the
/// same string as a dropdown). Advances with input: empty -> "choose a weave"; a weave id -> its
/// shapes; a shape+version -> its fields. Reads only the engine's public domain data.
std::string guidance_for(const Console& engine, std::string_view partial_command);

/// Build the console's current UI tree from the engine's public domain data + the presentation
/// state: a root VStack of a Bus region (an HStack of a Weaves List and a Tap Log), a Buffer
/// List (m1, m2, ...), and a Compose Field (the partial command + the engine-produced guidance
/// hint). If `ui.pending` carries a NeedsInput/Error result, the tree gains a prompt region. The
/// tree is a fresh value each call (retained-mode is the renderer diffing successive trees).
Widget emit_ui_tree(const Console& engine, const UiState& ui);

// ---- The renderer-agnostic UI controller ----

/// Owns the presentation state (UiState), maps semantic Actions onto engine calls, and emits the
/// current widget tree. The TUI is THIS plus termios raw I/O plus a tree->cells layout; a
/// headless test drives THIS with scripted InputEvents and asserts on the emitted tree — so the
/// dataflow/interaction brain is proven with no terminal and every renderer inherits it whole.
class ConsoleUi {
public:
    explicit ConsoleUi(Console& engine) : engine_(engine) {}

    /// Apply one semantic action (the only mutation path). Submit composes via the ladder and
    /// pumps; the result is surfaced via the tree (Ready -> reply buffered; NeedsInput/Error ->
    /// a prompt region next emit).
    void dispatch(const InputEvent& ev);

    /// Emit the current widget tree (engine-built standing regions + any transient prompt).
    Widget tree() const;

    const UiState& state() const noexcept { return ui_; }
    Console& engine() noexcept { return engine_; }

private:
    void submit_command(); ///< parse the command line, run the ladder, surface the result

    Console& engine_;
    UiState ui_;
};

} // namespace loom

#endif // ZEN_CONSOLE_UI_HPP
