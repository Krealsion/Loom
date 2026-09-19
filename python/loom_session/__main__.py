# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""``python -m loom_session`` -- the human CLI (see cli.py)."""

import sys

from .cli import main

sys.exit(main())
