// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The Console's first, throwaway skin: a deliberately plain terminal REPL over the
// ConsoleEngine. It calls the engine API and formats the returned DOMAIN DATA as plain
// text — no cursor addressing, no panes (that is Stage 3). Its only job is to exercise
// the engine; the engine, not this, is the durable artifact. A GUI later replaces only
// this file, inheriting the engine whole.
//
// Stage 2 adds the dataflow surface to the skin: it lexes each argument to its narrowest
// type, recognizes `$mN.field` references and `field=value` named args, and renders the
// engine's NeedsInput as a plain prompt. The DECISIONS (resolution, the assumption ladder)
// are the engine's; this file only turns text into structured Args and structured results
// back into text.
//
// A demo "greeter" Weave is mounted at startup so a standalone terminal has something to
// drive (discover, send, see a reply buffered). In a real deployment, Weaves arrive on the
// bus by other means; the console drives whatever is there.

#include "zen/weave/poke_weave.hpp"

#include <zen/console/console.hpp>
#include <zen/terminal/input_lex.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

// The text lexers (tokenize / lex_arg / parse_u64) are shared with the TUI and the tests; they
// live in <zen/terminal/input_lex.hpp>. Bring the ones this REPL uses into scope.
using loom::parse_u64;
using loom::Token;
using loom::tokenize;
using loom::lex_arg;

std::shared_ptr<const loom::Schema> greet_schema() {
    static const auto s = loom::SchemaBuilder("Greet", 1).field("msg", loom::Kind::Text).build();
    return s;
}

// A trivial demo responder: accepts Greet{msg} and echoes it back to the sender's reply
// address — so the terminal has a live Weave to discover and drive.
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
        static const auto s = loom::SchemaBuilder("GreeterState", 1).field("n", loom::Kind::Int).build();
        return s;
    }
};

void print_weaves(const loom::ConsoleEngine& engine) {
    const auto weaves = engine.weaves();
    if (weaves.empty()) {
        std::cout << "  (no weaves on the bus)\n";
        return;
    }
    for (const auto& s : weaves) {
        std::cout << "  weave " << s.id.value << "  accepts:";
        if (s.accepts.empty()) {
            std::cout << " (none)";
        }
        for (const auto& a : s.accepts) {
            std::cout << ' ' << a.name << " v" << a.version;
        }
        std::cout << '\n';
    }
}

void print_fields(const loom::ShapeDesc& desc) {
    std::cout << "  " << desc.name << " v" << desc.version << " fields:\n";
    for (const auto& f : desc.fields) {
        std::cout << "    " << f.name << " : " << f.type << (f.required ? " (required)" : " (optional)")
                  << '\n';
    }
}

void cmd_describe(const loom::ConsoleEngine& engine, const std::vector<Token>& tok) {
    std::uint64_t ver = 0;
    if (tok.size() != 3 || !parse_u64(tok[2].text, ver)) {
        std::cout << "usage: describe <Shape> <version>\n";
        return;
    }
    auto desc = engine.describe(tok[1].text, static_cast<std::uint32_t>(ver));
    if (!desc) {
        std::cout << "  no such registered shape: " << tok[1].text << " v" << ver << '\n';
        return;
    }
    print_fields(*desc);
}

// Render the engine's NeedsInput as a plain prompt: the still-open fields and the values the
// ladder could not safely place. The operator re-sends with explicit `field=value`.
void print_needs_input(const loom::Composed& c) {
    std::cout << "  needs input — the ladder could not unambiguously place every value.\n"
                 "  fill the open fields and re-send with field=value:\n";
    for (const auto& f : c.open_fields) {
        std::cout << "    " << f.name << " : " << f.type << (f.required ? " (required)" : " (optional)")
                  << '\n';
    }
    if (!c.unplaced.empty()) {
        std::cout << "  unplaced:";
        for (const auto& u : c.unplaced) {
            std::cout << ' ' << u;
        }
        std::cout << '\n';
    }
}

void cmd_send(loom::ConsoleEngine& engine, const std::vector<Token>& tok) {
    // Guided show-then-choose: each fuller prefix reveals the next choice.
    if (tok.size() == 1) {
        std::cout << "choose a weave:\n";
        print_weaves(engine);
        std::cout << "then: send <id> <Shape> <version> [value | field=value | $mN.field ...]\n";
        return;
    }
    std::uint64_t id = 0;
    if (!parse_u64(tok[1].text, id)) {
        std::cout << "  bad weave id: " << tok[1].text << '\n';
        return;
    }
    if (tok.size() == 2) {
        std::cout << "weave " << id << " accepts:\n";
        for (const auto& s : engine.weaves()) {
            if (s.id.value == id) {
                for (const auto& a : s.accepts) {
                    std::cout << "  " << a.name << " v" << a.version << '\n';
                }
            }
        }
        std::cout << "then: send " << id << " <Shape> <version> [value | field=value | $mN.field ...]\n";
        return;
    }
    std::uint64_t ver = 0;
    if (tok.size() < 4 || !parse_u64(tok[3].text, ver)) {
        std::cout << "usage: send <id> <Shape> <version> [value | field=value | $mN.field ...]\n";
        return;
    }

    // Lex the remaining tokens to structured Args and let the engine's ladder place them.
    std::vector<loom::Arg> args;
    for (std::size_t i = 4; i < tok.size(); ++i) {
        args.push_back(lex_arg(tok[i]));
    }

    // Total OBSERVED, not retained: once the bounded buffer saturates the retained count stops
    // moving, so "did a reply arrive?" has to be asked of the label that advances.
    const std::uint64_t before = engine.evicted().buffer + engine.buffer_size();
    const loom::Composed c =
        engine.compose(loom::WeaveId{id}, tok[2].text, static_cast<std::uint32_t>(ver), args);

    switch (c.status) {
    case loom::Composed::Status::Error:
        std::cout << "  compose error: " << c.error << '\n';
        return;
    case loom::Composed::Status::NeedsInput:
        print_needs_input(c);
        return;
    case loom::Composed::Status::Ready:
        break;
    }

    engine.pump();
    const loom::SendOutcome o = engine.outcome(c.ticket);
    if (o.delivered) {
        std::cout << "  sent (delivered).";
    } else if (o.refused) {
        std::cout << "  refused: " << o.reason; // the gate's verdict — the backstop spoke
    }
    const std::uint64_t after = engine.evicted().buffer + engine.buffer_size();
    if (after > before) {
        std::cout << "  reply -> m" << after; // the newest LABEL, past everything evicted
    }
    std::cout << '\n';
}

