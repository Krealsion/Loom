# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""A worker that exits while a child process of its own keeps running.

The child is started the ordinary way -- no job of its own, no new process group -- so it is in
the execution group the run manager owns, which is the whole question this tool is here to put.
"""

import subprocess
import sys


def run(ctx):
    seconds = int(ctx.inputs.get("seconds", 300))
    ctx.step("start a child of my own")
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(%d)" % seconds])
    ctx.produce("child.pid", str(child.pid).encode("utf-8"))
    return "child %d started; this worker is about to exit and leave it running" % child.pid
