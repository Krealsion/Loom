# The recommended setup

How we recommend a contributor work on Loom and [Zengine](https://github.com/Krealsion/Zengine),
whether a person or an agent does the work: the machine, the clone, where builds go, and how an
agent's harness reaches the start file. None of it is required. It is what keeps a change's
evidence honest and a machine usable while it builds.

## The machine

The two repositories build on one kind of machine, and their lanes differ in what they ask of it.

- **For documentation alone** any machine will do: the documentation lane needs CMake and Git,
  and builds nothing.
- **For Loom's whole lane**, a Linux machine with four cores or more and 8 GB of memory builds and
  runs it in reasonable time. [The tools you need](../guides/tools.md) names the compilers and
  CMake.
- **For Zengine's whole lane**, plan on 16 GB of memory and eight cores, and on tens of gigabytes
  of free disk for its build trees and a compile cache. Its sanitizer lane is the heaviest: its
  memory and time are held by Zengine's own lane rules, and its
  [lane memory tool](https://github.com/Krealsion/Zengine/blob/main/tests/lane_memory.py) reports
  what any lane's processes held at their peak.
- **Each compile job takes memory.** A build that runs more jobs than the memory holds is stopped
  by the system, and a compiler stopped for memory reads as a red. On a machine with less memory,
  lower the job count (`cmake --build build -j2`), build one tree at a time, and start a build
  only when another has ended. The hosted run carries the full lanes, so a smaller machine can
  work on a change and leave the heaviest lanes to it.
- **Builds go on a fast local disk.** A tree on a removable or network drive builds and links
  several times slower, and some file systems keep no permissions, which a session's own files
  need.

## The clone

- **Work in a clone of your own**, separate from any checkout you use day to day, so its running
  programs and its uncommitted work stay untouched. One clone of each repository is enough; check
  out each branch in it and rebuild only what changed.
- **Start each change from the current `main`**: fetch it, branch from it, and name the commit you
  started from in the pull request.
- **Keep build trees outside the clone**, or under the names its `.gitignore` already ignores
  (`build-*/`, and a tree named build). A tree is tied to the paths it was configured with: a tree that
  misbehaves is configured again in place, never moved to be reused.
- **Keep what a run produced** (its logs, its exits, its pictures) apart from the clone and the
  trees, so a rebuild does not take it with it.
- **Delete only what can be made again**: a clone whose work a remote holds, a tree its build
  makes again. Push or save unpushed work first. What a change made goes once its pull request
  merges.

## An agent

- **The start file.** Each repository's root `AGENTS.md` is short and holds no rule of its own:
  it sends its reader to `CONTRIBUTING.md`, then to the front page of the folder the task touches.
  A harness that loads `AGENTS.md` when a session begins gives the agent that path unasked.
  - Codex CLI loads `AGENTS.md` from the repository's root down to the working folder.
  - Claude Code loads `AGENTS.md` where no CLAUDE.md file stands, from version 2.1.277, and a
    folder's `AGENTS.md` when it reads a file in that folder. Its "Project instructions" setting
    chooses between the two files.
  - Cursor and GitHub Copilot's coding agent read `AGENTS.md` too.
  - Gemini CLI reads its own file by default; its context file name can be set to `AGENTS.md` in
    its settings.
  - A harness that loads neither can be told, in its first message, to read `AGENTS.md`.
- **After a summary or a compression** of the session, the agent reads the start file, the guide
  and the folder's front page again before going on, and measures again every number it will
  report: a summary keeps conclusions and drops the evidence behind them.
- **An agent's memory holds routes, never facts**: how the maintainers work where no page says
  it, and where each fact's owner is. A fact copied into memory goes stale while its owner moves.
  Stale memory is fixed by taking away, and a copy is replaced by a route.
- **Keep a short "resume here" note** for work that spans sessions: the clone and branch with its
  last tip, what is uncommitted, the open question and the next action, what is still running,
  and the evidence so far with the commit each piece ran at.
- **No credit line.** A harness that appends a co-author line or a credit to a commit or a pull
  request's description has a setting to stop it; turn it off. The attribution check refuses
  either ([attribution](../../CONTRIBUTING.md#attribution)).
- **The contributor answers for the work**, whatever ran it: an agent's choices, its subagents
  and its budget are the contributor's own.
