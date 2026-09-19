# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""THE SESSION JOURNEY, AS INDEPENDENT PROCESSES -- Loom only, no application behind it.

A real `loom-host --serve` (started once and kept), the real `loom-runs` artifact it boots, real
Python workers, and CLIENTS that attach, act and leave; nothing here is a fake of any of them.
Every claim is read from its owner: the run manager's answers and its `run.json` records, the
session door's description, the host's own history through the scoped reader. Waiting is the
harness's own decision and is never reported as anybody's outcome.

    journey.py --host <loom-host> --runs <loom-runs artifact> --runtime <dir with loom_session>
               --tools <tools/basics> --work <empty dir> [--evidence <file.json>]

Exit 0 only when every check held; every check is printed and kept in the evidence file.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

CHECKS = []
HOSTS = []  # every host this driver started; each is ended on every exit path
EVIDENCE = {"path": ""}


def check(name, condition, detail=""):
    CHECKS.append({"check": name, "ok": bool(condition), "detail": detail})
    shown = str(detail).splitlines()[0] if detail else ""
    print("%s  %s%s" % ("ok  " if condition else "FAIL", name, ("  -- " + shown) if shown
                        else ""), flush=True)
    return bool(condition)


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def read_record(run_dir):
    with open(os.path.join(run_dir, "run.json"), "r", encoding="utf-8") as f:
        env = json.load(f)
    return env["fields"]