void cmd_show(const loom::ConsoleEngine& engine, const std::vector<Token>& tok) {
    std::uint64_t n = 0;
    if (tok.size() != 2 || tok[1].text.empty() || tok[1].text[0] != 'm' ||
        !parse_u64(tok[1].text.substr(1), n)) {
        std::cout << "usage: show <mN>\n";
        return;
    }
    auto entry = engine.buffer_at(static_cast<std::size_t>(n));
    if (!entry) {
        std::cout << "  no such buffer entry: " << tok[1].text << '\n';
        return;
    }
    std::cout << "  " << entry->label << " : " << entry->name << " v" << entry->version << '\n';
    const loom::Value& v = entry->value;
    for (const loom::Field& f : v.schema().fields()) {
        const loom::Cell* c = v.get(f.name);
        std::cout << "    " << f.name << " = ";
        if (c == nullptr) {
            std::cout << "(absent)";
        } else {
            switch (c->kind()) {
            case loom::Kind::Int:
                std::cout << c->as_int();
                break;
            case loom::Kind::Float:
                std::cout << c->as_float();
                break;
            case loom::Kind::Text:
                std::cout << '"' << c->as_text() << '"';
                break;
            case loom::Kind::Bool:
                std::cout << (c->as_bool() ? "true" : "false");
                break;
            default:
                std::cout << "(" << loom::name_of(c->kind()) << ")";
                break;
            }
        }
        std::cout << '\n';
    }
}

void cmd_tap(const loom::ConsoleEngine& engine) {
    const auto events = engine.tap();
    if (events.empty()) {
        std::cout << "  (no bus events yet)\n";
        return;
    }
    // A bounded window that claimed to be the complete history would trade a memory lie for an
    // observability lie, so say so whenever it is not.
    if (const std::uint64_t gone = engine.evicted().tap; gone != 0) {
        std::cout << "  (" << gone << " older events evicted — the tap retains the most recent "
                  << loom::kConsoleTapCapacity << ")\n";
    }
    for (const auto& e : events) {
        std::cout << "  " << e.kind << " " << e.schema << "  " << e.sender.value << " -> "
                  << e.target.value;
        if (!e.refusal.empty()) {
            std::cout << "  [" << e.refusal << "]";
        }
        std::cout << '\n';
    }
}

} // namespace

int main() {
    loom::Switchboard bus;
    loom::ConsoleEngine engine(bus);

    // Mount a demo greeter so the terminal has something to drive.
    bus.register_weave(std::make_unique<Greeter>(), loom::Grant{}.allow_any());
    bus.register_weave(std::make_unique<loom::PokeWeave>(), loom::Grant{}.allow_any());

    std::cout << "zen console (stage 2). commands: weaves | describe <Shape> <v> | "
                 "send [<id> [<Shape> <v> [args ...]]] | buffer | show <mN> | tap | quit\n"
                 "args: a bare value is positional/type-directed; field=value names a field; "
                 "$mN.field references a buffered reply; quote to force Text (\"5\" is text, 5 is Int).\n";

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        const std::vector<Token> tok = tokenize(line);
        if (tok.empty()) {
            continue;
        }
        const std::string& cmd = tok[0].text;
        if (cmd == "quit" || cmd == "exit") {
            break;
        } else if (cmd == "weaves") {
            print_weaves(engine);
        } else if (cmd == "describe") {
            cmd_describe(engine, tok);
        } else if (cmd == "send") {
            cmd_send(engine, tok);
        } else if (cmd == "buffer") {
            const std::size_t n = engine.buffer_size();
            const std::uint64_t gone = engine.evicted().buffer;
            if (n == 0) {
                std::cout << "  (buffer empty)\n";
            } else if (gone != 0) {
                std::cout << "  (" << gone << " older replies evicted — the console retains the "
                          << "most recent " << loom::kConsoleBufferCapacity << ")\n";
            }
            // Over the retained LABEL range: labels are identities, so the window starts past
            // everything already evicted.
            for (std::uint64_t label = gone + 1; label <= gone + n; ++label) {
                auto e = engine.buffer_at(static_cast<std::size_t>(label));
                if (e) {
                    std::cout << "  " << e->label << " : " << e->name << " v" << e->version << '\n';
                }
            }
        } else if (cmd == "show") {
            cmd_show(engine, tok);
        } else if (cmd == "tap") {
            cmd_tap(engine);
        } else if (cmd == "help") {
            std::cout << "weaves | describe <Shape> <v> | send [<id> [<Shape> <v> [args ...]]] | "
                         "buffer | show <mN> | tap | quit\n"
                         "  send args: bare value (positional/type-directed), field=value (named), "
                         "$mN.field (reference); quote to force Text.\n";
        } else {
            std::cout << "  unknown command: " << cmd << " (try 'help')\n";
        }
    }
    return 0;
}
