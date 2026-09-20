# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""A DIRECT ask that asks to be settled, through the real bridge, of an office whose answering
sets more work in motion on the host's own bus.

Starting a run is such an office: the manager does not answer at once -- it registers the run
with the session door first, waits for the door's answer, starts the worker process, and only
then answers the client that asked. All of that is what THIS send set in motion, and the host's
settlement for this send is its word that all of it has been dispatched.

What is written out is what the conversation actually had at the moment it completed: whether
the attested answer was there, whether the settlement was, and whether the client would have
called it complete on the answer alone.
"""

import json
import time


def run(ctx):
    name = ctx.inputs["name"]
    tool = ctx.inputs.get("tool", "basics/steps")
    ctx.step("ask for a run, asking for the send to be settled")
    pending = ctx.ask_async("loom.runs", "loom.runs.Start",
                            {"tool": tool, "name": name, "inputs": json.dumps({"count": 1})},
                            settle=True)
    corr = pending.correlation
    answer_seen_at = None
    settled_seen_at = None
    started = time.monotonic()
    # ZERO-TIMEOUT POLLS: each one services whatever has arrived and waits for nothing.
    while not pending.done():
        if answer_seen_at is None and ctx.conn.answered(corr):
            answer_seen_at = time.monotonic() - started
        if settled_seen_at is None and ctx.conn.settled(corr):
            settled_seen_at = time.monotonic() - started
        if time.monotonic() - started > 60.0:
            ctx.fail("the Start asked with settle=True did not complete within 60s")
        time.sleep(0.005)
    if answer_seen_at is None:
        answer_seen_at = time.monotonic() - started
    if settled_seen_at is None:
        settled_seen_at = time.monotonic() - started
    settled_at_completion = ctx.conn.settled(corr)
    answer = pending.wait(5.0)
    ctx.produce("settlement.json", json.dumps({
        "shape": answer.shape,
        "run": answer.get("name", ""),
        "pid": answer.get("pid", 0),
        "settled_at_completion": bool(settled_at_completion),
        "answer_seen_s": answer_seen_at,
        "settled_seen_s": settled_seen_at,
    }).encode("utf-8"))
    ctx.check(settled_at_completion,
              "the conversation was complete while its settlement had not arrived")
    ctx.check(answer.shape == "loom.runs.Run",
              "the answer was %s, not the run that was asked for" % answer.shape)
    return "settled Start of '%s': answer %s, pid %d" % (name, answer.shape, answer.get("pid", 0))
