# From nothing to a running weave

Install Loom, start it, build something of your own, and run it under authority you
chose. No Zengine, no Workshop, no Builder, and no C++ host you have to write first.

You need a compiler and CMake — see [the tools you need](tools.md) — and nothing else.

Every command below was run to write this page. Paths and prompts are what they actually
print.

---

## 1. Build and install Loom

```sh
git clone https://github.com/Krealsion/Loom.git
cd Loom
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DZEN_BUILD_TESTS=OFF -DZEN_BUILD_EXAMPLES=OFF \
      -DCMAKE_INSTALL_PREFIX=$HOME/loom
cmake --build build -j
cmake --install build
```

On **Windows**, add `-DLOOM_ENABLE_WINDOWS_KERNEL=ON` if you want to host loadable
weaves. That backend applies **no sandbox** — it is a development and demo backend, which
is why it is an explicit choice rather than a default. Without it everything below still
works except loading weaves, and `loom-host` says so in one sentence.

What you now have:

```text
$HOME/loom/bin/loom-host      the supplied host — this is the thing you run
$HOME/loom/lib/               the libraries a weave of yours links
$HOME/loom/lib/cmake/loom/    what find_package(loom) reads
$HOME/loom/include/zen/       the headers
```

## 2. Start it

```sh
mkdir ~/work && cd ~/work
~/loom/bin/loom-host
```

```text
loom-host 0.1.0   containment: in-process; trusted; no OS sandbox (out-of-process isolation is the isolation host's job)
  boot plan: nothing requested
type 'help' for commands, 'quit' to shut this host down.
loom>
```

An empty host, and it says so. It found no boot plan and no standing decisions, which is
where everyone starts. `help` lists the commands; `quit` shuts the host down.

Two files live in whatever directory you run it from, and **both are yours**:

| file | what it is |
|---|---|
| `loom-boot.json` | what to start, in the order you wrote |
| `loom-authority.json` | what may run, and what it may say |

Neither is required to start, and the host writes the second one for you as you make
decisions. `--boot` and `--authority` point at other paths; `--check` reads both, says
what they contain, and exits without starting anything.

**One host owns one decision file while it runs.** A second host that names the same file
is refused, by name, and says what to do about it:

```text
loom-host: another loom-host already owns the decision store 'loom-authority.json'
loom-host: decisions are written whole, so a second host sharing this file would put back
           what the first one revoked. Either quit the other host, or give this one its
           own file:
             loom-host --authority <other-file>
```

That is not tidiness. Every decision replaces the **whole** file from the writer's own
copy, so two hosts do not merely lose each other's approvals: the second one to write puts
back what the first one **revoked**. One person with two terminals is enough. The claim is
an OS-level lock on `<file>.lock`, so a host that is killed leaves nothing to clean up and
the next host starts normally.

The host's exit codes, because a script will want them:

| | |
|---|---|
| `0` | it ran and you quit it |
| `2` | the command line was wrong |
| `3` | a file would not parse |
| `4` | another host owns the decision store (or, with `--serve`, already serves that directory) |
| `5` | `--serve` could not open its session: the directory, its files, its listener or randomness |

## 3. Write a weave of your own

A new directory, three files, nothing from Loom's source tree.

`tally.hpp` — the shapes, shared so the two libraries agree by construction:

```cpp
#pragma once
#include <zen/weave.hpp>
#include <cstdint>
#include <string>

struct Bump {                       // ask the counter to count one more
    std::string what;
    ZEN_SHAPE(Bump, 1, ZEN_FIELD(what));
};

struct Noted {                      // what the counter tells the logbook
    std::string what;
    std::int64_t total;
    ZEN_SHAPE(Noted, 1, ZEN_FIELD(what), ZEN_FIELD(total));
};

inline constexpr const char* kLogbookRole = "logbook";
```

`counter.cpp`:

```cpp
#include "tally.hpp"
#include <zen/kernel/export.hpp>
#include <zen/weave/standard_shapes.hpp>
#include <string>

struct CounterState {
    std::int64_t total;
    ZEN_SHAPE(CounterState, 1, ZEN_FIELD(total));
};

class Counter : public loom::WeaveBase<Counter, CounterState, loom::Accept<Bump>,
                                       loom::Emit<loom::Result, Noted>> {
public:
    void on(const Bump& b, loom::Mail& mail) {
        ++state_.total;
        mail.answer(loom::Result{std::to_string(state_.total)});   // authorized by the ask
        mail.send_to_role(kLogbookRole, Noted{b.what, state_.total}); // needs authority
    }
};

ZEN_EXPORT_WEAVE(Counter)
```

