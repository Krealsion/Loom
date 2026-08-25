// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE ASKER'S OWN BOOK (FRIC-2) — what it means for one of MY conversations to be
// outstanding, and for one of them to be settled.
//
// TWO KINDS OF CASE, and the split is deliberate. The first half drives the book
// directly, because the book is ordinary state and a claim about ordinary state is
// cheapest to make where nothing else can interfere. The second half puts it inside a
// real weave on a real Switchboard, because the load-bearing half of the settlement
// rule is that THE BUS STAMPS THE SENDER — and a test that hands `from` in as a
// parameter has proved nothing about that. There the impostor is a genuine second
// participant, holding a grant this host wrote, speaking as itself.

#include <doctest.h>

#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using loom::AskBook;
using loom::AskOpened;
using loom::PendingAsk;
using loom::WeaveId;

namespace {

// Two respondents that are not each other, and one number that is not a conversation.
constexpr WeaveId kAlice{7};
constexpr WeaveId kBob{9};
constexpr WeaveId kNobody{};

/// TWO REPLY SHAPES, so nothing below can quietly become a claim about one of them.
/// The book never sees either: they exist to prove that a conversation is a
/// conversation whatever answers it.
struct Answer {
    std::string text;
    ZEN_SHAPE(Answer, 1, ZEN_FIELD(text));
};
struct Receipt {
    std::int64_t n;
    ZEN_SHAPE(Receipt, 1, ZEN_FIELD(n));
};
struct Question {
    std::string q;
    ZEN_SHAPE(Question, 1, ZEN_FIELD(q));
};
struct Nudge {
    ZEN_SHAPE(Nudge, 1);
};

struct AskerState {
    ZEN_EXPOSE();
    ZEN_SHAPE(AskerState, 1);
};

/// AN ORDINARY WEAVE THAT ASKS, and keeps its book. It interprets nothing: both reply
/// shapes are recorded the same way, because "which conversation did this settle?" is
/// the only question this weave asks of an arrival.
class Asker : public loom::WeaveBase<Asker, AskerState, loom::Accept<Answer, Receipt>,
                                     loom::Emit<Question>> {
public:
    AskBook book{4};
    std::vector<std::uint64_t> settled;   ///< the local ask ids, in the order they closed
    std::vector<std::string> shape_of;    ///< read OUT of the record while handling it
    int arrivals = 0;

    void on(const Answer&, loom::Mail& mail) { record(mail); }
    void on(const Receipt&, loom::Mail& mail) { record(mail); }

private:
    void record(const loom::Mail& mail) {
        ++arrivals;
        // THE ONE DOOR BOTH SHAPES PASS THROUGH. A wall applied to one reply shape and
        // not the other is not a wall.
        if (const std::optional<PendingAsk> mine = book.settle(mail.correlation(), mail.sender())) {
            settled.push_back(mine->id);
            shape_of.push_back(mine->shape); // the record is still readable, here
        }
    }
};

struct ResponderState {
    ZEN_EXPOSE();
    ZEN_SHAPE(ResponderState, 1);
};

/// A respondent that TAKES ITS ANSWER AWAY WITH IT, so an ask is genuinely outstanding
/// while everything else in a case happens. It answers on a later nudge, oldest first.
class Responder : public loom::WeaveBase<Responder, ResponderState,
                                         loom::Accept<Question, Nudge>, loom::Emit<Answer>> {
public:
    void on(const Question&, loom::Mail& mail) { held_.push_back(mail.defer_answer()); }
    void on(const Nudge&, loom::Mail& mail) {
        if (held_.empty()) {
            return;
        }
        loom::answer_deferred(held_.front(), mail, Answer{"here"});
        held_.erase(held_.begin());
    }
    std::size_t owed() const { return held_.size(); }

private:
    std::vector<loom::DeferredAnswer> held_;
};

struct StrayState {
    ZEN_EXPOSE();
    ZEN_SHAPE(StrayState, 1);
};

/// A PERFECTLY LEGITIMATE THIRD PARTICIPANT. It is not forging anything: the host
/// granted it the reply shapes, and it speaks as itself with a number it chose.
class Stray : public loom::WeaveBase<Stray, StrayState, loom::Accept<Nudge>,
                                     loom::Emit<Answer, Receipt>> {
public:
    WeaveId at{};
    std::uint64_t correlation = 0;

    void on(const Nudge&, loom::Mail& mail) { mail.send(at, Answer{"not yours"}, correlation); }
};

} // namespace

