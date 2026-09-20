# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""The bridge's frames, as a Python peer speaks them.

This is Loom's crossing (docs/reference/bridge.md), protocol v4, spoken from Python with the
standard library only. A frame is ``[u32 payload_len][u8 op][payload]``, little-endian; a
payload's values are Zen's compat JSON envelope, because the session host admits every Python
session to that encoding (``PayloadEncoding::Compat``) and re-admits what it sends through the
one gate. Nothing here decides meaning: it moves bytes and says which frame they were.

READING AND WRITING SHARE ONE SOCKET AND NOTHING ELSE. A read says how long it is prepared to
wait, and a zero-timeout poll waits for nothing at all; a WRITE has a bound of its own
(``WRITE_SECONDS``) and sets it every time, so what the last read or poll left the socket in
never decides what the next frame does. Backpressure from a peer that is merely slow is waited
on inside that bound; a peer that takes nothing more, or a socket that fails, ends the channel
and says how much of the frame had already gone -- because a frame half on the wire cannot be
taken back, and this client never sends one twice.
"""

import socket
import struct
import time

PROTOCOL = 4
MAX_FRAME = 64 * 1024 * 1024
#: How many buffer-fuls one zero-timeout poll may take before it returns what it has. Enough for
#: a whole frame at the limit, so a poll can complete the largest thing the host may send, and a
#: bound all the same: a peer that never stops sending cannot hold a poll forever.
MAX_POLL_READS = MAX_FRAME // (1 << 16) + 2
#: How long ONE WHOLE FRAME may take to reach a live peer. A write's bound is its own: it is set
#: on every send, so a preceding read or poll never decides it. Generous, because this is how
#: long a healthy peer may be busy before it takes the rest of a frame -- not a throughput
#: target; nothing here tries to make a slow peer faster.
WRITE_SECONDS = 30.0

# client -> host
OP_HELLO = 1
OP_LIST_WEAVES = 2
OP_DESCRIBE = 3
OP_SEND = 4
# host -> client
OP_WELCOME = 16
OP_WEAVES = 17
OP_SCHEMA = 18
OP_SCHEMA_NONE = 19
OP_DELIVERED = 20
OP_TAP = 21
OP_SEND_REFUSED = 22
OP_DENIED = 23
OP_SETTLED = 24

SEND_TO_TARGET = 0
SEND_PUBLISH = 1
SEND_TO_ROLE = 2
SEND_SETTLE = 1

DELIVERED_ANSWERS_ASK = 1
DELIVERED_DISPATCH_REFUSED = 2


class Disconnected(Exception):
    """The socket ended. What was in flight is not known to have failed -- only unanswered."""


def _u32(n):
    return struct.pack("<I", n)


def _u64(n):
    return struct.pack("<Q", n)


def _bytes(b):
    if isinstance(b, str):
        b = b.encode("utf-8")
    return _u32(len(b)) + b


class _Reader:
    def __init__(self, data):
        self.data = data
        self.i = 0

    def u8(self):
        v = self.data[self.i]
        self.i += 1
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.data, self.i)[0]
        self.i += 4
        return v

    def u64(self):
        v = struct.unpack_from("<Q", self.data, self.i)[0]
        self.i += 8
        return v

    def bytes(self):
        n = self.u32()
        if self.i + n > len(self.data):
            raise ValueError("truncated field")
        v = self.data[self.i:self.i + n]
        self.i += n
        return v

    def rest(self):
        v = self.data[self.i:]
        self.i = len(self.data)
        return v

    def remaining(self):
        return len(self.data) - self.i


class Event:
    """One frame the host shipped, decoded to its fields (which depend on ``kind``)."""

    __slots__ = ("kind", "session", "established", "sender", "correlation", "answers_ask",
                 "dispatch_refused", "authored_role", "payload", "reason", "shape", "version")

    def __init__(self, kind):
        self.kind = kind
        self.session = 0
        self.established = ""
        self.sender = 0
        self.correlation = 0
        self.answers_ask = False
        self.dispatch_refused = False
        self.authored_role = ""
        self.payload = b""
        self.reason = ""
        self.shape = ""
        self.version = 0

    def __repr__(self):
        return "Event(%s corr=%d sender=%d)" % (self.kind, self.correlation, self.sender)


def decode(op, payload):
    """A frame's payload, as an Event; None for a frame this peer does not use."""
    r = _Reader(payload)
    if op == OP_WELCOME:
        e = Event("welcome")
        e.session = r.u64()
        e.version = r.u32()
        e.established = r.bytes().decode("utf-8") if r.remaining() >= 4 else ""
        return e
    if op == OP_DENIED:
        e = Event("denied")
        e.reason = r.bytes().decode("utf-8", "replace") if r.remaining() >= 4 else ""
        return e
    if op == OP_DELIVERED:
        e = Event("delivered")
        e.sender = r.u64()
        e.correlation = r.u64()
        flags = r.u8()
        e.answers_ask = bool(flags & DELIVERED_ANSWERS_ASK)
        e.dispatch_refused = bool(flags & DELIVERED_DISPATCH_REFUSED)
        e.authored_role = r.bytes().decode("utf-8", "replace")
        e.payload = r.rest()
        return e
    if op == OP_SEND_REFUSED:
        e = Event("send-refused")
        e.correlation = r.u64()
        e.reason = r.bytes().decode("utf-8", "replace")
        return e
    if op == OP_SETTLED:
        e = Event("settled")
        e.correlation = r.u64()
        return e
    if op == OP_SCHEMA:
        e = Event("schema")
        e.payload = payload
        return e
    if op == OP_SCHEMA_NONE:
        e = Event("schema-none")
        e.shape = r.bytes().decode("utf-8", "replace")
        e.version = r.u32()
        return e
    return None  # Weaves, Tap: not used by a session client


