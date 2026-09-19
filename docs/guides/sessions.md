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

**Leaving is not cancelling.** Detach at any time; the run, its worker and its links carry on.
The four ways a run can stop mean four different things:

| | what happened | what the record says |
|---|---|---|
| a client leaves | nothing, to the run | still `running` |
| `cancel` | a REQUEST: the worker is told at its next wait, runs its cleanup, ends | `cancelled` once its process ended |
| `cancel --force` | the worker's process tree is ended now; no cleanup ran | `cancelled`, process `killed` |
| the host ends | every active worker ends with it | `interrupted` |

A wait is the client's decision (`--wait`, `Session.wait(timeout=...)`): running out of it says
nothing about the run, which is still its manager's to finish.

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
- `ctx.ask_async(...)` returns a `Pending`; several may be open at once.
- `ctx.hold(gate)` waits until `<run>/release/<gate>` exists — for demonstrations and tests that
  must decide when a run proceeds while no client is attached.
- `ctx.inputs` are typed as the manifest declares them (`text`, `int`, `bool`, `number`).

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
| `no Python interpreter was found` | the manager could not start a worker | `"python"` in `loom-tools.json` |

**After a crash of the host**, nothing resumes: a restarted host is a new lifetime, its runs
start empty, and the old lifetime's runs keep the last record their manager wrote (a run that was
running then shows the state it had). What can be recovered is the record and the outputs on disk.

## What this does not do

- Resume a run, or a worker's Python stack, across a host restart.
- Sandbox a tool, or authenticate a person: the owner's key is a local shared secret.
- Listen anywhere but loopback, or add transport security.
- Serve another platform's clients over the network, discover sessions, or share one across users.
- Queue work for a single-holder resource of another application: a tool that meets one is
  refused by its owner and says so.
