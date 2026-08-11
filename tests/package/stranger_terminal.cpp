// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE STRANGER'S TERMINAL (TERM-0) — a second presentation, built outside Loom's
// build tree, reaching the terminal core only through `find_package(loom)`.
//
// TERM-0's Workshop-readiness claim is that a presentation which is not in this
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
#include <zen/terminal/session.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
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

    // 3. Drive it WITHOUT stdin: structured commands, not parsed text.
    std::vector<loom::Arg> args;
    args.push_back(loom::Arg{std::nullopt, loom::FieldValue{std::string("is anybody there")}});
    const loom::TerminalResult asked =
        session.session->ask(loom::Address::to_role("oracle"), "Question", 1, args);
    ok(asked.outcome == loom::TerminalOutcome::Submitted, "a pane can ask without a terminal");
    ok(session.session->awaiting(), "...and the core, not the pane, owns the pending state");

    // 4. The pane owns the LOOP; the core never pumps.
    for (int turn = 0; turn < 4 && session.session->awaiting(); ++turn) {
        bus.pump();
    }
    ok(!session.session->awaiting(), "the answer settled the ask");
    ok(oracle_raw->heard_from == session.id, "the service heard the PARTICIPANT, not the host");

    // 5. Render the transcript from STRUCTURE, never from console strings.
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

    // 6. Two identities, and the refusal to merge them, reach a stranger too.
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
