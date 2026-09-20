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


def observed_exit_code(handle):
    """The exit code of a process THIS driver held a handle to, read after it died -- evidence
    about the operating system's own answer, obtained without asking the manager anything.

    Windows keeps a dead process's code readable for as long as a handle is open, so the handle
    is taken BEFORE the host is told to end. POSIX has no equivalent for a process that is not
    this driver's child: there the code the manager reports is checked against what the signal
    it sends must produce (SIGKILL: 128 + 9), which a default of 0 can never be."""
    if handle is None:
        return None
    import ctypes
    k32 = ctypes.windll.kernel32
    k32.WaitForSingleObject(handle, 20000)
    code = ctypes.c_ulong(0)
    ok = k32.GetExitCodeProcess(handle, ctypes.byref(code))
    k32.CloseHandle(handle)
    return int(code.value) if ok else None


def hold_process(pid):
    """A handle on that process, kept open across its death (Windows). None elsewhere."""
    if os.name != "nt" or not pid:
        return None
    import ctypes
    # SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION
    h = ctypes.windll.kernel32.OpenProcess(0x00100000 | 0x1000, False, int(pid))
    return h or None


def stop_code():
    """What ending a worker's execution group MUST produce as the leader's code on this
    platform: the job object's termination code, or the signal's 128 + 9."""
    return 1 if os.name == "nt" else 137


def alive(pid):
    """Is that process running, asked of the operating system? Never inferred from a report.

    On POSIX a process that has been killed but not yet reaped by its parent still answers to
    `kill(pid, 0)`, so a check that a worker has ENDED asks the manager (its parent) as well."""
    if not pid or pid <= 0:
        return False
    if os.name == "nt":
        out = subprocess.run(["tasklist", "/FI", "PID eq %d" % int(pid), "/NH"],
                             capture_output=True, text=True)
        return ("%d" % int(pid)) in out.stdout
    try:
        os.kill(int(pid), 0)
        return True
    except OSError:
        return False


def released(session, name):
    """True once the manager lets that record go -- it refuses while an execution is alive."""
    try:
        session.release(name)
        return True
    except Exception:
        return False


