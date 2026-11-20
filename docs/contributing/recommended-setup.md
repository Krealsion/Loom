# The recommended setup

The setup we recommend for working on Loom and [Zengine](https://github.com/Krealsion/Zengine),
whether a person or an agent does the work: the machine, the clone and where builds go, and the
harness an agent runs in. None of it is required. It is what keeps a machine usable while it
builds, and an agent on the path the guide sets. How the work itself is done is
[the guide](../../CONTRIBUTING.md#making-a-change) and [best practices](best-practices.md).

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

## The clone

- **Work in a clone of your own**, separate from any checkout you use day to day, so its running
  programs and its uncommitted work stay untouched. One clone of each repository is enough; check
  out each branch in it and rebuild only what changed.
- **Put the clone and its builds on a fast local disk.** A tree on a removable or network drive
  builds and links several times slower, and some file systems keep no permissions, which a
  session's own files need.
- **Keep build trees outside the clone**, or under the names its `.gitignore` already ignores
  (`build-*/`, and a tree named build). A tree is tied to the paths it was configured with: a tree
  that misbehaves is configured again in place, never moved to be reused.

## The harness

- **The start file.** Each repository's root `AGENTS.md` is short and holds no rule of its own:
  it sends its reader to `CONTRIBUTING.md`, then to the front page of the folder the task touches.
  A harness that loads `AGENTS.md` when a session begins gives the agent that path unasked. Most
  harnesses read `AGENTS.md`; check how yours does.
  - Claude Code reads `AGENTS.md` where no CLAUDE.md file stands, from version 2.1.277, and a
    folder's `AGENTS.md` when it reads a file in that folder.
  - A harness that does not load it can be told, in its first message, to read `AGENTS.md`.
- **No credit line.** A harness that appends a co-author line or a credit to a commit or a pull
  request's description has a setting to stop it; turn it off. The attribution check refuses
  either ([attribution](../../CONTRIBUTING.md#attribution)).
