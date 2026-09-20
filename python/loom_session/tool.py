# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""What a tool is handed: its run's context.

A tool is a Python file with a ``run(ctx)`` function. The run manager copies the tool's package
into the run's own directory (a snapshot -- an edit made meanwhile changes the next run, never
this one), starts a worker for it, and the worker calls ``run(ctx)`` with a ``Context``::

    def run(ctx):
        ctx.step("inspect")
        rows = ctx.ask("zengine.guests", "GuestConnectionsRequested", via="workshop")
        ctx.check(rows["rows"], "Workshop lists no connection")
        ...
        return "one line saying what passed"

WHAT A TOOL MAY SAY is its session's grant -- the rules its package ``asks`` for, as far as the
operator let the run manager pass them on. A refused send is an exception here, never a silent
nothing. WHAT A TOOL CAN TOUCH on this machine is NOT bounded by any of that: it runs as the
session's user, with that user's files and network. This is a working environment, not a
sandbox, and nothing here says otherwise.

THE VERDICT: returning passes; ``ctx.fail`` / a failed ``ctx.check`` fails; any other exception
is an error; a cancellation that arrives at a wait point ends it as cancelled. Cleanups registered
with ``ctx.on_cleanup`` run in every case, newest first, and each one's outcome is recorded.

THE CLEANUP PHASE IS ITS OWN PHASE, and a cancellation does not reach into it. Cancelling stops
the tool's ORDINARY work -- at the next wait point, as ``Cancelled`` -- and then the cleanups it
registered run with the session still open and the same grant they always had, so a far resource
this run took can actually be given back. What bounds that phase is its own deadline
(``CLEANUP_SECONDS``), not the cancellation; and the manager's force-stop ends the process at any
moment, because the worker is a process of its own and the host never waits on it.

WHAT A CLEANUP'S OUTCOME MEANS. ``done`` is the far owner's own answer; anything else is named
by what it was -- a refusal, a timeout, a lost link whose outcome is UNKNOWN, a disconnected
session. Having attempted a cleanup is never recorded as having finished one.
"""

import hashlib
import json
import os
import time

from . import client
from .client import (DispatchRefused, NotAnswered, Refused, SendRefused)  # noqa: F401 (re-export)

RUNS_ROLE = "loom.runs"

#: How long the whole cleanup phase may take. It is a bound on a tool's registered cleanups, not
#: a promise about any one of them: a cleanup's own ask carries its own timeout. When it is spent
#: the remaining cleanups are not attempted and each says so, which is a truthful record of what
#: was and was not given back.
CLEANUP_SECONDS = 30.0


class ToolFailed(Exception):
    """The tool's own verdict: something it checked did not hold."""


class Cancelled(Exception):
    """A cancellation was requested; raised at the tool's next wait point."""


class CleanupExpired(Exception):
    """The cleanup phase ran out of time; this cleanup was not attempted, or did not finish."""


class LinkOutcome(Exception):
    """The link's own word about a crossing that did not come back as a far answer.

    ``state`` is ``refused`` (nothing was submitted), ``dispatch-refused`` (the far bus said no),
    ``unlinked`` (no session to send on) or ``lost`` (the link ended AFTER the send: the outcome
    is UNKNOWN -- not a failure, not a refusal, and never a reason to send it again)."""

    def __init__(self, state, reason, attempt, shape):
        Exception.__init__(self, "link %s at %s: %s" % (state, shape, reason))
        self.state = state
        self.reason = reason
        self.attempt = attempt
        self.shape = shape

    @property
    def unknown(self):
        return self.state == "lost"


class Pending(object):
    """One ask in flight. ``wait()`` returns its Answer or raises what became of it."""

    def __init__(self, ctx, correlation, office, shape, version, via, settle):
        self.ctx = ctx
        self.correlation = correlation
        self.office = office
        self.shape = shape
        self.version = version
        self.via = via
        self.settle = settle
        self.reported_pending = False
        self.outcome = None

    def describe(self):
        where = "%s via link %s" % (self.office, self.via) if self.via else self.office
        return "%s v%d -> %s%s (conversation %d)" % (self.shape, self.version, where,
                                                     " [settle]" if self.settle else "",
                                                     self.correlation)

    def done(self):
        self.ctx._pump(0.0)
        return self.ctx.conn.done(self.correlation)

    def wait(self, timeout=60.0):
        return self.ctx._wait(self, timeout)


