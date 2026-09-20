# A session that outlives its client

Keep a Loom host running on purpose, attach to it, start named runs of editable Python tools,
leave, come back from a fresh client, and find the same runs, their outputs and where their
answers came from. No compiler between editing a tool and running it again, and no restart.

A **session** is `loom-host --serve <dir>`: the [supplied host](running-loom.md) kept alive for
clients that come and go. A **run** is one execution of one **tool** — a Python file with a
`run(ctx)` function, in a **package** the session's operator approved. The pieces, and who owns
what:

| part | what it is | owns |
|---|---|---|
| `loom-host --serve` | the host, serving one directory | its lifetime, admission to it, its history |
| `loom.session` | the host's door (host wiring) | who may attach, as whom, under what grant; ending the lifetime |
| `loom.history` | a scoped reader over the host's Recorder (host wiring) | nothing new — it reads what the host remembers |
| `loom-runs` | the run manager: an ordinary artifact you boot and approve | the catalog, runs, their workers and records |
| a worker | one Python process per run, its own session | the run's asks and its verdict |
| a client | the `loom-session` CLI, or `loom_session.Session` in Python | nothing: it asks the owners |

Everything below was run to write this page, on Windows and on Linux.

## 1. What you need

Loom installed with its session tooling (the default wherever the kernel is built — see
[the tools you need](tools.md)): `bin/loom-host`, `bin/loom-session` (and `loom-session.cmd` on
Windows), `lib/loom/loom-runs.<so|dll>`, `lib/loom/python/loom_session`, and a small example
package at `share/loom/tools/basics`. A project of yours that drives a session from its own
tests finds them through the package rather than this layout: `find_package(loom)` sets
`LOOM_HOST_PROGRAM`, `LOOM_SESSION_TOOLS` (`TRUE` where the tooling was installed), and with it
`LOOM_RUNS_ARTIFACT` and `LOOM_SESSION_RUNTIME` (the directory `PYTHONPATH` names).

**Python 3.8 or newer**, standard library only, for the CLI and for the tools. It is optional
tooling: a basic Loom installation — and `loom-host --serve` itself — runs without it. The CLI
looks for `python3`/`python` on `PATH`, or `LOOM_PYTHON`, and says so when there is none; the run
manager uses the interpreter the catalog names (`"python"`), or the first it finds on `PATH`, and
refuses a run in words when there is none.

## 2. Start a session, and decide what it may do

Pick a directory; it is the session. `loom-boot.json` boots the run manager:

```json
{ "boot": [ { "name": "runs", "path": "<prefix>/lib/loom/loom-runs.dll", "role": "loom.runs" } ],
  "history": { "log": "session.log", "recent": "1024" } }
```

`loom-tools.json` is **your** catalog: which tool packages this session knows, and which may run.

```json
{ "packages": [ { "path": "<prefix>/share/loom/tools/basics", "approve": "any-revision" } ] }
```

`"approve"` is `"any-revision"` (every edit may run — your own tools, while you write them), one
64-hex revision (exactly that content; an edit is refused until you approve again), or absent
(listed and described, never run). A path is never approval. Two optional keys beside
`"packages"`: `"python"`, the interpreter the workers run under, and `"runtime"`, the directory
holding the `loom_session` package they import (by default, the one installed beside the run
manager).

