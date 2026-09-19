# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""One admitted session with a Loom host, and the conversations it holds.

An ASK is a message sent under a correlation this session minted. Its ANSWER is the one delivery
Loom attests as THE answer to it (``answers_ask`` on the Delivered frame) under that correlation
-- never the newest message that looks like one. Everything else that can happen to an ask is
kept apart, because each sends a person somewhere different:

  answered              ``Answer`` (and ``zen.Refused`` is raised as ``Refused``: the owner said no)
  refused before the bus  ``SendRefused``: the host dropped it (unknown shape, its gate, not admitted)
  refused by the bus    ``DispatchRefused``: e.g. CapabilityDenied -- this session may not say it
  not answered yet      ``NotAnswered``: the deadline is THIS CLIENT'S decision to stop waiting,
                        not evidence that the far side failed; the ask stays in the book
  the socket ended      ``Disconnected``: whatever was in flight is unknown, and is not resent
"""

import collections
import time

from . import values, wire
from .wire import Disconnected


class Denied(Exception):
    """The host refused this connection, in its own words."""


class Refused(Exception):
    """The owner answered ``zen.Refused``: it heard the ask and said no."""

    def __init__(self, reason, answer=None):
        Exception.__init__(self, reason)
        self.reason = reason
        self.answer = answer


class SendRefused(Exception):
    """The host dropped the send before its bus; nothing acted."""


class DispatchRefused(Exception):
    """The host's bus refused the delivery (the notice's reason, e.g. CapabilityDenied)."""


class NotAnswered(Exception):
    """No attested answer within the wait this client chose. The ask is still open."""


class UnknownShape(Exception):
    """The host resolves no such shape, so nothing in it can be said or read here."""


class Answer(object):
    """An attested answer: its shape, its decoded fields, and what Loom stamped on it."""

    def __init__(self, shape, version, fields, sender, correlation, raw):
        self.shape = shape
        self.version = version
        self.fields = fields
        self.sender = sender
        self.correlation = correlation
        self.raw = raw

    def __getitem__(self, key):
        return self.fields[key]

    def get(self, key, default=None):
        return self.fields.get(key, default)

    def __repr__(self):
        return "Answer(%s v%d from #%d corr %d: %r)" % (self.shape, self.version, self.sender,
                                                       self.correlation, self.fields)


class _Pending(object):
    __slots__ = ("shape", "version", "answer", "error", "settled", "asked_at", "fire")

    def __init__(self, shape, version, fire=False):
        self.shape = shape
        self.version = version
        self.answer = None
        self.error = None
        self.settled = False
        self.asked_at = time.monotonic()
        self.fire = fire  # nobody will wait: drop it once answered; keep only a failure


class Connection(object):
    """A session admitted by a Loom host's bridge under a compat encoding."""

    #: The most conversations this client holds open at once.
    MAX_OPEN = 256

    def __init__(self, endpoint, credential, claimed="python", admit_timeout=10.0):
        host, _, port = endpoint.rpartition(":")
        self.channel = wire.Channel(host or "127.0.0.1", int(port))
        self.channel.hello(claimed, credential)
        self.session = 0
        self.established = ""
        self._book = {}
        self._next = 0
        self._schemas = {}
        self._schema_replies = collections.deque()
        #: Deliveries that were not an answer to anything this client asked, newest last.
        self.unsolicited = collections.deque(maxlen=64)
        #: What became of fire-and-forget asks that did NOT come back answered, newest last.
        self.fire_failures = collections.deque(maxlen=64)
        self.codec = values.Codec(self.schema)
        deadline = time.monotonic() + admit_timeout
        while not self.session:
            left = deadline - time.monotonic()
            if left <= 0:
                self.channel.close()
                raise Denied("no Welcome or Denied from the host within %.1fs" % admit_timeout)
            for e in self.channel.read(left):
                if e.kind == "welcome":
                    self.session = e.session
                    self.established = e.established
                elif e.kind == "denied":
                    self.channel.close()
                    raise Denied(e.reason)
                else:
                    self._dispatch(e)

    def close(self):
        self.channel.close()

    # ---- the host's shapes -------------------------------------------------------------------

    def schema(self, name, version=1):
        """The host's own descriptor for a shape (cached for this connection: a snapshot)."""
        key = (name, int(version))
        if key in self._schemas:
            return self._schemas[key]
        self.channel.describe(name, int(version))
        deadline = time.monotonic() + 10.0
        while True:
            # A nested lookup (decoding an arrival while this one waits) may already have taken
            # this reply off the queue and cached it: replies are FIFO and carry no correlation.
            if key in self._schemas:
                return self._schemas[key]
            while self._schema_replies:
                e = self._schema_replies.popleft()
                if e.kind == "schema-none":
                    if (e.shape, e.version) == key:
                        raise UnknownShape("the host resolves no shape %s v%d" % key)
                    continue
                n, v, fields = values.open_envelope(e.payload)
                s = values.schema_from_desc(fields)
                self._schemas[(s.name, s.version)] = s
                if (s.name, s.version) == key:
                    return s
            left = deadline - time.monotonic()
            if left <= 0:
                raise NotAnswered("the host did not describe %s v%d" % key)
            for e in self.channel.read(left):
                self._dispatch(e)

    def encode(self, shape, fields=None, version=1):
        """A value's envelope bytes, checked against the host's schema before it is sent."""
        s = self.schema(shape, version)
        return values.envelope(shape, version, self.codec.encode(s, fields or {}))

    def decode(self, payload):
        """``(shape, version, fields)`` of envelope bytes, typed by the host's schema."""
        name, version, fields = values.open_envelope(payload)
        return name, version, self.codec.decode(self.schema(name, version), fields)

    # ---- asking ----------------------------------------------------------------------------

    def ask(self, shape, fields=None, version=1, role=None, target=0, settle=False, fire=False):
        """Send ``shape`` under a new correlation of this session's; returns the correlation.
        ``fire``: nobody will wait for the answer -- it is dropped when it comes, and a refusal
        is kept in ``fire_failures`` instead."""
        return self.ask_payload(self.encode(shape, fields, version), shape, version, role=role,
                                target=target, settle=settle, fire=fire)

    def ask_payload(self, payload, shape, version=1, role=None, target=0, settle=False,
                    fire=False):
        open_now = sum(1 for p in self._book.values() if p.answer is None and p.error is None)
        if open_now >= self.MAX_OPEN:
            raise SendRefused("this client already holds %d open conversations" % self.MAX_OPEN)
        self._next += 1
        corr = self._next
        self._book[corr] = _Pending(shape, version, fire)
        self.channel.send_message(payload, corr, role=role, target=target, settle=settle)
        return corr

    def done(self, corr):
        p = self._book.get(corr)
        return p is not None and (p.answer is not None or p.error is not None)

    def poll(self, timeout=0.0):
        """Read whatever arrived within ``timeout`` seconds and settle what it answers."""
        for e in self.channel.read(timeout):
            self._dispatch(e)

    def wait(self, corr, timeout):
        """The attested answer to ``corr``, or the reason there is none. Raises NotAnswered when
        this client's own wait runs out -- the ask stays open and may still be answered."""
        p = self._book.get(corr)
        if p is None:
            raise KeyError("no conversation %d on this connection" % corr)
        deadline = time.monotonic() + timeout
        while p.answer is None and p.error is None:
            left = deadline - time.monotonic()
            if left <= 0:
                raise NotAnswered("no attested answer to %s (conversation %d) within %.1fs"
                                  % (p.shape, corr, timeout))
            for e in self.channel.read(min(left, 0.25)):
                self._dispatch(e)
        del self._book[corr]
        if p.error is not None:
            raise p.error
        return p.answer

    def forget(self, corr):
        """Stop recognising the answer to ``corr``. Nothing at the far end is told anything."""
        self._book.pop(corr, None)

    def call(self, shape, fields=None, role=None, version=1, timeout=30.0, target=0,
             settle=False):
        """Ask and wait. ``zen.Refused`` is raised as ``Refused``."""
        a = self.wait(self.ask(shape, fields, version, role=role, target=target, settle=settle),
                      timeout)
        if a.shape == "zen.Refused":
            raise Refused(a.get("reason", ""), a)
        return a

    def open_asks(self):
        return [(c, p.shape) for c, p in self._book.items() if p.answer is None and p.error is None]

    # ---- what arrived ----------------------------------------------------------------------

    def _dispatch(self, e):
        self._arrive(e)
        # Fire-and-forget asks leave the book as soon as they are answered; a failure is kept.
        for corr in [c for c, p in self._book.items() if p.fire and (p.answer or p.error)]:
            p = self._book.pop(corr)
            if p.error is not None:
                self.fire_failures.append((p.shape, p.error))
            elif p.answer is not None and p.answer.shape == "zen.Refused":
                self.fire_failures.append((p.shape, Refused(p.answer.get("reason", ""))))

    def _arrive(self, e):
        if e.kind in ("schema", "schema-none"):
            self._schema_replies.append(e)
            return
        if e.kind == "send-refused":
            p = self._book.get(e.correlation)
            if p is not None and p.answer is None and p.error is None:
                p.error = SendRefused(e.reason)
            return
        if e.kind == "settled":
            p = self._book.get(e.correlation)
            if p is not None:
                p.settled = True
            return
        if e.kind != "delivered":
            return
        if e.dispatch_refused:
            p = self._book.get(e.correlation)
            reason = "the bus refused the delivery"
            try:
                _, _, notice = self.decode(e.payload)
                reason = notice.get("reason") or reason
            except Exception:  # a notice that does not read is still a refusal
                pass
            if p is not None and p.answer is None and p.error is None:
                p.error = DispatchRefused(reason)
            return
        p = self._book.get(e.correlation)
        if e.answers_ask and p is not None and p.answer is None and p.error is None:
            shape, version, fields = self.decode(e.payload)
            p.answer = Answer(shape, version, fields, e.sender, e.correlation, e.payload)
            return
        # NOT AN ANSWER to anything this client asked: somebody's ordinary word. Kept, bounded,
        # and never allowed to settle a conversation.
        self.unsolicited.append(e)
