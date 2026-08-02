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
- capacity is bounded (`kMaxDeferredAnswers = 64`) **per Loom, not per weave**
  — overflow refuses visibly as `Exhausted` and the immediate right survives.

DOES NOT MEAN
- that deferral capacity is a private per-weave resource — 64 outstanding
  conversations anywhere exhaust the 65th *everywhere* (measured in Night Lab's
  pull-inversion pricing);
- that a deferred right survives reload, death, or the requester changing.

PROVEN BY — `defer_answer_as` / `spend_deferred_as`; suites `switchboard`,
`provenance`, `kernel` (held-full capacity case).

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
- an observer of a death sees the conversations already over.

DOES NOT MEAN
- that a live code reload ends conversations the same way — reload sweeps by
  *staleness* (incarnation), death by *life*, and the two questions are asked
  by different functions on purpose.

PROVEN BY — `abandon_deferred_for` at `kill`/`unregister_weave`; suite
`provenance`.

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
carry real success/failure across the C ABI (v4).

MEANS
- a refused dynamic answer is *told* to the weave (`ZEN_ERR_REFUSED`), never
  silently dropped;
- out-of-process children get null answer doors and fail closed.

DOES NOT MEAN
- that an ordinary dynamic `send` returns a bus ticket — it structurally cannot
  (no seq crosses the seam); delivery fate is observed at the recipient, not
  the ticket.

PROVEN BY — `abi.h` v4 answer doors; suite `kernel` (dynamic parity cases);
Night Lab `repro_answer_seam.cpp`.

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
