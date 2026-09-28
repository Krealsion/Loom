#!/usr/bin/env bash
# Run the given command inside an unprivileged, delegated cgroup-v2 scope when one is available,
# so the isolation suite's resource enforcement is real; a plain shell, or CI without a login
# session, lands in the root cgroup with no delegation, where every default mount is refused.
# A delegated scope is a PRECONDITION for a green isolation or policy run: without one the
# command still runs, and the OS-enforcement cases FAIL naming the missing capability
# (tests/enforcement_gate.hpp), unless ZEN_ALLOW_UNENFORCEABLE=1 makes them marked skips.
if command -v systemd-run >/dev/null 2>&1 &&
    systemd-run --user --scope -p Delegate=yes --quiet true >/dev/null 2>&1; then
    exec systemd-run --user --scope -p Delegate=yes --quiet -- "$@"
fi
exec "$@"
