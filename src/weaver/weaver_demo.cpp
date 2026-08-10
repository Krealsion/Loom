// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE FIRST SEAT — a running composition in which a human being decides, by
// typing ordinary Loom messages, what one live session is allowed to say.
//
//     USER          decides          <- you, at this prompt
//     WEAVER        delegates        <- loom::Weaver, holding one GrantAuthority
//     SESSION       acts             <- an ordinary weave with a tiny baseline
//     SWITCHBOARD   enforces         <- the Kernel, which has the last word
//
// THIS FILE IS THE HOST, and a host is exactly two things: the mounting, and a
// skin. The mounting below is the whole security posture of the demo, written
// out where it can be read in one sitting — who exists, what each may say, who
// the user is, and what the Weaver may ever hand out. The skin underneath it is
// a plain generic REPL over `loom::ConsoleEngine`, the durable console.
//
// THE SKIN KNOWS NOTHING ABOUT AUTHORITY. There is no `approve` command, no
// `grant` command and no weaver branch of any kind — grep this file for
// "Approve" and the only hits are in the mounting and in the help text. The
// operator approves by composing `zen.ApproveAuthority` the same way it would
// compose any other registered shape, and it travels down the same gated send
// path as any other message. That absence is the point: if the console needed a
// privileged branch to make this work, the architecture would have failed.
//
// Try it:
//
//     weaves                                  who is on the bus
//     send <session> AskForAuthority 1        the session asks the Weaver
//     buffer                                  a prompt arrived for you
//     show m1                                 read it: requester, rule, purpose
//     send <weaver> zen.ApproveAuthority 1    you decide
//     send <session> DoWork 1                 the session retries, and lands
//     send <weaver> zen.DescribeAuthority 1   what the Kernel now enforces
//     send <weaver> zen.RevokeAuthority 1     the off switch
//     send <session> DoWork 1                 denied again (see: tap)

#include <zen/console/console.hpp>
#include <zen/console/input_lex.hpp>
#include <zen/host/grant_wiring.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weaver/weaver.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using loom::lex_arg;
using loom::parse_u64;
using loom::Token;
using loom::tokenize;

constexpr const char* kServiceRole = "some.service";
constexpr const char* kWeaverRole = "loom.weaver";

// ---- the cast's own scenery -------------------------------------------------

struct Work {
    std::int64_t n = 0;
    ZEN_SHAPE(Work, 1, ZEN_FIELD(n));
};
/// Ask the session to try the thing it is not (yet) allowed to do.
struct DoWork {
    ZEN_SHAPE(DoWork, 1);
};
/// Ask the session to request the authority it lacks.
struct AskForAuthority {
    ZEN_SHAPE(AskForAuthority, 1);
};
struct Nothing {
    std::int64_t n = 0;
    ZEN_SHAPE(Nothing, 1, ZEN_FIELD(n));
};

/// An ordinary service in an ordinary office. It reports who it heard from,
/// which is the fact the whole architecture is arranged to protect.
class Service final : public loom::WeaveBase<Service, Nothing, loom::Accept<Work>> {
public:
    void on(const Work& w, loom::Mail& mail) {
        std::cout << "  [service] Work{" << w.n << "} from weave " << mail.sender().value
                  << (mail.sender() == expected_session ? "  (the SESSION — not the Weaver)" : "")
                  << '\n';
    }
    loom::WeaveId expected_session{};
};

