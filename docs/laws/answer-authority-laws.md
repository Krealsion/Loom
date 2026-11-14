# Answer-authority laws (ANS)

Reference: [messaging](../reference/messaging.md#answers).

## ANS-01 — Answer authority IS the delivery

LAW — One delivered request grants at most one answer, to the weave being
dispatched, while it is being dispatched. Answer provenance is a delivery fact
with no wire form.

MEANS
- `mail.answer()` outside a delivery, from the wrong weave, or a second time,
  refuses visibly;
- `answers_ask()` on the recipient's side is Loom's word, not a payload claim;
- provenance cannot be copied: every ordinary enqueue path overwrites it (see
  ANS-07).

DOES NOT MEAN
- that a *reply* (ordinary speech back at a sender) is an answer — an
  unsolicited reply is ordinary and carries no provenance;
- that answering is a right of the *address* — it is a right of the exact
  delivery.

PROVEN BY — `ReplyAuthority` (bus-owned, per-dispatch); suites `provenance`,
`switchboard`.

## ANS-02 — Deferring converts; it never adds

LAW — `defer_answer()` converts the immediate answer right into a retained one.
One delivered request still grants exactly one answer.

MEANS
- after deferring, `answer()` finds nothing left; a second `defer` finds
  nothing to convert;
- the retained right binds the exact respondent incarnation that earned it;
- capacity is bounded twice: per respondent (`kMaxDeferredPerWeave`) and per
  Loom (`kMaxDeferredAnswers`), the first well under the second, so a weave
  that keeps too many answers exhausts itself and nobody else; either overflow
  refuses visibly as `Exhausted` and the immediate right survives;
- owed work is held as a kept answer, which Loom can end out loud (ANS-08).

DOES NOT MEAN
- that one weave can take the whole Loom's capacity — Night Lab measured that
  hole when the only bound was the Loom's ([bounds](../reference/bounds.md));
- that a deferred right survives reload, death, or the requester changing.

PROVEN BY — `defer_answer_as` / `spend_deferred_as`, the kept answers'
`KeptAnswers` (`zen/switchboard/asks.hpp`); suites `switchboard`,
`provenance`, `kernel` (held-full capacity case), `ask` (one respondent at its
bound, another still keeping one).

## ANS-03 — An answer belongs to the life that asked

LAW — An answer is delivered only to the exact requester life *and* incarnation
captured when the request was delivered (`AnswerTargetChanged` otherwise).

MEANS
- a revived or reloaded occupant of the same id does not inherit a completed
  conversation;
- ordinary messages deliberately keep logical addressing — only answers carry
  the expectation.

DOES NOT MEAN
- that the *address* changed — the occupant did.

PROVEN BY — `AnswerTarget` on the envelope; suite `provenance`.

## ANS-04 — Death ends the conversation first

LAW — Killing or permanently removing a weave ends every unfinished
conversation it was party to, before `Died` is announced.

MEANS
- revival begins with no inherited answer rights, in either direction;
- an observer of a death sees the conversations already over;
- an asker whose ask the dead or removed respondent kept is told so
  (`RespondentDied`, `RespondentRemoved`; ANS-08), queued before `Died`.

DOES NOT MEAN
- that a live code reload ends conversations the same way — reload sweeps by
  *staleness* (incarnation), death by *life*, and the two questions are asked
  by different functions on purpose.

PROVEN BY — `KeptAnswers::end_life` at `kill`/`unregister_weave`; suites
`provenance`, `ask`.

## ANS-05 — Correlation identifies; it never authenticates

LAW — A correlation names a conversation for its participants. It is a number a
sender chooses (or Loom mints), and possessing it authorizes nothing.

MEANS
- matching correlations route bookkeeping; they prove nothing about who spoke
  or what was answered;
- everywhere authenticity matters, a bus-private envelope fact decides instead
  (answers: the reply authority; readiness: the ask's own envelope identity).

DOES NOT MEAN
- that correlations are secrets — they travel on the wire by design.

PROVEN BY — readiness ignores matching correlations without the envelope fact:
suite `kernel` (forged-readiness cases).

## ANS-06 — The answer means the same on both sides of the seam

LAW — A public delivery operation means the same thing for a dynamically loaded
weave as for a native one, or fails loudly. Dynamic `answer`/`defer_answer`
carry real success/failure across the C ABI (the doors are in ABI v4 and later;
the current version is `ZEN_ABI_VERSION`, see [dynamic ABI](../reference/dynamic-abi.md)).

MEANS
- a refused dynamic answer is *told* to the weave (`ZEN_ERR_REFUSED`), never
  silently dropped;
- out-of-process children get null answer doors and fail closed.

DOES NOT MEAN
- that a dynamic answer's success sentinel is a queued attempt. Since ABI v7,
  ordinary addressed sends return actual queued identities for dispatch-refusal
  matching; neither kind of return is proof of delivery or semantic success;
- that `kMaxPendingPerSender` refuses an answer. An answer spends a right one
  delivery created, so it is not that bound, and `ZEN_ERR_REFUSED` on these
  doors still means there was no answer to spend.

PROVEN BY — `abi.h`'s answer doors (`answer` / `defer_answer` /
`answer_deferred` / `release_deferred`); suite `kernel` (dynamic parity cases);
[Night Lab](../evidence/night-lab.md)'s `marathon/repro_answer_seam.cpp`.

## ANS-07 — Raw replay strips provenance

LAW — A stored, copied or re-sent `Message` is ordinary: every ordinary enqueue
path overwrites provenance with nothing.

MEANS
- copy-what-you-observed cannot manufacture an answer or an activation;
- only the Switchboard's attesting paths write a non-empty provenance.

DOES NOT MEAN
- that the payload bytes are protected — the *standing* is what cannot be
  copied.

PROVEN BY — every enqueue rebuilds provenance; suites `provenance`, `kernel`
(replayed-activation cases).

## ANS-08 — An ask is told its end

LAW — An ask sent through the ask door ends exactly once: its answer is
delivered, or Loom tells the exact life and incarnation that asked, with
`zen.AskEnded`, how far the ask got and why no answer will come. A weave that
cannot hear the notice cannot ask.

MEANS
- the ask door (`mail.ask`, `ask_role` and their `as_role` forms; one host door,
  `ask`, since ABI v15) refuses at the call, queueing nothing, a weave whose
  accept-set lacks `zen.AskEnded` (`AskEndedNotAccepted`); otherwise an ask is a
  send in every way;
- each ending is told where its last record goes: `queued` (refused before any
  handler: the six reasons MSG-12 names, and `AskerSealed`), `delivered` (the
  handler returned with the right neither spent nor kept, `Unanswered`, or did
  not finish, `HandlerFailed`), `deferred` (the kept right ended: `Released`,
  `RespondentReplaced`, `RespondentRemoved`, `RespondentDied`,
  `RespondentRetired`), `answered` (the answer was refused, `AnswerRefused`);
- the notice is Loom's attestation (`mail.ask_ended()`), names the ask's queued
  attempt, carries the ask's correlation, grants no answer right, and is checked
  against the exact asker when it is made and when it is delivered;
- `queued` is the one stage after which sending the ask again cannot repeat
  work; after the others the respondent may have been half done.

DOES NOT MEAN
- that a plain send is followed — `send` and `send_to_role` keep
  `zen.DispatchRefused` as their one notice (MSG-12), and an ask is told this
  notice and never that one;
- that work promised in ordinary words is followed — a respondent that answers
  in a later ordinary send ended the ask `Unanswered` when its handler returned;
  owed work is a kept answer (ANS-02);
- that a live respondent that has not answered has ended anything — Loom has no
  clock, and a timeout is the asker's or a package's;
- that an asker whose own life or code changed is told — the exact asker is
  gone, and its successor's book closes what it inherited
  (`AskBook::end_all`);
- that a notice which could not be made (no memory, no sequence) is retried —
  it is dropped, as `zen.DispatchRefused` is, and the ending stays on the tap;
- that the isolation pipe or a bridge session can ask — the pipe refuses a
  manifest accepting the notice ([known seams](../reference/known-seams.md)).

PROVEN BY — the ask door and `tell_ask_ended` (`Switchboard`), the kept answers
(`zen/switchboard/asks.hpp`); suites `ask` (each ending, in process), `ask_loaded`
(loaded, and an image built against ABI v14), `box` (its twins), `isolation`
(the pipe's refusal), `terminal` and `console` (the helpers' ENDED), `manager`
(the control door's and the Weave Manager's answers, and their endings).
