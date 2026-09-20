# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""loom_session -- clients, the human CLI and the tool runtime for a persistent Loom session.

Optional tooling shipped with Loom: it needs Python 3.8 or newer and nothing else (the standard
library only). A basic Loom installation does not need it; a session host (``loom-host --serve``)
runs without it, and only clients written in Python and the tools a run manager starts use it.
See docs/guides/sessions.md in Loom.
"""

import sys

MINIMUM_PYTHON = (3, 8)

if sys.version_info[:2] < MINIMUM_PYTHON:  # pragma: no cover - a clear refusal, not a crash
    raise ImportError("loom_session needs Python %d.%d or newer; this is %d.%d"
                      % (MINIMUM_PYTHON + tuple(sys.version_info[:2])))

__version__ = "0.1.0"