/// The governed session. Deliberately NOT called a TerminalSession: it has no
/// transcript, no composer and no command language, and borrowing that name for
/// something with none of them would mislead the next reader.
class Session final
    : public loom::WeaveBase<Session, Nothing,
                             loom::Accept<DoWork, AskForAuthority, loom::AuthorityGranted,
                                          loom::Refused, loom::AuthorityDescription>,
                             loom::Emit<Work, loom::RequestAuthority, loom::DescribeAuthority>> {
public:
    void on(const DoWork&, loom::Mail& mail) {
        // AN EXPLICIT ACT, EVERY TIME. Nothing replays a refused message; if the
        // session wants to try again after being granted authority, it says so
        // itself — right here, in its own code.
        (void)mail.send_to_role(kServiceRole, Work{++attempts_});
        std::cout << "  [session] sent Work{" << attempts_ << "} to role " << kServiceRole
                  << " (watch `tap` for whether it landed)\n";
    }
    void on(const AskForAuthority&, loom::Mail& mail) {
        (void)mail.send_to_role(
            kWeaverRole, loom::RequestAuthority{"Work", 1, kServiceRole,
                                                "so I can finish the job I was given"});
        std::cout << "  [session] asked the Weaver for: Work v1 -> role " << kServiceRole << '\n';
    }
    void on(const loom::AuthorityGranted& g, loom::Mail& mail) {
        std::cout << "  [session] AuthorityGranted{" << g.basis << "}"
                  << (mail.answers_ask() ? "  (Loom says: this answers MY ask)" : "  (unsolicited!)")
                  << "\n             ...and nothing happened. I must retry, myself.\n";
    }
    void on(const loom::Refused& r, loom::Mail& mail) {
        std::cout << "  [session] refused: " << r.reason
                  << (mail.answers_ask() ? "  (the authenticated answer to my ask)" : "") << '\n';
    }
    void on(const loom::AuthorityDescription& d, loom::Mail&) {
        std::cout << "  [session] my own authority:\n";
        for (const std::string& line : d.base) {
            std::cout << "             base      " << line << '\n';
        }
        for (const std::string& line : d.delegated) {
            std::cout << "             delegated " << line << '\n';
        }
    }

private:
    std::int64_t attempts_ = 0;
};

// ---- the skin: generic text over the durable console engine ----------------

/// One received cell as text. Scalars, plus lists of scalars — a console that
/// can receive a shape ought to be able to show it, and a list of strings is an
/// ordinary thing for a shape to carry.
std::string render_cell(const loom::Cell& c);

std::string render_scalar(const loom::Cell& c) {
    switch (c.kind()) {
    case loom::Kind::Int:
        return std::to_string(c.as_int());
    case loom::Kind::Float:
        return std::to_string(c.as_float());
    case loom::Kind::Text:
        return '"' + c.as_text() + '"';
    case loom::Kind::Bool:
        return c.as_bool() ? "true" : "false";
    default:
        return std::string("(") + loom::name_of(c.kind()) + ")";
    }
}

std::string render_cell(const loom::Cell& c) {
    if (c.kind() != loom::Kind::List) {
        return render_scalar(c);
    }
    const loom::Cell::Array& items = c.as_list();
    if (items.empty()) {
        return "[]"; // an empty list is a fact, not a missing value
    }
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        out += (i == 0 ? " " : ", ");
        out += render_cell(items[i]);
    }
    return out + " ]";
}

void print_weaves(const loom::ConsoleEngine& engine) {
    for (const loom::WeaveInfo& s : engine.weaves()) {
        std::cout << "  weave " << s.id.value << "  accepts:";
        if (s.accepts.empty()) {
            std::cout << " (none)";
        }
        for (const loom::ShapeRef& a : s.accepts) {
            std::cout << ' ' << a.name << " v" << a.version;
        }
        std::cout << '\n';
    }
}

void cmd_show(const loom::ConsoleEngine& engine, const std::vector<Token>& tok) {
    std::uint64_t n = 0;
    if (tok.size() != 2 || tok[1].text.size() < 2 || tok[1].text[0] != 'm' ||
        !parse_u64(tok[1].text.substr(1), n)) {
        std::cout << "usage: show <mN>\n";
        return;
    }
    const auto entry = engine.buffer_at(static_cast<std::size_t>(n));
    if (!entry) {
        std::cout << "  no such buffer entry: " << tok[1].text << '\n';
        return;
    }
    std::cout << "  " << entry->label << " : " << entry->name << " v" << entry->version << '\n';
    for (const loom::Field& f : entry->value.schema().fields()) {
        const loom::Cell* c = entry->value.get(f.name);
        std::cout << "    " << f.name << " = " << (c == nullptr ? "(absent)" : render_cell(*c))
                  << '\n';
    }
}

