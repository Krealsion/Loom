# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""THE OBSERVATION JOURNEY, AS INDEPENDENT PROCESSES: a subscription across two real hosts.

A real `loom-host --serve` boots the real run manager and the probe vocabulary, and links twice
to a real far host (tests/observe_far/far_host.cpp) under two guest credentials. The far host's
relay lets only one of them observe its producer, and only two shapes. Real Python workers run
the probe package (tests/session/observe_probe) as named runs; this driver starts them, waits,
and reads what each run recorded and what the run manager says. Nothing here fakes a host, a
link, a relay or a worker.

    observe_journey.py --host <loom-host> --runs <loom-runs> --runtime <dir with loom_session>
                       --far <zen-observe-far-host> --vocab <zen_observe_probe_vocab library>
                       --probe <tests/session/observe_probe> --work <dir> [--evidence <file>]

What it proves, each against the other process's real behaviour: a guest the far policy does not
name, and a shape it does not list, are refused in the far host's words and told nothing; a
settled ask's observations arrive in order, numbered from 1, marked with the asker's own
correlation, before its answer; a late subscriber is told nothing from before it began, and
another run's send is never its cause; a burst past a small window is dropped, counted exactly and
said, while the state shape keeps its newest; a publication by a participant not holding the
office is never told, and a replaced holder is visible; the far host's revocation ends the
subscription in words; a cancelled run's cleanup frees its subscription, and the next run on the
same link starts clean; a client that stops waiting leaves the run to its manager; a worker that
goes without releasing -- stopped outright with its producer silent, or exiting with its window
full, more times than the far relay's allowance for one session -- is released by the link on the
host's own turn, and a fresh run still subscribes; and when the far host dies the link says so
(`lost`), and a new session after it is a new epoch and a new relay lifetime -- nothing of the
old one continues. Exit 0 only when every check held.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

# The far relay's bound on subscriptions held for one subscriber (zen/observe/relay.hpp
# kMaxPerSubscriber): every run on one link is that one subscriber there.
PER_SUBSCRIBER = 8

CHECKS = []
PROCS = []


def check(name, condition, detail=""):
    CHECKS.append({"check": name, "ok": bool(condition), "detail": str(detail)[:2000]})
    shown = str(detail).splitlines()[0][:300] if detail else ""
    print("%s  %s%s" % ("ok  " if condition else "FAIL", name, ("  -- " + shown) if shown else ""),
          flush=True)
    return bool(condition)


def until(predicate, seconds, what):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            if predicate():
                return True
        except Exception:
            pass
        time.sleep(0.05)
    print("     (the driver stopped waiting for %s after %.0fs)" % (what, seconds), flush=True)
    return False