`logbook.cpp`:

```cpp
#include "tally.hpp"
#include <zen/kernel/export.hpp>

struct LogbookState {
    std::int64_t entries;
    ZEN_SHAPE(LogbookState, 1, ZEN_FIELD(entries));
};

class Logbook : public loom::WeaveBase<Logbook, LogbookState, loom::Accept<Noted>, loom::Emit<>> {
public:
    void on(const Noted&, loom::Mail&) { ++state_.entries; }
};

ZEN_EXPORT_WEAVE(Logbook)
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(tally LANGUAGES CXX)

find_package(loom REQUIRED)

foreach(w counter logbook)
    add_library(${w} SHARED ${w}.cpp)
    target_link_libraries(${w} PRIVATE loom::core loom::switchboard)
    loom_weave_build_contract(${w})   # the one line that is not yours to skip
endforeach()
```

Build it:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/loom -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`CMAKE_PREFIX_PATH` is the whole connection between your project and the Loom you
installed. If you get `Could not find a package configuration file provided by "loom"`,
that path is wrong — it is the **prefix**, not the `lib/cmake/loom` directory inside it.

Two things about the code, because they are the two a first weave gets wrong:

- **`mail.answer` is authorized by the ask; `mail.send_to_role` needs a rule.** The ask that
  arrived is what lets a weave answer, and to whom. Like every delivery, the answer is still
  checked against its author's grant for its shape: an admitted artifact's baseline covers the
  standard replies (`zen.Result`, `zen.Ack`, `zen.Refused`), one more reason the counter
  answers in one of them. An unsolicited send is speech, and speech is granted.
- **The counter answers with `zen.Result`, not a shape of its own.** A shape reaches a
  generic operator console only if the registry can resolve it, and the registry learns
  shapes from weaves' *accept*-sets. A custom shape that only ever travels *to* the
  console has nobody to declare it, and the send is refused `SeamUnresolved` before it
  reaches any door. The standard reply shapes are always resolvable. (See
  [known seams](../reference/known-seams.md).)

## 4. Say what to start

`~/work/loom-boot.json`:

```json
{
  "boot": [
    { "name": "logbook", "path": "/home/you/tally/build/liblogbook.so", "role": "logbook" },
    { "name": "counter", "path": "/home/you/tally/build/libcounter.so" }
  ]
}
```

A list, not a graph: the order is yours to state, and a host that inferred it would be
making a decision you cannot see. Each row takes `name`, `path`, an optional `role`,
an optional `"enabled": false`, and an optional `"on_failure": "stop"` — which leaves
every later row *pending* and says so, instead of continuing past a row you needed.

Run it again:

```text
loom-host 0.1.0   containment: in-process; trusted; no OS sandbox (…)
  refused    logbook @logbook
              admission refused at open: no standing decision for 'logbook' (build da352f71, …); approve it at the console with:  authority trust logbook
  refused    counter
              admission refused at open: no standing decision for 'counter' (build 521b4d17, …); approve it at the console with:  authority trust counter
  0 started, 2 refused by policy  -- boot INCOMPLETE
  the boot did not complete. The console is up: 'status' for the table, 'authority pending' for what is waiting on you.
```

**Nothing ran.** Being named in a file you wrote is not permission to execute native code
in your process — that decision is separate, and you have not made it yet. The refusal
names the build it was asked about and the command that answers it.

## 5. Decide what may run

```text
loom> authority trust logbook
  'logbook' may run; a rebuild of it will ask again. It may still say nothing until you allow something.
loom> authority trust counter
  'counter' may run; a rebuild of it will ask again. It may still say nothing until you allow something.
loom> quit
```

That is written to `loom-authority.json` immediately. Start again and the boot completes:

```text
  started    logbook @logbook  (weave 5)
  started    counter  (weave 6)
  2 started  -- boot COMPLETE
```

Those two words are the whole ceremony, once per artifact. Everything after this is free.

## 6. Talk to it