TEST_SUITE("ask_book") {

// =============================================================================
// 1. THE BOOK ITSELF -- opening, settling, and what neither one means
// =============================================================================

TEST_CASE("a fresh book is waiting on nothing, and says what it will hold") {
    AskBook book(3);
    CHECK_FALSE(book.awaiting());
    CHECK(book.outstanding() == 0);
    CHECK(book.capacity() == 3);
    CHECK_FALSE(book.full());
    CHECK(book.entries().empty());
    // A BOOK WITH NO ROOM IS A MISCONFIGURATION, not an empty book: it could never hold
    // a conversation, so it refuses at construction rather than at the first ask.
    CHECK_THROWS_AS(AskBook(0), std::invalid_argument);
}

TEST_CASE("A: an ask is opened, is outstanding, and is settled by its own answer") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice, "Query", 1);
    REQUIRE(a);
    CHECK(a.id == 1);
    CHECK(a.correlation == 1);
    CHECK(book.awaiting());
    CHECK(book.waiting_on(a.id));
    REQUIRE(book.entries().size() == 1);

    // WHAT IT REMEMBERS, and every field of it answers somebody's question.
    const PendingAsk& p = book.entries().front();
    CHECK(p.respondent == kAlice);
    CHECK(p.role.empty());
    CHECK_FALSE(p.to_role());
    CHECK(p.shape == "Query");
    CHECK(p.version == 1);

    // ASKED WHILE IT IS STILL OPEN, because settling closes it.
    CHECK(book.match(a.correlation, kAlice) != nullptr);
    CHECK(book.is_settled_by(a.id, a.correlation, kAlice));

    const std::optional<PendingAsk> closed = book.settle(a.correlation, kAlice);
    REQUIRE(closed);
    CHECK(closed->id == a.id);
    CHECK(closed->shape == "Query"); // the consumer reads the record it just closed
    CHECK_FALSE(book.awaiting());
    CHECK_FALSE(book.waiting_on(a.id));
}

TEST_CASE("MATCHING IS NOT SETTLING: looking does not destroy what the consumer needs") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice);
    const PendingAsk* seen = book.match(a.correlation, kAlice);
    REQUIRE(seen != nullptr);
    CHECK(seen->id == a.id);
    // ...and again, and again. Recognizing an arrival is a question, not an act.
    CHECK(book.match(a.correlation, kAlice) != nullptr);
    CHECK(book.outstanding() == 1);
    CHECK(book.settle(a.correlation, kAlice));
    CHECK(book.match(a.correlation, kAlice) == nullptr);
}

TEST_CASE("B: the right respondent, about the WRONG conversation, settles nothing") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice, "Query", 1);
    // The exact weave this asker is waiting on, granted the shape, answering the asker
    // it really was asked by -- and the only thing wrong is the number naming which
    // conversation it is about. That is the shape a STALE answer takes.
    CHECK(book.match(a.correlation + 1, kAlice) == nullptr);
    CHECK_FALSE(book.settle(a.correlation + 1, kAlice));
    // ...and the same defect read forwards: an answer to a conversation not yet opened.
    CHECK_FALSE(book.settle(a.correlation + 7, kAlice));
    CHECK(book.waiting_on(a.id));
    CHECK(book.outstanding() == 1);
}

TEST_CASE("C: the right conversation, from the WRONG respondent, settles nothing") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice, "Query", 1);
    CHECK(book.match(a.correlation, kBob) == nullptr);
    CHECK_FALSE(book.settle(a.correlation, kBob));
    CHECK_FALSE(book.is_settled_by(a.id, a.correlation, kBob));
    CHECK(book.waiting_on(a.id));
    // AND NOTHING WITH NO BUS-STAMPED AUTHOR SETTLES AN ASK. An arrival from outside
    // every participant is a real thing that can happen; it is not an answer.
    CHECK_FALSE(book.settle(a.correlation, kNobody));
    CHECK(book.waiting_on(a.id));
}

