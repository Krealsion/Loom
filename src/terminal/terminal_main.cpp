// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE FIRST PRESENTATION OF THE TERMINAL CORE — a plain REPL that owns no
// terminal semantics whatever.
//
// Everything this file does is parse a line, name a participant, call the core,
// and render what came back. It holds no transcript of its own, decides nothing
// about authority, and has no privileged path to anything: delete it and the
// terminal is still there, in `zen-terminal`, waiting for another presentation.
// That is the claim this package makes, and this file is where it is either true or
// obviously false.
//
// THREE LENSES, VISIBLY DIFFERENT, AND NEVER SILENTLY CROSSED:
//
//     session>   the governed participant. Its own WeaveId, its own small grant.
//     operator>  the seat whose word the Weaver treats as the user's. A DIFFERENT
//                WeaveId, with a different grant, whose messages leave through its
//                own door.
//     debug>     THE HOST LOOKING. Not a participant at all — this is the process
//                that owns the Switchboard reading its own registry and its own
//                tap. It can see things neither participant can, and every line it
//                prints says so.
//
// A command typed at one prompt is authored by that participant and by no other.
// There is no fallback, no automatic retry as somebody with more authority, and
// no command that quietly changes who is speaking. If the session lacks the
// authority for what you typed, you get what an ordinary participant gets:
// nothing back, because Loom does not tell a sender its send's fate.
//
// IT HOSTS ITS OWN LOOM. This executable boots a Switchboard, a service, a
// Weaver, a governed session and an operator seat, all in this process. It does
// NOT attach to another program's Loom — there is no socket here, no token, no
// authentication, and no remote anything. A "terminal" here means a
// presentation of participants that live in the same process.

#include <zen/host/grant_wiring.hpp>
#include <zen/host/terminal_wiring.hpp>
#include <zen/switchboard.hpp>
#include <zen/terminal/input_lex.hpp>
#include <zen/terminal/session.hpp>
#include <zen/weave.hpp>
#include <zen/weaver/weaver.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using loom::lex_arg;
// The address half of the command grammar is shared with every other presentation of this
// core since WT-1, exactly as the value half already was -- one parser, in
// <zen/terminal/input_lex.hpp>, so `#12` / `@office` / `*` cannot come to mean two things.
using loom::parse_address;
using loom::parse_u64;
using loom::Token;
using loom::tokenize;

constexpr const char* kServiceRole = "some.service";
constexpr const char* kWeaverRole = "loom.weaver";

// ---- the world this demo puts a terminal into -------------------------------
//
// Application-neutral scenery, exactly as the weaver suite keeps its own: a job
// the session may not do yet, a question anyone may ask, an answer, and a
// notification nobody asked for.

struct Work {
    std::int64_t n = 0;
    ZEN_SHAPE(Work, 1, ZEN_FIELD(n));
};
struct Query {
    std::string q;
    ZEN_SHAPE(Query, 1, ZEN_FIELD(q));
};
struct Reply {
    std::string answer;
    ZEN_SHAPE(Reply, 1, ZEN_FIELD(answer));
};
struct Notice {
    std::string text;
    ZEN_SHAPE(Notice, 1, ZEN_FIELD(text));
};
struct Nothing {
    std::int64_t n = 0;
    ZEN_SHAPE(Nothing, 1, ZEN_FIELD(n));
};

/// An ordinary service in an ordinary office. It answers questions and does work,
/// and it reports WHO it heard from — the fact the whole architecture protects.
class Service final
    : public loom::WeaveBase<Service, Nothing, loom::Accept<Work, Query>, loom::Emit<Reply>> {
public:
    void on(const Work& w, loom::Mail& mail) {
        std::cout << "  [service] Work{" << w.n << "} from weave #" << mail.sender().value
                  << (mail.sender() == expected_session
                          ? "   (the SESSION — not the operator, not the Weaver)"
                          : "")
                  << '\n';
    }
    void on(const Query& q, loom::Mail& mail) {
        // ANSWERED THROUGH LOOM'S OWN ANSWER AUTHORITY, so the asker gets provenance rather than
        // a message that merely looks like a reply — and gets its OWN correlation back with it.
        (void)mail.answer(Reply{"you asked: " + q.q});
    }
    loom::WeaveId expected_session{};
};

