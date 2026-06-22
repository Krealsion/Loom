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
// A demo "greeter" Shard is mounted at startup so a standalone terminal has something to
// drive (discover, send, see a reply buffered). In a real deployment, Shards arrive on the
// bus by other means; the console drives whatever is there.

#include <zen/console/console.hpp>
#include <zen/console/input_lex.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

// The text lexers (tokenize / lex_arg / parse_u64) are shared with the TUI and the tests; they
// live in <zen/console/input_lex.hpp>. Bring the ones this REPL uses into scope.
using zen::console::parse_u64;
using zen::console::Token;
using zen::console::tokenize;
using zen::console::lex_arg;

std::shared_ptr<const zen::Schema> greet_schema() {
    static const auto s = zen::SchemaBuilder("Greet", 1).field("msg", zen::Kind::Text).build();
    return s;
}

// A trivial demo responder: accepts Greet{msg} and echoes it back to the sender's reply
// address — so the terminal has a live Shard to discover and drive.
class Greeter final : public zen::sb::Shard {
public:
    std::vector<std::shared_ptr<const zen::Schema>> accepted_schemas() const override {
        return {greet_schema()};
    }
    void handle(const zen::sb::Message& in, zen::sb::Bus& bus) override {
        zen::Value v(greet_schema());
        v.set("msg", zen::Cell::text(in.payload.get("msg")->as_text()));
        bus.send(in.reply_to, zen::sb::Message(std::move(v)));
    }
    zen::Value snapshot() const override {
        zen::Value v(state_schema());
        v.set("n", zen::Cell::integer(0));
        return v;
    }
    zen::Value policy() const override {
        zen::Value v(zen::sb::lifecycle_policy_schema());
        v.set("max_reloads", zen::Cell::integer(0));
        v.set("revive_from_last_good", zen::Cell::boolean(true));
        return v;
    }
    void revive(const zen::Value&) override {}

private:
    static std::shared_ptr<const zen::Schema> state_schema() {
        static const auto s = zen::SchemaBuilder("GreeterState", 1).field("n", zen::Kind::Int).build();
        return s;
    }
};

void print_shards(const zen::console::ConsoleEngine& engine) {
    const auto shards = engine.shards();
    if (shards.empty()) {
        std::cout << "  (no shards on the bus)\n";
        return;
    }
    for (const auto& s : shards) {
        std::cout << "  shard " << s.id.value << "  accepts:";
        if (s.accepts.empty()) {
            std::cout << " (none)";
        }
        for (const auto& a : s.accepts) {
            std::cout << ' ' << a.name << " v" << a.version;
        }
        std::cout << '\n';
    }
}

void print_fields(const zen::console::ShapeDesc& desc) {
    std::cout << "  " << desc.name << " v" << desc.version << " fields:\n";
    for (const auto& f : desc.fields) {
        std::cout << "    " << f.name << " : " << f.type << (f.required ? " (required)" : " (optional)")
                  << '\n';
    }
}

void cmd_describe(const zen::console::ConsoleEngine& engine, const std::vector<Token>& tok) {
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
void print_needs_input(const zen::console::Composed& c) {
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

void cmd_send(zen::console::ConsoleEngine& engine, const std::vector<Token>& tok) {
    // Guided show-then-choose: each fuller prefix reveals the next choice.
    if (tok.size() == 1) {
        std::cout << "choose a shard:\n";
        print_shards(engine);
        std::cout << "then: send <id> <Shape> <version> [value | field=value | $mN.field ...]\n";
        return;
    }
    std::uint64_t id = 0;
    if (!parse_u64(tok[1].text, id)) {
        std::cout << "  bad shard id: " << tok[1].text << '\n';
        return;
    }
    if (tok.size() == 2) {
        std::cout << "shard " << id << " accepts:\n";
        for (const auto& s : engine.shards()) {
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
    std::vector<zen::console::Arg> args;
    for (std::size_t i = 4; i < tok.size(); ++i) {
        args.push_back(lex_arg(tok[i]));
    }

    const std::size_t before = engine.buffer_size();
    const zen::console::Composed c =
        engine.compose(zen::sb::ShardId{id}, tok[2].text, static_cast<std::uint32_t>(ver), args);

    switch (c.status) {
    case zen::console::Composed::Status::Error:
        std::cout << "  compose error: " << c.error << '\n';
        return;
    case zen::console::Composed::Status::NeedsInput:
        print_needs_input(c);
        return;
    case zen::console::Composed::Status::Ready:
        break;
    }

    engine.pump();
    const zen::console::SendOutcome o = engine.outcome(c.ticket);
    if (o.delivered) {
        std::cout << "  sent (delivered).";
    } else if (o.refused) {
        std::cout << "  refused: " << o.reason; // the gate's verdict — the backstop spoke
    }
    if (engine.buffer_size() > before) {
        std::cout << "  reply -> m" << engine.buffer_size();
    }
    std::cout << '\n';
}

void cmd_show(const zen::console::ConsoleEngine& engine, const std::vector<Token>& tok) {
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
    const zen::Value& v = entry->value;
    for (const zen::Field& f : v.schema().fields()) {
        const zen::Cell* c = v.get(f.name);
        std::cout << "    " << f.name << " = ";
        if (c == nullptr) {
            std::cout << "(absent)";
        } else {
            switch (c->kind()) {
            case zen::Kind::Int:
                std::cout << c->as_int();
                break;
            case zen::Kind::Float:
                std::cout << c->as_float();
                break;
            case zen::Kind::Text:
                std::cout << '"' << c->as_text() << '"';
                break;
            case zen::Kind::Bool:
                std::cout << (c->as_bool() ? "true" : "false");
                break;
            default:
                std::cout << "(" << zen::name_of(c->kind()) << ")";
                break;
            }
        }
        std::cout << '\n';
    }
}

void cmd_tap(const zen::console::ConsoleEngine& engine) {
    const auto events = engine.tap();
    if (events.empty()) {
        std::cout << "  (no bus events yet)\n";
        return;
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
    zen::sb::Switchboard bus;
    zen::console::ConsoleEngine engine(bus);

    // Mount a demo greeter so the terminal has something to drive.
    bus.register_shard(std::make_unique<Greeter>(), zen::sb::Grant{}.allow_any());

    std::cout << "zen console (stage 2). commands: shards | describe <Shape> <v> | "
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
        } else if (cmd == "shards") {
            print_shards(engine);
        } else if (cmd == "describe") {
            cmd_describe(engine, tok);
        } else if (cmd == "send") {
            cmd_send(engine, tok);
        } else if (cmd == "buffer") {
            const std::size_t n = engine.buffer_size();
            if (n == 0) {
                std::cout << "  (buffer empty)\n";
            }
            for (std::size_t i = 1; i <= n; ++i) {
                auto e = engine.buffer_at(i);
                std::cout << "  " << e->label << " : " << e->name << " v" << e->version << '\n';
            }
        } else if (cmd == "show") {
            cmd_show(engine, tok);
        } else if (cmd == "tap") {
            cmd_tap(engine);
        } else if (cmd == "help") {
            std::cout << "shards | describe <Shape> <v> | send [<id> [<Shape> <v> [args ...]]] | "
                         "buffer | show <mN> | tap | quit\n"
                         "  send args: bare value (positional/type-directed), field=value (named), "
                         "$mN.field (reference); quote to force Text.\n";
        } else {
            std::cout << "  unknown command: " << cmd << " (try 'help')\n";
        }
    }
    return 0;
}
