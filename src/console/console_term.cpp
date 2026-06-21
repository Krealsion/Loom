// The Console's first, throwaway skin: a deliberately plain terminal REPL over the
// ConsoleEngine. It calls the engine API and formats the returned DOMAIN DATA as plain
// text — no cursor addressing, no panes (that is Stage 3). Its only job is to exercise
// the engine; the engine, not this, is the durable artifact. A GUI later replaces only
// this file, inheriting the engine whole.
//
// A demo "greeter" Shard is mounted at startup so a standalone terminal has something to
// drive (discover, send, see a reply buffered). In a real deployment, Shards arrive on the
// bus by other means; the console drives whatever is there.

#include <zen/console/console.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

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

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) {
        out.push_back(tok);
    }
    return out;
}

bool parse_u64(const std::string& s, std::uint64_t& out) {
    try {
        std::size_t pos = 0;
        const unsigned long long v = std::stoull(s, &pos);
        if (pos != s.size()) {
            return false;
        }
        out = static_cast<std::uint64_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

// Parse "field=value" against the shape's field types into the engine's typed FieldValue.
bool parse_field(const zen::console::ShapeDesc& desc, const std::string& pair,
                 std::string& field, zen::console::FieldValue& value, std::string& err) {
    const std::size_t eq = pair.find('=');
    if (eq == std::string::npos) {
        err = "expected field=value, got '" + pair + "'";
        return false;
    }
    field = pair.substr(0, eq);
    const std::string raw = pair.substr(eq + 1);
    const zen::console::FieldDesc* fd = nullptr;
    for (const auto& f : desc.fields) {
        if (f.name == field) {
            fd = &f;
        }
    }
    if (fd == nullptr) {
        err = "shape has no field '" + field + "'";
        return false;
    }
    try {
        if (fd->type == "Int") {
            value = static_cast<std::int64_t>(std::stoll(raw));
        } else if (fd->type == "Float") {
            value = std::stod(raw);
        } else if (fd->type == "Text") {
            value = raw;
        } else if (fd->type == "Bool") {
            value = (raw == "true" || raw == "1");
        } else if (fd->type == "Bytes") {
            value = zen::Bytes(raw.begin(), raw.end());
        } else {
            err = "field '" + field + "' has type " + fd->type +
                  " — the Stage 1 terminal sets only scalar fields";
            return false;
        }
    } catch (...) {
        err = "could not parse '" + raw + "' as " + fd->type;
        return false;
    }
    return true;
}

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

void cmd_describe(const zen::console::ConsoleEngine& engine, const std::vector<std::string>& tok) {
    std::uint64_t ver = 0;
    if (tok.size() != 3 || !parse_u64(tok[2], ver)) {
        std::cout << "usage: describe <Shape> <version>\n";
        return;
    }
    auto desc = engine.describe(tok[1], static_cast<std::uint32_t>(ver));
    if (!desc) {
        std::cout << "  no such registered shape: " << tok[1] << " v" << ver << '\n';
        return;
    }
    print_fields(*desc);
}

void cmd_send(zen::console::ConsoleEngine& engine, const std::vector<std::string>& tok) {
    // Guided show-then-choose: each fuller prefix reveals the next choice.
    if (tok.size() == 1) {
        std::cout << "choose a shard:\n";
        print_shards(engine);
        std::cout << "then: send <id> <Shape> <version> [field=value ...]\n";
        return;
    }
    std::uint64_t id = 0;
    if (!parse_u64(tok[1], id)) {
        std::cout << "  bad shard id: " << tok[1] << '\n';
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
        std::cout << "then: send " << id << " <Shape> <version> [field=value ...]\n";
        return;
    }
    std::uint64_t ver = 0;
    if (tok.size() < 4 || !parse_u64(tok[3], ver)) {
        std::cout << "usage: send <id> <Shape> <version> [field=value ...]\n";
        return;
    }
    auto desc = engine.describe(tok[2], static_cast<std::uint32_t>(ver));
    if (!desc) {
        std::cout << "  no such registered shape: " << tok[2] << " v" << ver << '\n';
        return;
    }
    if (tok.size() == 4) { // guided: show the fields to fill, then stop
        std::cout << "fill these fields, then re-send with field=value:\n";
        print_fields(*desc);
        return;
    }
    std::map<std::string, zen::console::FieldValue> fields;
    for (std::size_t i = 4; i < tok.size(); ++i) {
        std::string field;
        zen::console::FieldValue value;
        std::string err;
        if (!parse_field(*desc, tok[i], field, value, err)) {
            std::cout << "  " << err << '\n';
            return;
        }
        fields.insert_or_assign(field, std::move(value));
    }
    const std::size_t before = engine.buffer_size();
    std::string err;
    const zen::sb::Ticket t =
        engine.submit(zen::sb::ShardId{id}, tok[2], static_cast<std::uint32_t>(ver), fields, &err);
    if (!t.valid()) {
        std::cout << "  compose error: " << err << '\n';
        return;
    }
    engine.pump();
    const zen::console::SendOutcome o = engine.outcome(t);
    if (o.delivered) {
        std::cout << "  sent (delivered).";
    } else if (o.refused) {
        std::cout << "  refused: " << o.reason;
    }
    if (engine.buffer_size() > before) {
        std::cout << "  reply -> m" << engine.buffer_size();
    }
    std::cout << '\n';
}

void cmd_show(const zen::console::ConsoleEngine& engine, const std::vector<std::string>& tok) {
    std::uint64_t n = 0;
    if (tok.size() != 2 || tok[1].empty() || tok[1][0] != 'm' || !parse_u64(tok[1].substr(1), n)) {
        std::cout << "usage: show <mN>\n";
        return;
    }
    auto entry = engine.buffer_at(static_cast<std::size_t>(n));
    if (!entry) {
        std::cout << "  no such buffer entry: " << tok[1] << '\n';
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

    std::cout << "zen console (stage 1). commands: shards | describe <Shape> <v> | "
                 "send [<id> [<Shape> <v> [f=v ...]]] | buffer | show <mN> | tap | quit\n";

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        const std::vector<std::string> tok = tokenize(line);
        if (tok.empty()) {
            continue;
        }
        const std::string& cmd = tok[0];
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
            std::cout << "shards | describe <Shape> <v> | send [<id> [<Shape> <v> [f=v ...]]] | "
                         "buffer | show <mN> | tap | quit\n";
        } else {
            std::cout << "  unknown command: " << cmd << " (try 'help')\n";
        }
    }
    return 0;
}
