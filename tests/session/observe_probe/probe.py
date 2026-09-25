# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""observe-probe/script -- the observation journey's hands (tests/session/observe_journey.py): a
plan of small steps against the far test host, through a link, and everything that came back.

  {"subscribe": {"shapes": ["ProbeTick@1"], "latest": [], "window": 0, "as": "s"}}
  {"ask": {"verb": "tick", "count": 3, "settle": true, "as": "a"}}
  {"take": {"from": "s", "count": 4, "seconds": 5}}      waits for up to `count` items
  {"drain": {"from": "s"}}                              what has arrived, no waiting
  {"status": {"as": "st"}}                              the relay's rows for this session
  {"release": {"from": "s"}}
  {"hold": "gate"}                                      until <run>/release/<gate> exists
  {"sleep": 0.5}

Every step's record goes to probe.json in order; a refusal is recorded, never raised, so the
journey reads what the far host said."""
import json
import time

from loom_session.tool import LinkOutcome, NotAnswered, Refused

OFFICE = "far.ticker"


def run(ctx):
    link = ctx.inputs.get("link", "far")
    plan = json.loads(ctx.inputs["plan"])
    subs, done = {}, []
    try:
        for step in plan:
            verb = next(iter(step))
            arg = step[verb]
            rec = {"step": verb}
            if verb == "subscribe":
                try:
                    s = ctx.observe(OFFICE, arg.get("shapes", ["ProbeTick@1", "ProbeState@1"]),
                                    via=link, latest=arg.get("latest", []),
                                    window=int(arg.get("window", 0)))
                    subs[arg.get("as", "s")] = s
                    rec.update(ok=True, subscription=s.subscription, relay=s.relay,
                               holder=s.holder, incarnation=s.incarnation, window=s.window)
                except Refused as err:
                    rec.update(ok=False, refused=str(err))
            elif verb == "ask":
                try:
                    a = ctx.ask(OFFICE, "ObserveProbeCommand",
                                {"verb": arg["verb"], "count": int(arg.get("count", 0))},
                                via=link, settle=bool(arg.get("settle", False)), timeout=30.0)
                    rec.update(ok=True, answer=a.shape, correlation=a.correlation)
                except (Refused, LinkOutcome, NotAnswered) as err:
                    rec.update(ok=False, error="%s: %s" % (type(err).__name__, err))
            elif verb == "take":
                s = subs[arg["from"]]
                items, end = [], time.monotonic() + float(arg.get("seconds", 5))
                while len(items) < int(arg.get("count", 1)) and time.monotonic() < end:
                    item = s.next(max(0.0, end - time.monotonic()))
                    if item is None:
                        break
                    items.append(item.record())
                    if item.kind == "ended":
                        break
                rec.update(items=items)
            elif verb == "drain":
                rec.update(items=[i.record() for i in subs[arg["from"]].drain()])
            elif verb == "status":
                try:
                    st = ctx.ask("loom.observe", "loom.observe.StatusRequested", {}, via=link)
                    rec.update(ok=True, rows=[dict(r) for r in st["rows"]])
                except (Refused, LinkOutcome) as err:
                    rec.update(ok=False, error=str(err))
            elif verb == "release":
                rec.update(ended=subs[arg["from"]].release().record())
            elif verb == "summary":
                rec.update(summary=subs[arg["from"]].summary())
            elif verb == "hold":
                ctx.hold(arg)
            elif verb == "sleep":
                time.sleep(float(arg))
            done.append(rec)
    finally:
        for name, s in subs.items():
            done.append({"step": "final", "as": name, "summary": s.summary()})
        ctx.produce("probe.json", json.dumps(done, indent=1).encode())
    return "%d step(s)" % len(plan)
