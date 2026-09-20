# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""A cleanup that cannot finish, so that a blocked cleanup phase can be looked at.

Its cleanup waits at a release gate the journey never opens. Nothing here can end that wait
except the cleanup phase's own budget or somebody stopping the process; the session host is a
different process and goes on answering every client throughout.
"""


def run(ctx):
    ctx.cleanup_seconds = float(ctx.inputs.get("budget", 60))

    def never():
        ctx.hold("never-opened", "a cleanup waiting at a gate nobody opens")

    ctx.on_cleanup(never, "wait at a gate nobody opens")
    ctx.hold(ctx.inputs.get("hold", "go"), "waiting with a cleanup that cannot finish")
    return "released without being cancelled"
