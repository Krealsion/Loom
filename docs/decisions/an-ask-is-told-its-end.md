# An ask is told its end

**Status: current since ABI v15.** Laws:
[ANS-08](../laws/answer-authority-laws.md#ans-08--an-ask-is-told-its-end),
[ANS-02](../laws/answer-authority-laws.md#ans-02--deferring-converts-it-never-adds),
[MSG-12](../laws/messaging-laws.md#msg-12--a-sender-may-learn-an-authenticated-refusal-of-its-exact-send).
Reference: [asks](../reference/messaging.md#asks-and-how-each-one-ends),
[bounds](../reference/bounds.md), [dynamic ABI](../reference/dynamic-abi.md#the-ask-door).

## The pressure

A question and a remark looked the same to Loom. Every delivered message carried the right to one
answer whether its sender wanted one or not, and the right was gone when the handler returned
unless the handler kept it. So a respondent that returned without answering, let a kept answer
go, was reloaded, removed, killed or retired, or answered with something Loom then refused, left
its asker waiting for ever: Loom never knew there was a question to tell anyone about. The one
notice a sender had, `zen.DispatchRefused`, covered a refusal before any handler and nothing
after it. And the Loom-wide bound on kept answers (64) let one weave holding them all stop every
other weave from keeping one, so Loom's own advice was to acknowledge at once and deliver the
result in ordinary later speech, which nothing follows.

## The decision

- **An ask is the asker's.** A weave says it waits on an answer by sending through the ask door
  (`mail.ask`, `ask_role`, and their `as_role` forms; one host door since ABI v15). Otherwise an
  ask is a send in every way, and the respondent sees nothing new.
- **Using the door is the opting in.** An ask from a weave that does not accept `zen.AskEnded` is
  refused at the call, queueing nothing, so no ask's end has nowhere to go.
- **Loom follows it with the records it already keeps** -- the envelope while queued, the answer
  right while the handler runs, the kept answer after -- each carrying the ask's facts, to its
  answer or to one `zen.AskEnded`: Loom's attestation, to the exact asking life and incarnation,
  granting nothing, once. Its shape is a union of four stages, each with its own reasons, since
  how far the ask got is what makes a retry safe.
- **Each ending is told where its last record goes**, and none where nobody is left to tell.
- **Owed long work is a kept answer**, under a bound per respondent (64) beneath a raised Loom-wide
  one (1024), so a weave that keeps too many exhausts itself and nobody else.

## Alternatives considered, and why rejected

1. **What an ask is.** (b) No door, and a notice only where a right was kept or an answer
   attempted: a respondent that returned in silence would still leave its asker waiting. (c) No
   door, and every delivered send of a weave that opted in told when unanswered: a notice for
   every remark. Reading a correlation or a payload field as "this is an ask" was a mistake under
   both: a correlation identifies and never means (ANS-05).
2. **Whether the asker opts in.** Optional, as `zen.DispatchRefused` is, rebuilds today's silent
   waiting behind a new door; a default handler that swallows the ending is the same mistake.
3. **One notice for an ask, or two.** `zen.DispatchRefused` for every pre-handler refusal and
   `zen.AskEnded` only after delivery would make an asker accept both and match two shapes to one
   book entry. Widening `zen.DispatchRefused` v1 in place, or sending both for one refused ask,
   were mistakes. One shape for asks; plain sends keep theirs (MSG-12).
4. **Where owed long work lives.** The Loom-wide bound as it was, with the advice left as it was,
   keeps the floor blind to exactly the work that most needs following. Raising the Loom-wide bound
   with no bound per weave reopens the hole Night Lab measured. A second kind of record for "work
   owed" beside the kept answer was a mistake.
5. **What stays outside.** A live respondent that never answers (Loom has no clock; a timeout is
   the asker's), work promised in ordinary words, the isolation pipe and a bridge session's far
   side. Said in ANS-08's DOES NOT MEAN, where the promise is.

## Consequences

- Every way an ask ends is a stage and a reason the asker can act on: `queued` (never reached a
  handler: safe to send again), `delivered`, `deferred`, `answered`.
- A respondent that answers in ordinary words now ends an ask `Unanswered` when its handler
  returns. So an asker asks only a respondent that answers through Loom's answer right. Loom's own
  control door and Weave Manager still answer in ordinary words, and the console's conversations
  with them stay plain sends (`ConsoleTracking::Tracked`); `Asked` is the ask.
- A retirement's end is not yet a place (it ends at an admission's switch, where a sealed weave's
  answer cannot leave), so an answer the retired incumbent owes ends `RespondentRetired` there.
- The bus's census holds: the kept answers and the ask's record are a unit the bus consults
  (`zen/switchboard/asks.hpp`), and a notice to the exact recipient is one door, `queue_notice`, for
  both notices.

## Evidence

Suites `ask` (each ending in process, the exact asker, the per-weave bound, the incumbent's speech
queued at the switch), `ask_loaded` and its `box` twins (each ending through the one door, and an
image built against v14), `isolation` (the pipe's refusal), `terminal` and `console` (ENDED).