void cmd_send(loom::ConsoleEngine& engine, const std::vector<Token>& tok) {
    std::uint64_t id = 0, ver = 0;
    if (tok.size() < 4 || !parse_u64(tok[1].text, id) || !parse_u64(tok[3].text, ver)) {
        std::cout << "usage: send <id> <Shape> <version> [value | field=value ...]\n";
        return;
    }
    std::vector<loom::Arg> args;
    for (std::size_t i = 4; i < tok.size(); ++i) {
        args.push_back(lex_arg(tok[i]));
    }
    const std::uint64_t before = engine.evicted().buffer + engine.buffer_size();
    const loom::Composed c =
        engine.compose(loom::WeaveId{id}, tok[2].text, static_cast<std::uint32_t>(ver), args);
    if (c.status == loom::Composed::Status::Error) {
        std::cout << "  compose error: " << c.error << '\n';
        return;
    }
    if (c.status == loom::Composed::Status::NeedsInput) {
        std::cout << "  needs input; still-open fields:\n";
        for (const loom::FieldDesc& f : c.open_fields) {
            std::cout << "    " << f.name << " : " << f.type << '\n';
        }
        return;
    }
    engine.pump();
    const loom::SendOutcome o = engine.outcome(c.ticket);
    std::cout << "  " << (o.refused ? "refused: " + o.reason : std::string("sent (delivered)."));
    const std::uint64_t after = engine.evicted().buffer + engine.buffer_size();
    if (after > before) {
        std::cout << "   -> replies m" << (before + 1) << " .. m" << after;
    }
    std::cout << '\n';
}

void cmd_tap(const loom::ConsoleEngine& engine, std::size_t last) {
    const std::vector<loom::TapEvent> events = engine.tap();
    const std::size_t from = events.size() > last ? events.size() - last : 0;
    for (std::size_t i = from; i < events.size(); ++i) {
        const loom::TapEvent& e = events[i];
        std::cout << "  " << e.kind << ' ' << e.schema << "  " << e.sender.value << " -> "
                  << e.target.value << (e.refusal.empty() ? "" : "  [" + e.refusal + "]") << '\n';
    }
}

} // namespace

