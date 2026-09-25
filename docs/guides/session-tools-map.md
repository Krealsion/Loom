# Where a session's own tooling answers each question

One page of routing, not a new explanation: [the sessions guide](sessions.md) is the whole
account of `loom-host --serve`, runs and tools, and this page exists because a first-time
reader of it rarely comes with "read the whole guide" as their actual question. They come
with a narrower one. Each row below is that question, and where in this repository's
own docs it is already answered — never a second copy of the answer.

This page is about the session and its tools **in general** — the part that is the same
whichever application a tool speaks to. What a *particular* target lets a tool say to it (which
doors it opens, which powers a guest may hold) is that target's own documentation to write; a
target that builds an external-host capability on this page's mechanism should publish a
companion entry of its own, grouped where its readers already look, the same way this page is
grouped beside [the sessions guide](sessions.md) rather than inside it.

| A reader wants to... | Read |
|---|---|
| **Start or return to work** | [sessions §1](sessions.md#1-what-you-need) (Python, the installed layout), [§2](sessions.md#2-start-a-session-and-decide-what-it-may-do) (`loom-boot.json`, `loom-tools.json`, the operator's first-run decisions), [§3](sessions.md#3-attach-look-leave) (`loom-session status`, and that every command is a fresh client that attaches, asks and leaves) |
| **Repeat an experiment or start a dependent step** | [sessions §4](sessions.md#4-runs) — names reconcile existing runs, new experiments need new names, and `run` returns without waiting by default; [three things a run says](sessions.md#three-things-a-run-says-and-why-none-stands-for-another) distinguishes a verdict from an ended execution |
| **Find an existing capability** | [sessions §3](sessions.md#3-attach-look-leave) — `loom-session tools <dir>` lists a package's tools by their own words, `describe <dir> <tool>` is one tool's whole help (inputs, outputs, requirements, the bus rules it asks for, an example) read from the catalog, never from running its code |
| **Act on the target application** | Not this page: what a tool may say to a target is that target's own admission and its own guest shapes, in that target's own docs. This page's part of it is generic — [sessions §7](sessions.md#7-writing-a-tool) shows `ctx.ask`/`ctx.ask_async` against whatever role a target's own vocabulary declares, and [§6](sessions.md#6-four-decisions-kept-apart) is the shape every such capability takes: trusting a package, widening what its workers may ask, and — separately, at the target's own door — what a *guest* may say once connected. Zengine is not Loom's: it is the separate **Zengine** repository, and its own Workshop entry is named there, at `Zengine/docs/workshop/external-host.md` (Zengine-owned pages are named that way throughout and never linked, [as the docs index says](../README.md)) |
| **Follow what a target's participant says, as it happens** | [sessions §7](sessions.md#7-writing-a-tool) (`ctx.observe`: subscribe before you act, attribute by `cause`, a wait that runs out is yours), [observing](observing.md#following-another-participants-publications) (the five things to get right), [the observation reference](../reference/observation.md) (readiness, order, bounds, endings, a link's custody). What may be observed is the target host's decision at its own door, in its own docs |
| **Build a tool on another package** | [sessions: a package that builds on another](sessions.md#a-package-that-builds-on-another) — `"uses"` in its manifest; each package is approved on its own, and nothing is found by path |
| **Understand the result** | [sessions §4](sessions.md#4-runs) (the run's states, `run.json`, verified outputs in `out/`) and its subsection [Three things a run says](sessions.md#three-things-a-run-says-and-why-none-stands-for-another) (a tool's verdict vs. the execution vs. the saved record — three different questions with three different answers); [§5](sessions.md#5-where-answers-came-from) for `crossings`, which reads the session host's own delivery history separately from anything a run's own summary already said |
| **Recover or finish** | [sessions §8](sessions.md#8-when-something-goes-wrong) — its own table of symptom → meaning → command, `cancel`/`release`/`runs --past`, and what a clean vs. a killed host leaves behind (a worker's own cleanup, `ctx.on_cleanup`, is where a tool gives back whatever it was holding at a **target's** door — a target's own docs say what that is for it) |
| **Extend the harness** | [sessions §7](sessions.md#7-writing-a-tool) — a tool is a Python file with `run(ctx)` in a package the operator approved; edit it and a new run uses the edit, no compiler and no restart. [Package files and run outputs](sessions.md#package-files-and-run-outputs) explains where the copied script, workspace inputs and artifacts live. Its own manifest (`loom-tool.json`) is what `describe` reads, so a tool worth reusing documents itself there, not only in its own comments |

**One session, one target, many tools.** Nothing above is about any one target application —
that is deliberate. A session that drives one is `loom-boot.json` naming a `links` row to it and
a `loom-tools.json` approving that target's own tool package, exactly as any other session names
what it boots and approves what it runs; the target supplies the vocabulary weave and the
package, this repository supplies the session underneath both.
