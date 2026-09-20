# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""Asks, and then says NOTHING until a file appears.

The quiet is the point. A run's record claims to hold the view a client is being given; between
an acknowledged ask and whatever the tool says next there is an interval in which nothing else
would notice a claim that had stopped being true. This tool makes that interval as long as its
driver wants, and it waits at its gates WITHOUT a progress report -- a report would save the
record and answer the question by accident.
"""

import os
import time


def run(ctx):
    seconds = float(ctx.inputs.get("seconds", 120))
    ctx.step("ask")
    ctx.ask("loom.session", "loom.session.Describe", {})
    _quiet_until(ctx, "again", seconds)
    ctx.ask("loom.session", "loom.session.Describe", {})
    _quiet_until(ctx, ctx.inputs.get("hold", "go"), seconds)
    return "asked twice, and said nothing at all in between"


def _quiet_until(ctx, gate, seconds):
    path = os.path.join(ctx.run_dir, "release", gate)
    deadline = time.monotonic() + seconds
    while not os.path.exists(path) and time.monotonic() < deadline:
        time.sleep(0.05)
