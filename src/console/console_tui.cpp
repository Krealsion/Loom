// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The full-screen TUI frontend: the in-process console driving a local Switchboard. It owns its I/O
// loop SYNCHRONOUSLY (block on read_byte -> dispatch -> redraw), which is correct here because the
// ONLY event source is the keyboard. The renderer + key mapping live in tui_render.cpp (shared with
// the remote console); the engine + controller live in zen-console. This file is just the local
// wiring + a demo responder. The remote console (console_remote.cpp) is THIS, with the engine behind
// a socket and the synchronous loop generalized to an event-driven multiplexer over {input, socket}.

#include "tui_render.hpp"

#include <zen/console/console.hpp>
#include <zen/console/ui.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <memory>
#include <vector>

namespace {

using namespace loom;

// ---- A demo responder so a standalone TUI has something to drive ----
std::shared_ptr<const loom::Schema> greet_schema() {
    static const auto s = loom::SchemaBuilder("Greet", 1).field("msg", loom::Kind::Text).build();
    return s;
}
class Greeter final : public loom::Weave {
public:
    std::vector<std::shared_ptr<const loom::Schema>> accepted_schemas() const override {
        return {greet_schema()};
    }
    void handle(const loom::Message& in, loom::Bus& bus) override {
        loom::Value v(greet_schema());
        v.set("msg", loom::Cell::text(in.payload.get("msg")->as_text()));
        bus.send(in.reply_to, loom::Message(std::move(v)));
    }
    loom::Value snapshot() const override {
        loom::Value v(state_schema());
        v.set("n", loom::Cell::integer(0));
        return v;
    }
    loom::Value policy() const override {
        loom::Value v(loom::lifecycle_policy_schema());
        v.set("max_reloads", loom::Cell::integer(0));
        v.set("revive_from_last_good", loom::Cell::boolean(true));
        return v;
    }
    void revive(const loom::Value&) override {}

private:
    static std::shared_ptr<const loom::Schema> state_schema() {
        static const auto s =
            loom::SchemaBuilder("GreeterState", 1).field("n", loom::Kind::Int).build();
        return s;
    }
};

} // namespace

int main() {
    loom::Switchboard bus;
    ConsoleEngine engine(bus);
    bus.register_weave(std::make_unique<Greeter>(), loom::Grant{}.allow_any());

    ConsoleUi ui(engine);
    std::unique_ptr<loom::TerminalBackend> term = loom::make_terminal(); // enters raw mode

    int rows = 24, cols = 80;
    if (!term->size(rows, cols)) {
        rows = 24;
        cols = 80;
    }
    tui_draw(ui.tree(), rows, cols, *term);

    for (int c = term->read_byte(); c >= 0; c = term->read_byte()) {
        InputEvent ev;
        if (!tui_map_key(c, *term, ev)) {
            break; // quit (Ctrl-X)
        }
        ui.dispatch(ev);
        (void)engine.take_dirty(); // consume the per-region change flags (full repaint below)
        if (!term->size(rows, cols)) {
            rows = 24;
            cols = 80;
        }
        tui_draw(ui.tree(), rows, cols, *term);
    }

    term->write("\x1b[H\x1b[2J"); // leave a clean screen (output behind the seam)
    term->flush();
    return 0;
} // term's dtor restores cooked mode here