```text
loom> weaves
  weave 6  accepts: Bump v1 zen.PokeDescribe v1 …
loom> send 6 Bump 1 what="first"
  sent.  reply -> m3
loom> show m3
  m3 : zen.Result v1  1
```

Now look at what else happened:

```text
loom> tap 4
  Delivered Bump  1 -> 6
  Delivered zen.Result  6 -> 1
  Refused Noted  6 -> 0  [CapabilityDenied]
```

The counter answered you, and its note to the logbook was **refused**. It may run; it may
not yet speak. Loom told the counter so as well — a refused send comes back to its sender
as a later `zen.DispatchRefused` naming the exact attempt, so a weave can notice and
react rather than believing a message arrived.

## 7. Let it speak, and take it back

```text
loom> authority allow counter Noted v1 -> role logbook
  remembered. weave 6 may now say: Noted v1 -> role logbook
loom> send 6 Bump 1 what="second"
  sent.  reply -> m4
loom> tap 3
  Delivered Bump  1 -> 6
  Delivered zen.Result  6 -> 1
  Delivered Noted  6 -> 5
```

The counter's own send, retried by the counter. **Nothing was sent on its behalf**: your
approval changed what it may say, and it said it.

Rules are written exactly as `authority show` prints them — that is one spelling, not
two:

```text
Greet v1 -> any target        …to anyone who accepts it
Tick v1 -> role clock         …to whoever holds that office, now and after a swap
any shape -> any target       broad authority, and it took writing it out
observe Tick v1               may READ a published claim (a different permission)
```

A rule may not name `weave #6`: a WeaveId is minted fresh every run, so a remembered rule
that named one would mean a different participant tomorrow, or nobody.

```text
loom> authority show counter
  counter: may run
    build pinned: 5ce05df9d3d346e7189728856ca93140
    may say:  Noted v1 -> role logbook
    LIVE, weave 6:
      baseline (frozen at admission): zen.Result v1 -> any target
      …
      delegated (revocable): Noted v1 -> role logbook
```

Two halves, and the difference is not cosmetic:

- **baseline** is minted when the artifact is admitted and is frozen for that weave's
  whole life ([GATE-05](../laws/admission-laws.md)). The host keeps it deliberately tiny —
  enough to be inspectable and to answer, and nothing else.
- **delegated** is what you approved. It is replaceable while the weave runs, which is
  exactly why your approvals live there: a permission you cannot take back is not one you
  really granted.

```text
loom> authority revoke counter
  revoked, for this run and the next. weave 6 may now say: nothing
```

One word, both halves: the running weave stops being able to say it, and the file stops
remembering. Restart and it is still revoked.

Three things about taking something back, each of which is a way it could have been a
promise half kept:

- **Approving the same thing twice does not make it twice as hard to revoke.** A repeated
  `authority allow` says so and stores nothing new, and one `authority revoke` takes the
  permission away for good — including when the duplicate came from you editing the file
  by hand, which the host collapses when it reads it and tells you it did.
- **A narrow rule taken back under a broad one you still hold has changed nothing about
  what the weave may do**, and the host says which you got:
  ```text
  loom> authority revoke counter Noted v1 -> role logbook
    revoked, for this run and the next. weave 6 may now say: any shape -> any target
    NOTE: 'Noted v1 -> role logbook' is STILL PERMITTED by another rule you have granted
          'counter'. See 'authority show counter'.
  ```
- **A decision that could not be written did not happen.** The file is written before the
  live policy changes, so a full disk or a read-only directory gets you a refusal and a
  host that permits exactly what it permitted a moment ago — not an approval good until the
  next restart that nobody told you about:
  ```text
  loom> authority trust counter
    cannot write 'loom-authority.json.tmp'
    'counter' is unchanged: it still may NOT run, here and after a restart.
  ```

## 8. Change it, rebuild it, run it again

Edit `counter.cpp`, then:

```sh
cmake --build build -j
```

Start the host again:

```text
  refused    counter
              admission refused at open: 'counter' changed since it was approved
              (approved 5ce05df9, now ca824b8c); its speech authority is unchanged, but
              the code is not the code that was approved. Re-approve this build with:
              authority trust counter    or, if you are developing it:
              authority trust counter --rebuilds
```

An artifact is pinned by the **bytes** of the build you approved, not by its path or its
name. A rebuild is new code, and the default is to ask.