TEST_CASE("correlation 0 is the sentinel and never a conversation") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice);
    CHECK(a.correlation != 0);
    CHECK(book.settle(0, kAlice) == std::nullopt);
    CHECK(book.find(0) == nullptr);
    CHECK_FALSE(book.waiting_on(0));
    CHECK(book.waiting_on(a.id));
}

TEST_CASE("an ask to an OFFICE cannot name its respondent, and says so") {
    AskBook book(4);
    const AskOpened a = book.open_to_role("some.office", "Query", 1);
    REQUIRE(a);
    const PendingAsk& p = book.entries().front();
    CHECK(p.to_role());
    CHECK(p.role == "some.office");
    CHECK_FALSE(p.respondent.valid());
    // WHOEVER HELD THE OFFICE AT DELIVERY is not knowable here, so the record does not
    // pretend to constrain it -- the correlation is the whole wall, and a caller that
    // needs more has Loom's own answer provenance, which this type cannot supply.
    CHECK(book.settle(a.correlation, kBob));
    CHECK_FALSE(book.awaiting());
}

TEST_CASE("neither door accepts a respondent it cannot name") {
    AskBook book(4);
    // An invalid WeaveId is never quietly read as "anybody": the caller that means that
    // has a word for it, and it is a different word.
    CHECK_FALSE(book.open(kNobody, "Query", 1));
    CHECK_FALSE(book.open_to_role("", "Query", 1));
    CHECK(book.outstanding() == 0);
    // ...and a refused open consumed no number, so the first real one is still 1.
    const AskOpened a = book.open(kAlice);
    CHECK(a.correlation == 1);
    CHECK(a.id == 1);
}

// =============================================================================
// 2. SEVERAL AT ONCE -- the reason this is not a one-slot record
// =============================================================================

TEST_CASE("E: two conversations are outstanding at once and settle independently") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice, "Query", 1);
    const AskOpened b = book.open(kBob, "Other", 2);
    REQUIRE(a);
    REQUIRE(b);
    // TWO ASKS MAY LEGITIMATELY HAVE DIFFERENT RESPONDENTS, which is exactly what a
    // one-slot record cannot hold and what an expected-sender field on the OWNER
    // (rather than on the record) gets wrong.
    CHECK(a.id != b.id);
    CHECK(a.correlation != b.correlation);
    CHECK(book.outstanding() == 2);

    // ANSWER B FIRST. Nothing about arrival order says which is which.
    const std::optional<PendingAsk> closed_b = book.settle(b.correlation, kBob);
    REQUIRE(closed_b);
    CHECK(closed_b->id == b.id);
    CHECK(closed_b->shape == "Other");
    CHECK(book.waiting_on(a.id));
    CHECK_FALSE(book.waiting_on(b.id));
    CHECK(book.outstanding() == 1);

    const std::optional<PendingAsk> closed_a = book.settle(a.correlation, kAlice);
    REQUIRE(closed_a);
    CHECK(closed_a->id == a.id);
    CHECK_FALSE(book.awaiting());
}

TEST_CASE("D: a lucky guess is a guess -- B's number in A's respondent's mouth is nothing") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice, "Query", 1);
    const AskOpened b = book.open(kBob, "Other", 1);
    // EACH HALF OF THE WALL, CROSSED OVER. Alice is a real respondent of this book and
    // b.correlation is a real conversation of this book; the pair is not.
    CHECK_FALSE(book.settle(b.correlation, kAlice));
    CHECK_FALSE(book.settle(a.correlation, kBob));
    CHECK(book.outstanding() == 2);
    CHECK(book.waiting_on(a.id));
    CHECK(book.waiting_on(b.id));
}

TEST_CASE("F: a settled conversation is not settled a second time") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice, "Query", 1);
    const AskOpened b = book.open(kBob, "Other", 1);
    REQUIRE(book.settle(a.correlation, kAlice));
    // THE DUPLICATE IS INERT, and so is a late copy of it: the record is gone, so there
    // is nothing left for it to close. Nothing is refused and nothing throws -- it is
    // simply not this asker's business any more.
    CHECK_FALSE(book.settle(a.correlation, kAlice));
    CHECK(book.match(a.correlation, kAlice) == nullptr);
    // ...and it did not reach past its own conversation into the one still open.
    CHECK(book.waiting_on(b.id));
    CHECK(book.outstanding() == 1);
}

