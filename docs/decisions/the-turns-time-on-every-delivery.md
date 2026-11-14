# The turn's time on every delivery

**Status: current since ABI v16.** Reference:
[the turn's time](../reference/dynamic-abi.md#the-turns-time),
[messaging](../reference/messaging.md#the-turns-time-on-a-delivery).

## Context

A host that rests in one wait (`loom::host::Reactor`) reads its clock when the wait ends, and a
source the wait woke is told that reading in its `loom.host.Waited`. A handler woken by anything
else -- a message, an answer, a weave's own work -- had no time to act on but a clock it read
itself, which a suite cannot move, a replay cannot repeat and a box must not be given. Several
participants claimed a wait only to learn the time, or took an extra wake to learn it, and Loom's
own host code read the calendar beside its clock.

## Decision

Every delivery carries the turn's time and date as delivery facts, beside the sender and the
correlation: the host's steady clock and the calendar, read together when the wait ended and
began the turn the delivery is dispatched in. The wait hands them to the bus
(`Switchboard::begin_turn`); each dispatch call takes them for its deliveries and lets them go when
it returns. A handler reads `mail.turn_ms()` and `mail.turn_date_ms()`, or ignores them. They are
empty in a turn no wait began, and for a boxed or contained weave whose grant gives it no time
(`Grant::with_time`). The time is for durations and deadlines; the date is for a person and a
record, and nothing is scheduled on it. History records both with each delivery.

## Alternatives considered

1. **A field a shape declares.** A sender writes every field of a shape, and Loom writes none: a
   time in a shape is the sender's claim about the time, which a recipient cannot trust and a
   wire would carry. A delivery fact is the bus's, as the sender stamp is.
2. **A clock read in a handler.** It is the rule the deadlines rounds made to keep: a suite moves
   its clock, so a handler that reads the machine's escapes it, two handlers in one turn read two
   times, and a box cannot be given a clock without being given a timing channel.
3. **The time only to the sources the wait woke.** It is what `Waited` already does, and it is why
   a participant claimed a wait to learn the time: the time a turn began is a fact of the turn,
   not of the source that ended the wait.
4. **The time at enqueue.** A message queued at one turn and dispatched at another would carry a
   time its handler did not run in.

## Consequences

- A handler acts on the time its turn was handed, the same for every delivery of that turn.
- A suite that turns the bus by hand hands none; one that runs its host's real loop on a
  `ManualClock` sets both, the date moving with the readings.
- An image built before ABI v16 loads and works, handed none; a box and a contained child are
  handed them only where their grant gives time.
- The bus keeps the facts in a unit it consults (`zen/switchboard/turn.hpp`), not in more of
  itself.

## Laws supported

MSG-01 (one dispatch thread: the facts are read on it, once per turn), GATE-01 (no sender writes
a delivery fact).

## Evidence

Suites `turn` (the bus, the wait, a sender's attempt, history and the log), `turn_loaded` (a
loaded image, an image built against v15's header), `box` (a box with and without the grant),
`isolation` (a contained child with and without it).
