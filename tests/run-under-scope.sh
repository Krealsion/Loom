#!/usr/bin/env bash
# Run the given command inside an unprivileged, delegated cgroup-v2 scope when one is
# available, so the isolation suite's B5 resource enforcement is real. A plain
# invocation (e.g. `wsl bash`, or CI without a login session) lands in the root cgroup
# with no delegation; there B5 would fail-safe-refuse every default mount. If no
# delegated scope can be obtained, fall back to running directly (the resource tests
# then WARN-skip, but everything else still runs).
if command -v systemd-run >/dev/null 2>&1 &&
    systemd-run --user --scope -p Delegate=yes --quiet true >/dev/null 2>&1; then
    exec systemd-run --user --scope -p Delegate=yes --quiet -- "$@"
fi
exec "$@"