TEST_CASE("no live conversation reuses another live conversation's number") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice);
    const AskOpened b = book.open(kAlice);
    const AskOpened c = book.open(kAlice);
    CHECK(a.correlation != b.correlation);
    CHECK(b.correlation != c.correlation);
    CHECK(a.correlation != c.correlation);
    // A SETTLED NUMBER IS NOT RECYCLED EITHER, which is what keeps a late answer to a
    // closed conversation from settling a newer one.
    REQUIRE(book.settle(b.correlation, kAlice));
    const AskOpened d = book.open(kAlice);
    CHECK(d.correlation != b.correlation);
    CHECK_FALSE(book.settle(b.correlation, kAlice));
}

TEST_CASE("an ordinary send draws from the SAME sequence as the asks") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice, "Query", 1);
    const std::uint64_t plain = book.mint_correlation();
    // THE DEFECT A SECOND COUNTER WOULD MAKE POSSIBLE: a fire-and-forget send wearing a
    // number an open conversation is already using, whose answer would then settle it.
    CHECK(plain != a.correlation);
    CHECK_FALSE(book.settle(plain, kAlice));
    CHECK(book.waiting_on(a.id));
    // ...and it is not an ask: nothing was opened, and the local numbering is untouched.
    CHECK(book.outstanding() == 1);
    CHECK(book.open(kBob).id == a.id + 1);
}

// =============================================================================
// 3. CAPACITY AND FORGETTING -- the two ways a record leaves without an answer
// =============================================================================

TEST_CASE("G: at capacity the NEW ask is refused and the old ones are untouched") {
    AskBook book(2);
    const AskOpened a = book.open(kAlice, "first", 1);
    const AskOpened b = book.open(kBob, "second", 1);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(book.full());

    const AskOpened over = book.open(kAlice, "third", 1);
    CHECK_FALSE(over);
    CHECK(over.id == 0);
    CHECK(over.correlation == 0);
    // NOT THE SHED-OLDEST POLICY A RELAY USES, deliberately: a new ask must never be
    // able to displace a conversation somebody is waiting on.
    CHECK(book.outstanding() == 2);
    CHECK(book.waiting_on(a.id));
    CHECK(book.waiting_on(b.id));
    CHECK(book.entries().front().shape == "first");

    // ...AND THE REFUSAL CONSUMED NOTHING. A refused open that had burned a correlation
    // would leave a gap in the sequence a reader would have to explain.
    REQUIRE(book.settle(a.correlation, kAlice));
    const AskOpened now = book.open(kAlice, "third", 1);
    REQUIRE(now);
    CHECK(now.correlation == b.correlation + 1);
}

TEST_CASE("H: forgetting is local, and a late answer does not resurrect the record") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice, "Query", 1);
    const AskOpened b = book.open(kBob, "Other", 1);

    const std::optional<PendingAsk> dropped = book.forget(a.id);
    REQUIRE(dropped);
    CHECK(dropped->id == a.id);
    CHECK(dropped->shape == "Query"); // the owner can say WHAT it stopped waiting on
    CHECK_FALSE(book.waiting_on(a.id));
    CHECK(book.waiting_on(b.id));

    // NOTHING AT THE FAR END WAS TOLD ANYTHING -- there is no cancellation vocabulary in
    // Loom -- so the answer may still arrive. When it does it matches nothing, settles
    // nothing, and does not put the record back.
    CHECK(book.match(a.correlation, kAlice) == nullptr);
    CHECK_FALSE(book.settle(a.correlation, kAlice));
    CHECK_FALSE(book.waiting_on(a.id));
    CHECK(book.outstanding() == 1);

    // Forgetting the same one twice, or one that never existed, is not an error here --
    // it is the absence of a record, and the owner is told exactly that.
    CHECK_FALSE(book.forget(a.id));
    CHECK_FALSE(book.forget(0));
    CHECK_FALSE(book.forget(9999));
}

