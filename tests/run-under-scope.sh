#!/usr/bin/env bash
# Run the given command inside an unprivileged, delegated cgroup-v2 scope when one is
# available, so the isolation suite's B5 resource enforcement is real. A plain
# invocation (e.g. `wsl bash`, or CI without a login session) lands in the root cgroup
# with no delegation; there B5 would fail-safe-refuse every default mount.
#
# The delegated scope is therefore a PRECONDITION for a green isolation/policy run, not
# a nicety. If no scope can be obtained we still exec the command, but the OS-enforcement
# cases FAIL-HARD (naming the missing capability) rather than skip — the harness refuses
# to report a pass it did not earn (see tests/enforcement_gate.hpp). To run anyway on a
# host that genuinely cannot enforce, set ZEN_ALLOW_UNENFORCEABLE=1 to convert those
# fail-hards into marked-degraded skips.
if command -v systemd-run >/dev/null 2>&1 &&
    systemd-run --user --scope -p Delegate=yes --quiet true >/dev/null 2>&1; then
    exec systemd-run --user --scope -p Delegate=yes --quiet -- "$@"
fi
exec "$@"
