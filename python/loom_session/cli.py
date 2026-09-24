# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""loom-session -- the human view of a persistent Loom session.

Every command below attaches as a fresh client, does its one thing through the same operations
the Python ``Session`` class uses, prints, and detaches. Nothing is remembered between commands:
each one learns what it needs from the session's owners. ``--json`` prints the owner's answer as
data instead of sentences.

    loom-session start   <dir> [--host PATH] [--listen PORT]   start a session host, detached
    loom-session status  <dir>                                  the lifetime, connections, links
    loom-session tools   <dir> [words...]                       the catalog, optionally searched
    loom-session describe <dir> <package/tool>                  one tool's help
    loom-session shape   <dir> <Name> [version]                 a shape's fields, as the host has it
    loom-session run     <dir> <package/tool> --name N [--input k=v]... [--wait SECONDS]
    loom-session runs    <dir> [--past]                         this lifetime's runs (or earlier ones)
    loom-session show    <dir> <name> [--lifetime L]            one run
    loom-session wait    <dir> <name> [--timeout SECONDS]       until the run is final
    loom-session cancel  <dir> <name> [--force] [--reason R]    ask for a cancellation
    loom-session release <dir> <name> [--remove]                forget a finished run
    loom-session crossings <dir> <name>                         where its answers came from
    loom-session delivery <dir> <seq>                           what became of one bus delivery
    loom-session stop    <dir> [--reason R]                     end the session host