The pin is recorded the first time an approved artifact loads, and recording it is part of
letting that build run. If the host cannot write it — a read-only directory, a full disk —
the build is **refused** rather than run unrecorded, because an approval with no pin could
not tell the next, different build from this one:

```text
loom> start counter /home/you/tally/build/libcounter.so
  refused: admission refused at open: 'counter' is approved, but this host could not record
  which build of it runs (cannot write 'loom-authority.json.tmp'). You chose to be asked again
  when it changes, and without that record a different build would not be noticed, so build
  5ce05df9 was not started. Your decision is unchanged: make the decision store writable and
  start it again.
```

Nothing about your approval changed and nothing is waiting in `authority pending`: once the
file can be written, `start` it again and the build is pinned as usual.

While you are the one writing it, say so once:

```text
loom> authority trust counter --rebuilds
```

Now every rebuild comes up without a question — and never silently. The note is printed
with the boot report, and straight after the `start` or `reload` that caused it:

```text
  started    counter  (weave 6)
  note: counter: REBUILT since you approved it (ca824b8c -> 84f68879), admitted because trust_rebuilds is on
```

`--rebuilds` widens nothing. The rules you approved are unchanged; all it decides is
whether *new bytes* may run under authority you already granted. `authority trust <name>
--ask-again` turns it back off. It is also the one case where a pin that cannot be written
does not refuse the build: you have already said any build may run, so the host notes that
it could not record which one did.

It covers a changed **request** too, without a second mechanism to understand. A weave's
declared ask (`ZEN_ASK`) is compiled into it, so an artifact that starts asking for
something new is an artifact whose bytes changed — the same question, reached by the same
door. What `--rebuilds` turns on is "I am the one rebuilding this"; leave it off and you
see every change, including that one.

What the host **cannot** show you is the ask itself. A manifest exists only once the
artifact has been opened and its code has run, and everything the policy refuses, it
refuses before that. Not running unapproved code and being told what unapproved code wants
cannot both be had, and this host chooses the first. So `authority pending` shows you the
build ids and the path, and the artifact's documentation is where you learn what it wants
to say.

### Without restarting

```text
loom> reload counter /home/you/tally/build/libcounter.so
  ok
```

Reload is in place: same weave id, state carried across through the gate. Its authority
is unchanged — a reload keeps the incumbent's baseline, so reloaded code cannot arrive
with more than the code it replaced. A deliberately different one is a *replacement*, not
a reload.

`start`, `stop` and `reload` are your three lifecycle words:

```text
loom> stop counter
  'counter' stopped. This host is still running.
loom> start counter /home/you/tally/build/libcounter.so
  7
  weave 7 may now say: Noted v1 -> role logbook
```

The second line is not this command's doing. Your decisions were installed **while** the
artifact was being loaded — by the same door, for every route that can load anything — so
a boot row, `start`, `reload` and a `zen.LoadWeave` that another weave sends all produce
the same governed participant. There is no route that loads something and leaves it
ungoverned, and nothing here picks a subject out of a message.

`stop` ends **an application**. `quit` ends **the host**. Your standing decisions survive
both, which is why a restarted artifact came back with the authority you had approved.

## 9. Work that starts by itself, and work that never stops

Two things a weave does on its own, and the host has one answer to each.

### It is told when it is live

A weave that declares `zen.Activated` is told, by Loom, the moment its incarnation is
committed — on a boot row, on `start`, on `reload`, and on a `zen.LoadWeave` that some
other weave sent. Whatever it does with that is its own business, and the usual thing is
to start working:

```cpp
void on(const loom::Activated&, loom::Mail& mail) {
    if (mail.lifecycle_attested()) { ++state_.activations; }   // Loom's own word, not a shape
    mail.send_to_role(kLogbookRole, Noted{"opened", 0});       // ...and it begins
}
```

**The authority you approved is already in force when that runs.** That is the whole point
of the order: your decisions are installed while the artifact is being loaded, before it is
told it is live, so a weave whose first act needs a permission you granted gets it rather
than a refusal. Ask it afterwards and it will tell you:

```text
loom> send 6 Bump 1 what=first
  sent.  reply -> m3  total=1 activations=1 opened=1
```

### It keeps the bus busy