def refuses(call, words):
    """True when `call()` was refused in words containing `words`."""
    try:
        call()
    except Exception as err:
        return words in str(err)
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
    # For the lifecycle package's `settle` tool: a DIRECT ask of this manager, asking to be
    # settled, made by a worker this manager may pass that rule on to.
    "authority allow runs loom.runs.Start v1 -> role loom.runs",
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
    from loom_session.session import FINAL_STATES as FINAL, Session, SessionGone
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
    # The lifecycle package: tools whose verdict and whose execution end at different moments,
    # and whose cleanups speak on the bus. It lives beside this driver, not in the shipped
    # examples -- a tool that deliberately leaves a process behind is a witness, not a sample.
    lifecycle = os.path.join(pkgs, "lifecycle")
    shutil.copytree(os.path.join(os.path.dirname(os.path.abspath(__file__)), "lifecycle"),
                    lifecycle, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
    catalog = {"python": sys.executable, "runtime": os.path.abspath(args.runtime),
               "packages": [{"path": basics, "approve": "any-revision"},
                            {"path": greedy, "approve": "any-revision"},
                            {"path": pinned, "approve": "PINNED"},
                            {"path": chatty, "approve": "any-revision"},
                            {"path": lifecycle, "approve": "any-revision"}]}

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
        s.wait("force-me", timeout=60)
        # `killed` is said once the execution is actually over -- which is also when there is
        # an exit code to read. Waiting for it is waiting for the owner, not for a clock.
        until(lambda: s.run("force-me")["process"] == "killed", 30, "the process to be gone")
        forced = s.run("force-me")
        check("J3 a forced cancellation ends the process: cancelled, killed, with its code",
              forced["state"] == "cancelled" and forced["process"] == "killed",
              (forced["state"], forced["process"], forced["exit_code"]))

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

    # ---- N. a verdict is not the end of an execution -----------------------------------------
    #
    # The tool returns while its process goes on running -- a thread it never joined, or a child
    # it started. The verdict is in; the execution is not over; and the manager must say both,
    # keep counting the execution against its capacity, still be able to stop it, and refuse to
    # let a client lose it by releasing the record.
    with Session.attach(session_dir) as s:
        before_active = s.runs()["active"]
        s.start("lifecycle/linger", "linger", {"seconds": 300})
        s.wait("linger", timeout=60)
        r = s.run("linger")
        check("N1 the tool's verdict arrives while its process is still running",
              r["state"] == "passed" and r["process"] == "running" and alive(r["pid"]),
              (r["state"], r["process"], r["pid"]))
        check("N2 a live execution still counts against the manager's capacity",
              s.runs()["active"] == before_active + 1,
              (before_active, s.runs()["active"]))
        try:
            s.release("linger")
            why = ""
        except Refused as err:
            why = str(err)
        check("N3 releasing is not a way to kill it: the record is kept and the client is told "
              "to stop the execution on purpose", "execution is still alive" in why, why)
        stopped = s.cancel("linger", reason="the journey stops the execution")
        # Asked of BOTH owners: the manager, whose answer is what a client acts on, and the
        # operating system. On POSIX the manager is the worker's parent, so it is also the only
        # thing that can reap it -- which it does the next time a client asks it anything, and
        # asking is what this wait does.
        ended = until(lambda: s.run("linger")["process"] == "killed" and not alive(r["pid"]), 30,
                      "the execution to end")
        check("N4 the execution is stopped, and the VERDICT is untouched",
              stopped["state"] == "passed" and
              stopped["process"] in ("killing", "killed") and ended,
              (stopped["state"], stopped["process"], s.run("linger")["process"]))
        check("N5 the evidence of a run that passed and was then stopped is readable, and still "
              "says it passed", read_record(r["directory"])["state"] == "passed" and
              read_record(r["directory"])["process"] == "killed",
              read_record(r["directory"])["process"])
        s.release("linger")
        check("N6 once nothing is running, the record releases", "linger" not in
              [x["name"] for x in s.runs()["rows"]])

    # A worker that EXITS leaving a child behind: the leader is gone, the execution is not.
    control = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(600)"])
    try:
        with Session.attach(session_dir) as s:
            s.start("lifecycle/descendants", "descendants", {"seconds": 300})
            s.wait("descendants", timeout=60)
            until(lambda: s.run("descendants")["process"] == "descendants", 30,
                  "the worker to exit and its child to be noticed")
            d = s.run("descendants")
            art = dict((a["name"], a) for a in d["artifacts"]).get("child.pid")
            child = int(open(art["path"], "rb").read()) if art else 0
            check("N7 the worker leader exited and the manager says the execution goes on",
                  d["state"] == "passed" and d["process"] == "descendants" and alive(child),
                  (d["state"], d["process"], child))
            check("N8 it is still counted, and still cannot be released",
                  s.runs()["active"] >= 1 and refuses(lambda: s.release("descendants"),
                                                      "execution is still alive"))
            s.cancel("descendants", reason="the journey stops the child")
            check("N9 cancelling reaches the child the worker left behind",
                  until(lambda: not alive(child), 20, "the child to end"))
            check("N10 an unrelated process of the same user was never this manager's to touch",
                  control.poll() is None)
            until(lambda: released(s, "descendants"), 20, "the record to release")

        # AND THE WORKER THAT DIED BEFORE ITS VERDICT. The manager has watched it go: there is
        # nobody to ask for a cleanup, and nobody to hear a request -- but there is still an
        # execution it owns. An ORDINARY cancellation has to reach it, and the record has to
        # keep the worker's own failure apart from what was then done about what it left.
        with Session.attach(session_dir) as s:
            before_active = s.runs()["active"]
            s.start("lifecycle/crashes", "crashed-child", {"seconds": 300})
            gone = until(lambda: s.run("crashed-child")["process"] == "descendants", 60,
                         "the worker to die leaving its child")
            c = s.run("crashed-child")
            # FROM THE FILE THE TOOL WROTE, not from the report it sent: a worker that dies with
            # `os._exit` may have its last frames aborted with its socket, and what this section
            # is about is the process, not the message.
            pid_file = os.path.join(c["directory"], "out", "child.pid")
            orphan = int(open(pid_file, "rb").read()) if os.path.exists(pid_file) else 0
            check("N11 a worker that died BEFORE any verdict still leaves an execution this "
                  "manager owns", gone and not c["state"] in FINAL and
                  c["process"] == "descendants" and c["exit_code"] == 9 and orphan > 0 and
                  alive(orphan), (c["state"], c["process"], c["exit_code"], orphan))
            check("N12 it is counted while that execution runs, and cannot be released",
                  s.runs()["active"] == before_active + 1 and
                  refuses(lambda: s.release("crashed-child"), "is running"),
                  (before_active, s.runs()["active"]))
            asked = s.cancel("crashed-child", reason="the journey asks, ordinarily")
            check("N13 an ORDINARY cancellation reaches what the dead worker left behind -- no "
                  "force, and nothing left running",
                  orphan > 0 and until(lambda: not alive(orphan), 30,
                                       "the orphaned child to end"),
                  (asked["process"], orphan))
            until(lambda: s.run("crashed-child")["state"] in FINAL, 30, "the run to end")
            ended = s.run("crashed-child")
            check("N14 the record keeps the two apart: the worker CRASHED, and the execution it "
                  "left was then stopped -- with the LEADER's own exit code, not the stop's",
                  ended["state"] == "crashed" and ended["process"] == "killed" and
                  ended["exit_code"] == 9 and "without a verdict" in ended["failure"] and
                  "was then stopped" in ended["failure"],
                  (ended["state"], ended["process"], ended["exit_code"],
                   ended["failure"][:200]))
            check("N15 the same thing is on disk, for whoever reads it after this host",
                  read_record(ended["directory"])["state"] == "crashed" and
                  int(read_record(ended["directory"])["exit_code"]) == 9,
                  (read_record(ended["directory"])["state"],
                   read_record(ended["directory"])["process"]))
            check("N16 capacity comes back and the record releases",
                  until(lambda: s.runs()["active"] == before_active, 20, "capacity") and
                  until(lambda: released(s, "crashed-child"), 20, "the record to release"),
                  s.runs()["active"])
            check("N17 the unrelated process is still not this manager's to touch",
                  control.poll() is None)
    finally:
        if control.poll() is None:
            control.kill()
            control.wait()

    # ---- O. the cleanup phase: a cancellation stops work, not the giving back ----------------
    with Session.attach(session_dir) as s:
        s.start("lifecycle/cleanup", "cleanup-me", {"hold": "never", "refuse": True})
        until(lambda: s.run("cleanup-me")["step"] == "held: never", 30, "the run to hold")
        s.cancel("cleanup-me", reason="the journey cancels a run holding a far thing")
        done = s.wait("cleanup-me", timeout=60)
        notes = " | ".join(done["notes"])
        check("O1 a cleanup that must SPEAK still speaks after the cancellation, and the owner "
              "answered it", "cleanup ask the door, which this run may ask: done" in notes,
              notes[-400:])
        check("O2 the door really answered it: the cleanup said which lifetime it heard",
              ("cleanup asked the door and it answered: lifetime " + first_lifetime[:12]) in notes,
              notes[-400:])
        check("O3 a cleanup that cannot finish is recorded as what it was -- the bus's own "
              "refusal -- never as done",
              "ask an office this run was never granted: RuntimeError" in notes and
              "CapabilityDenied" in notes, notes[-400:])
        check("O4 the run is cancelled, and its account of its cleanups is part of why",
              done["state"] == "cancelled" and "cleanup" in done["failure"],
              (done["state"], done["failure"][:200]))
        # AND THE SESSION IS STILL A SESSION: the next run on it works.
        s.start("basics/steps", "after-cleanup", {"count": 1, "message": "after"})
        check("O5 the session serves the next run normally",
              s.wait("after-cleanup", timeout=60)["state"] == "passed")

    # A tool that WAITS AT NOTHING still cooperates, through the public property alone: no ask,
    # no gate, nothing blocking -- the case a cancellation used to have no way to reach.
    with Session.attach(session_dir) as s:
        s.start("basics/steps", "cooperates", {"count": 3, "cooperate": 60})
        until(lambda: s.run("cooperates")["state"] == "running" and
              s.run("cooperates")["session"] != 0, 30, "the run to be working")
        s.cancel("cooperates", reason="the journey asks a busy tool to stop")
        coop = s.wait("cooperates", timeout=45)
        check("O10 a tool looping on ctx.cancel_requested stops when asked, without waiting at "
              "an ask or a gate and without anybody forcing it",
              coop["cancel_requested"] and coop["state"] == "passed" and
              "asked to stop" in coop["summary"] and coop["process"] != "killed",
              (coop["state"], coop["process"], coop["summary"]))
        check("O11 ...and it was its own turn of work that ended, with its report written",
              any("asked to stop after" in n for n in coop["notes"]) and
              any(a["name"] == "report.txt" and a["state"] == "verified"
                  for a in coop["artifacts"]), " | ".join(coop["notes"])[-200:])

    # A cleanup that BLOCKS: the host is a different process and never waits on it.
    with Session.attach(session_dir) as s:
        s.start("lifecycle/stuck", "stuck", {"hold": "never", "budget": 600})
        until(lambda: s.run("stuck")["step"] == "held: never", 30, "the run to hold")
        s.cancel("stuck", reason="the journey cancels a run whose cleanup cannot finish")
        blocked = until(lambda: s.run("stuck")["step"].startswith("cleanup"), 30,
                        "the run to reach its cleanup")
        st = s.run("stuck")
        check("O6 a blocked cleanup is visible AS a cleanup, not as a run that is stuck",
              blocked and st["state"] == "running" and st["step"].startswith("cleanup") and
              st["pending"], (st["state"], st["step"], st["pending"]))
        check("O7 the host answers every other client while that cleanup is blocked",
              s.describe()["lifetime"] == first_lifetime and
              s.run("after-cleanup")["state"] == "passed")
        forced = s.cancel("stuck", reason="no waiting", force=True)
        s.wait("stuck", timeout=60)
        until(lambda: s.run("stuck")["process"] == "killed", 30, "the process to be gone")
        ended = s.run("stuck")
        check("O8 a forced cancellation ends a blocked cleanup's process",
              ended["state"] == "cancelled" and ended["process"] == "killed",
              (forced["process"], ended["state"], ended["process"]))
        # ...and a budget the tool set ends one by itself, with a truthful word for it.
        s.start("lifecycle/stuck", "expires", {"hold": "never", "budget": 3})
        until(lambda: s.run("expires")["step"] == "held: never", 30, "the run to hold")
        s.cancel("expires", reason="let the cleanup budget end it")
        out = s.wait("expires", timeout=90)
        # The VERDICT is in at `wait`; the process exiting is a separate fact, so it is waited
        # for separately -- which is the very distinction this phase came to make.
        ended_by_itself = until(lambda: s.run("expires")["process"] == "exited", 30,
                                "the worker to exit by itself")
        out = s.run("expires")
        check("O9 a cleanup phase that runs out of its own budget says so, and the run ends "
              "without anybody forcing it",
              out["state"] == "cancelled" and ended_by_itself and out["process"] == "exited" and
              "CleanupExpired" in " | ".join(out["notes"]),
              (out["state"], out["process"], " | ".join(out["notes"])[-200:]))

    # ---- P. evidence that could not be saved is said, not silently lost ----------------------
    with Session.attach(session_dir) as s:
        s.start("basics/steps", "faulted", {"count": 2, "hold": "go", "message": "faulted"})
        until(lambda: s.run("faulted")["step"] == "held: go", 30, "the run to hold")
        run_dir = s.run("faulted")["directory"]
        record = os.path.join(run_dir, "run.json")
        tmp = record + ".tmp"
        # The temporary is transient. The fault is only INJECTED once it is not there, and it is
        # checked to have taken -- an attempt that did not establish the fault proves nothing.
        cleared = until(lambda: not os.path.exists(tmp), 30, "the temporary record to clear")
        established = False
        if cleared:
            try:
                os.mkdir(tmp)
                established = os.path.isdir(tmp)
            except OSError:
                established = False
        check("P1 the fault is established: the name the record is written through is taken",
              established, tmp)
        if established:
            saved_before = read_record(run_dir)
            os.makedirs(os.path.join(run_dir, "release"), exist_ok=True)
            open(os.path.join(run_dir, "release", "go"), "w").close()
            def stale_and_finished():
                r = s.run("faulted")
                return r["record"] == "stale" and r["state"] == "passed"

            told = until(stale_and_finished, 60,
                         "the run to finish with its record unsaveable")
            live = s.run("faulted")
            check("P2 the manager says the evidence on disk is stale, and why", told and
                  live["record"] == "stale" and tmp in live["record_error"],
                  live.get("record_error", "")[:200])
            check("P3 the tool's verdict is NOT relabelled because its evidence could not be "
                  "saved", live["state"] == "passed" and not live["failure"],
                  (live["state"], live["failure"][:120]))
            on_disk = read_record(run_dir)
            check("P4 the last valid record is still there, whole and readable",
                  on_disk is not None and on_disk["name"] == "faulted" and
                  on_disk["state"] == saved_before["state"],
                  (on_disk or {}).get("state"))
            os.rmdir(tmp)
            again = until(lambda: s.run("faulted")["record"] == "saved", 30,
                          "the record to be written again")
            recovered = s.run("faulted")
            check("P5 with the fault gone the next time a client asks about this run the "
                  "record is saved again, and the record on disk is the run",
                  again and recovered["record"] == "saved" and
                  read_record(run_dir)["state"] == "passed",
                  (recovered["record"], read_record(run_dir)["state"]))

    # ...and `record: saved` has to be true of EVERY change it covers, not only the ones that
    # happen to be followed by something else. A tool that asks and then says nothing leaves an
    # interval in which nothing would notice; this section lives in that interval.
    with Session.attach(session_dir) as s:
        # `slow` makes every answer take longer than the 0.15s after which the runtime would
        # report an ask pending -- the condition under which a later Progress used to save the
        # asks for free and cover a missing AskReport save. The tool collects its own answers,
        # so the interval after each AskReport is quiet anyway; running it slow on purpose is
        # what keeps that true rather than lucky.
        s.start("lifecycle/asks", "quiet-asks", {"hold": "go", "seconds": 180, "slow": 400})
        first = until(lambda: len(s.run("quiet-asks")["asks"]) == 1, 60,
                      "the first ask to be reported")
        live = s.run("quiet-asks")
        quiet_dir = live["directory"]
        on_disk = read_record(quiet_dir)
        check("P6 an acknowledged ask is covered by the `saved` the same answer carries -- in "
              "the quiet before the tool says anything else",
              first and live["record"] == "saved" and len(live["asks"]) == 1 and
              [a["shape"] for a in on_disk["asks"]] == [a["shape"] for a in live["asks"]] and
              [str(a["correlation"]) for a in on_disk["asks"]] ==
              [str(a["correlation"]) for a in live["asks"]],
              (live["record"], len(live["asks"]), len(on_disk["asks"])))
        # The same mutation under an ESTABLISHED write fault: the promise has to become an
        # explanation, the last valid record has to survive, and the verdict is not involved.
        quiet_tmp = os.path.join(quiet_dir, "run.json.tmp")
        cleared = until(lambda: not os.path.exists(quiet_tmp), 30, "the temporary to clear")
        established = False
        if cleared:
            try:
                os.mkdir(quiet_tmp)
                established = os.path.isdir(quiet_tmp)
            except OSError:
                established = False
        check("P7 the fault is established for the second ask: the name the record is written "
              "through is taken", established, quiet_tmp)
        if established:
            before_second = read_record(quiet_dir)
            os.makedirs(os.path.join(quiet_dir, "release"), exist_ok=True)
            open(os.path.join(quiet_dir, "release", "again"), "w").close()
            told = until(lambda: len(s.run("quiet-asks")["asks"]) == 2 and
                         s.run("quiet-asks")["record"] == "stale", 60,
                         "the second ask to be reported with its record behind")
            behind = s.run("quiet-asks")
            still = read_record(quiet_dir)
            check("P8 an ask the record could not carry is SAID to be behind, and the last "
                  "valid record is still whole on disk", told and
                  behind["record"] == "stale" and quiet_tmp in behind["record_error"] and
                  len(behind["asks"]) == 2 and len(still["asks"]) == 1 and
                  still["name"] == "quiet-asks" and behind["state"] == "running",
                  (behind["record"], len(behind["asks"]), len(still["asks"]),
                   behind.get("record_error", "")[:120]))
            os.rmdir(quiet_tmp)
            saved_again = until(lambda: s.run("quiet-asks")["record"] == "saved", 30,
                                "the record to be saved again")
            recovered = read_record(quiet_dir)
            check("P9 recovery saves the evidence as it stands NOW -- no verdict and no later "
                  "message required to make the earlier promise true",
                  saved_again and len(recovered["asks"]) == 2 and
                  recovered["state"] == "running",
                  (len(recovered["asks"]), recovered["state"]))
        open(os.path.join(quiet_dir, "release", "go"), "w").close()
        verdict = s.wait("quiet-asks", timeout=90)
        check("P10 the tool's verdict came through all of that untouched",
              verdict["state"] == "passed" and "said nothing" in verdict["summary"] and
              read_record(quiet_dir)["state"] == "passed",
              (verdict["state"], verdict["summary"][:80]))

    # ---- Q. settlement asked for is settlement waited for ------------------------------------
    with Session.attach(session_dir) as s:
        s.start("lifecycle/settle", "settled-ask", {"name": "started-by-a-settled-ask"})
        q = s.wait("settled-ask", timeout=90)
        art = dict((a["name"], a) for a in q["artifacts"]).get("settlement.json")
        told = json.load(open(art["path"], "r", encoding="utf-8")) if art else {}
        check("Q1 a direct ask that asked for settlement was complete only with BOTH halves, "
              "through the real bridge", q["state"] == "passed" and
              told.get("settled_at_completion") is True and told.get("shape") == "loom.runs.Run",
              (q["state"], told, q["failure"][:200]))
        check("Q2 the work that ask set in motion really had happened: the run it asked for "
              "exists, with a worker process of its own",
              told.get("pid", 0) != 0 and
              s.wait("started-by-a-settled-ask", timeout=60)["state"] == "passed",
              told.get("pid"))
        asked = [a for a in q["asks"] if a["shape"] == "loom.runs.Start"]
        check("Q3 the run's own account says the ask asked to be settled, and was answered",
              len(asked) == 1 and asked[0]["settle"] and asked[0]["outcome"] == "answer", asked)

    # ---- M. the host ends; a new lifetime refuses the old handle; records remain -------------
    #
    # THREE RUNS, because a clean shutdown has three different things to say. One with no
    # verdict and a live worker; one whose verdict is in while its worker still runs; and one
    # whose worker has ALREADY exited, leaving a child this manager owns. Each leaves a record
    # that outlives this host, so each record's account of the operating system is checked
    # against the operating system -- a handle this driver holds open across the death, where
    # the platform has one, and otherwise the code the manager's own signal must produce.
    with Session.attach(session_dir) as s:
        s.start("basics/steps", "left-held", {"count": 2, "hold": "never"})
        until(lambda: s.run("left-held")["step"] == "held: never", 30, "the run to hold")
        held_pid = s.run("left-held")["pid"]
        held_dir = s.run("left-held")["directory"]
        held_handle = hold_process(held_pid)
        s.start("lifecycle/linger", "left-lingering", {"seconds": 300})
        s.wait("left-lingering", timeout=60)
        linger_run = s.run("left-lingering")
        linger_handle = hold_process(linger_run["pid"])
        check("M0b a run whose VERDICT is in still has a live worker when the host is told to "
              "end", linger_run["state"] == "passed" and linger_run["process"] == "running" and
              alive(linger_run["pid"]), (linger_run["state"], linger_run["process"]))
        s.start("lifecycle/descendants", "left-descendants", {"seconds": 300})
        s.wait("left-descendants", timeout=60)
        until(lambda: s.run("left-descendants")["process"] == "descendants", 30,
              "the worker to exit leaving its child")
        left = s.run("left-descendants")
        left_art = dict((a["name"], a) for a in left["artifacts"]).get("child.pid")
        left_child = int(open(left_art["path"], "rb").read()) if left_art else 0
        check("M0 a run whose worker exited still owns a live child when the host is told to end",
              left["process"] == "descendants" and alive(left_child),
              (left["process"], left_child))
        leader_code = left["exit_code"]
        s.shutdown("the journey ends this lifetime")
    code = host.wait(timeout=30)
    check("M0a a CLEAN shutdown ends an execution whose leader had already exited",
          until(lambda: not alive(left_child), 30, "the orphaned child to end"), left_child)
    check("M1 Shutdown ends the host with exit 0 and removes its session files",
          code == 0 and not os.path.exists(os.path.join(session_dir, "session.json")), code)
    # THE RECORDS THE HOST LEFT, against what the operating system says happened.
    held_observed = observed_exit_code(held_handle)
    linger_observed = observed_exit_code(linger_handle)
    held_record = read_record(held_dir)
    linger_record = read_record(linger_run["directory"])
    left_record = read_record(left["directory"])
    check("M1a a run stopped by the shutdown records the code the OPERATING SYSTEM gave its "
          "leader, not a default nobody read", int(held_record["exit_code"]) == stop_code() and
          held_record["process"] == "killed" and
          (held_observed is None or held_observed == int(held_record["exit_code"])),
          (held_record["process"], held_record["exit_code"], held_observed, stop_code()))
    check("M1b ...and so does one whose VERDICT was already in: the verdict stands, and the "
          "execution's end is an observation", linger_record["state"] == "passed" and
          linger_record["process"] == "killed" and
          int(linger_record["exit_code"]) == stop_code() and
          (linger_observed is None or linger_observed == int(linger_record["exit_code"])),
          (linger_record["state"], linger_record["process"], linger_record["exit_code"],
           linger_observed))
    check("M1c a LEADER whose code was already known keeps it: stopping the group it left "
          "behind is not a new result for the leader",
          int(left_record["exit_code"]) == int(leader_code) and
          int(left_record["exit_code"]) != stop_code() and
          left_record["process"] == "killed",
          (left_record["process"], left_record["exit_code"], leader_code, stop_code()))
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

    # ---- R. a host that is KILLED is not a host that shut down -------------------------------
    #
    # The clean end runs the manager's destructor and every execution goes with it (M8, N4). An
    # abruptly killed host runs nothing at all, so what happens to its workers is the operating
    # system's answer, not this manager's -- and the two platforms answer differently. Both
    # answers are pinned here, because a guarantee nobody tested is not a guarantee, and a
    # limit nobody wrote down gets mistaken for one.
    abrupt = os.path.join(work, "abrupt")
    os.makedirs(abrupt)
    shutil.copy(os.path.join(session_dir, "loom-boot.json"),
                os.path.join(abrupt, "loom-boot.json"))
    shutil.copy(os.path.join(session_dir, "loom-tools.json"),
                os.path.join(abrupt, "loom-tools.json"))
    killed_host = start_host(args, abrupt, APPROVALS + ["start runs %s loom.runs" % args.runs])
    worker_pid = 0
    if check("R0 a host of its own is serving, for a death nothing else has to survive",
             until(lambda: Session.attach(abrupt).close() or True, 30, "the third host")):
        with Session.attach(abrupt) as s:
            s.start("lifecycle/linger", "outlives", {"seconds": 300})
            s.wait("outlives", timeout=60)
            worker_pid = s.run("outlives")["pid"]
        check("R1 its worker is running when the host is killed", alive(worker_pid), worker_pid)
        killed_host.kill()
        killed_host.wait(timeout=30)
        if os.name == "nt":
            check("R2 (Windows) the worker goes with an abruptly killed host: the job object "
                  "the kernel closes on its behalf takes the whole execution group",
                  until(lambda: not alive(worker_pid), 30, "the worker to go with its host"),
                  worker_pid)
        else:
            # NOT a defect and NOT a guarantee: POSIX has no equivalent of the job's
            # kill-on-close, and docs/guides/sessions.md says so rather than implying otherwise.
            check("R2 (POSIX) the worker SURVIVES an abruptly killed host -- there is no such "
                  "guarantee on this platform, and this is what that means",
                  alive(worker_pid), worker_pid)
        check("R3 the session files of a host that was killed are still there, saying which "
              "lifetime nothing is answering for",
              os.path.exists(os.path.join(abrupt, "session.json")))
        if alive(worker_pid):
            # The journey owns what it started: nothing of this section is left running.
            if os.name == "nt":
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(worker_pid)],
                               capture_output=True)
            else:
                os.kill(worker_pid, 9)
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
