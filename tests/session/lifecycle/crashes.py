# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""A worker that DIES BEFORE ITS VERDICT, leaving a child of its own running.

`descendants.py` returns first and exits cleanly; this one never reports anything. The manager
therefore knows two things at once -- its worker is gone, and an execution it owns is not -- and
the two must stay apart: the run crashed, whatever is then done about what it left behind.

The child is started the ordinary way, so it is in the execution group the manager owns.
"""

import os
import subprocess
import sys


def run(ctx):
    seconds = int(ctx.inputs.get("seconds", 300))
    ctx.step("start a child of my own")
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(%d)" % seconds])
    ctx.produce("child.pid", str(child.pid).encode("utf-8"))
    sys.stdout.write("about to die with no verdict, leaving %d running\n" % child.pid)
    sys.stdout.flush()
    os._exit(9)  # not an exception: no verdict is reported, and no cleanup runs