TEST_CASE("an ask may stay outstanding forever, and the book does not pretend otherwise") {
    AskBook book(4);
    const AskOpened a = book.open(kAlice, "Query", 1);
    // NOTHING HAPPENS TO IT. There is no clock here, no turn count, no expiry and no
    // notice: Loom has no word for "that answer became impossible", so neither does
    // this. The only thing that ends this conversation locally is the asker saying so.
    bool held_throughout = true;
    for (int i = 0; i < 1000; ++i) {
        held_throughout = held_throughout && book.waiting_on(a.id);
    }
    CHECK(held_throughout);
    CHECK(book.awaiting());
    REQUIRE(book.forget(a.id));
    CHECK_FALSE(book.awaiting());
}

// =============================================================================
// 4. ON A REAL BUS -- where the sender is a stamp rather than a parameter
// =============================================================================

TEST_CASE("the wall holds when the sender is Loom's stamp and the impostor is real") {
    loom::Switchboard bus;
    const WeaveId responder = loom::mount<Responder>(bus);

    loom::Grant asking;
    asking.allow_to_any(Question::zen_name, Question::zen_version);
    const WeaveId asker = loom::mount_granted<Asker>(bus, std::move(asking));

    // THE IMPOSTOR IS NOT FORGING ANYTHING: this host granted it both reply shapes, and
    // it speaks as itself. What it cannot do is claim another participant's identity.
    loom::Grant straying;
    straying.allow_to_any(Answer::zen_name, Answer::zen_version);
    straying.allow_to_any(Receipt::zen_name, Receipt::zen_version);
    const WeaveId stray = loom::mount_granted<Stray>(bus, std::move(straying));

    Asker& me = *static_cast<Asker*>(bus.weave(asker));
    Stray& other = *static_cast<Stray*>(bus.weave(stray));

    const AskOpened mine = me.book.open(responder, Question::zen_name, Question::zen_version);
    REQUIRE(mine);
    bus.send_as(asker, responder,
                loom::Message(loom::to_value(Question{"hello"}), asker, asker, mine.correlation));
    bus.drain_until_idle();
    REQUIRE(static_cast<Responder*>(bus.weave(responder))->owed() == 1); // genuinely outstanding

    // THE LUCKY GUESS. The number is not a secret: this book's first conversation is 1,
    // which anybody may write on anything.
    other.at = asker;
    other.correlation = mine.correlation;
    (void)bus.send(stray, loom::Message(loom::to_value(Nudge{}), WeaveId{}, WeaveId{}, 0));
    bus.drain_until_idle();
    CHECK(me.arrivals == 1);          // it really was delivered...
    CHECK(me.settled.empty());        // ...and it settled nothing
    CHECK(me.book.waiting_on(mine.id));

    // ...AND THE REAL ANSWER, from the weave that was actually asked.
    (void)bus.send(responder, loom::Message(loom::to_value(Nudge{}), WeaveId{}, WeaveId{}, 0));
    bus.drain_until_idle();
    CHECK(me.arrivals == 2);
    REQUIRE(me.settled.size() == 1);
    CHECK(me.settled.front() == mine.id);
    CHECK(me.shape_of.front() == Question::zen_name);
    CHECK_FALSE(me.book.awaiting());
}

TEST_CASE("two live conversations with DIFFERENT respondents settle on their own answers") {
    loom::Switchboard bus;
    const WeaveId first = loom::mount<Responder>(bus);
    const WeaveId second = loom::mount<Responder>(bus);
    loom::Grant asking;
    asking.allow_to_any(Question::zen_name, Question::zen_version);
    const WeaveId asker = loom::mount_granted<Asker>(bus, std::move(asking));
    Asker& me = *static_cast<Asker*>(bus.weave(asker));

    const AskOpened a = me.book.open(first, "to-first", 1);
    const AskOpened b = me.book.open(second, "to-second", 1);
    bus.send_as(asker, first,
                loom::Message(loom::to_value(Question{"a"}), asker, asker, a.correlation));
    bus.send_as(asker, second,
                loom::Message(loom::to_value(Question{"b"}), asker, asker, b.correlation));
    bus.drain_until_idle();
    CHECK(me.book.outstanding() == 2);

    // THE SECOND RESPONDENT ANSWERS FIRST, and only its own conversation closes.
    (void)bus.send(second, loom::Message(loom::to_value(Nudge{}), WeaveId{}, WeaveId{}, 0));
    bus.drain_until_idle();
    REQUIRE(me.settled.size() == 1);
    CHECK(me.settled.front() == b.id);
    CHECK(me.shape_of.front() == "to-second");
    CHECK(me.book.waiting_on(a.id));

    (void)bus.send(first, loom::Message(loom::to_value(Nudge{}), WeaveId{}, WeaveId{}, 0));
    bus.drain_until_idle();
    REQUIRE(me.settled.size() == 2);
    CHECK(me.settled.back() == a.id);
    CHECK_FALSE(me.book.awaiting());
}

