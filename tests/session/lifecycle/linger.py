# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""A verdict that arrives before its process can exit."""

import threading
import time


def run(ctx):
    seconds = float(ctx.inputs.get("seconds", 300))
    ctx.step("start a thread nobody joins")
    threading.Thread(target=time.sleep, args=(seconds,), daemon=False).start()
    return "the tool returned; a thread of this process is still running"
