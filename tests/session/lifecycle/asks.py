# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""Asks, and then says NOTHING until a file appears.

The quiet is the point. A run's record claims to hold the view a client is being given; between
an acknowledged ask and whatever the tool says next there is an interval in which nothing else
would notice a claim that had stopped being true. This tool makes that interval as long as its
driver wants, and it waits at its gates WITHOUT a progress report -- a report would save the
record and answer the question by accident.

WHICH IS WHY IT DOES NOT CALL ``ctx.ask``. That one reports an ask as pending once it has been
waiting 0.15s for the answer, and reports progress AGAIN when it stops waiting (``_after_wait``)
-- a save that would cover the AskReport whether or not the AskReport itself saved anything, and
so an interval that is quiet only while the answers happen to be quick. The product's
pending-progress is useful and is left exactly as it is; this tool collects its answer itself,
so that the quiet after an AskReport is quiet however long the answer took. ``slow`` makes it
take long on purpose, so the witness runs in the condition it was hiding from.
"""

import os
import time


def run(ctx):
    seconds = float(ctx.inputs.get("seconds", 120))
    slow = float(ctx.inputs.get("slow", 0)) / 1000.0
    ctx.step("ask")
    _ask_quietly(ctx, slow)
    _quiet_until(ctx, "again", seconds)
    _ask_quietly(ctx, slow)
    _quiet_until(ctx, ctx.inputs.get("hold", "go"), seconds)
    return "asked twice, and said nothing at all in between"


def _ask_quietly(ctx, slow):
    """Ask, be busy for ``slow`` seconds, and only then collect the answer.

    ``Pending.wait`` reports an ask as pending when IT has been waiting; by the time this calls
    it the answer is already in hand, so the only thing the manager hears about this ask is its
    AskReport -- whatever the answer cost. That is the whole of the repair.
    """
    p = ctx.ask_async("loom.session", "loom.session.Describe", {})
    started = time.monotonic()
    while not p.done() or time.monotonic() - started < slow:
        time.sleep(0.01)
    return p.wait(60.0)


def _quiet_until(ctx, gate, seconds):
    path = os.path.join(ctx.run_dir, "release", gate)
    deadline = time.monotonic() + seconds
    while not os.path.exists(path) and time.monotonic() < deadline:
        time.sleep(0.05)
