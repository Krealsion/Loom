// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE STRANGER'S TERMINAL — a second presentation, built outside Loom's
// build tree, reaching the terminal core only through `find_package(loom)`.
//
// The Workshop-readiness claim is that a presentation which is not in this
// repository can own a terminal participant, drive it, and render its transcript
// without parsing console strings or acquiring host authority. This file is that
// claim, made falsifiable: it is what a Workshop pane would do, minus the pane.
//
// It links `loom::terminal` and nothing that is not exported. If the terminal
// core ever stops being reachable that way — a header left out of the install, a
// target dropped from the export set, a public type that needs an unexported
// one — this fails to configure or to compile, on the DEFAULT path, which is
// exactly where that mistake should surface.

#include <zen/host/terminal_wiring.hpp>
#include <zen/switchboard.hpp>
#include <zen/terminal/input_lex.hpp>
#include <zen/terminal/session.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

int failures = 0;

void ok(bool condition, const char* what) {
    std::printf("  %-5s %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

struct Nothing {
    std::int64_t n = 0;
    ZEN_SHAPE(Nothing, 1, ZEN_FIELD(n));
};
struct Question {
    std::string q;
    ZEN_SHAPE(Question, 1, ZEN_FIELD(q));
};
struct Answer {
    std::string a;
    ZEN_SHAPE(Answer, 1, ZEN_FIELD(a));
};

/// An ordinary service the stranger also wrote. It answers through Loom's own
/// answer authority, which is what gives the terminal a correlation to match.
class Oracle final
    : public loom::WeaveBase<Oracle, Nothing, loom::Accept<Question>, loom::Emit<Answer>> {
public:
    void on(const Question& q, loom::Mail& mail) {
        heard_from = mail.sender();
        (void)mail.answer(Answer{"you asked: " + q.q});
    }
    loom::WeaveId heard_from{};
};

} // namespace

int main() {
    std::printf("stranger terminal witness: a presentation outside Loom's tree\n");

    loom::Switchboard bus;

    // 1. A vocabulary the STRANGER chose, out of the stranger's own shapes.
    loom::TerminalVocabulary vocabulary;
    vocabulary.knows(loom::schema_of<Question>()).accepts(loom::schema_of<Answer>());

    // 2. Mount the participant, exactly as any host does.
    loom::Grant grant;
    grant.allow_to_role("Question", 1, "oracle");
    const loom::MountedTerminal session = loom::host_mount_terminal(
        bus, std::make_unique<loom::TerminalSession>("pane", std::move(vocabulary)),
        std::move(grant));
    ok(session.session != nullptr && session.id.valid(), "a stranger can mount a TerminalSession");

    auto oracle = std::make_unique<Oracle>();
    Oracle* oracle_raw = oracle.get();
    loom::Grant oracle_grant;
    oracle_grant.allow_to_any("Answer", 1);
    const loom::WeaveId oracle_id =
        bus.register_weave(std::move(oracle), std::move(oracle_grant), "oracle");
    oracle_raw->zen_set_self(oracle_id);

    // 3. THE COMMAND GRAMMAR IS LOOM'S, and a stranger gets it through the package (WT-1).
    //
    // A presentation that had to re-author `#N` / `@office` / `*` would be a second grammar
    // pretending to be the first, and this witness is the only lane that can tell whether the
    // shared one is actually REACHABLE from outside the build tree -- a header left out of the
    // install fails right here rather than in a downstream repository.
    loom::Address to;
    ok(loom::parse_address("@oracle", to) && to.mode == loom::Addressing::Role &&
           to.role == "oracle",
       "the installed package carries the shared address grammar");
    ok(!loom::parse_address("oracle", to), "...including its refusal to guess at a bareword");
    (void)loom::parse_address("@oracle", to);

    // 4. Drive it WITHOUT stdin: structured arguments, and an address the core's own parser
    // produced -- never a console string this file taught itself to read.
    std::vector<loom::Arg> args;
    args.push_back(loom::Arg{std::nullopt, loom::FieldValue{std::string("is anybody there")}});
    const loom::TerminalResult asked = session.session->ask(to, "Question", 1, args);
    ok(asked.outcome == loom::TerminalOutcome::Submitted, "a pane can ask without a terminal");
    ok(session.session->awaiting(), "...and the core, not the pane, owns the pending state");

    // 5. The pane owns the LOOP; the core never pumps.
    for (int turn = 0; turn < 4 && session.session->awaiting(); ++turn) {
        bus.drain_until_idle();
    }
    ok(!session.session->awaiting(), "the answer settled the ask");
    ok(oracle_raw->heard_from == session.id, "the service heard the PARTICIPANT, not the host");

    // 5b. THE RECORD BEHIND THAT `awaiting()` IS ITSELF PART OF THE PACKAGE (FRIC-2).
    //
    // A stranger writing an ordinary weave -- no terminal anywhere near it -- needs the same
    // two facts the session just used, and it should not have to rewrite them. This is the
    // only lane that can say whether `loom::AskBook` is REACHABLE through `find_package`
    // rather than merely present in the source tree.
    loom::AskBook book(2);
    const loom::AskOpened opened = book.open(oracle_id, "Question", 1);
    ok(static_cast<bool>(opened) && opened.correlation != 0,
       "a stranger can open a conversation with the installed asker record");
    ok(book.settle(opened.correlation, session.id) == std::nullopt,
       "...it is not settled by the right conversation from the wrong respondent");
    ok(book.settle(opened.correlation + 1, oracle_id) == std::nullopt,
       "...nor by the right respondent about a different conversation");
    const std::optional<loom::PendingAsk> closed = book.settle(opened.correlation, oracle_id);
    ok(closed.has_value() && closed->shape == "Question" && !book.awaiting(),
       "...and the pair closes it, handing back the record it closed");

    // Existing aggregate initialization keeps its field positions; the optional
    // attempt is appended, so an older source consumer has no migration burden.
    const loom::PendingAsk legacy{1, 2, oracle_id, {}, "Question", 1};
    ok(legacy.respondent == oracle_id && legacy.shape == "Question" && legacy.attempt == 0,
       "existing PendingAsk aggregate source remains usable without an attempt binding");

    // 6. Render the transcript from STRUCTURE, never from console strings.
    bool saw_answer = false;
    for (const loom::TranscriptEntry& e : session.session->transcript().entries()) {
        if (e.kind == loom::TranscriptKind::AnswerReceived) {
            saw_answer = e.answers == asked.ask && e.sender == oracle_id && e.answers_ask;
        }
    }
    ok(saw_answer, "the transcript carries the answer's ask, sender and provenance as data");

    const std::optional<loom::ReceivedMessage> got =
        session.session->received(session.session->transcript().last_received_id());
    ok(got.has_value() && got->value.get("a") != nullptr &&
           got->value.get("a")->as_text() == "you asked: is anybody there",
       "the exact Value is readable, verbatim, by id");

    // A denied ask is still a Submitted fact; only a later authenticated receipt
    // retires its local record. This consumer sees only installed public headers.
    const auto denied=session.session->ask(loom::Address::to_role("ungranted.office"),"Question",1,args);
    ok(static_cast<bool>(denied),"a stranger can author an ask without a delivery guarantee");
    bus.pump_pending();
    ok(session.session->waiting_on(denied.ask),"refusal notification is a later delivery");
    bus.pump_pending();
    ok(!session.session->waiting_on(denied.ask),"authenticated dispatch refusal retires the exact ask");
    bool saw_refusal=false;
    for(const auto& e : session.session->transcript().entries()) {
        if(e.dispatch_refusal) {
            saw_refusal=e.kind==loom::TranscriptKind::Received && e.shape=="zen.DispatchRefused" &&
                e.dispatch_refusal->send.reason=="CapabilityDenied" &&
                e.dispatch_refusal->retired_ask==denied.ask && !e.answers_ask;
        }
    }
    ok(saw_refusal,"installed transcript separates the authenticated refusal from ordinary speech");

    // 7. Two identities, and the refusal to merge them, reach a stranger too.
    const loom::MountedTerminal seat = loom::host_mount_terminal(
        bus, std::make_unique<loom::TerminalSession>("seat", loom::TerminalVocabulary{},
                                                     session.session->order()),
        loom::Grant::nothing());
    bool refused_self_pairing = false;
    try {
        loom::TerminalDesk bad(*session.session, *session.session);
        (void)bad;
    } catch (const std::invalid_argument&) {
        refused_self_pairing = true;
    }
    ok(refused_self_pairing, "a desk refuses to pair a participant with itself");
    const loom::TerminalDesk desk(*session.session, *seat.session);
    ok(!desk.chronology().empty(), "a merged, lens-labelled chronology is available by value");

    std::printf("stranger terminal witness: %s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
