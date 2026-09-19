# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""The bridge's frames, as a Python peer speaks them.

This is Loom's crossing (docs/reference/bridge.md), protocol v4, spoken from Python with the
standard library only. A frame is ``[u32 payload_len][u8 op][payload]``, little-endian; a
payload's values are Zen's compat JSON envelope, because the session host admits every Python
session to that encoding (``PayloadEncoding::Compat``) and re-admits what it sends through the
one gate. Nothing here decides meaning: it moves bytes and says which frame they were.
"""

import socket
import struct
import time

PROTOCOL = 4
MAX_FRAME = 64 * 1024 * 1024

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
    """A connected socket, framed. Blocking reads with a deadline; writes are whole."""

    def __init__(self, host, port, connect_timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=connect_timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.inbox = b""
        self.closed = False

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
        try:
            self.sock.sendall(_u32(len(payload)) + bytes([op]) + payload)
        except OSError as err:
            self.close()
            raise Disconnected(str(err))

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

    def read(self, timeout):
        """Every complete frame available within ``timeout`` seconds, as Events (maybe none)."""
        events = []
        deadline = time.monotonic() + max(0.0, timeout)
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
                return events
            self.sock.settimeout(left)
            try:
                chunk = self.sock.recv(1 << 16)
            except socket.timeout:
                return events
            except OSError as err:
                self.close()
                raise Disconnected(str(err))
            if not chunk:
                self.close()
                raise Disconnected("the host closed the connection")
            self.inbox += chunk