The first time, start the host at a terminal and make the operator's decisions at its console —
the same `authority` words as for any artifact ([running-loom § 5](running-loom.md#5-decide-what-may-run)):

```text
$ loom-host --serve work
  session: serving /home/you/work at 127.0.0.1:64912  (lifetime ae547299e9fa26c0cc60c5ee91b87585)
  refused    runs @loom.runs
              admission refused at open: no standing decision for 'runs' (...)
loom> authority trust runs
loom> authority allow runs loom.session.ExpectRun v1 -> role loom.session
loom> authority allow runs loom.session.ForgetRun v1 -> role loom.session
loom> authority allow runs loom.runs.Tools v1 -> any target
loom> authority allow runs loom.runs.ToolDescription v1 -> any target
loom> authority allow runs loom.runs.Run v1 -> any target
loom> authority allow runs loom.runs.RunList v1 -> any target
loom> authority allow runs loom.runs.Directive v1 -> any target
loom> start runs <prefix>/lib/loom/loom-runs.dll loom.runs
```

What each decision means:

- `trust runs` — this artifact may run in your process (pinned to this build).
- `ExpectRun`/`ForgetRun` — it may register the one worker connection each run needs, and
  forget it. Without these it can list tools and nothing more.
- `Tools`, `ToolDescription`, `Run`, `RunList`, `Directive` — it may **answer** in its own
  vocabulary. Every delivery is checked against its author's grant, answers included; an
  admitted artifact's baseline covers only the standard replies (`zen.Result`, `zen.Ack`,
  `zen.Refused`), so a manager that answers in typed shapes needs these.
- Anything more you allow `runs` is the **ceiling** of what its workers may be granted — see
  [section 6](#6-four-decisions-kept-apart). `basics/session-report` needs
  `authority allow runs loom.session.Describe v1 -> role loom.session`.

The decisions are written to `loom-authority.json` in the directory, as always. From then on the
boot row starts the manager by itself, and you can start the session detached:

```text
$ loom-session start work
serving /home/you/work at 127.0.0.1:50113 -- lifetime 3f17415b4ee53b1ddce611b8ac90b0f7 (pid 36712)
```

A **serving host does not end when its console closes** — closing stdin closes only the console.
It ends when a client asks (`loom-session stop work`, `Session.shutdown`), or at `quit` typed at
its console. Exit codes are the host's usual ones, plus `4` when another host already serves the
directory and `5` when the session could not be opened.

While it runs, the directory holds `session.json` (where to attach, and the **lifetime** — this
host run's identity, minted at start and never reused) and `session.key` (the owner's client key:
created owner-only on POSIX; on Windows it inherits the directory's permissions, so keep session
directories under your profile). Both are removed when the host ends cleanly.

## 3. Attach, look, leave

Every `loom-session` command attaches as a fresh client, asks the owners, prints, and leaves; add
`--json` for the owners' answers as data.

```text
$ loom-session status work
session /home/you/work -- lifetime 3f17415b4ee53b1ddce611b8ac90b0f7
  loom-host 0.1.0, pid 36712, weave ABI v9, listening on 127.0.0.1:50113
  connection 4: client client (claims 'loom-session-cli') admitted, session 11, compat
  0 run connection(s) expected
$ loom-session tools work
basics/steps                     Walk a few named steps, optionally wait at a release gate, ...
                                 any revision; revision 51986a014e7b
basics/session-report            Ask the session's door what this session is -- lifetime, ...
$ loom-session describe work basics/steps
```

`describe` is the tool's help: purpose, inputs and their types, outputs, what it requires, the
bus rules it asks for, the shapes it speaks with their fields **as this host's schemas have them**,
its source, revision and approval, an example, and what its refusals mean. The same answer is the
machine description (`--json`). Reading the catalog runs no tool code.

From Python, the same operations:

```python
from loom_session.session import Session
with Session.attach("work") as s:
    s.tools("gate")                   # search by the package's own words
    s.tool("basics/steps")            # the description
    s.start("basics/steps", "first", {"count": 2, "hold": "go"})
    s.run("first")                    # the run, as its manager holds it
```

## 4. Runs

```text
$ loom-session run work basics/steps --name first --input count=2 --input message=hi --wait 30
run 3f17415b/first -- passed (live)
  tool basics/steps, revision 51986a014e7b
  worker: session 14 (run:first), pid 33776, process exited, exit 0
  summary: 2 step(s); report.txt written
  artifact report.txt: verified, 27 bytes, sha256 a42644fe78226502
```

- **A name is one run for the whole lifetime.** The handle is `(lifetime, name)`. Asking to start
  the same name with the same tool and inputs again answers the existing run — so a client whose
  answer was lost asks again and is told what became of it, and nothing starts twice. The same
  name for anything else is refused.
- **A run executes a snapshot.** The package is copied into `runs/<lifetime prefix>-<name>/package`
  and its digest is the run's `revision`. Edit the tool and start another run: the new run uses
  the edit, the running and finished ones keep theirs. No compiler, no restart.
- **The states:** `starting`, `running`, then one of `passed`/`failed` (the tool's verdict),
  `error` (the tool raised), `cancelled`, `crashed` (the worker ended without a verdict),
  `interrupted` (the host ended first). `step` and `pending` say where a running run is and what
  it is waiting on, in the worker's words.
- **Outputs** land in the run's `out/`. The manager checks each one itself — inside `out/`, its
  size, its SHA-256 — before listing it `verified`.
- **`run.json`** in the run's directory is the manager's record, rewritten at every change and
  read back through Loom's gate. It survives the host; `loom-session runs work --past` lists
  earlier lifetimes' records — evidence of what happened, never live state.

### Three things a run says, and why none stands for another

A returning client has to be able to tell a finished tool from a finished execution, and either
from evidence that got saved. A run answers all three separately:

| field | the question it answers | its words |
|---|---|---|
| `state` | what did the TOOL conclude? | the states above. Settled once, never rewritten |
| `process` | is anything still RUNNING? | `not-started`, `running`, `descendants`, `killing`, `exited`, `killed`, `unknown`. `exit_code` is the WORKER LEADER's own code, and it means something only at `exited` or `killed` -- the two words the manager writes only once it has watched the whole execution end and read one |
| `record` | is `run.json` this run? | `saved`, `stale` (with `record_error`), `unknown` (a record written before this manager said). `saved` covers every change to the answer it comes with, not just the interesting ones |

- **A verdict does not end an execution.** A tool that returns having left a thread it never
  joined, or a child process of its own, is `passed` with `process` `running` or `descendants`.
  Between asking for a stop and the group actually going, `process` is `killing` — and an exit
  code is printed only once there is one to read, never as a 0 standing in for one.
  While that is true the run still counts against the 8 active, `cancel` still stops it, and
  `release` refuses — releasing a record is not a way to kill something, and the refusal says so.
  Stopping it afterwards never rewrites the verdict: the run stays `passed`, `process` becomes
  `killed`, and a note says what was stopped and why.
- **An execution can also outlive a worker that never got to a verdict.** A worker that dies
  outright — leaving a child of its own running — is not yet a finished run of any kind, and
  that is what you see: the run is still `running`, `process` is `descendants`, and `exit_code`
  is already the worker's own. `cancel` (no `--force` needed) is what stops what it left behind;
  there is nobody left to clean anything up, so nothing claims to have been given back. **The
  verdict arrives when the execution does.** Once the group has gone the run is `crashed`, with
  `process` `killed` — the manager latched that the worker was LOST when it watched it go, so a
  cancellation asked for in between stops what was left without turning a crash into a
  cancellation. The record keeps the two facts apart: its `failure` says the worker ended
  without a verdict AND that the execution it left was then stopped, and `exit_code` remains
  the worker's own.
- **An exit code is something the manager read.** `exited` and `killed` carry the leader's own
  code — never a descendant's, never one number for the whole group, and never a default filling
  in for an observation nobody made. A leader whose code is already known keeps it when the group
  it left behind is stopped later, and keeps it the moment it is read — a code the shutdown's own
  moment of watching is the first to see is still that leader's, and is written down. If a
  shutdown ends an execution and does not see the WHOLE of it finish in the moment it waits — the
  leader's own exit is not that — the record says `killing`, which promises no code at all;
  `unknown` is the answer about an execution that really is over and whose code was never
  readable. At either word the `exit_code` field is not a promise, and **the record's note is
  what says whether a leader code was read**: `exit_code` is a number the manager observed only
  where the note says one was, and otherwise it is the field's unread default.
- **A record that could not be saved is not a failed tool.** If the manager cannot write
  `run.json` — a full disk, a permission, something else holding the temporary name — the last
  valid record is left exactly where it is, `record` becomes `stale` and `record_error` says the
  operating system's reason and what is still on disk. The verdict is untouched. The manager
  tries again at the run's next change and whenever a client asks it about its runs; nothing
  loops and nothing retries on its own. `loom-session show` prints the line; `--json` has both
  fields.

**Leaving is not cancelling.** Detach at any time; the run, its worker and its links carry on.
The ways a run can stop mean different things:

| | what happened | what the record says |
|---|---|---|
| a client leaves | nothing, to the run | still `running` |
| `cancel` | a REQUEST: the worker is told at its next wait, its cleanups run, it ends | `cancelled` once its process ended |
| `cancel --force` | the worker's execution group is ended now; no cleanup ran | `cancelled`, process `killed` |
| `cancel` after the verdict | there is nobody left to ask, so the execution is stopped outright | the verdict it had, process `killed` |
| the host ends cleanly (`stop`, `quit`, `Shutdown`) | every execution this manager owns ends with it | `interrupted` — or its verdict, with process `killed` |
| the host is KILLED | it runs nothing, so see below | whatever was last saved |

A wait is the client's decision (`--wait`, `Session.wait(timeout=...)`): running out of it says
nothing about the run, which is still its manager's to finish. `wait` returns on the **verdict**;
if you also need the execution to be over, read `process` after it.

Bounds: 8 active runs and 64 held per lifetime; release finished ones with
`loom-session release work <name>` (`--remove` also deletes its directory). The rest are in
[bounds](../reference/bounds.md#sessions-and-runs).

## 5. Where answers came from

A run's worker is its own session, so the host's Recorder holds what was delivered to it.
`loom-session crossings work <name>` asks the history reader for those deliveries and joins them
with the worker's own account of its asks, by correlation. When an answer came through a
[link](running-loom.md#10-link-to-another-host), the row carries the crossing it descends from —
the link, the far session and the name the far host established, the far author and office, the
attempt — as the link recorded what **arrived**; it is not an observation of the far execution.
Its fields need the crossing record's bytes, which is your retention choice
([§ 11](running-loom.md#11-what-this-host-remembers-and-what-it-keeps)); without them the reader says
the bytes were declined or evicted, and a released record is `forgotten`, never "nothing". Bulk
answers cross as bytes too — a picture fetched in chunks is most of a run's traffic — and share
the Recorder's payload budget (1 MiB unless `history.payload_budget` says otherwise), so the
earliest crossings' bytes are evicted first: give the budget room for what you mean to read back.
The CLI prints the answers to the run's asks and the crossings, and counts the rest; `--json`
lists every delivery.

## 6. Four decisions, kept apart

| decision | made by | where |
|---|---|---|
| who may attach | the host: the owner's key admits a client; nothing else does | `session.key` |
| which package may run, at which revision | you | `loom-tools.json` `"approve"` |
| what the manager may say, and so pass on | you | `authority allow runs ...` |
| what a link's session may do on another host | that host | e.g. a Workshop's guests file |

**A client** is admitted with exactly the session, run-manager and history vocabularies, to exactly
those offices, and no tap. It cannot register runs or speak to your other participants.

**A worker** is admitted once, with a one-time credential its manager minted, as `run:<name>`,
under the rules its package asks for (`"asks"` in `loom-tool.json`) — and the door grants a rule
only when the manager's OWN approved authority already contains it. A package that asks for more
is refused whole, and the refusal names the `authority allow runs ...` that would allow it. A run
name narrows nothing; the door does. Several runs through one link share that link's far session
and its grant, and a far host sees them as one participant: give runs distinct far authority
with separate links.

**What a tool can touch is not bounded by any of this.** A worker is a process of the session's
user, with that user's files and network; its grant bounds what it may **say** on the bus. The
worker inherits the host's environment and nothing else — its standard streams go to
`worker.log`, and no socket or file the host holds is passed on — but it is not a sandbox, and
approving a package is trusting its code. See
[the exec boundary](../reference/capabilities.md#the-exec-boundary-three-independent-facts).

**Credentials** stay out of the record: the manager tells the door only a credential's SHA-256,
the worker deletes its credential file on reading it, and the owner's key is never on the bus.

## 7. Writing a tool

A package is a directory with `loom-tool.json` and Python files. `tools/basics` in Loom's source
is a complete small one. A tool:

```python
def run(ctx):
    ctx.step("inspect")                                   # where the run is
    rows = ctx.ask("zengine.guests", "GuestConnectionsRequested", via="workshop")
    ctx.check(rows["rows"], "Workshop lists no connection")   # fail with a reason
    ctx.on_cleanup(lambda: ..., "close the input session")    # runs however the tool ends
    ctx.produce("picture.bmp", data)                      # verified output
    return "one line saying what passed"
```

- `ctx.ask(office, shape, fields, via=<link>, settle=...)` sends one ask and waits for **its**
  answer — only a delivery Loom attests as that answer, typed by the host's own schema. A refusal
  is an exception: `Refused` (the owner said no), `DispatchRefused` (the bus said no),
  `SendRefused` (dropped before the bus), `LinkOutcome` (the link's word; `lost` means the outcome
  is unknown and is never retried for you), `NotAnswered` (your own wait ran out).
- **`settle=True` asks for two things and waits for both**: the attested answer, and the host's
  word that everything that send set in motion has been dispatched. They may arrive in either
  order; an answer alone does not finish such an ask, and a `NotAnswered` from one says which
  half is missing while the half that came is kept. Without `settle` the answer alone is enough.
  Across a link the flag means the same on the far host's bus, which is how the Workshop tool
  orders a picture after a chord.
- `ctx.ask_async(...)` returns a `Pending`; several may be open at once. `Pending.done()` polls
  without waiting and services whatever has already arrived.
- `ctx.hold(gate)` waits until `<run>/release/<gate>` exists — for demonstrations and tests that
  must decide when a run proceeds while no client is attached.
- `ctx.inputs` are typed as the manifest declares them (`text`, `int`, `bool`, `number`).

**Cancelling, and giving things back.** A cancellation ends a tool's ORDINARY work: it is raised
as `Cancelled` at the next wait point. What the tool registered with `ctx.on_cleanup` then runs
in a phase of its own, with the session still open and the same grant it always had — so a run
that took a far resource can hand it back after being cancelled, which is the only way the next
run gets it. Each cleanup's outcome is recorded as what it actually was: `done` only when the
far owner answered, and otherwise the refusal, the timeout, the lost link or the disconnection,
by name. A cleanup that cannot finish does not become a lie, and it does not change the verdict —
a tool that passed still passed, with a line on its summary saying what it could not give back.

- **A cancellation is a request while there is a worker to ask.** If the worker has already
  ended — its connection gone, its leader exited — `cancel` stops the execution the run still
  owns instead, and no cleanup runs, because there is nothing left to run one. What the run took
  is then the far owner's to reclaim. The verdict is still the worker's: a worker that died
  without one leaves a run that becomes `crashed` when its execution ends, not `cancelled` —
  what its worker did is not rewritten by what was done about what it left.
- The phase has a budget of its own (`ctx.cleanup_seconds`, 30 s by default). It is **checked
  between cleanups and at the context's own wait points** — an `ask`, a `hold`, anything that
  goes through `ctx` — so a cleanup that blocks inside ordinary Python (a `sleep`, a socket of
  its own, a subprocess) is not interrupted by it: nothing here preempts arbitrary code. What
  the budget does guarantee is that once it is spent the remaining cleanups are not attempted
  and each says so. The host is a different process and goes on answering every other client
  throughout, and `cancel --force` is the escape for a cleanup blocked outside those APIs: it
  ends the worker process.
- `ctx.cancel_requested` is the cooperative door for a tool doing its own looping: it reads
  whatever has already arrived, and once true stays true. It does not wait for a cancellation
  and it does not raise `Cancelled` — that is what the wait points do. It is not, however,
  guaranteed to do nothing else: its poll can raise `Disconnected` if the session has ended
  under it, and reading an arrival that needs a shape this client has not seen before asks the
  host for that shape. Finish the piece you are on and return — or raise — on your own terms.
- `--force` runs no cleanup at all. What the run took is then the far owner's to reclaim, in its
  own way and on its own terms; the run does not claim to have given anything back.

A tool that talks to another host through a link can only use shapes this host knows: the far
answer is re-admitted here, and a request written as JSON is encoded here. Load a participant
that declares that application's vocabulary (Zengine ships one for Workshop).

## 8. When something goes wrong

| you see | it means | do |
|---|---|---|
| `loom-session: no session.json in <dir>` | no host serves it | `loom-session start <dir>` |
| `... nothing answers there` | the host that wrote `session.json` has ended | start it again: a new lifetime |
| `run handle ... belongs to another host lifetime` | that run belonged to an ended host | `runs --past` shows its record |
| `not approved to run` / `changed since it was approved` | the catalog's decision | approve in `loom-tools.json` |
| `the session door refused run ...: ... may not say itself` | the manager's ceiling | `authority allow runs <rule>` |
| a run `crashed` | the worker ended without a verdict | its `failure` ends with the tail of `worker.log` |
| a run is still `running` but `process` is `descendants` | the worker exited and left something running; a note names its pid and code. If it exited without a verdict there is nobody left to ask, and the crash is recorded when the execution ends | `cancel <name>` stops what it left; no cleanup runs, because the worker is gone. The run is then `crashed` (or keeps the verdict it had), with `exit_code` still the worker's own |
| `no Python interpreter was found` | the manager could not start a worker | `"python"` in `loom-tools.json` |
| a run is `passed` but `process` is `running` or `descendants` | the tool finished; something it started did not | `cancel <name>` to end it, then `release` |
| `... its execution is still alive ...: cancel it to stop that` | you asked to release a record whose work is still going | `cancel <name>` first — releasing is not a way to kill |
| `record: stale — cannot open .../run.json.tmp ...` | `run.json` is behind the live run, for the reason given | clear the reason (disk, permission, a name in the way); the next question you ask saves it, as it stands then |
| a past record with `process: killing` | the host stopped that execution and did not see the whole of it finish. `exit_code` promises nothing here: the record's own note says whether a leader code was read — if it was, the number beside it is that leader's; if it was not, the field is its unread default | nothing — `killing` is the honest absence of an observed END. Read the note, not the number |
| a past record with `process: unknown` | that execution really did end, and no exit code was ever readable for it; the `exit_code` beside it is the field's unread default | nothing — it is the honest absence of a CODE, and the note says so too |
| a cleanup line that is not `done` | the run tried to give something back and could not | the line names what happened; the far owner decides what it does about a guest that went away |

**Recovering a session, start to finish.**

```text
loom-session runs work                 # what this lifetime holds, and how many are active
loom-session show work <name>          # state (the verdict), process (the execution), record
loom-session cancel work <name>        # stop a live execution -- after a verdict too
loom-session show work <name>          # ...and read it back: process killed, the verdict intact
loom-session release work <name>       # only once nothing of it is running
loom-session runs work --past          # earlier lifetimes' records, read from their run.json
```

If `show` says `record: stale`, believe the answer and not the file: the answer is the manager's
live state and the file is the last thing it managed to write. Fix the reason it gives and ask
again — the next question saves it, and `record` goes back to `saved`.

**After the host ends.** Nothing resumes: a restarted host is a new lifetime, its runs start
empty, and the old lifetime's runs keep the last record their manager wrote. Which record that is
depends on how the host ended, and the two are not the same thing:

- **Ended cleanly** (`loom-session stop`, `quit`, `Shutdown`): the manager ends every execution
  it owns and writes each run's last record — `interrupted` for one that had no verdict. Every
  stop is issued first, and then one bounded moment (two seconds over the whole shutdown, not
  two per run) is spent watching those execution GROUPS actually go, so that a last record
  saying `exited` or `killed` is an end the manager saw, with an exit code it read. A leader's
  own exit is not that end: a worker that had already exited leaving a child behind is not over
  because it was already over. A run whose group it could not see go in that moment is recorded
  `killing`, which promises no code — the shutdown does not wait on a proof it may never get,
  and it does not write down a zero in place of one.
- **Killed outright** (a crash, `kill -9`, the machine going down): the manager runs nothing, so
  the records stay as they were and the states in them are whatever was last saved. What happens
  to the workers is then the operating system's answer, not this manager's, and it differs:
  **on Windows** each worker's execution group is a job object the kernel closes on the dying
  host's behalf, so the workers go with it; **on POSIX there is no such guarantee** — the workers
  keep running, reparented, with nothing left that owns them. Look for them by the `pid` in the
  old records and end them yourself.

What can be recovered either way is the record and the outputs on disk.

## What this does not do

- Resume a run, or a worker's Python stack, across a host restart.
- Outlive a host that was killed outright, on POSIX: only Windows' job object survives that, and
  it is the kernel's doing rather than this manager's (§ 8).
- Contain a worker that deliberately escapes its execution group — a process that breaks away is
  an OS sandbox's problem, and this is not one.
- Sandbox a tool, or authenticate a person: the owner's key is a local shared secret.
- Listen anywhere but loopback, or add transport security.
- Serve another platform's clients over the network, discover sessions, or share one across users.
- Queue work for a single-holder resource of another application: a tool that meets one is
  refused by its owner and says so.