A weave whose handler queues its own next message never lets the bus go idle. That is
legitimate — a clock does it, a poller does it — and the host serves it a **bounded turn**
at a time, so the console is never taken away from you. Type during it and you are heard:
`stop`, `authority revoke`, `tap` and `quit` all still work while a weave is talking to
itself as fast as it can.

And the bus keeps turning **while** you type. A command you have half written, or stopped
in the middle of, holds nothing up: the host waits only for lines you have finished. Your
terminal's own editing — echo, backspace, history — is untouched, because the host never
changes its mode. (On Windows a console cannot be asked whether a line is finished, so the
host reads it on a thread of its own and hands each finished line to the loop.)

A command is one line of at most **4000 bytes**. A longer line is refused where it stands,
and none of it runs — not a shortened version, and not its end as a second command. Say it
again, shorter; what you type next is read as usual:

```text
loom> send 6 Bump 1 what="aaaaaaaaaaaa…"
  refused: input line 12 is longer than 4000 bytes, the longest command this host reads, so none of it was run. It began: send 6 Bump 1 what="aaaaaaaaaaaa...
  send it again, shorter; the lines after it are read as usual.
```

The same rules hold for a script you pipe in (`loom-host < commands.txt`). The
host reads it as fast as it runs the commands, however long it is, and holds at most 64 KiB
of it at a time; a line that is too long is refused by its line number, and the rest of
the script still runs. A Linux terminal itself keeps only the first 4095 bytes of a typed
line, which is why the limit sits below that: a line the terminal cut short is refused,
never run as the shorter command it became.

The other side of that bound: an answer may not have arrived by the time the host would
like to print one. It says so, and does not invent one:

```text
loom> send 6 SlowThing 1
  sent.  no answer yet (ask 4; 'asks' to see it)
loom> asks
  4  SlowThing v1  waiting on weave 6
  1 of 32 tracked.
loom> 
  [ask 4 settled] zen.Result v1  from weave 6  done
```

`asks forget <n>` stops waiting on one. It is called *forget* rather than *cancel* because
nothing at the far end is told anything: whatever you asked for may still be happening, and
this console simply stops recognising the answer.

The console holds each of these conversations for the host until the host has its answer —
and the host takes every answer as soon as it lands, printing the late ones. So thousands
of commands later it is holding exactly what `asks` lists, and never runs out of the 32.

**An answer is something Loom attributed**, never the newest thing in the window. Every
question this host asks is settled by two facts together — the conversation number it
minted, and the bus's own stamp of who spoke — so a message that merely *looks* like an
answer settles nothing. It matters because a loaded artifact may legitimately send
`zen.Result` to anyone: that is in the baseline every admitted artifact gets. `show <mN>`
prints both facts for any entry:

```text
loom> show m2
  m2 : zen.Result v1  1
    from weave 5, conversation 0
```

Conversation `0` means "named none" — an announcement, not an answer to anything.

## 10. Link to another host

A host can hold a connection to **another running Loom** — a Workshop, another
`loom-host` — and put an office in front of it that your own weaves ask through. That is a
`links` row in the boot plan:

```json
{
  "boot":  [ { "name": "probe", "path": "/home/you/probe/build/zengine-workshop-probe.so" } ],
  "links": [ { "name": "workshop", "connect": "127.0.0.1:7654",
               "identity": "agent", "credential": "open-sesame" } ]
}
```

At boot the host connects, says who it claims to be and what it presents, and reports what
the far host decided:

```text
  link workshop -> 127.0.0.1:7654: admitted as 'agent' (session 9; office loom.link.workshop, weave 3)
```

**The far host decides.** `identity` is what this host *claims*; the far host's own policy —
a Workshop's guests file, say — establishes a name (printed above) and a grant, or refuses
in its own words, or keeps the connection waiting for a person to decide. A refused or
unreachable link is still mounted: it answers `unlinked` to every ask, and `links connect
<name>` tries again (a new session; nothing from the old one carries over).

**Asking across.** A weave of yours sends the link's office one `loom.link.Ask` — the far
office it means and its message as bytes — under a correlation of its own
(`zen/bridge/link.hpp` has the one-line composer). The link answers that ask **exactly once,
through Loom's own answer door**, so what comes back is attested by *your* bus as the answer
to your ask (`mail.answers_ask()`), under your correlation, from the link:

