# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""basics/session-report -- what this session is, in its door's own words (see loom-tool.json)."""

import json


def run(ctx):
    office = ctx.inputs.get("office") or "loom.session"
    # Partial work first, on purpose: a refusal below comes AFTER something was already done,
    # and the run's record shows both.
    ctx.step("record the question")
    ctx.produce("request.txt", ("asking %s what this session is\n" % office).encode("utf-8"))
    ctx.step("ask " + office)
    d = ctx.ask(office, "loom.session.Describe", {})
    ctx.check(d.shape == "loom.session.Description",
              "%s answered %s, not loom.session.Description" % (office, d.shape))
    ctx.check(d["lifetime"] == ctx.lifetime,
              "the door describes lifetime %s, but this run belongs to %s"
              % (d["lifetime"], ctx.lifetime))
    me = [c for c in d["connections"] if c["name"] == "run:" + ctx.name]
    ctx.check(me and me[0]["state"] == "admitted" and me[0]["kind"] == "run",
              "the door does not list this run's own session as an admitted run")
    ctx.step("write the report")
    ctx.produce("session.json", json.dumps(d.fields, indent=1).encode("utf-8"))
    return "lifetime %s: %d connection(s), %d link(s); this run is session %d" % (
        d["lifetime"][:8], len(d["connections"]), len(d["links"]), me[0]["session"])