int main() {
    loom::Switchboard bus;

    // THE OPERATOR'S WINDOW. It declares the shapes that only ever travel TO an
    // operator: `AcceptMode::AnyRegistered` resolves a shape through the
    // registry, and a notification nobody else accepts is in no registry. This
    // is the console saying what it is for — not a grant, and not a bypass.
    loom::ConsoleEngine engine(bus, {loom::schema_of<loom::AuthorityPrompt>(),
                                     loom::schema_of<loom::AuthorityDescription>(),
                                     loom::schema_of<loom::Ack>(),
                                     loom::schema_of<loom::Refused>()});
    const loom::WeaveId operator_seat = engine.console_id();

    // THE SERVICE — holds an office, speaks to nobody.
    auto service = std::make_unique<Service>();
    Service* service_raw = service.get();
    const loom::WeaveId service_id =
        bus.register_weave(std::move(service), loom::Grant::nothing(), kServiceRole);
    service_raw->zen_set_self(service_id);

    // THE GOVERNED SESSION'S FLOOR. It may ask its Weaver two questions. It may
    // NOT speak Work — that is the whole premise. Note that its baseline names
    // the Weaver by ROLE, which is what lets it be admitted before the Weaver
    // exists (the Weaver cannot be built until a capability naming THIS session
    // exists, and that needs this id).
    loom::Grant session_base;
    session_base.allow_to_role("zen.RequestAuthority", 1, kWeaverRole);
    session_base.allow_to_role("zen.DescribeAuthority", 1, kWeaverRole);
    auto session = std::make_unique<Session>();
    Session* session_raw = session.get();
    const loom::WeaveId session_id = bus.register_weave(std::move(session), std::move(session_base));
    session_raw->zen_set_self(session_id);
    service_raw->expected_session = session_id;

    // THE TWO POWERS, MINTED SEPARATELY.
    //
    // What the Weaver may DELEGATE — the ceiling, on exactly one subject. Try
    // asking for anything outside this line and watch the Kernel refuse a
    // request the human already approved.
    loom::LiveAuthority ceiling;
    ceiling.allow_to_role("Work", 1, kServiceRole);
    loom::GrantAuthority cap = loom::host_grant_authority(bus, session_id, std::move(ceiling));

    // What the Weaver may SAY — an ordinary grant, and deliberately not the
    // ceiling. It may say "no" to anyone who reaches it; it may say "yes" to
    // exactly two weaves.
    loom::Grant weaver_grant;
    weaver_grant.allow("zen.AuthorityPrompt", 1, operator_seat);
    weaver_grant.allow("zen.AuthorityGranted", 1, session_id);
    weaver_grant.allow("zen.AuthorityDescription", 1, operator_seat);
    weaver_grant.allow("zen.AuthorityDescription", 1, session_id);
    weaver_grant.allow("zen.Ack", 1, operator_seat);
    weaver_grant.allow_to_any("zen.Refused", 1);

    auto weaver = std::make_unique<loom::Weaver>(std::move(cap), operator_seat);
    loom::Weaver* weaver_raw = weaver.get();
    const loom::WeaveId weaver_id =
        bus.register_weave(std::move(weaver), std::move(weaver_grant), kWeaverRole);
    weaver_raw->zen_set_self(weaver_id);

    std::cout << "zen weaver demo — you are the operator (weave " << operator_seat.value << ").\n"
              << "  weaver  " << weaver_id.value << "   holds one GrantAuthority over the session\n"
              << "  session " << session_id.value << "   may ask the weaver; may NOT speak Work\n"
              << "  service " << service_id.value << "   holds role " << kServiceRole << "\n\n"
              << "commands: weaves | describe <Shape> <v> | send <id> <Shape> <v> [args] |\n"
              << "          buffer | show <mN> | tap | quit\n\n"
              << "try: send " << session_id.value << " DoWork 1        (denied — see `tap`)\n"
              << "     send " << session_id.value << " AskForAuthority 1\n"
              << "     show m1\n"
              << "     send " << weaver_id.value << " zen.ApproveAuthority 1\n"
              << "     send " << session_id.value << " DoWork 1        (lands, as the SESSION)\n"
              << "     send " << weaver_id.value << " zen.DescribeAuthority 1\n"
              << "     send " << weaver_id.value << " zen.RevokeAuthority 1\n\n";

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        const std::vector<Token> tok = tokenize(line);
        if (tok.empty()) {
            continue;
        }
        const std::string& cmd = tok[0].text;
        if (cmd == "quit" || cmd == "exit") {
            break;
        }
        const std::size_t tap_before = engine.tap().size();
        if (cmd == "weaves") {
            print_weaves(engine);
        } else if (cmd == "describe" && tok.size() == 3) {
            std::uint64_t v = 0;
            const auto d = parse_u64(tok[2].text, v)
                               ? engine.describe(tok[1].text, static_cast<std::uint32_t>(v))
                               : std::nullopt;
            if (!d) {
                std::cout << "  no such registered shape\n";
            } else {
                for (const loom::FieldDesc& f : d->fields) {
                    std::cout << "    " << f.name << " : " << f.type << '\n';
                }
            }
        } else if (cmd == "send") {
            cmd_send(engine, tok);
        } else if (cmd == "buffer") {
            const std::uint64_t gone = engine.evicted().buffer;
            for (std::uint64_t n = gone + 1; n <= gone + engine.buffer_size(); ++n) {
                if (const auto e = engine.buffer_at(static_cast<std::size_t>(n))) {
                    std::cout << "  " << e->label << " : " << e->name << " v" << e->version << '\n';
                }
            }
        } else if (cmd == "show") {
            cmd_show(engine, tok);
        } else if (cmd == "tap") {
            cmd_tap(engine, 20);
        } else {
            std::cout << "  unknown command: " << cmd << '\n';
        }
        // Anything a command caused is on the tap; show only what is new, so a
        // refusal is never something the operator has to go looking for.
        engine.pump();
        if (engine.tap().size() > tap_before) {
            std::cout << "  --- bus ---\n";
            cmd_tap(engine, engine.tap().size() - tap_before);
        }
    }
    return 0;
}
