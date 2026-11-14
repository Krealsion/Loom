# A call is a function of its argument alone

**Status: current since ABI v17.** Reference:
[calls](../reference/messaging.md#calls-a-function-of-its-argument-alone),
[the call door and the provide slot](../reference/dynamic-abi.md#the-call-door-and-the-provide-slot),
[a call rule](../reference/capabilities.md#a-call-rule).

## Context

A weave sometimes needs an answer inside the handler it is running, from code that is not its
own: what an operator in force makes of two values, what an element does with one press, which
source a recipe names. A message cannot give it, since the answer would come in a later turn, and
a Sense reads a value somebody already claimed: it answers a question that has no argument. An
application that needed one built its own C table beside Loom's, one per seam, each with its own
version, symbol and statuses, and opened the image a second time to reach it.

## Decision

A weave may call a function another weave provides, inside its own turn, and the function is a
function of its argument alone. A provider lists what it provides (`Provides<In>`, beside a static
`Out provide(const In&)`), and a library's manifest carries the list from version 7, so a host
knows before the image runs. A caller names the role and the argument
(`mail.call<Out>(role, In{...})`), resolved to the role's holder at that moment, as a send to the
role is. The function is handed the argument the gate admitted at its input shape, and nothing
else: no instance, no bus, no claim. It runs no handler and delivers nothing. Its answer is
admitted at the output shape the caller asked for. A call needs a call rule
(`Grant::allow_call(role, shape, version)`), absent by default; no send or observe rule is read
for one. Every way a call ends without an answer is its own word (`loom::CallRefusal`), and the
tap tells each call and how it ended inside the caller's delivery, before that delivery's own
event. Across the seam the host table gains one door, `call`, and the descriptor one slot,
`provide`, which takes no instance; a box runs a provider's function in a store of its own with
every door refusing; the isolation pipe and a link carry none, each refused by name.

"Pure" is said in its true words: in a box it is imposed, since the function runs in a store made
for the call and dropped after it, where it can reach no weave and keep nothing; in native code it
is asked, since a function in the host's process shares its memory and a grant bounds what code
says, never what it touches. Every host door refuses while a loaded image's function runs, so a
table it kept from an earlier delivery reaches nothing.

## Alternatives considered

1. **A call into the provider's live instance.** Simplest to write, and a nested delivery: the
   callee could send, change its state and call back, and the caller would be running inside
   somebody else's turn, which MSG-01 forbids. Refused.
2. **The provider's own claims handed in beside the argument.** It lets a function read its
   provider's state without the caller passing it, at the price of a function whose result depends
   on facts the caller did not name and a reading the provider's grant must authorize for
   somebody else. A caller that needs a provider's state reads its claim, under its own observe
   rule, and passes it in. Widening a function's inputs later is easy, and narrowing them is not;
   so the argument alone.
3. **A door, or a table, handed at creation.** `create` runs before the image has an identity or a
   grant; a door open there would be used by code nobody admitted, against a grant that does not
   exist yet. A weave's first need is met in its first turn, whose table already has the door.
4. **No call at all.** Loom stays smaller, and every application that needs an answer in its turn
   keeps its own C table and its second open of the image. One seam is better than many.

## Consequences

- A provider says its state as a claim before a caller can use it; a function computes from what
  it is handed.
- One input shape names one function of a provider; a second is refused at registration and at
  load.
- A provider replaced between two calls answers the second with its successor's code; nothing is
  held between calls.
- A native function's time is not bounded, as a native handler's is not; a boxed one's is its
  entry's steps.
- The bus keeps the subject in a unit it consults (`zen/switchboard/calls.hpp`), and its own file
  and class do not grow for it.
- History keeps no record of a call: the tap tells it, and the caller's delivery is recorded as it
  ended.

## Laws supported

MSG-14 (this), MSG-01 (no delivery nests), GATE-01 (the argument and the answer each meet the gate),
GATE-05 (the call rule is read at each call, delegable as the other live rules are).

## Evidence

Suites `call` (the answer, the rule, each refusal, the tap, a successor, delegation, the wall),
`call_loaded` (loaded as caller and provider, a reload between two calls, the forge, an image built
against v16's header), `box` (a boxed caller and provider, a provider unable to change its weave),
`isolation` (a provider refused at mount, a call refused both ways), `bridge` (a link's role).