class Channel:
    """A connected socket, framed. Reads with a deadline; writes whole, within a bound of
    their own (``WRITE_SECONDS``) that no read or poll can change."""

    #: This channel's bound on one whole frame's write, as a class default so that every
    #: Channel has one -- including one a test builds without running ``__init__``.
    write_seconds = WRITE_SECONDS

    def __init__(self, host, port, connect_timeout=5.0, write_seconds=WRITE_SECONDS):
        self.sock = socket.create_connection((host, port), timeout=connect_timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.inbox = b""
        self.closed = False
        self.write_seconds = write_seconds

    def close(self):
        if not self.closed:
            self.closed = True
            try:
                self.sock.close()
            except OSError:
                pass

    def send(self, op, payload):
        if self.closed:
            raise Disconnected("the connection is closed")
        if len(payload) > MAX_FRAME:
            raise ValueError("a frame over %d bytes is refused, never cut" % MAX_FRAME)
        self._write(_u32(len(payload)) + bytes([op]) + payload)

    def _write(self, frame):
        """ONE WHOLE FRAME, under this channel's own write bound.

        The timeout is SET HERE, every time, so a preceding ``read(0)`` -- which leaves the
        shared socket non-blocking on purpose -- cannot turn ordinary backpressure from a live
        peer into a lost frame. Inside the bound a peer that is merely slow is waited for and
        the frame goes whole.

        A write that does not finish ENDS THE CHANNEL, and says how much had already gone.
        Nothing is resent: a peer that has taken part of a frame is no longer reading frames
        where this client thinks it is, and replaying the whole of one would say the same thing
        twice. That is the honest end of a channel, not a retry policy."""
        view = memoryview(frame)
        total = len(view)
        sent = 0
        deadline = time.monotonic() + max(0.0, self.write_seconds)
        while sent < total:
            left = deadline - time.monotonic()
            if left <= 0:
                self._write_ended(sent, total, "the peer took no more of it within %.1fs"
                                  % self.write_seconds)
            try:
                self.sock.settimeout(left)  # EXPLICIT, never whatever the last read left behind
                sent += self.sock.send(view[sent:])
            except OSError as err:  # socket.timeout is an OSError too, and is one of these
                self._write_ended(sent, total, str(err))

    def _write_ended(self, sent, total, why):
        self.close()
        raise Disconnected(
            ("%s; nothing of this %d-byte frame was transmitted" % (why, total)) if sent == 0 else
            ("%s; %d of this frame's %d bytes had already been transmitted, so the channel is no "
             "longer framed and nothing is sent again" % (why, sent, total)))

    def hello(self, claimed, credential):
        self.send(OP_HELLO, _u32(PROTOCOL) + _bytes(claimed) + _bytes(credential))

    def describe(self, name, version):
        self.send(OP_DESCRIBE, _bytes(name) + _u32(version))

    def send_message(self, payload, correlation, role=None, target=0, settle=False):
        kind = SEND_TO_ROLE if role else SEND_TO_TARGET
        flags = SEND_SETTLE if settle else 0
        body = (bytes([kind, flags]) + _u64(0) + _u64(target) + _u64(0) + _u64(correlation) +
                _bytes(role or "") + payload)
        self.send(OP_SEND, body)

    def _frame(self):
        if len(self.inbox) < 5:
            return None
        n = struct.unpack_from("<I", self.inbox, 0)[0]
        if n > MAX_FRAME:
            self.close()
            raise Disconnected("the host sent a frame over the limit")
        if len(self.inbox) < 5 + n:
            return None
        op = self.inbox[4]
        payload = self.inbox[5:5 + n]
        self.inbox = self.inbox[5 + n:]
        return op, payload

    def _fill(self, timeout):
        """One read from the socket, waiting at most ``timeout`` seconds. True when bytes were
        added. A ZERO timeout takes what has already arrived and never waits for what has not;
        it is not the same as reading nothing. It leaves the shared socket non-blocking, which
        is this read's business and nobody else's: ``_write`` sets the mode it needs."""
        self.sock.settimeout(max(0.0, timeout))
        try:
            chunk = self.sock.recv(1 << 16)
        except (socket.timeout, BlockingIOError):
            return False  # a zero timeout makes the socket non-blocking: "nothing here yet"
        except OSError as err:
            self.close()
            raise Disconnected(str(err))
        if not chunk:
            self.close()
            raise Disconnected("the host closed the connection")
        self.inbox += chunk
        return True

    def read(self, timeout):
        """Every complete frame available within ``timeout`` seconds, as Events (maybe none).

        ``read(0)`` is a POLL, not a no-op: it services every frame already on this socket and
        returns without waiting for one more. A frame that has arrived is therefore seen at zero
        timeout, and a partial frame is kept for the next read rather than blocking for its
        remainder. The work is bounded either way -- at most one whole frame's worth of reads."""
        events = []
        deadline = time.monotonic() + max(0.0, timeout)
        polls = 0
        while True:
            f = self._frame()
            while f is not None:
                e = decode(f[0], f[1])
                if e is not None:
                    events.append(e)
                f = self._frame()
            if events or self.closed:
                return events
            left = deadline - time.monotonic()
            if left <= 0:
                polls += 1
                if polls > MAX_POLL_READS or not self._fill(0.0):
                    return events
                continue
            if not self._fill(left):
                return events
