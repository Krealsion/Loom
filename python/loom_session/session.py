# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""Attach to a persistent session host and use it: the Python client.

A session is a ``loom-host --serve <dir>`` process. Its directory holds ``session.json`` (where
to attach, and which LIFETIME this is) and ``session.key`` (the owner's client key). A client
attaches, asks, and leaves; nothing it knows is needed by the next client, which learns the same
facts from their owners: the session door (``loom.session``), the run manager (``loom.runs``) and
the history reader (``loom.history``). The human CLI (``python -m loom_session``) is this class,
printed.

A RUN HANDLE is ``(lifetime, name)``. ``Session.run(name)`` means "in the lifetime I attached
to"; a handle from another lifetime is refused by the manager rather than applied to new work.
"""

import json
import os
import time

from .client import (Connection, Denied, DispatchRefused, NotAnswered, Refused, SendRefused,
                     UnknownShape)
from .wire import Disconnected

SESSION_ROLE = "loom.session"
HISTORY_ROLE = "loom.history"
RUNS_ROLE = "loom.runs"

#: The states after which a run changes no more.
FINAL_STATES = ("passed", "failed", "error", "cancelled", "crashed", "interrupted")


class SessionGone(Exception):
    """The session this client was told about is not answering (it may have ended)."""


def read_session_file(directory):
    """``session.json`` and the key beside it, as a dict with ``key`` holding the key text."""
    path = os.path.join(directory, "session.json")
    try:
        with open(path, "r", encoding="utf-8") as f:
            info = json.load(f)
    except FileNotFoundError:
        raise SessionGone("no session.json in %s: no session host is serving it (start one "
                          "with: loom-host --serve %s)" % (directory, directory))
    key_path = os.path.join(directory, info.get("key", "session.key"))
    with open(key_path, "r", encoding="utf-8") as f:
        info["key"] = f.read().strip()
    info["directory"] = os.path.abspath(directory)
    return info


class Session(object):
    """One client's attachment to one session host lifetime."""

    def __init__(self, connection, info):
        self.connection = connection
        self.info = info
        self.lifetime = info["lifetime"]
        self.directory = info["directory"]

    @classmethod
    def attach(cls, directory, claimed="python-client", timeout=10.0):
        info = read_session_file(directory)
        try:
            conn = Connection(info["endpoint"], info["key"], claimed=claimed,
                              admit_timeout=timeout)
        except (OSError, Disconnected) as err:
            raise SessionGone("session.json in %s says lifetime %s at %s (pid %s), and nothing "
                              "answers there: %s. The host may have ended; its runs' records "
                              "remain under %s/runs." % (directory, info.get("lifetime"),
                                                         info.get("endpoint"), info.get("pid"),
                                                         err, directory))
        s = cls(conn, info)
        d = s.describe()
        if d["lifetime"] != info["lifetime"]:
            # The file and the door disagree: a different host now answers at that endpoint.
            conn.close()
            raise SessionGone("session.json names lifetime %s but the host at %s is lifetime %s"
                              % (info["lifetime"], info["endpoint"], d["lifetime"]))
        return s

    def close(self):
        self.connection.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _call(self, shape, fields=None, role=RUNS_ROLE, timeout=30.0):
        return self.connection.call(shape, fields, role=role, timeout=timeout)

    # ---- the session ---------------------------------------------------------------------------

    def describe(self):
        return self._call("loom.session.Describe", {}, role=SESSION_ROLE).fields

    def shutdown(self, reason="ended by a client"):
        return self._call("loom.session.Shutdown", {"reason": reason}, role=SESSION_ROLE)

    # ---- the catalog -----------------------------------------------------------------------------

    def tools(self, query=""):
        return self._call("loom.runs.ListTools", {"query": query}).fields

    def tool(self, tool_id):
        return self._call("loom.runs.DescribeTool", {"tool": tool_id}).fields

    def shape(self, name, version=1):
        """The host's structural truth for one shape: ``[(field, type, required)]``."""
        return self.connection.schema(name, version).describe()

    # ---- runs --------------------------------------------------------------------------------

    def start(self, tool, name, inputs=None):
        return self._call("loom.runs.Start", {"tool": tool, "name": name,
                                              "inputs": json.dumps(inputs or {})}).fields

    def runs(self):
        return self._call("loom.runs.List", {}).fields

    def run(self, name, lifetime=None):
        return self._call("loom.runs.Get", {"lifetime": lifetime or self.lifetime,
                                            "name": name}).fields

    def cancel(self, name, reason="", force=False, lifetime=None):
        return self._call("loom.runs.Cancel", {"lifetime": lifetime or self.lifetime,
                                               "name": name, "reason": reason,
                                               "force": force}).fields

    def release(self, name, remove=False, lifetime=None):
        return self._call("loom.runs.Release", {"lifetime": lifetime or self.lifetime,
                                                "name": name, "remove": remove})

    def past(self):
        return self._call("loom.runs.Past", {}).fields

    def wait(self, name, until=FINAL_STATES, timeout=60.0, interval=0.1, lifetime=None):
        """Read the run from its manager until its state is in ``until``. The timeout is this
        client's decision to stop waiting (``NotAnswered``); the run's state stays the manager's."""
        deadline = time.monotonic() + timeout
        while True:
            r = self.run(name, lifetime=lifetime)
            if r["state"] in until:
                return r
            if time.monotonic() >= deadline:
                raise NotAnswered("run %s is %s (step %r) after %.1fs of waiting; it is still the "
                                  "manager's to finish" % (name, r["state"], r["step"], timeout))
            time.sleep(interval)

    # ---- history -----------------------------------------------------------------------------

    def deliveries_to(self, participant, shape="", limit=0):
        return self._call("loom.history.DeliveriesTo", {"participant": int(participant),
                                                        "shape": shape, "limit": int(limit)},
                          role=HISTORY_ROLE).fields

    def delivery(self, seq):
        return self._call("loom.history.Delivery", {"seq": int(seq)}, role=HISTORY_ROLE).fields

    def crossings(self, name, lifetime=None):
        """The link crossings behind the answers a run's worker was handed, joined with the
        worker's own account of its asks by correlation: ``(run, rows, truncated)`` --
        ``truncated`` when the reader had more to say than one answer carries."""
        r = self.run(name, lifetime=lifetime)
        if not r["session"]:
            return r, [], False
        records = self.deliveries_to(r["session"])
        by_corr = dict((a["correlation"], a) for a in r["asks"])
        rows = []
        for rec in records["rows"]:
            ask = by_corr.get(rec["correlation"])
            rows.append((rec, ask))
        return r, rows, bool(records["truncated"])


__all__ = ["Session", "SessionGone", "read_session_file", "FINAL_STATES", "Denied", "Refused",
           "SendRefused", "DispatchRefused", "NotAnswered", "UnknownShape", "Disconnected"]
