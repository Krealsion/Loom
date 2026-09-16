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

- **`mail.answer` needs no authority; `mail.send_to_role` does.** An answer is authorized
  by the ask that arrived. An unsolicited send is speech, and speech is granted.
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
  'logbook' may run. It may still say nothing until you allow something.
loom> authority trust counter
  'counter' may run. It may still say nothing until you allow something.
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

While you are the one writing it, say so once:

```text
loom> authority trust counter --rebuilds
```

Now every rebuild comes up without a question — and never silently:

```text
  started    counter  (weave 6)
  note: counter: REBUILT since you approved it (ca824b8c -> 84f68879), admitted because trust_rebuilds is on
```

`--rebuilds` widens nothing. The rules you approved are unchanged; all it decides is
whether *new bytes* may run under authority you already granted.

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
  now under administration; weave 7 may now say: Noted v1 -> role logbook
```

`stop` ends **an application**. `quit` ends **the host**. Your standing decisions survive
both, which is why a restarted artifact came back with the authority you had approved.

## When something goes wrong

Four different failures, four different places to look. Telling them apart is most of
what debugging this is:

| what you see | what failed | where to fix it |
|---|---|---|
| `error: expected ';' before …` from `cmake --build` | **your compiler.** Loom is not involved and never ran | your source |
| `Could not find a package configuration file provided by "loom"` | **CMake**, at configure time | `CMAKE_PREFIX_PATH` — the prefix, not `lib/cmake/loom` |
| `open failed: …cannot open shared object file` | **the loader.** The path in your boot plan is wrong, or you have not built | the path, or `cmake --build` |
| `admission refused at open: …` | **your own policy.** Nothing about the artifact is wrong | `authority trust <name>` at the console |
| `admission refused at speak: …` | your policy again, but the code already ran — see [admission](../reference/capabilities.md#admitting-a-loaded-artifact) | the console |
| `Refused … [CapabilityDenied]` on the tap | **authority.** The weave tried to say something it may not | `authority allow <name> <rule>` |
| `Refused … [NotAccepted]` | **addressing.** That weave does not accept that shape | `weaves`, and send it to one that does |
| `Refused … [SeamUnresolved]` | **vocabulary.** Nothing has ever declared that shape | have some weave accept it |

`authority pending` lists everything the policy refused and is waiting on, with the build
it was asked about and the artifact's own declared ask beside it. A declared ask is
advice — the host shows it to you and grants nothing because of it.

If a boot plan is what is broken, `loom-host --no-boot` comes up with the console and
starts nothing, and `--check` validates both files without running anything at all.

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
