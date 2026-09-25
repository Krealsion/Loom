# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""A subscription to another participant's publications, through an observation relay.

``ctx.observe(producer, shapes, via=link)`` asks the relay at ``loom.observe`` -- on this host, or
on a far one through a link -- to tell this run each publication of ``shapes`` by whoever holds
the office ``producer``, from now on. What comes back is a ``Subscription``; ``next()`` hands over,
in order, what it was told:

  ``Observation``   one publication: ``shape``, ``fields`` (decoded with the descriptors the relay
                    sent, so this host need not know the shape), ``seq``, ``producer`` and
                    ``incarnation``, ``cause``, and where it came from (``link``, ``epoch``,
                    ``session``, the far ``delivery`` and ``published_in`` history numbers)
  ``Gap``           publications that were not told. ``said`` gaps are the relay's own count
                    (its window was shut); a ``local`` gap is a hole in the numbering this client
                    found, or words this client dropped past its own bound -- a loss somewhere
                    between the relay and here, with how many
  ``Ended``         the subscription is over: ``released``, ``revoked`` by the far host, ``lost``
                    (the link's session ended) or ``gone``. Nothing follows it

WHAT IT PROMISES. Every number the relay says arrives, or its absence is handed over as a Gap:
nothing is lost without being said. It does NOT promise that nothing is lost -- a window, a
dropped connection or a slow reader each cost observations, and each is reported. A count that
must be exact must treat any Gap as "cannot tell", never as zero.

``cause`` is this run's own correlation for the settle-requested ask whose synchronous work
published it (``Pending.correlation``), or 0. A settled ask's caused observations have arrived
before the ask's own answer returns, so they can be checked right after it.

WHAT IT IS NOT. Not an answer to anything: an observation is ordinary speech the relay said, and
knowing it grants nothing. Ending a subscription stops the words, not whatever the producer is
doing. The relay's word "released" answers this run's release; this run's own cleanup releases
every subscription it still holds.
"""

import collections
import time

from . import values

ROLE = "loom.observe"
WORDS = ("loom.observe.Observed", "loom.observe.Gap", "loom.observe.Ended")


class Observation(object):
    """One publication the relay told."""

    __slots__ = ("shape", "version", "fields", "seq", "producer", "incarnation", "office",
                 "coalesced", "cause", "delivery", "published_in", "link", "epoch", "session",
                 "received")

    def __init__(self, shape, version, fields, word, received):
        self.shape, self.version, self.fields = shape, version, fields
        self.seq = int(word["seq"])
        self.producer = int(word.get("producer", 0))
        self.incarnation = int(word.get("incarnation", 0))
        self.office = word.get("office", "")
        self.coalesced = int(word.get("coalesced", 0))
        self.cause = int(word.get("cause", 0))
        self.delivery = int(word.get("delivery", 0))
        self.published_in = int(word.get("published_in", 0))
        self.link = word.get("link", "")
        self.epoch = int(word.get("epoch", 0))
        self.session = int(word.get("session", 0))
        self.received = received

    kind = "observed"

    def __getitem__(self, key):
        return self.fields[key]

    def get(self, key, default=None):
        return self.fields.get(key, default)

    def record(self):
        """A plain dict for evidence files."""
        return {"kind": "observed", "seq": self.seq, "shape": self.shape, "version": self.version,
                "fields": _plain(self.fields), "producer": self.producer,
                "incarnation": self.incarnation, "office": self.office,
                "coalesced": self.coalesced, "cause": self.cause, "delivery": self.delivery,
                "published_in": self.published_in, "link": self.link, "epoch": self.epoch,
                "session": self.session, "received": round(self.received, 4)}

    def __repr__(self):
        return "Observation(#%d %s %r)" % (self.seq, self.shape, self.fields)


class Gap(object):
    """Publications that were not told. ``local`` when this client found or made the hole;
    ``through`` is the last number it covers."""

    kind = "gap"

    def __init__(self, seq, lost, reason, local, through=None):
        self.seq, self.lost, self.reason, self.local = seq, lost, reason, local
        self.through = seq if through is None else through

    def record(self):
        return {"kind": "gap", "seq": self.seq, "through": self.through, "lost": self.lost,
                "reason": self.reason, "local": self.local}

    def __repr__(self):
        return "Gap(#%s lost %s%s: %s)" % (self.seq, self.lost, " local" if self.local else "",
                                           self.reason)


class Ended(object):
    """The subscription is over, and why."""

    kind = "ended"

    def __init__(self, word):
        self.seq = int(word.get("seq", 0))
        self.last = int(word.get("last", 0))
        self.lost = int(word.get("lost", 0))
        self.how = word.get("kind", "")
        self.reason = word.get("reason", "")
        self.link = word.get("link", "")
        self.epoch = int(word.get("epoch", 0))

    def record(self):
        return {"kind": "ended", "seq": self.seq, "last": self.last, "lost": self.lost,
                "how": self.how, "reason": self.reason, "link": self.link, "epoch": self.epoch}

    def __repr__(self):
        return "Ended(#%d %s: %s)" % (self.seq, self.how, self.reason)


def _covers(item):
    """The last number an item accounts for: a Gap covers its whole range."""
    return int(getattr(item, "through", getattr(item, "seq", 0)) or 0)


def _plain(v):
    if isinstance(v, dict):
        return dict((k, _plain(x)) for k, x in v.items())
    if isinstance(v, list):
        return [_plain(x) for x in v]
    if isinstance(v, (bytes, bytearray)):
        return {"bytes": len(v)}
    return v


def shape_refs(shapes):
    """``[("BuildStatus", 4), "TdSeen@1"]`` -> ``[{"name": ..., "version": ...}]``."""
    out = []
    for s in shapes:
        if isinstance(s, (tuple, list)):
            name, version = s[0], int(s[1])
        else:
            name, _, version = str(s).partition("@")
            version = int(version or 1)
        out.append({"name": name, "version": version})
    return out


class Subscription(object):
    """One subscription this run holds (see the module note). Made by ``Context.observe``."""

    def __init__(self, ctx, answer, via, requested, max_pending=None):
        self.ctx = ctx
        self.via = via
        self.subscription = int(answer["subscription"])
        self.relay = answer["relay"]
        self.producer = answer["producer"]
        self.holder = int(answer.get("holder", 0))
        self.incarnation = int(answer.get("incarnation", 0))
        self.window = int(answer.get("window", 0))
        self.encoding = answer.get("encoding", "")
        self.latest = list(answer.get("latest") or [])
        self.requested = requested
        #: Who says this subscription's words here: the relay, or the link that carries them.
        self.speaker = answer.sender
        self.key = (self.speaker, self.subscription, self.relay)
        self.schemas = self._descriptors(answer.get("shapes") or b"")
        self.codec = values.Codec(self._resolve)
        self.max_pending = int(max_pending or max(2 * self.window, 64))
        self._items = collections.deque()
        self._expected = int(answer.get("next", 1))  # the next number the relay will say
        self._consumed = self._expected - 1          # the last number handed to the reader
        self._acked = self._expected - 1
        self._ack = None                             # the one acknowledgement in flight
        self._local_lost = 0                         # dropped here past max_pending, not yet said
        self._local_from = 0
        self._local_through = 0
        self.ended = None
        self.said = collections.Counter()            # observed / gap / ended words arrived
        self.lost_said = 0                           # the relay's own Gap counts
        self.lost_here = 0                           # holes and drops this client found or made
        self.repeated = 0                            # numbers that arrived again: kept out
        self.began = time.monotonic()

    # ---- what the relay said ------------------------------------------------------------------

    def _descriptors(self, data):
        """The subscribed shapes and their closure, from ``loom.observe.Shapes``."""
        if not data:
            return {}
        _, _, fields = values.open_envelope(bytes(data))
        out = {}
        for desc in list(fields.get("referenced") or []) + list(fields.get("shapes") or []):
            s = values.schema_from_desc(desc)
            out[(s.name, s.version)] = s
        return out

    def _resolve(self, name, version):
        key = (name, int(version))
        if key in self.schemas:
            return self.schemas[key]
        return self.ctx.conn.schema(name, version)  # this host's own, for a shape it knows

    def _take(self, shape, word):
        """One word from the router (``Connection._observation``), in arrival order."""
        if self.ended is not None:
            return  # nothing follows an ending
        seq = int(word.get("seq", 0))
        self.said[shape.rsplit(".", 1)[-1]] += 1
        if seq < self._expected:
            # A number already passed: a duplicate, or a word from before a hole was said. Kept
            # out -- a counted observation must be counted once.
            self.repeated += 1
            return
        if seq > self._expected:
            missing = seq - self._expected
            self.lost_here += missing
            self._push(Gap(self._expected, missing, "numbers %d to %d never arrived here"
                           % (self._expected, seq - 1), local=True, through=seq - 1), force=True)
        self._expected = seq + 1
        if shape == "loom.observe.Ended":
            self.ended = Ended(word)
            self._push(self.ended, force=True)
            self.ctx.conn.unwatch(self)
            return
        if shape == "loom.observe.Gap":
            self.lost_said += int(word.get("lost", 0))
            self._push(Gap(seq, int(word.get("lost", 0)), word.get("reason", ""), local=False),
                       force=True)
            return
        try:
            name, version, fields = values.open_envelope(bytes(word.get("payload") or b""))
            decoded = self.codec.decode(self._resolve(name, version), fields)
        except Exception as err:  # unreadable is reported, never skipped silently
            self.lost_here += 1
            self._push(Gap(seq, 1, "observation %d could not be read: %s" % (seq, err),
                           local=True), force=True)
            return
        self._push(Observation(name, version, decoded, word, time.monotonic()))

    def _push(self, item, force=False):
        observations = sum(1 for i in self._items if i.kind == "observed")
        if not force and observations >= self.max_pending:
            if self._local_lost == 0:
                self._local_from = item.seq
            self._local_through = item.seq
            self._local_lost += 1
            self.lost_here += 1
            return
        self._say_local_loss()
        self._items.append(item)

    def _say_local_loss(self):
        """Observations dropped here come after everything already waiting, so their Gap goes at
        the end -- as soon as anything else arrives, or the reader has taken what waits."""
        if self._local_lost:
            self._items.append(Gap(self._local_from, self._local_lost,
                                   "%d observation(s) dropped here: %d were already waiting "
                                   "to be read" % (self._local_lost, self.max_pending), local=True,
                                   through=self._local_through))
            self._local_lost = 0

    # ---- reading ------------------------------------------------------------------------------

    def poll(self):
        """Read whatever has arrived; return how many items wait."""
        self.ctx._pump(0.0)
        self._collect_ack()
        return len(self._items)

    def next(self, timeout=None):
        """The next item (Observation, Gap or Ended), waiting up to ``timeout`` seconds (None: as
        long as it takes). None when the wait ran out -- this run's own decision, never evidence
        that the producer is silent for good. A cancellation of the run ends the wait."""
        end = None if timeout is None else time.monotonic() + timeout
        while True:
            self.ctx._check_cancel()
            self._collect_ack()
            if not self._items:
                self._say_local_loss()
            if self._items:
                item = self._items.popleft()
                self._consumed = max(self._consumed, _covers(item))
                self._acknowledge()
                return item
            if self.ended is not None:
                return None
            self._acknowledge(idle=True)
            left = None if end is None else end - time.monotonic()
            if left is not None and left <= 0:
                return None
            self.ctx._pump(0.25 if left is None else min(0.25, max(0.0, left)))

    def drain(self):
        """Every item that has already arrived, without waiting."""
        self.poll()
        out = []
        while self._items:
            item = self._items.popleft()
            self._consumed = max(self._consumed, _covers(item))
            out.append(item)
            if not self._items:
                self._say_local_loss()
        self._acknowledge(idle=True)
        return out

    # ---- the window -------------------------------------------------------------------------

    def _acknowledge(self, idle=False):
        """Tell the relay what this reader has taken, so it may say more: half a window at a time,
        or -- once there is nothing left to read -- an eighth, so a burst after a quiet spell finds
        nearly the whole window open without an ask for every word. One acknowledgement in flight."""
        if self.ended is not None or self._ack is not None:
            return
        taken = self._consumed - self._acked
        if taken <= 0 or not (taken >= max(1, self.window // 2) or
                              (idle and not self._items and taken >= max(1, self.window // 8))):
            return
        self._ack = (self._consumed, self.ctx.ask_async(
            ROLE, "loom.observe.Acknowledge", {"subscription": self.subscription,
                                                "relay": self.relay, "through": self._consumed},
            via=self.via))

    def _collect_ack(self):
        if self._ack is None:
            return
        through, pending = self._ack
        if not pending.done():
            return
        self._ack = None
        try:
            pending.wait(0.0)
            self._acked = max(self._acked, through)
        except Exception as err:  # a refused or lost acknowledgement is a fact about the window
            self.ctx.note("observation %d: acknowledgement through %d came back %s: %s"
                          % (self.subscription, through, type(err).__name__, err))

    # ---- ending -------------------------------------------------------------------------------

    def release(self, timeout=15.0):
        """End this subscription: the relay's own ``Ended`` answer, or why there is none. Items
        not yet read stay readable; nothing more is said after them."""
        if self.ended is not None:
            return self.ended
        self.ctx.conn.unwatch(self)
        a = self.ctx.ask(ROLE, "loom.observe.Release", {"subscription": self.subscription,
                                                         "relay": self.relay},
                         via=self.via, timeout=timeout)
        self.ended = Ended(a.fields)
        return self.ended

    def summary(self):
        return {"subscription": self.subscription, "relay": self.relay, "producer": self.producer,
                "holder": self.holder, "incarnation": self.incarnation, "window": self.window,
                "encoding": self.encoding, "requested": self.requested, "via": self.via,
                "words": dict(self.said), "lost_said": self.lost_said,
                "lost_here": self.lost_here, "repeated": self.repeated,
                "last_taken": self._consumed,
                "acknowledged": self._acked,
                "ended": self.ended.record() if self.ended is not None else None}
