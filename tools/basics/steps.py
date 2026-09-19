# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""basics/steps -- a journey with no application behind it (see loom-tool.json)."""

import os


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
    text = "".join("step %d done\n" % i for i in done) + ctx.inputs.get("message", "") + "\n"
    ctx.produce("report.txt", text.encode("utf-8"))
    return "%d step(s); report.txt written" % len(done)
