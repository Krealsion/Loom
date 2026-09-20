# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""basics/steps -- a journey with no application behind it (see loom-tool.json)."""

import os
import time


def run(ctx):
    count = int(ctx.inputs.get("count", 3))
    ctx.check(1 <= count <= 20, "count must be 1..20, not %d" % count)
    done = []

    def cleanup():
        # Runs however the tool ends; proves which way it went.
        with open(os.path.join(ctx.out_dir, "cleanup.txt"), "w", encoding="utf-8") as f:
            f.write("cleaned up after %d step(s)\n" % len(done))

    ctx.on_cleanup(cleanup, "write cleanup.txt")
    for i in range(1, count + 1):
        ctx.step("step %d of %d" % (i, count))
        if i == int(ctx.inputs.get("fail_at", 0)):
            ctx.fail("failing at step %d, as asked" % i)
        if i == int(ctx.inputs.get("crash_at", 0)):
            raise RuntimeError("a tool bug at step %d, as asked" % i)
        done.append(i)
        if i == 1 and ctx.inputs.get("hold"):
            ctx.hold(ctx.inputs["hold"], "held after step 1")
        if i == 1 and int(ctx.inputs.get("cooperate", 0)) > 0:
            asked = work_until_asked_to_stop(ctx, int(ctx.inputs["cooperate"]))
            if asked:
                # COOPERATING IS THE TOOL'S OWN DECISION, and so is the verdict it gives after
                # it: this run ends `passed` with `cancel_requested` set, not `cancelled`.
                # (Raising `loom_session.tool.Cancelled` here would end it `cancelled` instead.)
                text = "".join("step %d done\n" % n for n in done) + "stopped when asked\n"
                ctx.produce("report.txt", text.encode("utf-8"))
                return "asked to stop during step 1's work: %d step(s), nothing left half done" % (
                    len(done),)
    text = "".join("step %d done\n" % i for i in done) + ctx.inputs.get("message", "") + "\n"
    ctx.produce("report.txt", text.encode("utf-8"))
    return "%d step(s); report.txt written" % len(done)


def work_until_asked_to_stop(ctx, seconds):
    """Work in a loop that WAITS AT NOTHING of its own -- no ask, no gate, no blocking call --
    and still notices a cancellation. `ctx.cancel_requested` reads whatever has ALREADY arrived
    on the session; it does not wait for a cancellation and does not raise `Cancelled`, and once
    true it stays true. (Reading a session is not free: see its own note.) True when asked."""
    deadline = time.monotonic() + seconds
    turns = 0
    while time.monotonic() < deadline:
        if ctx.cancel_requested:
            ctx.note("asked to stop after %d turn(s) of work; finishing this one and returning"
                     % turns)
            return True
        turns += 1
        time.sleep(0.01)  # a turn of this tool's own work
    return False
