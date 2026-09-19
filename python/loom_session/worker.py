# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""The worker a run manager starts for one run: ``python -m loom_session.worker``.

It is its own session on the host: it presents the run's one-time credential (read from the
run's directory, then deleted), is admitted as ``run:<name>`` under the rules the door granted,
reports to the run manager, keeps one standing question open for a cancellation, loads the tool
from the run's SNAPSHOT and calls ``run(ctx)``. Its exit code is for a person reading a process
table; the run's state is the manager's, from the reports.

    0 passed   1 failed   2 error   3 cancelled   4 the session could not be reached or was lost
"""

import importlib.util
import os
import sys
import traceback

from . import client, tool
from .wire import Disconnected

EXIT = {"passed": 0, "failed": 1, "error": 2, "cancelled": 3}


def _say(text):
    sys.stdout.write(text + "\n")
    sys.stdout.flush()


def main():
    run_dir = os.environ.get("LOOM_RUN_DIR") or os.getcwd()
    endpoint = os.environ.get("LOOM_SESSION_ENDPOINT", "")
    request = tool.load_request(run_dir)
    cred_path = os.path.join(run_dir, "credential")
    try:
        with open(cred_path, "r", encoding="utf-8") as f:
            credential = f.read().strip()
    except OSError as err:
        _say("worker: no credential for this run (%s); it was already used, or never written" % err)
        return 4
    try:
        os.remove(cred_path)  # ONE-TIME: nothing on disk can connect as this run again
    except OSError:
        pass
    try:
        conn = client.Connection(endpoint, credential, claimed="run:" + request["name"])
    except (client.Denied, OSError, Disconnected) as err:
        _say("worker: the session at %s did not admit this run: %s" % (endpoint, err))
        return 4
    _say("worker: admitted as session %d (%s)" % (conn.session, conn.established))
    python = "Python %d.%d.%d at %s" % (sys.version_info[0], sys.version_info[1],
                                        sys.version_info[2], sys.executable)
    conn.ask("loom.runs.WorkerStarted", {"run": request["name"], "python": python,
                                         "revision": request["revision"]},
             role=tool.RUNS_ROLE, fire=True)
    control = conn.ask("loom.runs.Control", {}, role=tool.RUNS_ROLE)
    ctx = tool.Context(conn, request, run_dir, control)

    outcome, summary, failure = "error", "", ""
    try:
        script = os.path.join(request["snapshot"], request["script"])
        spec = importlib.util.spec_from_file_location("loom_tool_" + request["name"].replace(
            "-", "_").replace(".", "_"), script)
        module = importlib.util.module_from_spec(spec)
        sys.path.insert(0, request["snapshot"])  # the package's own helpers, from the snapshot
        spec.loader.exec_module(module)
        if not hasattr(module, "run"):
            raise tool.ToolFailed("%s defines no run(ctx)" % request["script"])
        result = module.run(ctx)
        outcome = "passed"
        summary = result if isinstance(result, str) else (
            "passed" if result is None else repr(result))
    except tool.ToolFailed as err:
        outcome, failure = "failed", str(err)
    except tool.Cancelled as err:
        outcome, failure = "cancelled", "cancelled: %s" % err
    except Disconnected as err:
        # The session is gone: nothing more can be reported, and nothing is claimed.
        _say("worker: the session connection ended mid-run: %s" % err)
        for line in ctx._run_cleanups():
            _say("worker: " + line)
        return 4
    except BaseException as err:  # the tool's own mistake: an error, with where it happened
        outcome = "error"
        failure = "%s: %s\n%s" % (type(err).__name__, err,
                                  "".join(traceback.format_exc(limit=8))[-1500:])
    cleanups = ctx._run_cleanups()
    for line in cleanups:
        _say("worker: " + line)
        try:
            ctx.note(line)
        except Disconnected:
            pass
    if cleanups and outcome != "passed":
        failure = failure + ("\n" if failure else "") + "; ".join(cleanups)
    _say("worker: %s -- %s" % (outcome, summary or failure))
    try:
        done = conn.ask("loom.runs.Finished", {"outcome": outcome, "summary": summary[:2000],
                                               "failure": failure[:4000]},
                        role=tool.RUNS_ROLE)
        conn.wait(done, 30.0)
    except Exception as err:
        _say("worker: the verdict could not be reported: %s" % err)
        conn.close()
        return 4
    conn.close()
    return EXIT.get(outcome, 2)


if __name__ == "__main__":
    sys.exit(main())