// ---- rendering: the presentation's whole job --------------------------------

std::string render_cell(const loom::Cell& c);

std::string render_scalar(const loom::Cell& c) {
    switch (c.kind()) {
    case loom::Kind::Int:
        return std::to_string(c.as_int());
    case loom::Kind::Float:
        return std::to_string(c.as_float());
    case loom::Kind::Text:
        // ESCAPED HERE, AT THE LAST MOMENT, and nowhere earlier: the core retains exactly the
        // bytes that arrived. A terminal is what needs protecting from a control byte, so a
        // terminal is what escapes one.
        return '"' + loom::safe_terminal_text(c.as_text()) + '"';
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

std::string render_address(const loom::TranscriptEntry& e) {
    switch (e.addressing) {
    case loom::Addressing::Weave:
        return "-> weave #" + std::to_string(e.target.value);
    case loom::Addressing::Role:
        return "-> role " + loom::safe_terminal_text(e.role);
    case loom::Addressing::Publish:
        return "-> published to " + std::to_string(e.recipients) + " accepter(s)";
    }
    return "->";
}

/// One transcript line. THE WORDING FOLLOWS THE EVIDENCE SOURCE and nothing else:
/// a message this participant authored is `SUBMITTED`, never "sent" and never
/// "delivered", because an ordinary sender is not told which it was.
std::string render_entry(const loom::DeskEntry& d) {
    const loom::TranscriptEntry& e = d.entry;
    const std::string out = "[" + d.lens + "] ";
    switch (e.kind) {
    case loom::TranscriptKind::LocalCommand:
        return out + "> " + loom::safe_terminal_text(e.text);
    case loom::TranscriptKind::LocalRefusal:
        return out + "refused here: " + loom::safe_terminal_text(e.text);
    case loom::TranscriptKind::LocalNotice:
        return out + loom::safe_terminal_text(e.text);
    case loom::TranscriptKind::Submitted:
        return out + "SUBMITTED " + e.shape + " v" + std::to_string(e.version) + " " +
               render_address(e) + "  (corr " + std::to_string(e.correlation) +
               ")   — delivery unknown to me";
    case loom::TranscriptKind::Received:
        return out + "RECEIVED  " + e.shape + " v" + std::to_string(e.version) + " from weave #" +
               std::to_string(e.sender.value) +
               (e.authored_role.empty()
                    ? ""
                    : " as role " + loom::safe_terminal_text(e.authored_role)) +
               "   r" + std::to_string(e.message) + "   (unsolicited)";
    case loom::TranscriptKind::AnswerReceived:
        return out + "ANSWERED  " + e.shape + " v" + std::to_string(e.version) + " from weave #" +
               std::to_string(e.sender.value) + "   r" + std::to_string(e.message) +
               (e.answers != 0
                    ? "   (Loom: this answers my ask " + std::to_string(e.answers) + ")"
                    : "   (Loom: an authenticated answer, matching no ask I am still waiting on)");
    }
    return out;
}

void print_result(const loom::TerminalResult& r) {
    if (r) {
        std::cout << "  submitted"
                  << (r.ask != 0 ? "  (ask " + std::to_string(r.ask) + " is now outstanding)"
                                 : std::string())
                  << '\n';
        return;
    }
    std::cout << "  refused here (" << loom::name_of(r.outcome) << "): " << r.detail << '\n';
    for (const loom::FieldDesc& f : r.open_fields) {
        std::cout << "      open field " << f.name << " : " << f.type
                  << (f.required ? " (required)" : " (optional)") << '\n';
    }
    if (!r.unplaced.empty()) {
        std::cout << "      unplaced:";
        for (const std::string& u : r.unplaced) {
            std::cout << ' ' << loom::safe_terminal_text(u);
        }
        std::cout << '\n';
    }
}

/// TRUST LABELS ONLY WHERE THE PROTOCOL PROVIDES THEM. Two facts are annotated
/// here and no others: the bus-stamped sender, which Loom guarantees, and — on an
/// authority prompt — the field the Weaver vocabulary itself names
/// `requester_says`, whose own name says it is prose the requester wrote. Nothing
/// else gets a label, because inventing one would teach a reader to trust a
/// payload field.
void print_message(const loom::ReceivedMessage& m) {
    std::cout << "  r" << m.id << " : " << m.value.schema().name() << " v"
              << m.value.schema().version() << "\n"
              << "     from weave #" << m.sender.value << "  (bus-stamped — TRUSTED)"
              << (m.authored_role.empty()
                      ? "   spoken personally"
                      : "   authored as role " + loom::safe_terminal_text(m.authored_role) +
                            " (Loom-verified)")
              << (m.answers_ask ? "   [Loom: this answers an ask of mine]" : "") << '\n';
    const bool is_prompt = m.value.schema().name() == "zen.AuthorityPrompt";
    for (const loom::Field& f : m.value.schema().fields()) {
        const loom::Cell* c = m.value.get(f.name);
        std::cout << "     " << f.name << " = " << (c == nullptr ? "(absent)" : render_cell(*c));
        if (is_prompt && f.name == "requester_says") {
            std::cout << "     <- UNTRUSTED: prose the requester wrote about itself";
        } else if (is_prompt && f.name == "requester") {
            std::cout << "     <- the Weaver's own trusted fact (the bus-stamped requester)";
        }
        std::cout << '\n';
    }
}

} // namespace

int main() {
    loom::Switchboard bus;

    // ---- the shapes each participant is allowed to KNOW ---------------------
    //
    // TYPE KNOWLEDGE, chosen by this host, and separate from authority in every direction: the
    // session knows how to compose `zen.ApproveAuthority` and will never be permitted to say one;
    // it knows `Work` long before it may send one. `accepts()` additionally opens a door.
    loom::TerminalVocabulary session_vocab;
    session_vocab.knows(loom::schema_of<Work>())
        .knows(loom::schema_of<Query>())
        .knows(loom::schema_of<loom::RequestAuthority>())
        .knows(loom::schema_of<loom::DescribeAuthority>())
        // ...including the one it must never be able to USE. Knowing it is what makes the refusal
        // a real measurement instead of a shape the terminal could not spell.
        .knows(loom::schema_of<loom::ApproveAuthority>())
        .accepts(loom::schema_of<Reply>())
        .accepts(loom::schema_of<Notice>())
        .accepts(loom::schema_of<loom::AuthorityGranted>())
        .accepts(loom::schema_of<loom::AuthorityDescription>())
        .accepts(loom::schema_of<loom::Refused>())
        .accepts(loom::schema_of<loom::Ack>());

    loom::TerminalVocabulary operator_vocab;
    operator_vocab.knows(loom::schema_of<loom::ApproveAuthority>())
        .knows(loom::schema_of<loom::RefuseAuthority>())
        .knows(loom::schema_of<loom::RevokeAuthority>())
        .knows(loom::schema_of<loom::DescribeAuthority>())
        .accepts(loom::schema_of<loom::AuthorityPrompt>())
        .accepts(loom::schema_of<loom::AuthorityDescription>())
        .accepts(loom::schema_of<loom::Ack>())
        .accepts(loom::schema_of<loom::Refused>());

    // ONE observation order, shared, so a merged chronology means "the order this presentation
    // learned these things" rather than an interleaving it invented.
    auto order = std::make_shared<loom::ObservationOrder>();

    // ---- the governed session ----------------------------------------------
    //
    // Its baseline is THREE RULES. Being a terminal confers none of them, and confers nothing
    // else either: no allow_any, no observation, no load capability, no host reach.
    loom::Grant session_grant;
    session_grant.allow_to_role("zen.RequestAuthority", 1, kWeaverRole);
    session_grant.allow_to_role("zen.DescribeAuthority", 1, kWeaverRole);
    session_grant.allow_to_role("Query", 1, kServiceRole); // one ordinary verb, deliberately given
    const loom::MountedTerminal session = loom::host_mount_terminal(
        bus, std::make_unique<loom::TerminalSession>("session", std::move(session_vocab), order),
        std::move(session_grant));

    // ---- the operator seat: a DIFFERENT weave -------------------------------
    //
    // Four rules, all of them "say one contentless decision to the policy office". No allow_any,
    // no tap, no discovery, no host root — the Weaver's bootstrap console held all four, and none
    // of them turned out to be necessary to be the user.
    loom::Grant operator_grant;
    operator_grant.allow_to_role("zen.ApproveAuthority", 1, kWeaverRole);
    operator_grant.allow_to_role("zen.RefuseAuthority", 1, kWeaverRole);
    operator_grant.allow_to_role("zen.RevokeAuthority", 1, kWeaverRole);
    operator_grant.allow_to_role("zen.DescribeAuthority", 1, kWeaverRole);
    const loom::MountedTerminal seat = loom::host_mount_terminal(
        bus, std::make_unique<loom::TerminalSession>("operator", std::move(operator_vocab), order),
        std::move(operator_grant));

    // The pairing — and the refusal to be one participant. Constructing this throws if the two
    // are the same weave.
    loom::TerminalDesk desk(*session.session, *seat.session);

    // ---- the service --------------------------------------------------------
    auto service = std::make_unique<Service>();
    Service* service_raw = service.get();
    loom::Grant service_grant;
    service_grant.allow_to_any("Reply", 1);
    const loom::WeaveId service_id =
        bus.register_weave(std::move(service), std::move(service_grant), kServiceRole);
    service_raw->zen_set_self(service_id);
    service_raw->expected_session = session.id;

    // ---- the Weaver: two powers, minted separately --------------------------
    loom::LiveAuthority ceiling;
    ceiling.allow_to_role("Work", 1, kServiceRole);
    loom::GrantAuthority cap = loom::host_grant_authority(bus, session.id, std::move(ceiling));

    loom::Grant weaver_grant;
    weaver_grant.allow("zen.AuthorityPrompt", 1, seat.id);
    weaver_grant.allow("zen.AuthorityGranted", 1, session.id);
    weaver_grant.allow("zen.AuthorityDescription", 1, seat.id);
    weaver_grant.allow("zen.AuthorityDescription", 1, session.id);
    weaver_grant.allow("zen.Ack", 1, seat.id);
    weaver_grant.allow_to_any("zen.Refused", 1);

    auto weaver = std::make_unique<loom::Weaver>(std::move(cap), seat.id);
    loom::Weaver* weaver_raw = weaver.get();
    const loom::WeaveId weaver_id =
        bus.register_weave(std::move(weaver), std::move(weaver_grant), kWeaverRole);
    weaver_raw->zen_set_self(weaver_id);

    // ---- the HOST DEBUG LENS ------------------------------------------------
    //
    // The tap, held by the host, wired to NEITHER participant. Everything it collects is
    // legitimately the host's — this process owns the Switchboard — and nothing it collects ever
    // reaches a transcript. That separation is the point: a participant that inherited this would
    // see every other weave's traffic and would look, line for line, exactly like one that could
    // not.
    struct Seen {
        std::string kind, schema, refusal;
        loom::WeaveId from, to;
    };
    std::vector<Seen> tap;
    const loom::ObserverId tap_id = bus.add_observer([&tap](const loom::BusEvent& e) {
        const char* kind = "?";
        switch (e.kind) {
        case loom::EventKind::Delivered:
            kind = "Delivered";
            break;
        case loom::EventKind::Refused:
            kind = "Refused";
            break;
        case loom::EventKind::Died:
            kind = "Died";
            break;
        case loom::EventKind::Revived:
            kind = "Revived";
            break;
        }
        tap.push_back(Seen{kind, e.schema_name,
                           e.kind == loom::EventKind::Refused ? loom::name_of(e.refusal.reason)
                                                              : "",
                           e.sender, e.target});
    });

    const auto print_tap = [&tap](std::size_t last) {
        const std::size_t from = tap.size() > last ? tap.size() - last : 0;
        for (std::size_t i = from; i < tap.size(); ++i) {
            const Seen& s = tap[i];
            std::cout << "  [debug] " << s.kind << ' ' << s.schema << "  #" << s.from.value
                      << " -> #" << s.to.value << (s.refusal.empty() ? "" : "  [" + s.refusal + "]")
                      << '\n';
        }
    };

    const auto print_who = [&] {
        // BOOTSTRAP FACTS, KNOWN BY THE HOST — not something either participant discovered. A
        // participant cannot enumerate the bus, so presenting this as its knowledge would be the
        // first lie.
        std::cout << "  [host configuration — this process wired all of it; neither participant\n"
                     "   discovered any of it, and neither can]\n"
                  << "     session       weave #" << session.id.value
                  << "   base: RequestAuthority/DescribeAuthority -> role " << kWeaverRole
                  << ", Query -> role " << kServiceRole << "\n"
                  << "     operator seat weave #" << seat.id.value
                  << "   base: Approve/Refuse/Revoke/DescribeAuthority -> role " << kWeaverRole
                  << "\n"
                  << "     weaver        weave #" << weaver_id.value << "   role " << kWeaverRole
                  << ", governs #" << session.id.value << ", ceiling: Work v1 -> role "
                  << kServiceRole << "\n"
                  << "     service       weave #" << service_id.value << "   role " << kServiceRole
                  << "\n"
                     "     the operator seat is a WeaveId this host designated. It is NOT an\n"
                     "     authenticated human, an account, or a login. There is none.\n";
    };

    const auto help = [] {
        std::cout <<
            R"(lenses (each command is authored by exactly one participant):
  session | operator            switch lens; the prompt always says who is speaking
  debug                         the HOST's own lens: it is not a participant

as the current participant:
  vocab                         the shapes this participant knows (and which are its doors)
  describe <Shape> <v>          the fields of one known shape
  send <addr> <Shape> <v> [args...]      author one message; expect no answer
  ask  <addr> <Shape> <v> [args...]      author one message; wait for LOOM's answer to it
  request <Shape> <v> @<role> [purpose]  ask the policy office for one exact send right
  authority                     ask the policy office what my authority is right now
  pending                       the conversations I am waiting on
  await [turns]                 pump the host loop until my asks settle (bounded)
  cancel <ask>                  stop waiting LOCALLY (the far end is not told)
  show r<N>                     one message I received, with its trusted provenance
  log [n]                       both transcripts, merged, each line labelled by lens

operator lens only (each reduces to an ordinary message from the operator seat):
  approve | refuse | revoke

debug lens only (the HOST looking - powers no participant has):
  weaves | tap [n] | notify <text>

  addr: #<id> one weave, @<role> whoever holds it at delivery, * publish
  args: bare value (positional/type-directed), field=value (named), $rN.field (reference)
        quote to force Text ("5" is Text, 5 is Int)
  who | help | quit
)";
    };

    std::cout << "zen terminal (TERM-0).\n"
                 "  This executable hosts its OWN Loom. It does not attach to another process:\n"
                 "  there is no socket, no login, and no remote anything here.\n\n";
    print_who();
    std::cout << "\n  try:  ask @some.service Query 1 \"are you there\"\n"
                 "        send @some.service Work 1 7          (no authority yet; see `debug` -> "
                 "tap)\n"
                 "        request Work 1 @some.service \"so I can finish the job\"\n"
                 "        operator   -> show r1  -> approve\n"
                 "        session    -> send @some.service Work 1 7\n\n"
                 "  'help' for everything.\n\n";

    enum class Lens { Session, Operator, Debug };
    Lens lens = Lens::Session;
    const auto lens_name = [](Lens l) {
        return l == Lens::Session ? "session" : (l == Lens::Operator ? "operator" : "debug");
    };
    const auto participant = [&desk](Lens l) -> loom::TerminalSession& {
        // EXPLICIT, ALWAYS. The lens names the author; nothing here ever substitutes the other
        // one, in either direction, for any reason.
        return l == Lens::Operator ? desk.operator_seat() : desk.acting();
    };

    // What this presentation has already shown, in observation order. Everything past it that
    // ARRIVED at either participant is echoed after each turn — inbound only, because the local
    // half of a command was already printed by the command that caused it.
    std::uint64_t last_shown = 0;

    std::string line;
    while (std::cout << lens_name(lens) << "> " && std::getline(std::cin, line)) {
        const std::vector<Token> tok = tokenize(line);
        if (tok.empty()) {
            continue;
        }
        const std::string& cmd = tok[0].text;
        if (cmd == "quit" || cmd == "exit") {
            break;
        }
        const std::size_t tap_before = tap.size();

        if (cmd == "session" || cmd == "operator" || cmd == "debug") {
            // Moving the hand is a PRESENTATION act, recorded as one in the transcript of the
            // participant being left, so a chronology shows when the author changed.
            const Lens next = cmd == "session" ? Lens::Session
                                               : (cmd == "operator" ? Lens::Operator : Lens::Debug);
            if (lens != Lens::Debug) {
                participant(lens).record_notice("presentation switched to the " + cmd + " lens");
            }
            lens = next;
        } else if (cmd == "help") {
            help();
        } else if (cmd == "who") {
            print_who();
        } else if (lens == Lens::Debug) {
            if (cmd == "weaves") {
                std::cout << "  [debug] the HOST reading its own registry — no participant can do "
                             "this:\n";
                for (const loom::WeaveId id : bus.list_weaves()) {
                    std::cout << "  [debug]   weave #" << id.value << "  accepts:";
                    for (const auto& s : bus.accepted_schemas(id)) {
                        std::cout << ' ' << s->name() << " v" << s->version();
                    }
                    std::cout << '\n';
                }
            } else if (cmd == "tap") {
                std::uint64_t n = 20;
                if (tok.size() == 2) {
                    (void)parse_u64(tok[1].text, n);
                }
                std::cout << "  [debug] the HOST's tap. Delivery outcomes here are facts NEITHER\n"
                             "  [debug] participant is told — never read them as something either\n"
                             "  [debug] transcript claims to know.\n";
                print_tap(static_cast<std::size_t>(n));
            } else if (cmd == "notify" && tok.size() >= 2) {
                // A root send: ungated, so it arrives whatever the grants say. It is how this demo
                // produces UNSOLICITED inbound traffic for the session.
                std::string text = tok[1].text;
                for (std::size_t i = 2; i < tok.size(); ++i) {
                    text += ' ' + tok[i].text;
                }
                loom::Value v(loom::schema_of<Notice>());
                v.set("text", loom::Cell::text(text));
                (void)bus.send(session.id, loom::Message(std::move(v)));
                std::cout << "  [debug] the HOST root-sent Notice to the session (nobody asked for "
                             "it)\n";
            } else {
                std::cout << "  [debug] unknown debug command: " << loom::safe_terminal_text(cmd)
                          << "  (weaves | tap [n] | notify <text>)\n";
            }
        } else {
            loom::TerminalSession& me = participant(lens);
            me.record_command(line);

            const auto lex_args = [&tok](std::size_t from) {
                std::vector<loom::Arg> args;
                for (std::size_t i = from; i < tok.size(); ++i) {
                    args.push_back(lex_arg(tok[i]));
                }
                return args;
            };

            if (cmd == "vocab") {
                for (const loom::VocabularyEntry& v : me.vocabulary().catalog()) {
                    std::cout << "  " << v.name << " v" << v.version
                              << (v.accepted ? "   [a door: may be delivered here]" : "") << '\n';
                }
                std::cout << "  (knowing a shape is not permission to send it, and not evidence "
                             "that anybody accepts it)\n";
            } else if (cmd == "describe") {
                std::uint64_t v = 0;
                const std::optional<loom::ShapeDesc> d =
                    (tok.size() == 3 && parse_u64(tok[2].text, v))
                        ? me.describe(tok[1].text, static_cast<std::uint32_t>(v))
                        : std::nullopt;
                if (!d) {
                    std::cout << "  this participant's vocabulary has no such shape (which is not "
                                 "the same as the world having none)\n";
                } else {
                    std::cout << "  " << d->name << " v" << d->version << '\n';
                    for (const loom::FieldDesc& f : d->fields) {
                        std::cout << "    " << f.name << " : " << f.type
                                  << (f.required ? " (required)" : " (optional)") << '\n';
                    }
                }
            } else if (cmd == "send" || cmd == "ask") {
                loom::Address to;
                std::uint64_t v = 0;
                if (tok.size() < 4 || !parse_address(tok[1].text, to) ||
                    !parse_u64(tok[3].text, v)) {
                    std::cout << "  usage: " << cmd << " <addr> <Shape> <version> [args...]\n";
                } else {
                    const std::vector<loom::Arg> args = lex_args(4);
                    print_result(cmd == "ask"
                                     ? me.ask(to, tok[2].text, static_cast<std::uint32_t>(v), args)
                                     : me.send(to, tok[2].text, static_cast<std::uint32_t>(v),
                                               args));
                }
            } else if (cmd == "request") {
                std::uint64_t v = 0;
                if (tok.size() < 4 || !parse_u64(tok[2].text, v) || tok[3].text.size() < 2 ||
                    tok[3].text[0] != '@') {
                    std::cout << "  usage: request <Shape> <version> @<role> [purpose]\n";
                } else {
                    std::string purpose;
                    for (std::size_t i = 4; i < tok.size(); ++i) {
                        purpose += (purpose.empty() ? "" : " ") + tok[i].text;
                    }
                    print_result(me.request_authority(tok[1].text, static_cast<std::int64_t>(v),
                                                      tok[3].text.substr(1), purpose));
                }
            } else if (cmd == "authority") {
                print_result(me.describe_authority());
            } else if (cmd == "approve" || cmd == "refuse" || cmd == "revoke") {
                // A CONVENIENCE, NEVER A PRIVILEGE. Each is one ordinary contentless message from
                // the operator seat's own door, asked so the Weaver's Ack or Refused comes back as
                // the authenticated answer to it. The generic ask path expresses exactly the same
                // thing — and from the session lens it is judged by Loom, not by this file.
                const std::string shape =
                    "zen." +
                    std::string(cmd == "approve"  ? "Approve"
                                : cmd == "refuse" ? "Refuse"
                                                  : "Revoke") +
                    "Authority";
                if (lens != Lens::Operator) {
                    std::cout << "  refused by this PRESENTATION: `" << cmd
                              << "` is an operator-seat convenience, and this presentation will\n"
                                 "  not quietly change who is speaking. Type `operator` to move "
                                 "the hand — or author\n  it yourself with `ask @"
                              << kWeaverRole << ' ' << shape
                              << " 1`, which Loom will judge on its own terms.\n";
                } else {
                    print_result(me.ask(loom::Address::to_role(kWeaverRole), shape, 1, {}));
                }
            } else if (cmd == "pending") {
                if (!me.awaiting()) {
                    std::cout << "  not waiting on anything\n";
                }
                for (const loom::PendingAsk& p : me.pending()) {
                    std::cout << "  ask " << p.id << "  " << p.shape << " v" << p.version << "  "
                              << (p.addressing == loom::Addressing::Role
                                      ? "-> role " + loom::safe_terminal_text(p.role)
                                      : "-> weave #" + std::to_string(p.target.value))
                              << "   (Loom correlation " << p.correlation << ")\n";
                }
                std::cout << "  'awaiting an answer' is all this means: not that the request was\n"
                             "  delivered, not that anyone is working on it, not that a person saw "
                             "it.\n";
            } else if (cmd == "await") {
                std::uint64_t turns = 8;
                if (tok.size() == 2) {
                    (void)parse_u64(tok[1].text, turns);
                }
                std::uint64_t spent = 0;
                // NO THREAD, NO SLEEP, NO BUSY LOOP: this drives the ordinary host loop the same
                // way every other command does, a bounded number of times, and stops the moment
                // the core says nothing is outstanding.
                while (me.awaiting() && spent < turns) {
                    bus.pump();
                    ++spent;
                }
                if (me.awaiting()) {
                    std::cout << "  still awaiting after " << spent
                              << " turn(s). Nothing else in this process is queued, so only a new\n"
                                 "  command can produce work. Whether the request was ever "
                                 "delivered is not\n  something this participant can know.\n";
                } else {
                    std::cout << "  settled after " << spent << " turn(s)\n";
                }
            } else if (cmd == "cancel") {
                std::uint64_t n = 0;
                if (tok.size() != 2 || !parse_u64(tok[1].text, n)) {
                    std::cout << "  usage: cancel <ask>\n";
                } else {
                    const loom::TerminalResult r = me.cancel_ask(n);
                    std::cout << (r ? "  stopped waiting locally. The request was NOT cancelled — "
                                      "nobody was told,\n  and its answer may still arrive.\n"
                                    : "  " + r.detail + "\n");
                }
            } else if (cmd == "show") {
                std::uint64_t n = 0;
                if (tok.size() != 2 || tok[1].text.size() < 2 || tok[1].text[0] != 'r' ||
                    !parse_u64(tok[1].text.substr(1), n)) {
                    std::cout << "  usage: show r<N>\n";
                } else if (const std::optional<loom::ReceivedMessage> m = me.received(n)) {
                    print_message(*m);
                } else {
                    std::cout << "  no such received message: "
                              << loom::safe_terminal_text(tok[1].text)
                              << (n != 0 && n <= me.transcript().received_evicted()
                                      ? "  (evicted — this participant retains the most recent few)"
                                      : "")
                              << '\n';
                }
            } else if (cmd == "log") {
                std::uint64_t n = 20;
                if (tok.size() == 2) {
                    (void)parse_u64(tok[1].text, n);
                }
                const std::vector<loom::DeskEntry> all = desk.chronology();
                const std::size_t want = static_cast<std::size_t>(n);
                const std::size_t from = all.size() > want ? all.size() - want : 0;
                for (std::size_t i = from; i < all.size(); ++i) {
                    std::cout << "  " << render_entry(all[i]) << '\n';
                }
                std::cout << "  (the order THIS PRESENTATION observed these things — not a global "
                             "Loom history)\n";
            } else {
                std::cout << "  unknown command: " << loom::safe_terminal_text(cmd)
                          << "  (try 'help')\n";
            }
        }

        // Drive the ordinary loop so whatever this command caused actually happens. Nothing else
        // in this process produces work, so the person typing is the clock — which is exactly why
        // a blocking getline is safe here and would not be in a process with its own traffic.
        bus.pump();

        if (lens == Lens::Debug && tap.size() > tap_before) {
            print_tap(tap.size() - tap_before);
        }
        // What ARRIVED at either participant this turn, in its own words. Deliberately not the
        // tap: to see what the host saw, ask the host.
        for (const loom::DeskEntry& d : desk.chronology()) {
            if (d.entry.seq > last_shown && (d.entry.kind == loom::TranscriptKind::Received ||
                                             d.entry.kind == loom::TranscriptKind::AnswerReceived)) {
                std::cout << "  " << render_entry(d) << '\n';
            }
        }
        last_shown = order->observed();
    }

    bus.remove_observer(tap_id);
    return 0;
}