"""

import argparse
import json
import os
import subprocess
import sys
import time

from . import session as sess
from .client import NotAnswered, Refused, SendRefused, DispatchRefused, UnknownShape, Denied
from .wire import Disconnected


def _out(args, data, human):
    if args.json:
        print(json.dumps(data, indent=1, default=lambda b: repr(b)))
    else:
        human(data)


def _attach(args):
    return sess.Session.attach(args.dir, claimed="loom-session-cli")


# ---- the session ----------------------------------------------------------------------------

def _find_host(explicit):
    if explicit:
        return explicit
    if os.environ.get("LOOM_HOST"):
        return os.environ["LOOM_HOST"]
    here = os.path.dirname(os.path.abspath(__file__))
    exe = "loom-host.exe" if os.name == "nt" else "loom-host"
    # installed: <prefix>/lib/loom/python/loom_session  ->  <prefix>/bin/loom-host
    # build tree: <build>/session-tools/python/loom_session  ->  <build>/loom-host
    for up in (os.path.join(here, "..", "..", "..", "..", "bin", exe),
               os.path.join(here, "..", "..", "..", exe)):
        if os.path.isfile(up):
            return os.path.abspath(up)
    return exe  # on PATH, or the error says it is not


def cmd_start(args):
    directory = os.path.abspath(args.dir)
    os.makedirs(directory, exist_ok=True)
    host = _find_host(args.host)
    command = [host, "--serve", directory]
    if args.listen:
        command += ["--listen", str(args.listen)]
    before = None
    try:
        before = sess.read_session_file(directory).get("lifetime")
    except Exception:
        pass
    log = open(os.path.join(directory, "host.log"), "ab")
    kwargs = {"stdin": subprocess.DEVNULL, "stdout": log, "stderr": subprocess.STDOUT,
              "cwd": directory, "close_fds": True}
    if os.name == "nt":
        flags = 0x00000008 | 0x00000200  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
        try:
            proc = subprocess.Popen(command, creationflags=flags | 0x01000000, **kwargs)
        except OSError:  # CREATE_BREAKAWAY_FROM_JOB refused by an enclosing job
            proc = subprocess.Popen(command, creationflags=flags, **kwargs)
    else:
        proc = subprocess.Popen(command, start_new_session=True, **kwargs)
    deadline = time.monotonic() + args.wait
    while True:
        code = proc.poll()
        if code is not None:
            print("loom-session: the host exited with code %d before it was serving; see %s"
                  % (code, os.path.join(directory, "host.log")))
            return 1
        try:
            info = sess.read_session_file(directory)
            if info.get("pid") == proc.pid and info.get("lifetime") != before:
                with sess.Session.attach(directory) as s:
                    d = s.describe()
                print("serving %s at %s -- lifetime %s (pid %d)"
                      % (directory, d["endpoint"], d["lifetime"], d["pid"]))
                return 0
        except Exception:
            pass
        if time.monotonic() >= deadline:
            print("loom-session: no session answered within %.0fs (this client stopped waiting; "
                  "the host may still come up -- see %s)" % (args.wait, directory))
            return 1
        time.sleep(0.1)


def cmd_status(args):
    with _attach(args) as s:
        d = s.describe()

    def human(d):
        print("session %s -- lifetime %s" % (d["directory"], d["lifetime"]))
        print("  %s, pid %d, weave ABI v%d, listening on %s" % (d["host"], d["pid"], d["abi"],
                                                                d["endpoint"]))
        print("  containment: %s" % d["containment"])
        for c in d["connections"]:
            print("  connection %d: %s %s (claims %r) %s, session %d, %s"
                  % (c["connection"], c["kind"], c["name"] or "-", c["claimed"], c["state"],
                     c["session"], c["encoding"]))
        for l in d["links"]:
            print("  link %s -> %s: %s%s, epoch %d, %d open%s"
                  % (l["name"], l["endpoint"], l["state"],
                     " as '%s' (far session %d)" % (l["established"], l["far_session"])
                     if l["state"] == "admitted" else "", l["epoch"], l["open"],
                     " -- " + l["detail"] if l["detail"] else ""))
        print("  %d run connection(s) expected%s" % (d["expected"],
                                                     "; ENDING" if d["ending"] else ""))
    _out(args, d, human)
    return 0


def cmd_stop(args):
    with _attach(args) as s:
        s.shutdown(args.reason)
    print("the session was asked to end; it ends after this turn")
    return 0


# ---- the catalog ------------------------------------------------------------------------------

def cmd_tools(args):
    with _attach(args) as s:
        t = s.tools(" ".join(args.words))

    def human(t):
        for row in t["rows"]:
            print("%-32s %s" % (row["id"], row["summary"]))
            print("%-32s %s; revision %s" % ("", row["approval"], row["revision"][:12]))
        if not t["rows"]:
            print("(no tool matches)")
        for p in t["problems"]:
            print("problem: " + p)
        print("catalog: " + t["catalog"])
    _out(args, t, human)
    return 0


def render_help(d, shapes):
    """The human help view of one tool, drawn from the manager's description and the host's
    schemas -- the same answer a machine reads with --json."""
    lines = ["%s -- %s" % (d["id"], d["summary"]), ""]
    if d["description"]:
        lines += [d["description"], ""]
    lines.append("inputs:")
    for i in d["inputs"]:
        extra = "required" if i["required"] else ("default %s" % i["default_json"]
                                                  if i["default_json"] else "optional")
        lines.append("  %-12s %-6s %s -- %s" % (i["name"], i["type"], extra, i["help"]))
    if not d["inputs"]:
        lines.append("  (none)")
    lines.append("outputs (in the run's out/ directory):")
    for o in d["outputs"]:
        lines.append("  %-20s %s" % (o["name"], o["help"]))
    if d["requires"]:
        lines.append("requires:")
        lines += ["  " + r for r in d["requires"]]
    lines.append("asks its worker to be granted:")
    lines += ["  " + a for a in d["asks"]] or ["  (only its reports to the run manager)"]
    if d["vocabulary"]:
        lines.append("speaks (fields from this host's schemas):")
        for v in d["vocabulary"]:
            fields = shapes.get(v)
            if fields is None:
                lines.append("  %s -- NOT resolvable on this host now" % v)
            else:
                lines.append("  %s (%s)" % (v, ", ".join("%s:%s" % (f, t) for f, t, _ in fields)
                                             or "no fields"))
    lines.append("source: %s" % d["source"])
    lines.append("version %s, revision %s; %s" % (d["version"] or "-", d["revision"][:12],
                                                 d["approval"]))
    if d["example"]:
        lines += ["example:", "  " + d["example"]]
    if d["recovery"]:
        lines += ["refusals and recovery:", "  " + d["recovery"]]
    return "\n".join(lines)


def cmd_describe(args):
    with _attach(args) as s:
        d = s.tool(args.tool)
        shapes = {}
        for v in d["vocabulary"]:
            name, _, ver = v.rpartition(" v")
            try:
                shapes[v] = s.shape(name, int(ver))
            except (UnknownShape, ValueError):
                shapes[v] = None
    if args.json:
        d = dict(d)
        d["shapes"] = shapes
        print(json.dumps(d, indent=1))
    else:
        print(render_help(d, shapes))
    return 0


def cmd_shape(args):
    with _attach(args) as s:
        fields = s.shape(args.name, args.version)
    _out(args, fields, lambda f: print("\n".join("  %-16s %-28s %s" % (n, t, "required" if r
                                                                         else "optional")
                                                 for n, t, r in f) or "  (no fields)"))
    return 0


# ---- runs -------------------------------------------------------------------------------------

def _typed_inputs(s, tool, pairs, raw):
    inputs = json.loads(raw) if raw else {}
    if pairs:
        declared = dict((i["name"], i["type"]) for i in s.tool(tool)["inputs"])
        for pair in pairs:
            key, _, value = pair.partition("=")
            kind = declared.get(key)
            if kind == "int":
                inputs[key] = int(value)
            elif kind == "number":
                inputs[key] = float(value)
            elif kind == "bool":
                inputs[key] = value.lower() in ("1", "true", "yes", "on")
            else:
                inputs[key] = value  # text, or an undeclared name the manager will refuse
    return inputs


def _human_run(r):
    print("run %s/%s -- %s (%s)" % (r["lifetime"][:8], r["name"], r["state"],
                                    "live" if r["live"] else "a past lifetime's record"))
    print("  tool %s, revision %s" % (r["tool"], r["revision"][:12]))
    print("  inputs %s" % r["inputs"])
    print("  directory %s" % r["directory"])
    if r["step"]:
        print("  step: %s" % r["step"])
    for p in r["pending"]:
        print("  waiting on: %s" % p)
    if r["cancel_requested"]:
        print("  cancellation requested: %s" % r["cancel_reason"])
    # AN EXIT CODE IS ONLY PRINTED ONCE THERE IS ONE: while an execution is still going the
    # manager has read nothing, and a 0 standing in for that would read as a clean end.
    print("  worker: session %d (%s), pid %d, process %s%s"
          % (r["session"], r["established"] or "-", r["pid"], r["process"],
             ", exit %d" % r["exit_code"] if r["process"] in ("exited", "killed") else ""))
    if r["process"] == "descendants":
        print("  the worker exited (%d) and left processes this manager still owns; the run's "
              "execution is not over. 'cancel' stops them." % r["exit_code"])
    if r["process"] == "killing":
        print("  a stop was issued; this run's execution was not seen to end, and no exit code "
              "was read for it.")
    if r["process"] == "unknown":
        print("  this run's execution is over and no exit code was ever readable for it.")
    if r.get("record") and r["record"] != "saved":
        print("  record: %s -- %s" % (r["record"], r["record_error"] or
                                      "no reason was recorded"))
    if r["summary"]:
        print("  summary: %s" % r["summary"])
    if r["failure"]:
        lines = r["failure"].splitlines()
        print("  failure: %s" % lines[0])
        for more in lines[1:]:
            print("           %s" % more)
    for a in r["artifacts"]:
        print("  artifact %s: %s, %d bytes, sha256 %s" % (a["name"], a["state"], a["bytes"],
                                                         a["sha256"][:16]))
    if r["asks_dropped"]:
        print("  (%d earlier ask(s) not kept)" % r["asks_dropped"])
    for first, last, count in _ask_groups(r["asks"]):
        print("  ask %s %s v%d -> %s%s: %s %s%s" % (
            first["correlation"] if count == 1 else "%d..%d" % (first["correlation"],
                                                                 last["correlation"]),
            first["shape"], first["version"], first["office"],
            " via " + first["via"] if first["via"] else "", first["outcome"], first["detail"],
            " (%d asks)" % count if count > 1 else ""))
    if r["notes_dropped"]:
        print("  (%d earlier note(s) not kept)" % r["notes_dropped"])
    for n in r["notes"]:
        print("  note: %s" % n)


def _ask_groups(asks):
    """Consecutive asks of one shape, office, route and outcome as one line -- a picture fetched
    in forty chunks is one step to a person; ``--json`` keeps each ask."""
    groups = []
    for a in asks:
        key = (a["shape"], a["version"], a["office"], a["via"], a["outcome"], a["detail"])
        if groups and groups[-1][0] == key:
            groups[-1][2] = a
            groups[-1][3] += 1
        else:
            groups.append([key, a, a, 1])
    return [(g[1], g[2], g[3]) for g in groups]


def cmd_run(args):
    with _attach(args) as s:
        inputs = _typed_inputs(s, args.tool, args.input, args.inputs)
        r = s.start(args.tool, args.name, inputs)
        if args.wait:
            r = s.wait(args.name, timeout=args.wait)
    _out(args, r, _human_run)
    return 0 if r["state"] not in ("failed", "error", "crashed", "cancelled", "interrupted") else 1


def cmd_runs(args):
    with _attach(args) as s:
        rl = s.past() if args.past else s.runs()

    def human(rl):
        print("lifetime %s: %d active, %d held (at most %d)" % (rl["lifetime"][:12], rl["active"],
                                                               len(rl["rows"]), rl["capacity"]))
        for r in rl["rows"]:
            print("  %-10s %-24s %-12s %s%s" % (r["lifetime"][:8], r["name"], r["state"],
                                               r["tool"], "" if r["live"] else "  (past)"))
    _out(args, rl, human)
    return 0


def cmd_show(args):
    with _attach(args) as s:
        r = s.run(args.name, lifetime=args.lifetime)
    _out(args, r, _human_run)
    return 0


def cmd_wait(args):
    with _attach(args) as s:
        r = s.wait(args.name, timeout=args.timeout)
    _out(args, r, _human_run)
    return 0


def cmd_cancel(args):
    with _attach(args) as s:
        r = s.cancel(args.name, reason=args.reason, force=args.force)
    _out(args, r, _human_run)
    return 0


def cmd_release(args):
    with _attach(args) as s:
        s.release(args.name, remove=args.remove)
    print("released %s%s" % (args.name, " and removed its directory" if args.remove else ""))
    return 0


def cmd_crossings(args):
    with _attach(args) as s:
        r, rows, truncated = s.crossings(args.name)

    def human(_):
        mine = [(rec, ask) for rec, ask in rows if ask or rec["crossing"]]
        rest = [rec for rec, ask in rows if not (ask or rec["crossing"])]
        print("run %s: worker session %d; %d remembered deliveries to it%s -- %d answered its "
              "asks or crossed a link" % (r["name"], r["session"], len(rows),
                                          " (the newest; the reader had more)" if truncated
                                          else "", len(mine)))
        for rec, ask in mine:
            line = "  seq %d %s v%d corr %d from #%d" % (rec["seq"], rec["shape"], rec["version"],
                                                        rec["correlation"], rec["sender"])
            if rec["crossing"]:
                if rec["crossing_payload"] == "retained":
                    line += (" <- crossing: link %s epoch %d, far session %d as '%s', attempt %d,"
                             " far sender #%d%s, %s"
                             % (rec["link"], rec["epoch"], rec["far_session"], rec["established"],
                                rec["attempt"], rec["far_sender"],
                                " as " + rec["far_role"] if rec["far_role"] else "", rec["kind"]))
                else:
                    line += " <- a link crossing whose record's bytes are %s" % rec["crossing_payload"]
            elif rec["parent"]:
                line += " <- parent seq %d: %s" % (rec["parent"], rec["parent_horizon"])
            if ask:
                line += " [the tool asked %s%s: %s]" % (ask["shape"], " via " + ask["via"]
                                                        if ask["via"] else "", ask["outcome"])
            print(line)
        if rest:
            kinds = []
            for rec in rest:
                kind = "%s v%d from #%d" % (rec["shape"], rec["version"], rec["sender"])
                for k in kinds:
                    if k[0] == kind:
                        k[1] += 1
                        break
                else:
                    kinds.append([kind, 1])
            print("  and %d other deliver%s to it: %s (--json lists each)" % (
                len(rest), "y" if len(rest) == 1 else "ies",
                ", ".join("%d %s" % (n, kind) for kind, n in kinds)))
    _out(args, {"run": r, "rows": [{"record": a, "ask": b} for a, b in rows],
                "truncated": truncated}, human)
    return 0


def cmd_delivery(args):
    with _attach(args) as s:
        rec = s.delivery(args.seq)
    _out(args, rec, lambda d: print("seq %d: %s%s" % (d["asked_seq"], d["asked_horizon"],
                                                      "; " + json.dumps(d["rows"][0])
                                                      if d["rows"] else "")))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(prog="loom-session", description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="command", required=True)

    def command(name, fn, help_text):
        c = sub.add_parser(name, help=help_text)
        c.add_argument("dir", help="the session directory")
        c.add_argument("--json", action="store_true", help="print the owner's answer as JSON")
        c.set_defaults(fn=fn)
        return c

    c = command("start", cmd_start, "start a session host serving <dir>, detached")
    c.add_argument("--host", help="the loom-host to run (default: beside this package, or PATH)")
    c.add_argument("--listen", type=int, default=0)
    c.add_argument("--wait", type=float, default=20.0)
    command("status", cmd_status, "what this session is right now")
    c = command("stop", cmd_stop, "end the session host")
    c.add_argument("--reason", default="ended from loom-session stop")
    c = command("tools", cmd_tools, "the tool catalog")
    c.add_argument("words", nargs="*")
    c = command("describe", cmd_describe, "one tool's help")
    c.add_argument("tool")
    c = command("shape", cmd_shape, "a shape's fields, from the host's schema")
    c.add_argument("name")
    c.add_argument("version", type=int, nargs="?", default=1)
    c = command("run", cmd_run, "start a named run (returns without waiting unless --wait is given)")
    c.add_argument("tool")
    c.add_argument("--name", required=True)
    c.add_argument("--input", action="append", default=[], help="name=value, typed by the tool")
    c.add_argument("--inputs", default="", help="all inputs as one JSON object")
    c.add_argument("--wait", type=float, default=0.0, metavar="SECONDS",
                   help="wait up to SECONDS for the run's verdict (default: 0, return without "
                        "waiting); a timeout does not cancel the run")
    c = command("runs", cmd_runs, "this lifetime's runs")
    c.add_argument("--past", action="store_true", help="records of earlier lifetimes instead")
    c = command("show", cmd_show, "one run")
    c.add_argument("name")
    c.add_argument("--lifetime", default=None)
    c = command("wait", cmd_wait, "wait for a run to finish")
    c.add_argument("name")
    c.add_argument("--timeout", type=float, default=60.0)
    c = command("cancel", cmd_cancel, "ask for a run's cancellation")
    c.add_argument("name")
    c.add_argument("--force", action="store_true")
    c.add_argument("--reason", default="")
    c = command("release", cmd_release, "forget a finished run")
    c.add_argument("name")
    c.add_argument("--remove", action="store_true")
    c = command("crossings", cmd_crossings, "the crossings behind a run's answers")
    c.add_argument("name")
    c = command("delivery", cmd_delivery, "what became of one bus delivery")
    c.add_argument("seq", type=int)
    args = p.parse_args(argv)
    try:
        return args.fn(args)
    except (Refused, SendRefused, DispatchRefused) as err:
        print("refused: %s" % err)
        return 3
    except NotAnswered as err:
        print("not answered: %s" % err)
        return 4
    except (sess.SessionGone, Denied, Disconnected, UnknownShape) as err:
        print("loom-session: %s" % err)
        return 5


if __name__ == "__main__":
    sys.exit(main())
