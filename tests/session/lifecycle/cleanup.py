# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""Cleanups that SPEAK, so the cleanup phase is exercised where it actually matters.

A cleanup that only writes a file proves nothing about a cancelled run's authority: the file
system was never in question. These two ask the bus. One asks the session door, which this run
was granted; the other asks an office it was never granted, which the bus refuses. Both run
after the cancellation that ended the tool's ordinary work, and each is recorded as what it
came to -- `done` for the one the door answered, the bus's own refusal for the other.
"""

from loom_session.tool import DispatchRefused


def run(ctx):
    said = {"described": 0}

    def describe():
        d = ctx.ask("loom.session", "loom.session.Describe", {}, timeout=20.0)
        said["described"] += 1
        ctx.note("cleanup asked the door and it answered: lifetime %s" % d["lifetime"][:12])

    def not_granted():
        try:
            ctx.ask("loom.history", "loom.history.DeliveriesTo",
                    {"participant": 0, "shape": "", "limit": 1}, timeout=20.0)
        except DispatchRefused as err:
            # THE BUS'S OWN WORD, carried out of the cleanup as the cleanup's outcome.
            raise RuntimeError("the bus refused this cleanup's ask: %s" % err)

    if ctx.inputs.get("refuse", True):
        ctx.on_cleanup(not_granted, "ask an office this run was never granted")
    ctx.on_cleanup(describe, "ask the door, which this run may ask")
    ctx.hold(ctx.inputs.get("hold", "go"), "waiting with two bus cleanups registered")
    return "released without being cancelled; the cleanups ran anyway (described %d)" % (
        said["described"],)