- the far owner's answer — only a delivery the far bus itself attested as the answer to this
  crossing — re-admitted through your bus's gate as the shape your weave declared it accepts;
- or `loom.link.Outcome`, the link's own word: `refused` (nothing was submitted: the link
  refused the ask, or the far host dropped it before its bus), `dispatch-refused` (the far bus
  said no), `unlinked` (no session to send on) or `lost` (the session ended after the send: the
  outcome is **unknown**, and nothing is resent).

Settle on that and nothing less: correlation, the link's stamp **and** `mail.answers_ask()`
([the asker's own book](../reference/messaging.md#the-askers-own-book)). A far participant's
ordinary word to the link's session — even under your conversation's number, even in the
shape you expect — is never handed to you; neither is a message from anyone on your own bus
that merely looks like an answer. Silence is the fifth outcome and has no shape.

Your correlation never crosses. The link puts its own number on the wire — the crossing's
`attempt` — and translates the far reply back to the ask it belongs to, so two weaves whose
books both start at 1 are each answered their own. A reconnect is a new session: whatever was
still open on the old one is told `lost`, and nothing that arrives for it later can answer
anything on the new one.

**Waiting for what the ask set in motion.** An ask with `settle` is answered only once the far
host has also said that everything the ask set in motion on its bus has been dispatched — the
far owner's handler and every delivery it caused synchronously, however many turns they took.
That is what an agent wants between typing a key and taking a picture: inject with `settle`,
then ask for the picture. It does not wait for work the far side deferred to a timer or a later
turn ([fences](../reference/messaging.md#fences-when-what-one-send-set-in-motion-has-been-dispatched)).

What your weave may say to the link is your decision, like everything else:

```text
loom> authority allow probe loom.link.Ask v1 -> role loom.link.workshop
```

What the link's session may say on the far bus is the far host's.

```text
loom> links
  workshop  127.0.0.1:7654  admitted  as 'agent', session 9  (0 open)
```

**What the history keeps of a crossing.** Every frame the far host ships to the link's session
becomes a `loom.link.Crossed` record the link says to itself before it acts on it: the far
session and the name the far host established, the far bus's stamp and office, the crossing's
attempt, what kind of frame it was (`answer`, `dispatch-refused`, `message`, `send-refused`,
`settled`, `ended`) and the bytes. Your Recorder and Logger hold it as the ordinary delivery it
is — the link's account of what arrived, never an observation of the far execution — and the
answer the link then hands your weave names that record as its dispatch parent.

## 11. What this host remembers, and what it keeps

`loom-host` mounts Loom's own two history lenses ([history](../reference/history.md)):
a bounded working memory of everything the bus did, and a durable stream of what you chose
not to forget. Both are configured in the boot plan's `history` section; neither costs a
message on the bus.

```json
{
  "history": {
    "log": "run.log",
    "recent": "256",
    "retain": [ { "shape": "SurfaceCaptureChunk", "last_n": "1", "in_recent": false,
                  "retain_payload": false },
                { "shape": "loom.link.Crossed", "last_n": "8", "retain_payload": false } ],
    "keep":   [ { "shape": "InputInjected" }, { "shape": "SurfaceCaptured" },
                { "shape": "loom.link.Outcome", "cap": "64" },
                { "shape": "loom.link.Crossed", "cap": "256" } ]
  }
}
```

`retain` rows are the recorder's per-shape rules (how many of the last observations to keep,
whether they take recent context, whether their bytes are kept); `keep` rows are the logger's
durable selection beside Loom's default (what code is loaded, who may speak, every failed
handler and every death), each with an optional per-shape cap. Numbers are written as
strings, as every integer in these files is. `--log <file>` names the stream from the
command line and wins over `history.log`.

Reading it back at the console:

```text
loom> history                      what memory holds now, and what it has forgotten
loom> history last InputInjected   the last observation of one shape, if any
loom> history recent 20            what happened around now
loom> history find 41              what became of bus delivery 41: retained, forgotten,
                                   not recorded, or unobserved
loom> history payload 17           the retained bytes of record 17, decoded
loom> log read 10                  the durable stream, read back through the gate
```

A durable record says where it came from — the bus, this host's own diagnostic, or the
logger's own selection change — and a fact the memory has released is reported as forgotten,
never as "nothing happened". `log read` flushes the stream first, so it reads what stands,
not what was last closed: ordinary observations are otherwise buffered until the host quits.

## 12. Keep it running for clients that come and go

Everything above is one person at one console, and the host ends when that console closes.
`loom-host --serve <dir>` is the same host kept alive on purpose: it works from `<dir>` (its boot
plan, decisions and history live there), listens on loopback for **clients** that attach, ask and
leave, writes `session.json` and `session.key` so a client can find and present itself, and does
**not** end when its console closes — a client's Shutdown ends it, or `quit` at the console.

```text
$ loom-host --serve work
  session: serving /home/you/work at 127.0.0.1:64912  (lifetime ae547299e9fa26c0cc60c5ee91b87585)
```

Its door admits the owner's clients — the session's own key, nothing else — with a grant to the
session's vocabulary and nothing more, and a run manager's workers with exactly what that manager
may itself say. With Loom's run manager booted, clients start named runs of editable Python tools
that keep running when the client leaves. That is its own page: [sessions](sessions.md).

## When something goes wrong

Four different failures, four different places to look. Telling them apart is most of
what debugging this is:

| what you see | what failed | where to fix it |
|---|---|---|
| `error: expected ';' before …` from `cmake --build` | **your compiler.** Loom is not involved and never ran | your source |
| `Could not find a package configuration file provided by "loom"` | **CMake**, at configure time | `CMAKE_PREFIX_PATH` — the prefix, not `lib/cmake/loom` |
| `open failed: …cannot open shared object file` | **the loader.** The path in your boot plan is wrong, or you have not built | the path, or `cmake --build` |
| `refused: input line N is longer than 4000 bytes` | **the line's length.** None of it ran | say it again shorter, or split it into several commands |
| `admission refused at open: …` | **your own policy.** Nothing about the artifact is wrong | `authority trust <name>` at the console |
| `admission refused at speak: …` | your policy again, but the code already ran — see [admission](../reference/capabilities.md#admitting-a-loaded-artifact) | the console |
| `Refused … [CapabilityDenied]` on the tap | **authority.** The weave tried to say something it may not | `authority allow <name> <rule>` |
| `Refused … [NotAccepted]` | **addressing.** That weave does not accept that shape | `weaves`, and send it to one that does |
| `Refused … [SeamUnresolved]` | **vocabulary.** Nothing has ever declared that shape | have some weave accept it |

`authority pending` lists everything the policy refused and is waiting on, with the build
it was asked about and the path it came from — but not what the artifact asks for, for the
reason above: it was refused before it could say.

If a boot plan is what is broken, **`loom-host --no-boot` comes up with the console and
starts nothing — including when the file does not parse at all**:

```text
$ loom-host --no-boot
loom-host 0.1.0   containment: in-process; …
the boot plan was NOT run: boot plan 'loom-boot.json' refused: <value>: MalformedBytes — not valid JSON: expected string key in object
  nothing from it was started. Fix loom-boot.json and restart, or start things by hand here.
loom> status
  boot plan:   loom-boot.json  COULD NOT BE READ, NOT RUN
               boot plan 'loom-boot.json' refused: …
```

Without `--no-boot` the same file is fatal (exit 3), and `--check` still refuses it — that
is what `--check` is for. A broken **authority** file is fatal either way and deliberately
so: it is the record of what you decided, and a host that came up ignoring it would be a
host running under decisions nobody made.

## What this host is not

`loom-host` is a **default**, not a privilege. Its console is `loom::ConsoleEngine`, its
lifecycle steward is `loom::WeaveManager`, its kernel door is `loom::ControlWeave` — all
ordinary participants, all replaceable by something you write, all reachable from the
installed package. Being the thing that ships is not the same as being the only thing
that fits.

And it contains nothing. An in-process weave shares this address space and is trusted at
that level; what a grant bounds is **speech**, never what code can touch. Read
[what loading it in-process means](dynamic-weaves.md#what-loading-it-in-process-means)
before you run an artifact you did not build.

## Next

- [Writing a weave](writing-a-weave.md) · [messaging](messaging.md) ·
  [dynamic weaves](dynamic-weaves.md)
- [Capabilities](../reference/capabilities.md) — the trust boundaries, and what they do
  not claim
- [Diagnostics](diagnostics.md) — when the behaviour rather than the build is wrong