def main():
    ap = argparse.ArgumentParser()
    for a in ("host", "runs", "runtime", "far", "vocab", "probe", "work"):
        ap.add_argument("--" + a, required=True)
    ap.add_argument("--evidence", default="")
    args = ap.parse_args()
    for name in ("host", "runs", "runtime", "far", "vocab", "probe", "work"):
        setattr(args, name, os.path.abspath(getattr(args, name)))
    sys.path.insert(0, args.runtime)
    from loom_session.session import Session
    from loom_session.client import NotAnswered

    shutil.rmtree(args.work, ignore_errors=True)
    session_dir = os.path.join(args.work, "session")
    os.makedirs(session_dir)
    port_file = os.path.join(args.work, "far.port")

    def start_far(port=0):
        if os.path.exists(port_file):
            os.remove(port_file)
        log = open(os.path.join(args.work, "far-%d.log" % int(time.time() * 1000)), "wb")
        p = subprocess.Popen([args.far, "--port-file", port_file, "--port", str(port)],
                             stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        PROCS.append(p)
        ok = until(lambda: os.path.exists(port_file) and open(port_file).read().strip(), 30,
                   "the far host's port")
        return p, (int(open(port_file).read().strip()) if ok else 0)

    far, port = start_far()
    if not check("F0 the far host listens", port > 0, port):
        return finish(args)
    with open(os.path.join(session_dir, "loom-boot.json"), "w", encoding="utf-8") as f:
        json.dump({"boot": [{"name": "runs", "path": args.runs, "role": "loom.runs"},
                            {"name": "vocab", "path": args.vocab}],
                   "links": [{"name": "far", "connect": "127.0.0.1:%d" % port,
                              "identity": "journey", "credential": "agent-cred"},
                             {"name": "other", "connect": "127.0.0.1:%d" % port,
                              "identity": "journey-other", "credential": "other-cred"}],
                   "history": {"log": "session.log", "recent": "4096"}}, f, indent=1)
    with open(os.path.join(session_dir, "loom-tools.json"), "w", encoding="utf-8") as f:
        json.dump({"python": sys.executable, "runtime": args.runtime,
                   "packages": [{"path": args.probe, "approve": "any-revision"}]}, f, indent=1)
    approvals = [
        "authority trust runs --rebuilds", "authority trust vocab --rebuilds",
        "authority allow runs loom.session.ExpectRun v1 -> role loom.session",
        "authority allow runs loom.session.ForgetRun v1 -> role loom.session",
        "authority allow runs loom.runs.Tools v1 -> any target",
        "authority allow runs loom.runs.ToolDescription v1 -> any target",
        "authority allow runs loom.runs.Run v1 -> any target",
        "authority allow runs loom.runs.RunList v1 -> any target",
        "authority allow runs loom.runs.Directive v1 -> any target",
        "authority allow runs loom.link.Ask v1 -> role loom.link.far",
        "authority allow runs loom.link.Ask v1 -> role loom.link.other",
        "start vocab %s" % args.vocab, "start runs %s loom.runs" % args.runs]
    host_log = open(os.path.join(session_dir, "host.log"), "wb")
    # ITS CONSOLE STAYS OPEN: a link reconnects when its operator says so (`links connect`).
    host = subprocess.Popen([args.host, "--serve", session_dir], stdin=subprocess.PIPE,
                            stdout=host_log, stderr=subprocess.STDOUT, cwd=session_dir)
    PROCS.append(host)
    host.stdin.write(("\n".join(approvals) + "\n").encode())
    host.stdin.flush()

    def links():
        with Session.attach(session_dir) as s:
            return dict((r["name"], r["state"]) for r in s.describe()["links"])

    def carried(name="far"):
        """The host console's own line for link `name`: (open, watched, releasing), or None."""
        path = os.path.join(session_dir, "host.log")
        start = os.path.getsize(path)
        host.stdin.write(b"links\n")
        host.stdin.flush()
        pattern = re.compile(r"(?:^|\s)%s\s.*\((\d+) open, (\d+) watched(?:, (\d+) releasing)?\)"
                             % re.escape(name))
        seen = {}

        def read():
            with open(path, "rb") as f:
                f.seek(start)
                for line in f.read().decode("utf-8", "replace").splitlines():
                    m = pattern.search(line)
                    if m:
                        seen["line"] = (int(m.group(1)), int(m.group(2)), int(m.group(3) or 0))
                        return True
            return False

        until(read, 20, "the console's line for link %s" % name)
        return seen.get("line")

    def released_all(name="far", seconds=30):
        """Until the link carries no subscription at all, as its console says."""
        last = {}

        def none():
            last["line"] = carried(name)
            return last["line"] is not None and last["line"][1] == 0

        return until(none, seconds, "link %s to carry nothing" % name), last.get("line")

    if not check("F1 the session serves, both links admitted by the far host",
                 until(lambda: links() == {"far": "admitted", "other": "admitted"}, 60,
                       "both links"), links() if host.poll() is None else "host ended"):
        return finish(args)

    def run(s, name, plan, link="far", wait=120):
        s.start("observe-probe/script", name, {"link": link, "plan": json.dumps(plan)})
        r = s.wait(name, timeout=wait)
        steps = []
        path = os.path.join(r.get("directory", ""), "out", "probe.json")
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                steps = json.load(f)
        return r, steps

    def observed(items, shape=None):
        return [i for i in items if i["kind"] == "observed" and (shape is None or i["shape"] == shape)]

    with Session.attach(session_dir) as s:
        # ---- R1 authority: asking is not being admitted ------------------------------------
        r, st = run(s, "refused-other", [{"subscribe": {}}], link="other")
        check("R1 a guest the far policy does not name is refused in the far host's words",
              r["state"] == "passed" and st and st[0].get("ok") is False and
              "only 'agent' observe; the asker is 'other'" in st[0].get("refused", ""), st[:1])
        r, st = run(s, "refused-shape", [{"subscribe": {"shapes": ["ProbeTick@1", "ObserveProbeCommand@1"]}}])
        check("R2 a named guest asking one shape the policy does not list is refused whole",
              st and st[0].get("ok") is False and
              "not observable here: ObserveProbeCommand v1" in st[0].get("refused", ""), st[:1])

        # ---- R3 ready means ready; cause; order; before the answer -------------------------
        r, st = run(s, "ready", [{"subscribe": {"latest": ["ProbeState"]}},
                                 {"ask": {"verb": "tick", "count": 3, "settle": True}},
                                 {"drain": {"from": "s"}}, {"release": {"from": "s"}}])
        ok = r["state"] == "passed" and len(st) >= 4
        corr = st[1].get("correlation") if ok else None
        items = st[2]["items"] if ok else []
        check("R3 a settled ask's observations are all here when its answer is: numbered from 1, in "
              "the producer's order, each marked with the asker's own correlation",
              ok and [i["seq"] for i in items] == [1, 2, 3, 4] and
              [i["fields"].get("n", i["fields"].get("last")) for i in items] == [1, 2, 3, 3] and
              [i["shape"] for i in items] == ["ProbeTick"] * 3 + ["ProbeState"] and
              all(i["cause"] == corr for i in items), (corr, items))
        check("R4 released at the asker's request: Ended `released`, the last number named",
              ok and st[3]["ended"]["how"] == "released" and st[3]["ended"]["last"] == 4, st[3:4])
        # THE PRODUCER'S OWN NUMBERING carries on across runs (it is the office's): what the next
        # checks expect is counted from here.
        said = items[-1]["fields"]["last"] if items else 0

        # ---- R5 a late subscriber: nothing replayed, another's send never its cause ---------
        s.start("observe-probe/script", "late-a", {"plan": json.dumps([
            {"ask": {"verb": "tick", "count": 5, "settle": True}}, {"hold": "go"},
            {"ask": {"verb": "tick", "count": 2, "settle": True}}])})
        until(lambda: s.run("late-a")["step"] == "held: go", 60, "late-a to hold")
        s.start("observe-probe/script", "late-b", {"plan": json.dumps([
            {"subscribe": {}}, {"hold": "b-go"}, {"take": {"from": "s", "count": 3, "seconds": 10}},
            {"release": {"from": "s"}}])})
        until(lambda: s.run("late-b")["step"] == "held: b-go", 60, "late-b to subscribe and hold")
        for name, gate in (("late-a", "go"), ("late-b", "b-go")):
            os.makedirs(os.path.join(s.run(name)["directory"], "release"), exist_ok=True)
            open(os.path.join(s.run(name)["directory"], "release", gate), "w").close()
            if name == "late-a":
                s.wait("late-a", timeout=60)
        rb = s.wait("late-b", timeout=60)
        with open(os.path.join(rb["directory"], "out", "probe.json"), encoding="utf-8") as f:
            stb = json.load(f)
        took = stb[2]["items"] if len(stb) > 2 else []
        check("R5 a late subscriber begins where it asked: the 5 ticks before it are not replayed, "
              "the next arrive numbered from 1, and another run's send is never its cause",
              [i["seq"] for i in took] == [1, 2, 3] and
              [i["fields"].get("n", i["fields"].get("last")) for i in took] ==
              [said + 6, said + 7, said + 7] and all(i["cause"] == 0 for i in took), (said, took))

        # ---- R6 a burst past a small window: dropped, counted exactly, said -----------------
        r, st = run(s, "burst", [{"subscribe": {"window": 8, "latest": ["ProbeState"]}},
                                 {"ask": {"verb": "burst", "count": 200, "settle": True}},
                                 {"take": {"from": "s", "count": 30, "seconds": 5}},
                                 {"summary": {"from": "s"}}, {"release": {"from": "s"}}])
        items = st[2]["items"] if len(st) > 2 else []
        ticks = observed(items, "ProbeTick")
        gaps = [i for i in items if i["kind"] == "gap"]
        state = observed(items, "ProbeState")
        first = ticks[0]["fields"]["n"] if ticks else 0
        check("R6 a burst past the window: the ticks told plus the Gap's count are exactly the 200 "
              "published, the Gap is the relay's own and said before anything later, and the "
              "state kept its newest",
              len(ticks) + sum(g["lost"] for g in gaps) == 200 and gaps and not gaps[0]["local"] and
              [t["fields"]["n"] for t in ticks] == list(range(first, first + len(ticks))) and
              state and state[-1]["fields"]["last"] == first + 199 and
              [i["seq"] for i in items] == list(range(1, len(items) + 1)),
              (len(ticks), gaps, state[-1:] if state else None))

        # ---- R7 the office's holder only; a replaced holder is visible ----------------------
        r, st = run(s, "identity", [{"subscribe": {}},
                                    {"ask": {"verb": "tick", "count": 1, "settle": True}},
                                    {"ask": {"verb": "foreign", "settle": True}}, {"sleep": 0.5},
                                    {"ask": {"verb": "replace", "settle": True}}, {"sleep": 0.5},
                                    {"ask": {"verb": "tick", "count": 1, "settle": True}},
                                    {"drain": {"from": "s"}}, {"status": {}},
                                    {"release": {"from": "s"}}])
        drained = observed(st[7]["items"]) if len(st) > 7 else []
        ticks = [i for i in drained if i["shape"] == "ProbeTick"]
        rows = st[8].get("rows", []) if len(st) > 8 else []
        check("R7 a publication by a participant not holding the office is never told (the relay "
              "counts it foreign), and a replaced holder shows as another producer",
              len(ticks) == 2 and all(t["fields"]["n"] > 0 for t in ticks) and
              ticks[0]["producer"] != ticks[1]["producer"] and
              any(row.get("foreign", 0) >= 1 for row in rows),
              ([(t["fields"], t["producer"], t["incarnation"]) for t in ticks], rows))

        # ---- R8 revocation, said -----------------------------------------------------------
        r, st = run(s, "revoke", [{"subscribe": {}}, {"ask": {"verb": "revoke", "settle": True}},
                                  {"sleep": 0.5}, {"take": {"from": "s", "count": 1, "seconds": 5}}])
        took = st[3]["items"] if len(st) > 3 else []
        check("R8 the far host's revocation ends the subscription in words, and nothing follows it",
              took and took[0]["kind"] == "ended" and took[0]["how"] == "revoked" and
              "withdrew" in took[0]["reason"], took)

        # ---- R9 cancellation frees; the next task starts clean -------------------------------
        s.start("observe-probe/script", "held", {"plan": json.dumps([{"subscribe": {}},
                                                                    {"hold": "never"}])})
        until(lambda: s.run("held")["step"] == "held: never", 60, "the held run")
        asked = s.cancel("held", "observe journey: cancel a subscriber")
        rc = s.wait("held", timeout=60)
        cleanup = [n for n in rc.get("notes", []) if n.startswith("cleanup")]
        check("R9 a cancellation is a request, then an ending: the run ends cancelled and its "
              "cleanup released its subscription, done",
              asked.get("cancel_requested") and rc["state"] == "cancelled" and
              any("release subscription" in n and n.endswith(": done") for n in cleanup), cleanup)
        r, st = run(s, "after", [{"status": {}}, {"subscribe": {}},
                                 {"ask": {"verb": "tick", "count": 1, "settle": True}},
                                 {"drain": {"from": "s"}}, {"release": {"from": "s"}}])
        rows = st[0].get("rows", []) if st else []
        items = st[3]["items"] if len(st) > 3 else []
        check("R10 the next run on the same link finds no subscription left behind, and its own "
              "begins at 1 with only its own cause",
              rows == [] and [i["seq"] for i in items] == [1, 2] and
              all(i["cause"] == st[2]["correlation"] for i in items), (rows, items))

        # ---- R11 a client that stops waiting leaves the run to its manager ------------------
        s.start("observe-probe/script", "slow", {"plan": json.dumps([{"hold": "go"}])})
        try:
            s.wait("slow", timeout=1.0)
            stopped = False
        except NotAnswered:
            stopped = True
        still = s.run("slow")["state"]
        d = s.run("slow")["directory"]
        os.makedirs(os.path.join(d, "release"), exist_ok=True)
        open(os.path.join(d, "release", "go"), "w").close()
        later = s.wait("slow", timeout=60)["state"]
        check("R11 a client's wait that runs out is the client's decision: the run stays running, "
              "and a later wait finds it passed", stopped and still in ("starting", "running") and
              later == "passed", (stopped, still, later))

        # ---- R14 a subscriber stopped outright while its producer is silent ----------------
        s.start("observe-probe/script", "gone-silent", {"plan": json.dumps([
            {"subscribe": {}}, {"hold": "never"}])})
        until(lambda: s.run("gone-silent")["step"] == "held: never", 60, "the silent subscriber")
        holding = carried()
        asked = s.cancel("gone-silent", "observe journey: stop a subscriber outright", force=True)
        rg = s.wait("gone-silent", timeout=60)
        released, line = released_all()
        r, st = run(s, "after-silent", [{"status": {}}])
        rows = st[0].get("rows") if st else None
        check("R14 a subscriber stopped outright -- no cleanup ran -- while its producer says nothing: "
              "its process ended, and the link released its far subscription on the host's own turn; "
              "the far relay holds nothing for the session and the link carries nothing",
              holding is not None and holding[1] == 1 and asked.get("cancel_requested") and
              rg["state"] == "cancelled" and rg.get("process") in ("killed", "exited") and
              not any(n.startswith("cleanup") for n in rg.get("notes", [])) and released and
              rows == [], (holding, rg["state"], rg.get("process"), line, rows))

        # ---- R15 a worker that exits with its window full -----------------------------------
        s.start("observe-probe/script", "gone-full", {"plan": json.dumps([
            {"subscribe": {"window": 1}}, {"ask": {"verb": "tick", "count": 2, "settle": True}},
            {"exit": 7}])})
        rf = s.wait("gone-full", timeout=60)
        with open(os.path.join(rf["directory"], "out", "probe.json"), encoding="utf-8") as f:
            stf = json.load(f)
        released, line = released_all()
        r, st = run(s, "after-full", [{"ask": {"verb": "tick", "count": 20, "settle": True}},
                                      {"status": {}}])
        rows = st[1].get("rows") if len(st) > 1 else None
        check("R15 a worker that exits with its window full -- a reader nothing can wake -- is released "
              "without another word reaching it; twenty more publications find nothing held",
              stf and stf[0].get("ok") and stf[0].get("window") == 1 and
              rf["state"] == "crashed" and rf.get("process") == "exited" and released and
              rows == [], (stf[:2], rf["state"], rf.get("process"), line, rows))

        # ---- R16 more abandoned workers than the far relay's allowance for one session ------
        abandoned = []
        for i in range(PER_SUBSCRIBER + 1):
            name = "gone-%d" % i
            s.start("observe-probe/script", name, {"plan": json.dumps([
                {"subscribe": {"window": 1}}, {"ask": {"verb": "tick", "count": 1, "settle": True}},
                {"exit": 3}])})
            ri = s.wait(name, timeout=60)
            with open(os.path.join(ri["directory"], "out", "probe.json"), encoding="utf-8") as f:
                sti = json.load(f)
            abandoned.append((name, bool(sti and sti[0].get("ok")), sti[0].get("refused", "") if sti
                              else "no record", ri["state"], ri.get("process")))
        released, line = released_all()
        r, st = run(s, "fresh", [{"subscribe": {}}, {"ask": {"verb": "tick", "count": 1, "settle": True}},
                                 {"drain": {"from": "s"}}, {"release": {"from": "s"}}])
        items = st[2]["items"] if len(st) > 2 else []
        check("R16 %d workers abandon their subscriptions on one link -- more than the far relay holds "
              "for one session -- and each is admitted, because each abandoned one was released; a "
              "fresh run then subscribes and is told" % (PER_SUBSCRIBER + 1),
              all(a[1] and a[3] == "crashed" and a[4] == "exited" for a in abandoned) and released and
              st and st[0].get("ok") and [i["seq"] for i in items] == [1, 2],
              (abandoned, line, st[:1], [(i["seq"], i["shape"]) for i in items]))

        # ---- R12 the far host dies; a new session is a new epoch and a new relay ------------
        before = run(s, "before-loss", [{"subscribe": {}}, {"release": {"from": "s"}}])[1]
        relay_before = before[0].get("relay") if before else None
        s.start("observe-probe/script", "lost", {"plan": json.dumps([
            {"subscribe": {}}, {"ask": {"verb": "quit"}},
            {"take": {"from": "s", "count": 1, "seconds": 20}}])})
        rl = s.wait("lost", timeout=90)
        with open(os.path.join(rl["directory"], "out", "probe.json"), encoding="utf-8") as f:
            stl = json.load(f)
        took = stl[2]["items"] if len(stl) > 2 else []
        check("R12 when the far host dies, the link says so: the subscription ends `lost`, and the "
              "run does not claim to know what was said after", until(lambda: far.poll() is not None,
                                                                     30, "the far host to end") and
              took and took[0]["kind"] == "ended" and took[0]["how"] == "lost", took)
    far2, port2 = start_far(port)
    host.stdin.write(b"links connect far\n")
    host.stdin.flush()
    until(lambda: links().get("far") == "admitted", 60, "the link to reconnect")
    with Session.attach(session_dir) as s:
        r, st = run(s, "again", [{"subscribe": {}}, {"ask": {"verb": "tick", "count": 1, "settle": True}},
                                 {"drain": {"from": "s"}}, {"release": {"from": "s"}}])
        items = st[2]["items"] if len(st) > 2 else []
        check("R13 after the far host restarts and the link reconnects, a new subscription is a new "
              "relay lifetime on a new epoch, numbered from 1: nothing of the old one continues",
              port2 == port and st and st[0].get("ok") and st[0].get("relay") != relay_before and
              [i["seq"] for i in items] == [1, 2] and all(i["epoch"] >= 2 for i in items),
              (relay_before, st[:1], [(i["seq"], i["epoch"]) for i in items]))
    return finish(args)


def finish(args):
    for p in reversed(PROCS):
        if p.poll() is None:
            if p.stdin is None:
                p.kill()  # a far host of this driver's own: it has no console to be asked through
            else:
                try:
                    p.stdin.write(b"quit\n")
                    p.stdin.flush()
                    p.stdin.close()
                except OSError:
                    pass
            try:
                p.wait(timeout=20)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=20)
    failed = [c for c in CHECKS if not c["ok"]]
    if args.evidence:
        with open(args.evidence, "w", encoding="utf-8") as f:
            json.dump({"checks": CHECKS}, f, indent=1)
    print("%d of %d checks held" % (len(CHECKS) - len(failed), len(CHECKS)), flush=True)
    return 0 if CHECKS and not failed else 1


if __name__ == "__main__":
    sys.exit(main())