TEST_CASE("the book does not know what an answer MEANS, and two shapes prove it") {
    loom::Switchboard bus;
    loom::Grant asking;
    asking.allow_to_any(Question::zen_name, Question::zen_version);
    const WeaveId asker = loom::mount_granted<Asker>(bus, std::move(asking));

    loom::Grant replying;
    replying.allow_to_any(Answer::zen_name, Answer::zen_version);
    replying.allow_to_any(Receipt::zen_name, Receipt::zen_version);
    const WeaveId teller = loom::mount_granted<Stray>(bus, std::move(replying));

    Asker& me = *static_cast<Asker*>(bus.weave(asker));
    const AskOpened a = me.book.open(teller, "shape-a", 1);
    const AskOpened b = me.book.open(teller, "shape-b", 1);

    // ONE CONVERSATION ANSWERED BY `Answer`, THE OTHER BY `Receipt`, from the same
    // respondent. The book has never heard of either type; it settles both.
    bus.send_as(teller, asker,
                loom::Message(loom::to_value(Answer{"prose"}), teller, teller, a.correlation));
    bus.send_as(teller, asker,
                loom::Message(loom::to_value(Receipt{42}), teller, teller, b.correlation));
    bus.drain_until_idle();

    REQUIRE(me.settled.size() == 2);
    CHECK(me.settled[0] == a.id);
    CHECK(me.settled[1] == b.id);
    CHECK(me.shape_of[0] == "shape-a");
    CHECK(me.shape_of[1] == "shape-b");
    CHECK_FALSE(me.book.awaiting());
}

TEST_CASE("M: a dispatch turn that delivers nothing settles nothing either way") {
    loom::Switchboard bus;
    const WeaveId responder = loom::mount<Responder>(bus);
    loom::Grant asking;
    asking.allow_to_any(Question::zen_name, Question::zen_version);
    const WeaveId asker = loom::mount_granted<Asker>(bus, std::move(asking));
    Asker& me = *static_cast<Asker*>(bus.weave(asker));

    const AskOpened mine = me.book.open(responder, "Query", 1);
    bus.send_as(asker, responder,
                loom::Message(loom::to_value(Question{"hi"}), asker, asker, mine.correlation));

    // THE RESPONDENT TOOK ITS ANSWER AWAY WITH IT (ANS-02), so the queue empties with
    // the answer genuinely OWED. "Nothing was delivered this turn" is a statement about
    // this instant's queue and about nothing else -- an asker that read it as "no answer
    // is coming" would be inventing a fact Loom never stated.
    bool empty_turn = false;
    for (int turn = 0; turn < 8 && me.book.awaiting(); ++turn) {
        if (bus.pump_pending() == 0) {
            empty_turn = true;
        }
    }
    REQUIRE(empty_turn);
    CHECK(bus.pending() == 0);
    CHECK(me.book.awaiting()); // unresolved, with nothing queued at all
    REQUIRE(static_cast<Responder*>(bus.weave(responder))->owed() == 1);

    // ...and the answer arrives afterwards anyway.
    (void)bus.send(responder, loom::Message(loom::to_value(Nudge{}), WeaveId{}, WeaveId{}, 0));
    for (int turn = 0; turn < 8 && me.book.awaiting(); ++turn) {
        bus.pump_pending();
    }
    REQUIRE(me.settled.size() == 1);
    CHECK(me.settled.front() == mine.id);
    CHECK_FALSE(me.book.awaiting());
}

} // TEST_SUITE
