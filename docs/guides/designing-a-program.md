# Designing a program

[Writing a weave](writing-a-weave.md) makes one participant. A program is several, and this page
is about dividing one into weaves: what the host keeps, how far to split, who owns each fact, who
acts, and how to name, time and test the parts. Read it after [the mental model](mental-model.md).
Each section links the law or page that owns its rule, and [best
practices](../contributing/best-practices.md#designing-a-program-on-loom) shows how to apply them.

## A program is weaves, and the host only starts them

The host is the program that holds the `Switchboard`. It keeps the roots: which weaves run, the
grant each is given, and the one wait the program rests in between turns of its bus
(`loom::host::Reactor`, then `pump_pending()`). Everything the program does is in its weaves, and
they reach one another only by messages, calls and Senses.

Code that grows in the host where a weave could hold it is the mistake to refuse. A weave can be
replaced, tested on its own and run by another program; code in the host can be none of these.
Holding a `Mail` is being a weave and holding the `Switchboard` is being the host, and the two
surfaces are the trust boundary ([capabilities](../reference/capabilities.md#the-grant-in-process)).
The supplied host, `loom-host`, is a host of this kind: it starts the weaves its plan names and
decides what each may do ([running Loom](running-loom.md)).

## Split until a piece is too small to be useful alone

Divide the program into weaves, and divide each again, until a piece is too small to be useful
alone. A piece that small is not a weave of its own. When it keeps nothing and answers with what it
is handed, it is a function: a provided function a weave offers (`Provides<In>`), which another
weave calls inside its own turn (`mail.call<Out>(role, In{...})`), handed its argument and nothing
else ([a call](writing-a-weave.md#call-a-function-another-weave-provides),
[MSG-14](../laws/messaging-laws.md#msg-14--a-call-is-a-function-of-its-argument-alone)).

There is no fixed test for when a piece is a weave of its own: it is judged by what each part is
for, and [identifying a provided
function](../contributing/best-practices.md#identifying-a-provided-function) shows how, on a real
program.

## Five kinds of unit

Each part of a program's source is one of five kinds, and each may reach only what its kind needs,
so that a part can be built, tested and replaced apart from the rest:

- a **weave**, which holds state or answers others, and speaks only through its `Mail`;
- a **host**, which starts the weaves and holds the `Switchboard`, the kernel and `loom::host`;
- a **library**, a function of its arguments that holds no state and no authority;
- a **vocabulary**, the shapes two parties speak and nothing else, kept with the contract it
  belongs to;
- a **test**, which runs what it tests as the program does.

Zengine holds its whole tree to these kinds, and each kind to what it may include, with a census
([five kinds](https://github.com/Krealsion/Zengine/blob/main/docs/architecture/README.md#every-unit-is-one-of-five-kinds)).

## One fact, one owner

Each fact the program holds has one owner, a weave, and every other part asks that owner or reads
its claim rather than keeping a copy it must keep in step. Save what was authored, what a person
or a weave chose, as the owner's declared state, which the bus snapshots and revives
([lifecycle](../reference/lifecycle.md)). Derive the rest from its live owner when it is needed. A
Sense is the latest observation its owner chose to claim, never current state
([SENSE-01](../laws/sense-laws.md#sense-01--a-claim-is-authored-observation-not-shared-state)),
and a claim Loom derives is made at each read from the bus's live tables, so it is never behind
them ([SENSE-08](../laws/sense-laws.md#sense-08--a-derived-claim-is-made-at-each-read-and-is-never-behind-what-it-is-made-of)).

## Who acts

The actor remains the actor. The bus stamps every message with its sender
([MSG-02](../laws/messaging-laws.md#msg-02--the-bus-stamps-the-sender)), a weave speaks as a role
only by choosing to ([MSG-07](../laws/messaging-laws.md#msg-07--role-authorship-is-explicit)), and
an answer's standing is a fact of its delivery, never of its payload
([ANS-01](../laws/answer-authority-laws.md#ans-01--answer-authority-is-the-delivery)). Nothing
lets one participant act as another: a terminal session's command that its authority does not
cover is refused, never retried as someone with more
([terminal](../reference/terminal.md#what-it-is-and-what-it-is-not)), and the Weaver changes a
session's authority and performs nothing itself ([the Weaver](../reference/weaver.md#the-flow)).

Authority is what a grant gives, and the host gives it. A weave gains none from who wrote it, from
being loaded, or because a file named it
([where a grant comes from](../reference/capabilities.md#where-a-grant-comes-from-the-powerbox)).

## Names and addresses

- **Address by role what must outlive its holder.** A send to a role reaches whoever holds it at
  delivery, so a service can be replaced while its callers keep working
  ([MSG-04](../laws/messaging-laws.md#msg-04--role-addressing-is-destination-not-authorship),
  [replacing a service](replacing-a-service.md)).
- **Name a thing by its limits.** `drain_until_idle()` does not return while work keeps arriving,
  and its name is where that is said ([the drain](../terminology.md)).
- **Make the wrong path the hard one.** A handler holds a `Mail`, which cannot do the host's work,
  and bytes that have not met the gate have no accessors
  ([GATE-02](../laws/admission-laws.md#gate-02--untrusted-until-proven)). A part that cannot
  express a mistake does not need a rule against it.

## Time

A handler acts on the turn's time and reads no clock of its own. Each delivery carries the time
the host's wait read when it began the turn, `mail.turn_ms()`, for durations and deadlines, and the
date beside it, `mail.turn_date_ms()`, for a person and a record; nothing is scheduled on the date
([the turn's time](../decisions/the-turns-time-on-every-delivery.md)). A time a sender writes into
a shape is that sender's claim about the time, which its reader cannot check; the turn's time is
the bus's, as the sender stamp is.

## Testing it

Test a weave as the program runs it: mounted or loaded as the host does, and spoken to through its
own doors by messages and calls. A suite moves time itself, with a host loop on a `ManualClock` or
the facts it hands the bus at the start of a turn (`Switchboard::begin_turn`), so a case about a
deadline runs in no time and the same way every run ([the turn's
time](../reference/messaging.md#the-turns-time-on-a-delivery)).

## When Loom cannot say what you need

Open an issue rather than working around Loom in your host. A workaround in one program's host is a
second mechanism no other program shares, and a problem inside Loom's architecture is the one worth
solving there ([taking an issue](../contributing/taking-an-issue.md)).