def until(predicate, seconds, what):
    """The harness's own wait: True when `predicate` held within `seconds`."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            if predicate():
                return True
        except Exception:
            pass
        time.sleep(0.05)
    print("     (the harness stopped waiting for %s after %.0fs)" % (what, seconds), flush=True)
    return False


APPROVALS = [
    "authority trust runs --rebuilds",
    "authority allow runs loom.session.ExpectRun v1 -> role loom.session",
    "authority allow runs loom.session.ForgetRun v1 -> role loom.session",
    "authority allow runs loom.runs.Tools v1 -> any target",
    "authority allow runs loom.runs.ToolDescription v1 -> any target",
    "authority allow runs loom.runs.Run v1 -> any target",
    "authority allow runs loom.runs.RunList v1 -> any target",
    "authority allow runs loom.runs.Directive v1 -> any target",
    "authority allow runs loom.session.Describe v1 -> role loom.session",
]


def start_host(args, session_dir, console_lines):
    """A session host serving `session_dir`; `console_lines` are typed at its console, which is
    then closed -- the host keeps serving."""
    log = open(os.path.join(session_dir, "host-%d.log" % int(time.time() * 1000)), "wb")
    proc = subprocess.Popen([args.host, "--serve", session_dir], stdin=subprocess.PIPE,
                            stdout=log, stderr=subprocess.STDOUT, cwd=session_dir)
    proc.stdin.write(("\n".join(console_lines) + "\n").encode("utf-8"))
    proc.stdin.close()
    HOSTS.append(proc)
    return proc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--runs", required=True)
    ap.add_argument("--runtime", required=True)
    ap.add_argument("--tools", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--evidence", default="")
    args = ap.parse_args()
    EVIDENCE["path"] = args.evidence
    for name in ("host", "runs", "runtime", "tools", "work"):
        setattr(args, name, os.path.abspath(getattr(args, name)))
    sys.path.insert(0, args.runtime)
    from loom_session import client as lclient
    from loom_session.session import Session, SessionGone
    from loom_session.client import Refused

    work = os.path.abspath(args.work)
    shutil.rmtree(work, ignore_errors=True)
    session_dir = os.path.join(work, "session")
    pkgs = os.path.join(work, "packages")
    os.makedirs(session_dir)
    # EDITABLE COPIES of the packages: the journey edits them, never the source tree.
    basics = os.path.join(pkgs, "basics")
    shutil.copytree(args.tools, basics, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    pinned = os.path.join(pkgs, "pinned")
    shutil.copytree(args.tools, pinned, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    with open(os.path.join(pinned, "loom-tool.json"), "r", encoding="utf-8") as f:
        manifest = json.load(f)
    manifest["package"] = "pinned"
    with open(os.path.join(pinned, "loom-tool.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    greedy = os.path.join(pkgs, "greedy")
    shutil.copytree(args.tools, greedy, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    manifest["package"] = "greedy"
    for t in manifest["tools"]:
        t["asks"] = ["any shape -> any target"]  # more than the manager may pass on
    with open(os.path.join(greedy, "loom-tool.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    # A package of this driver's own: one tool that asks the door as many times as it is told,
    # for the run record's bound on its account of asks.
    chatty = os.path.join(pkgs, "chatty")
    os.makedirs(chatty)
    with open(os.path.join(chatty, "loom-tool.json"), "w", encoding="utf-8") as f:
        json.dump({"package": "chatty", "version": "1", "summary": "Asks, many times.",
                   "tools": [{"name": "ask", "script": "ask.py", "summary": "Ask the door N times.",
                              "inputs": [{"name": "times", "type": "int", "default": 1,
                                          "help": "how many asks"}],
                              "asks": ["loom.session.Describe v1 -> role loom.session"]}]},
                  f, indent=1)
    with open(os.path.join(chatty, "ask.py"), "w", encoding="utf-8") as f:
        f.write("def run(ctx):\n"
                "    for _ in range(ctx.inputs['times']):\n"
                "        ctx.ask('loom.session', 'loom.session.Describe', {})\n"
                "    return 'asked %d times' % ctx.inputs['times']\n")
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
    catalog = {"python": sys.executable, "runtime": os.path.abspath(args.runtime),
               "packages": [{"path": basics, "approve": "any-revision"},
                            {"path": greedy, "approve": "any-revision"},
                            {"path": pinned, "approve": "PINNED"},
                            {"path": chatty, "approve": "any-revision"}]}

    def write_catalog():
        with open(os.path.join(session_dir, "loom-tools.json"), "w", encoding="utf-8") as f:
            json.dump(catalog, f, indent=1)

    write_catalog()
    with open(os.path.join(session_dir, "loom-boot.json"), "w", encoding="utf-8") as f:
        json.dump({"boot": [{"name": "runs", "path": os.path.abspath(args.runs),
                             "role": "loom.runs"}],
                   "history": {"log": "session.log", "recent": "2048"}}, f, indent=1)

    # ---- A. the session: started once, approved once at its console, kept ------------------
    host = start_host(args, session_dir, APPROVALS + ["start runs %s loom.runs" %
                                                      os.path.abspath(args.runs)])

    def serving():
        from loom_session.session import read_session_file
        info = read_session_file(session_dir)
        if info.get("pid") != host.pid:
            return False
        with Session.attach(session_dir) as s:
            s.tools()
            return True

    if not check("A1 a session host serves the directory, with the run manager loaded",
                 until(serving, 30, "the host to serve")):
        return finish(args, host)
    first_lifetime = None
    host_pid = host.pid

    # ---- B. discovery from a fresh client; reading the catalog runs nothing ---------------------
    with Session.attach(session_dir) as s:
        first_lifetime = s.lifetime
        tools = s.tools()
        ids = [r["id"] for r in tools["rows"]]
        check("B1 the catalog lists the tools", "basics/steps" in ids and
              "basics/session-report" in ids, ids)
        found = [r["id"] for r in s.tools("gate hold")["rows"]]
        check("B2 search narrows by the package's own words", found == ["basics/steps",
                                                                       "pinned/steps",
                                                                       "greedy/steps"] or
              ("basics/steps" in found and "basics/session-report" not in found), found)
        d = s.tool("basics/steps")
        types = dict((i["name"], i["type"]) for i in d["inputs"])
        check("B3 a description names inputs with their types",
              types.get("count") == "int" and types.get("hold") == "text", types)
        check("B4 structural truth comes from the host's schema",
              ("inputs", "Text", True) in [tuple(x) for x in s.shape("loom.runs.Start")])
        pinned_row = [r for r in tools["rows"] if r["id"] == "pinned/steps"][0]
        check("B5 an unapproved package is listed and described, never approved by its path",
              not pinned_row["approved"] and "not approved" in pinned_row["approval"],
              pinned_row["approval"])
        check("B6 listing and describing started no run", s.runs()["rows"] == [] and
              not os.path.exists(os.path.join(session_dir, "runs")))

    # ---- C. a run left pending, the controlling client gone completely ------------------------
    with Session.attach(session_dir) as c1:
        c1_session = c1.connection.session
        r = c1.start("basics/steps", "held", {"count": 3, "hold": "go", "message": "held-run"})
        check("C1 the run starts", r["state"] in ("starting", "running"), r["state"])
        until(lambda: c1.run("held")["step"] == "held: go", 30, "the run to reach its gate")
        held = c1.run("held")
        check("C2 the run is pending at its owner, and says on what",
              held["state"] == "running" and any("release gate 'go'" in p
                                                 for p in held["pending"]), held["pending"])
        c1_last_answer = c1.connection.channel  # closed below
        held_dir = held["directory"]
        manager_id = c1.connection.call("loom.runs.List", {}, role="loom.runs").sender
    # c1 is closed here: no client is attached.
    with open(os.path.join(held_dir, "run.json"), "r", encoding="utf-8") as f:
        on_disk = json.load(f)["fields"]
    check("C3 with no client attached, the manager's own record says running and pending",
          on_disk["state"] == "running" and on_disk["pending"], on_disk["pending"])

    # ---- D. advance it with no controller: the gate opens, the run finishes ------------------
    os.makedirs(os.path.join(held_dir, "release"), exist_ok=True)
    open(os.path.join(held_dir, "release", "go"), "w").close()
    finished = until(lambda: read_record(held_dir)["state"] == "passed", 60,
                     "the run's own record to say it finished")
    check("D1 the run finished with no client attached (its manager's record says so)", finished,
          read_record(held_dir)["state"])

    # ---- E. a NEW client recovers the same run, its output and the order of events -----------
    with Session.attach(session_dir) as c2:
        check("E1 the returning client is a new session of the same lifetime",
              c2.connection.session != c1_session and c2.lifetime == first_lifetime,
              (c1_session, c2.connection.session))
        h = c2.run("held")
        art = dict((a["name"], a) for a in h["artifacts"]).get("report.txt")
        check("E2 the same run, passed, its artifact verified by the manager on disk",
              h["state"] == "passed" and art is not None and art["state"] == "verified",
              (h["state"], art))
        on = open(art["path"], "rb").read() if art else b""
        check("E3 the artifact is what the tool wrote, whole",
              art is not None and hashlib.sha256(on).hexdigest() == art["sha256"] and
              b"held-run" in on and b"step 3 done" in on)
        c1_rows = c2.deliveries_to(c1_session)["rows"]
        fin = [x for x in c2.deliveries_to(manager_id, shape="loom.runs.Finished")["rows"]
               if x["sender"] == h["session"]]
        c2_rows = c2.deliveries_to(c2.connection.session)["rows"]
        order = (max(x["seq"] for x in c1_rows) if c1_rows else None,
                 fin[0]["seq"] if fin else None,
                 min(x["seq"] for x in c2_rows) if c2_rows else None)
        check("E4 the bus's own order: client 1's last answer < the run's verdict < client 2's "
              "first answer", None not in order and order[0] < order[1] < order[2], order)
        conns = c2.describe()["connections"]
        check("E5 the door lists only this client now",
              [c["session"] for c in conns if c["state"] == "admitted" and c["kind"] == "client"]
              == [c2.connection.session], conns)
        # The reader answers from the host's history; the host declared it to the Recorder's
        # blacklist, so its own questions and answers are no part of what it reads back.
        reader = c2.connection.call("loom.history.DeliveriesTo",
                                    {"participant": c2.connection.session, "shape": "",
                                     "limit": 1}, role="loom.history").sender
        about_reader = c2.deliveries_to(reader)
        check("E6 reading the history writes none of it: the reader, asked what was delivered to "
              "it, finds nothing", about_reader["rows"] == [] and about_reader["newest_seq"] > 0,
              (reader, len(about_reader["rows"]), about_reader["newest_seq"]))

    # ---- F. edit the tool, run it again: no compiler, no restart, the old run kept -----------
    with Session.attach(session_dir) as s:
        v1 = s.start("basics/steps", "v1", {"count": 1, "message": "one"})
        v1 = s.wait("v1", timeout=60)
        steps_py = os.path.join(basics, "steps.py")
        src = open(steps_py, "r", encoding="utf-8").read()
        edited = src.replace('ctx.inputs.get("message", "") + "\\n"',
                             '"EDITED: " + ctx.inputs.get("message", "") + "\\n"')
        check("F0 the edit changes the tool's source", edited != src)
        with open(steps_py, "w", encoding="utf-8") as f:
            f.write(edited)
        v2 = s.start("basics/steps", "v2", {"count": 1, "message": "two"})
        v2 = s.wait("v2", timeout=60)
        # A missing artifact is a CHECK that fails, never a driver that stops: a run whose
        # reports were attributed elsewhere would otherwise end the journey with a KeyError.
        a1 = dict((a["name"], a) for a in v1["artifacts"]).get("report.txt")
        a2 = dict((a["name"], a) for a in v2["artifacts"]).get("report.txt")
        check("F1 the edited tool ran: a new revision, a changed result",
              v2["state"] == "passed" and v2["revision"] != v1["revision"] and a2 is not None and
              b"EDITED: two" in open(a2["path"], "rb").read(), (v1["revision"][:12],
                                                                v2["revision"][:12],
                                                                [a["name"] for a in v2["artifacts"]]))
        again = s.run("v1")
        check("F2 the earlier run keeps its identity, revision and artifact",
              again["revision"] == v1["revision"] and again["state"] == "passed" and
              a1 is not None and sha256_file(a1["path"]) == a1["sha256"] and
              b"EDITED" not in open(a1["path"], "rb").read() and
              "EDITED" not in open(os.path.join(v1["snapshot"], "steps.py")).read())
        d = s.describe()
        check("F3 neither the host nor its lifetime changed", d["pid"] == host_pid and
              d["lifetime"] == first_lifetime)

    # ---- G. two named runs with overlapping lifetimes ----------------------------------------
    with Session.attach(session_dir) as s:
        s.start("basics/steps", "ov-a", {"count": 2, "hold": "a", "message": "only-A"})
        s.start("basics/steps", "ov-b", {"count": 2, "hold": "b", "message": "only-B"})
        until(lambda: s.run("ov-a")["step"] == "held: a" and s.run("ov-b")["step"] == "held: b",
              30, "both runs to hold")
        a, b = s.run("ov-a"), s.run("ov-b")
        check("G1 both runs are live at once, as two sessions with two directories",
              a["state"] == b["state"] == "running" and a["session"] != b["session"] and
              a["directory"] != b["directory"], (a["session"], b["session"]))
        os.makedirs(os.path.join(b["directory"], "release"), exist_ok=True)
        open(os.path.join(b["directory"], "release", "b"), "w").close()
        s.wait("ov-b", timeout=60)
        check("G2 one finishes while the other still waits", s.run("ov-a")["state"] == "running")
        os.makedirs(os.path.join(a["directory"], "release"), exist_ok=True)
        open(os.path.join(a["directory"], "release", "a"), "w").close()
        a, b = s.wait("ov-a", timeout=60), s.run("ov-b")
        pa = dict((x["name"], x) for x in a["artifacts"]).get("report.txt")
        pb = dict((x["name"], x) for x in b["artifacts"]).get("report.txt")
        ra = open(pa["path"], "rb").read() if pa else b""
        rb = open(pb["path"], "rb").read() if pb else b""
        check("G3 no mixed results and no overwritten output: each run's own report, produced by "
              "its own worker", pa is not None and pb is not None and
              b"only-A" in ra and b"only-B" not in ra and b"only-B" in rb and b"only-A" not in rb,
              ([x["name"] for x in a["artifacts"]], [x["name"] for x in b["artifacts"]]))

    # ---- H. a tool's own failure, a tool's bug, and a success after both ----------------------
    with Session.attach(session_dir) as s:
        s.start("basics/steps", "fails", {"count": 3, "fail_at": 2})
        f = s.wait("fails", timeout=60)
        check("H1 a failed check ends the run failed, with the tool's words",
              f["state"] == "failed" and "step 2" in f["failure"], f["failure"])
        check("H2 its cleanup ran and says so", os.path.exists(os.path.join(
            f["directory"], "out", "cleanup.txt")) and any("cleanup" in n and "done" in n
                                                          for n in f["notes"]))
        s.start("basics/steps", "crashes", {"count": 2, "crash_at": 1})
        c = s.wait("crashes", timeout=60)
        check("H3 a tool bug ends the run as an error, with where it happened",
              c["state"] == "error" and "RuntimeError" in c["failure"] and
              "steps.py" in c["failure"], c["failure"][:200])
        ok = s.wait("after", timeout=60) if s.start("basics/steps", "after", {"count": 1}) \
            else None
        check("H4 the next run passes", ok and ok["state"] == "passed")

    # ---- I. a refusal after work began: the bus says no, and it is attributed ----------------
    with Session.attach(session_dir) as s:
        s.start("basics/session-report", "wrong-office", {"office": "loom.history"})
        n = s.wait("wrong-office", timeout=60)
        asked = n["asks"][-1] if n["asks"] else {}
        partial = dict((x["name"], x) for x in n["artifacts"]).get("request.txt")
        check("I1 an ask outside the run's grant is refused by the bus after work began: the "
              "partial artifact exists and the refusal is the bus's own word",
              n["state"] == "error" and asked.get("outcome") == "dispatch-refused" and
              asked.get("detail") == "CapabilityDenied" and partial is not None and
              partial["state"] == "verified", (n["state"], asked))
        s.start("basics/session-report", "report", {})
        v = s.wait("report", timeout=60)
        check("I2 the permitted ask succeeds", v["state"] == "passed", v["failure"][:300])
        said = dict((x["name"], x) for x in v["artifacts"]).get("session.json")
        reported = json.load(open(said["path"], "r", encoding="utf-8")) if said else {}
        check("I3 ...and says this lifetime, with the run's own session among its connections",
              reported.get("lifetime") == first_lifetime and
              any(c["name"] == "run:report" and c["session"] == v["session"]
                  for c in reported.get("connections", [])), v["summary"])

    # ---- J. cancellation is a request; force ends the process -------------------------------
    with Session.attach(session_dir) as s:
        s.start("basics/steps", "cancel-me", {"count": 2, "hold": "never"})
        until(lambda: s.run("cancel-me")["step"] == "held: never", 30, "the run to hold")
        try:
            s.release("cancel-me")
            why = ""
        except Refused as err:
            why = str(err)
        check("J0 an active run cannot be released", "cancel it or let it finish" in why, why)
        asked = s.cancel("cancel-me", reason="the journey asked")
        check("J1 a cancellation is recorded as asked for, not as done",
              asked["cancel_requested"] and asked["state"] in ("running", "cancelled"),
              asked["state"])
        done = s.wait("cancel-me", timeout=60)
        check("J2 the worker heard it, cleaned up, and the run is cancelled",
              done["state"] == "cancelled" and os.path.exists(os.path.join(
                  done["directory"], "out", "cleanup.txt")), done["failure"])
        s.start("basics/steps", "force-me", {"count": 2, "hold": "never"})
        until(lambda: s.run("force-me")["step"] == "held: never", 30, "the run to hold")
        s.cancel("force-me", reason="no waiting", force=True)
        forced = s.wait("force-me", timeout=60)
        check("J3 a forced cancellation ends the process: cancelled, killed",
              forced["state"] == "cancelled" and forced["process"] == "killed",
              (forced["state"], forced["process"]))

    # ---- K. a Start whose answer was lost is recovered by name, never duplicated ------------
    # The Start ACTS (a second client sees the run exist) and its sender leaves without ever
    # reading the answer: exactly a lost response to starting work.
    from loom_session.session import read_session_file
    info = read_session_file(session_dir)
    raw = lclient.Connection(info["endpoint"], info["key"], claimed="lossy")
    raw.ask("loom.runs.Start", {"tool": "basics/steps", "name": "lost-answer",
                                "inputs": json.dumps({"count": 1})}, role="loom.runs")
    with Session.attach(session_dir) as s:
        acted = until(lambda: any(r["name"] == "lost-answer" and r["pid"] != 0
                                  for r in s.runs()["rows"]), 30, "the Start to act")
        raw.close()  # leaves without reading whatever answer was sent to it
        check("K0 the first Start acted, and its client left without its answer", acted)
        first = s.run("lost-answer")
        again = s.start("basics/steps", "lost-answer", {"count": 1})
        check("K1 asking again with the same request answers the same run",
              again["directory"] == first["directory"] and again["started_ms"] ==
              first["started_ms"])
        check("K2 there is one run of that name",
              [r["name"] for r in s.runs()["rows"]].count("lost-answer") == 1)
        try:
            s.start("basics/steps", "lost-answer", {"count": 2})
            refused = ""
        except Refused as err:
            refused = str(err)
        check("K3 the same name for another request is refused, not started", "already used" in
              refused, refused)

    # ---- L. approval pinned by content; attenuation by the door -------------------------------
    with Session.attach(session_dir) as s:
        rev = [r for r in s.tools()["rows"] if r["id"] == "pinned/steps"][0]["revision"]
        catalog["packages"][2]["approve"] = rev
        write_catalog()
        p = s.start("pinned/steps", "pinned-ok", {"count": 1})
        p = s.wait("pinned-ok", timeout=60)
        check("L1 a package pinned at its revision runs", p["state"] == "passed" and
              p["revision"] == rev)
        with open(os.path.join(pinned, "steps.py"), "a", encoding="utf-8") as f:
            f.write("\n# an edit after approval\n")
        try:
            s.start("pinned/steps", "pinned-edited", {"count": 1})
            why = ""
        except Refused as err:
            why = str(err)
        check("L2 an edit after pinning is refused until approved again",
              "changed since it was approved" in why, why)
        try:
            s.start("greedy/steps", "greedy", {"count": 1})
            why = ""
        except Refused as err:
            why = str(err)
        check("L3 a package asking more than its manager holds is refused by the door, with the "
              "decision that would allow it", "authority allow runs" in why and
              "any shape -> any target" in why, why)
        check("L4 a refused start leaves no run and no directory",
              all(r["name"] not in ("pinned-edited", "greedy") for r in s.runs()["rows"]))
        s.start("chatty/ask", "chatty", {"times": 131})
        c = s.wait("chatty", timeout=120)
        check("L5 a run's own account of its asks is bounded, and counts what it let go",
              c["state"] == "passed" and len(c["asks"]) == 128 and c["asks_dropped"] == 3,
              (c["state"], len(c["asks"]), c.get("asks_dropped"), c["failure"][:200]))

    # ---- M. the host ends; a new lifetime refuses the old handle; records remain -------------
    with Session.attach(session_dir) as s:
        s.start("basics/steps", "left-held", {"count": 2, "hold": "never"})
        until(lambda: s.run("left-held")["step"] == "held: never", 30, "the run to hold")
        s.shutdown("the journey ends this lifetime")
    code = host.wait(timeout=30)
    check("M1 Shutdown ends the host with exit 0 and removes its session files",
          code == 0 and not os.path.exists(os.path.join(session_dir, "session.json")), code)
    try:
        Session.attach(session_dir)
        gone = ""
    except SessionGone as err:
        gone = str(err)
    check("M2 attaching to an ended session says so", "no session host is serving" in gone, gone)
    host = start_host(args, session_dir, [])  # decisions persisted: runs boots by itself now
    if check("M3 a new host serves the same directory, booting the approved manager",
             until(serving, 30, "the second host")):
        with Session.attach(session_dir) as s:
            check("M4 it is a new lifetime", s.lifetime != first_lifetime)
            try:
                s.run("held", lifetime=first_lifetime)
                why = ""
            except Refused as err:
                why = str(err)
            check("M5 the old run handle is refused by name, never applied to new work",
                  "another host lifetime" in why, why)
            check("M6 the new lifetime holds no runs", s.runs()["rows"] == [])
            past = dict((r["name"], r) for r in s.past()["rows"])
            check("M7 earlier lifetimes' records remain as evidence, not live state",
                  past.get("held", {}).get("state") == "passed" and
                  past["held"]["live"] is False and past.get("fails", {}).get("state") ==
                  "failed", sorted(past))
            check("M8 a run still active when its host ended is recorded as interrupted -- not "
                  "cancelled, not failed", past.get("left-held", {}).get("state") ==
                  "interrupted", past.get("left-held", {}).get("failure"))
            s.shutdown("the journey is over")
        host.wait(timeout=30)
    return finish(args, None)


def finish(args, host):
    """The normal end of the journey; the evidence is written on every exit path below."""
    return 0 if CHECKS and not [c for c in CHECKS if not c["ok"]] else 1


def write_evidence():
    failed = [c for c in CHECKS if not c["ok"]]
    if EVIDENCE["path"]:
        with open(EVIDENCE["path"], "w", encoding="utf-8") as f:
            json.dump({"checks": CHECKS, "failed": len(failed)}, f, indent=1)
    print("%d of %d checks held" % (len(CHECKS) - len(failed), len(CHECKS)), flush=True)
    return 0 if CHECKS and not failed else 1


if __name__ == "__main__":
    try:
        code = main()
    except BaseException as err:  # a driver crash is a failure, and still ends its hosts
        import traceback
        traceback.print_exc()
        check("the journey driver ran to its end", False, "%s: %s" % (type(err).__name__, err))
        code = 1
    finally:
        for proc in HOSTS:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
    sys.exit(max(code, write_evidence()))