class Context(object):
    """The run a tool is part of, and the doors it may use."""

    def __init__(self, conn, request, run_dir, control_corr):
        self.conn = conn
        self.request = request
        self.name = request["name"]
        self.lifetime = request["lifetime"]
        self.tool = request["tool"]
        self.revision = request["revision"]
        self.inputs = dict(request.get("inputs") or {})
        self.run_dir = run_dir
        self.out_dir = request["out"]
        self._control = control_corr
        self._cancel = None
        self._pending = {}
        self._cleanups = []
        self._step = ""
        self._last_pending_report = None
        self._cleaning = False         # the cleanup phase is running (see the module note)
        self._cleanup_deadline = None  # when that phase is out of time
        #: How long this tool's whole cleanup phase may take. A tool that knows its cleanups are
        #: slow (or must not be) says so here, before it registers them.
        self.cleanup_seconds = CLEANUP_SECONDS

    # ---- telling the manager where the run is ----------------------------------------------

    def step(self, name, note=""):
        """Name the step the tool is in (and optionally say one line about it)."""
        self._set_step(name)
        self._report_progress(note)

    def _set_step(self, name):
        """A step in the cleanup phase says so, whatever the step calls itself: a client that
        returns to a run waiting at something must be able to tell cleaning up from working."""
        self._step = ("cleanup: " + name) if self._cleaning else name

    def note(self, text):
        self._report_progress(text)

    def _report_progress(self, note=""):
        pending = [p.describe() for p in self._pending.values()]
        self.conn.ask("loom.runs.Progress", {"step": self._step, "note": note,
                                             "pending": pending},
                      role=RUNS_ROLE, fire=True)
        self._last_pending_report = pending

    # ---- the verdict ----------------------------------------------------------------------

    def check(self, condition, message):
        """Fail the run with ``message`` unless ``condition`` holds."""
        if not condition:
            raise ToolFailed(message)

    def fail(self, message):
        raise ToolFailed(message)

    def on_cleanup(self, action, label):
        """Run ``action()`` when the tool ends -- passed, failed, errored or cancelled -- newest
        first, in the cleanup phase, where a cancellation no longer interrupts it. Not run when a
        CLIENT merely detaches: the run is still going then."""
        self._cleanups.append((label, action))

    @property
    def cancel_requested(self):
        """Has a cancellation been requested? Reads whatever has arrived, takes the manager's
        directive when it is there, and LATCHES: once true, true for the rest of the run. It
        never raises and never waits, so a tool that does its own looping can cooperate --
        finish the piece it is on, and return or raise on its own terms."""
        self._pump(0.0)
        self._take_cancel()
        return self._cancel is not None

    def _take_cancel(self):
        """Consume the manager's answer to the standing control question, if it has come."""
        if self._cancel is None and self.conn.done(self._control):
            try:
                d = self.conn.wait(self._control, 0.0)
                if d.get("directive") == "cancel":
                    self._cancel = d.get("reason") or "cancelled"
            except Exception:
                pass
        return self._cancel

    def _check_cancel(self):
        """The implicit check at every wait point: raise ``Cancelled`` once one was requested --
        unless this is the cleanup phase, whose bound is its own deadline."""
        self._pump(0.0)
        self._take_cancel()
        if self._cleaning:
            if self._cleanup_deadline is not None and time.monotonic() >= self._cleanup_deadline:
                raise CleanupExpired("the cleanup phase ran out of its %.0fs"
                                     % self.cleanup_seconds)
            return
        if self._cancel is not None:
            raise Cancelled(self._cancel)

    def _pump(self, timeout):
        self.conn.poll(timeout)

    # ---- asking -----------------------------------------------------------------------------

    def ask_async(self, office, shape, fields=None, version=1, via=None, settle=False):
        """Send one ask; returns a ``Pending``. ``via`` names a link: the ask crosses to the far
        host's ``office`` and is answered through the link (``zen/bridge/link.hpp``)."""
        self._check_cancel()
        if via:
            inner = self.conn.encode(shape, fields, version)
            corr = self.conn.ask("loom.link.Ask", {"role": office, "target": 0,
                                                   "payload": inner, "settle": bool(settle)},
                                 role="loom.link." + via)
        else:
            corr = self.conn.ask(shape, fields, version, role=office, settle=settle)
        p = Pending(self, corr, office, shape, version, via, settle)
        self._pending[corr] = p
        return p

    def ask(self, office, shape, fields=None, version=1, via=None, settle=False, timeout=60.0):
        """Ask and wait for its answer (see ``Pending.wait``)."""
        return self.ask_async(office, shape, fields, version, via, settle).wait(timeout)

    def _wait(self, p, timeout):
        deadline = time.monotonic() + timeout
        started = time.monotonic()
        while not self.conn.done(p.correlation):
            self._check_cancel()
            if not p.reported_pending and time.monotonic() - started > 0.15:
                # A WAIT WORTH SEEING: say what is open, once per ask, so the manager can tell a
                # client what this run is waiting on right now.
                p.reported_pending = True
                self._report_progress()
            if time.monotonic() >= deadline:
                self._report_ask(p, "pending", "the tool stopped waiting after %.1fs" % timeout)
                raise NotAnswered("no answer to %s within %.1fs" % (p.describe(), timeout))
            self._pump(min(0.25, max(0.0, deadline - time.monotonic())))
        self._pending.pop(p.correlation, None)
        try:
            a = self.conn.wait(p.correlation, 0.0)
        except SendRefused as err:
            self._report_ask(p, "refused", str(err))
            self._after_wait(p)
            raise
        except DispatchRefused as err:
            self._report_ask(p, "dispatch-refused", str(err))
            self._after_wait(p)
            raise
        if a.shape == "loom.link.Outcome":
            self._report_ask(p, a["state"], a.get("reason", ""))
            self._after_wait(p)
            raise LinkOutcome(a["state"], a.get("reason", ""), a.get("attempt", 0), p.shape)
        if a.shape == "zen.Refused":
            self._report_ask(p, "refused", a.get("reason", ""))
            self._after_wait(p)
            raise Refused(a.get("reason", ""), a)
        self._report_ask(p, "answer", a.shape)
        self._after_wait(p)
        return a

    def _after_wait(self, p):
        if p.reported_pending:
            self._report_progress()

    def _report_ask(self, p, outcome, detail):
        self.conn.ask("loom.runs.AskReport", {"ask": {
            "correlation": p.correlation, "office": p.office, "via": p.via or "",
            "shape": p.shape, "version": p.version, "settle": bool(p.settle),
            "outcome": outcome, "detail": detail[:400]}}, role=RUNS_ROLE, fire=True)

    # ---- producing files --------------------------------------------------------------------

    def produce(self, name, data):
        """Write ``data`` (bytes) as ``<run>/out/<name>``, check it reads back whole, and tell
        the manager -- which checks it again on disk before listing it as verified."""
        if os.path.basename(name) != name or name in ("", ".", ".."):
            raise ToolFailed("an output is a plain file name, not %r" % name)
        path = os.path.join(self.out_dir, name)
        with open(path, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        size = os.path.getsize(path)
        if size != len(data):
            raise ToolFailed("%s reads back as %d bytes, not %d" % (path, size, len(data)))
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        if digest != hashlib.sha256(data).hexdigest():
            raise ToolFailed("%s does not read back as what was written" % path)
        self.conn.ask("loom.runs.Produced", {"name": name, "path": path, "bytes": size,
                                             "sha256": digest}, role=RUNS_ROLE, fire=True)
        return path

    # ---- holding --------------------------------------------------------------------------

    def hold(self, gate, note=""):
        """Wait until ``<run>/release/<gate>`` exists. The run is pending meanwhile, and says on
        what; a cancellation ends the wait. For a demonstration or a test that must control when
        a run proceeds without a client being attached."""
        path = os.path.join(self.run_dir, "release", gate)
        self._set_step("held: " + gate)
        self.conn.ask("loom.runs.Progress", {
            "step": self._step, "note": note,
            "pending": [p.describe() for p in self._pending.values()] +
                       ["release gate '%s': create %s" % (gate, path)]},
            role=RUNS_ROLE, fire=True)
        while not os.path.exists(path):
            self._check_cancel()
            self._pump(0.1)
        self._report_progress("released: " + gate)

    # ---- the end ------------------------------------------------------------------------------

    def _run_cleanups(self, seconds=None):
        """THE CLEANUP PHASE. The tool's ordinary work is over, whatever ended it; what it
        registered now runs, newest first, with the session and the grant it has always had. A
        cancellation already requested does not end this phase -- that is the whole point of it
        -- and a cancellation requested DURING it does not either. Its own deadline does, and
        each cleanup's outcome is recorded as what it actually was."""
        if self._cleaning or not self._cleanups:
            return []
        budget = self.cleanup_seconds if seconds is None else seconds
        self._cleaning = True
        self._cleanup_deadline = time.monotonic() + max(0.0, float(budget))
        outcomes = []
        try:
            while self._cleanups:
                label, action = self._cleanups.pop()
                self._report_cleanup(label)
                if time.monotonic() >= self._cleanup_deadline:
                    outcomes.append("cleanup %s: CleanupExpired: not attempted; the cleanup "
                                    "phase had already run out of time" % label)
                    continue
                try:
                    action()
                    outcomes.append("cleanup %s: done" % label)
                except Exception as err:  # every cleanup runs, and says what it came to
                    outcomes.append("cleanup %s: %s: %s" % (label, type(err).__name__, err))
        finally:
            self._cleaning = False
            self._cleanup_deadline = None
        return outcomes

    def _report_cleanup(self, label):
        """Say the run is cleaning up and what is left of it, so a client that returns while a
        cleanup is blocked sees a run that is cleaning up rather than a run that is stuck."""
        self._step = "cleanup"
        left = ["cleanup '%s'" % label] + ["cleanup '%s' (queued)" % name for name, _ in
                                           reversed(self._cleanups)]
        try:
            self.conn.ask("loom.runs.Progress", {"step": self._step, "note": "", "pending": left},
                          role=RUNS_ROLE, fire=True)
        except Exception:
            pass  # a cleanup phase does not fail because the manager could not be told about it


def load_request(run_dir):
    with open(os.path.join(run_dir, "request.json"), "r", encoding="utf-8") as f:
        return json.load(f)
