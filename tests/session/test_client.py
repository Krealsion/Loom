# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""THE PYTHON CLIENT'S ANSWER RULE, ITS SETTLEMENT RULE AND ITS POLLING, without a host.

A conversation settles on the delivery Loom attests as ITS answer (``answers_ask`` under the
conversation's own correlation), or on Loom's refusal of the ask's send or dispatch -- and on
nothing else: not an ordinary word under the same number, not an answer under a number this
client never minted, not silence. The journey cannot stage a forged answer (no participant on a
session host may say one to a client), so the rule is pinned here, against the shipped client
with a stand-in channel that records what was sent and hands over the frames a case queues.

Three more rules are pinned here for the same reason -- a real host cannot be asked to deliver
one half of something and hold the other:

  SETTLEMENT (S1..S6)  an ask that asked to be settled is complete only with BOTH its attested
                       answer and its Settled frame, in either arrival order; a refusal completes
                       it whatever was asked; an answer alone leaves it open, and a wait that runs
                       out says which half is missing and keeps the half that came.
  POLLING (P1..P5)     ``read(0)`` services what is already on the socket and never waits for what
                       is not, over a REAL socket pair: whole frames, no data, half a frame.
  WRITING (W1..W4)     a write's bound is its OWN, not whatever mode the last poll left the
                       shared socket in: a slow-but-live peer still gets the whole frame, a peer
                       that takes nothing ends the write at the channel's number, and a write
                       that cannot happen at all is still Disconnected.
  CANCELLING (K1..K4)  the public ``cancel_requested`` takes the manager's directive, latches, and
                       never raises -- and the cleanup phase can still ask after a cancellation.

    test_client.py --runtime <dir holding loom_session>

Exit 0 only when every check held.
"""

import argparse
import collections
import socket
import struct
import sys
import threading
import time

CHECKS = []


def check(name, condition, detail=""):
    CHECKS.append((name, bool(condition)))
    print("%s  %s%s" % ("ok  " if condition else "FAIL", name,
                        ("  -- %s" % (detail,)) if detail != "" else ""), flush=True)


class StandInChannel(object):
    """What the client writes to and reads from: nothing crosses a socket."""

    def __init__(self):
        self.sent = []
        self.payloads = []
        self.frames = collections.deque()

    def send_message(self, payload, correlation, role=None, target=0, settle=False):
        self.sent.append((correlation, role, settle))
        self.payloads.append(payload)

    def read(self, timeout):
        out = list(self.frames)
        self.frames.clear()
        return out

    def close(self):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", required=True)
    sys.path.insert(0, ap.parse_args().runtime)
    from loom_session import client, values, wire

    def connection():
        # The shipped client, admitted as session 7, knowing one shape -- built without a socket.
        c = client.Connection.__new__(client.Connection)
        c.channel = StandInChannel()
        c.session = 7
        c.established = "client"
        c._book = {}
        c._next = 0
        c._schemas = {("zen.Ack", 1): values.Schema("zen.Ack", 1, [])}
        c._schema_replies = collections.deque()
        c.unsolicited = collections.deque(maxlen=64)
        c.fire_failures = collections.deque(maxlen=64)
        c._watching = {}
        c._unclaimed = collections.deque()
        c._evicted = 0
        c.strays = collections.deque(maxlen=16)
        c._words = collections.deque()
        c._routing = False
        c.codec = values.Codec(c.schema)
        return c

    def delivered(corr, answers, sender=3):
        e = wire.Event("delivered")
        e.correlation = corr
        e.answers_ask = answers
        e.sender = sender
        e.payload = values.envelope("zen.Ack", 1, {})
        return e

    c = connection()
    corr = c.ask("zen.Ack", {}, role="somewhere")
    check("C0 an ask goes out under a correlation this client minted",
          c.channel.sent == [(corr, "somewhere", False)], c.channel.sent)
    c.channel.frames.append(delivered(corr, False, sender=9))
    c.poll(0.0)
    check("C1 a word under the ask's own number that Loom does not attest as its answer settles "
          "nothing, and is kept aside", not c.done(corr) and len(c.unsolicited) == 1 and
          c.open_asks() == [(corr, "zen.Ack")])
    c.channel.frames.append(delivered(corr, True, sender=3))
    a = c.wait(corr, 1.0)
    check("C2 the attested answer settles it, with the author Loom stamped",
          a.shape == "zen.Ack" and a.sender == 3 and c.open_asks() == [])
    c.channel.frames.append(delivered(99, True))
    c.poll(0.0)
    check("C3 an attested answer under a number this client never minted settles nothing",
          len(c.unsolicited) == 2 and c.open_asks() == [])
    corr2 = c.ask("zen.Ack", {})
    refused = wire.Event("send-refused")
    refused.correlation = corr2
    refused.reason = "no grant for zen.Ack"
    c.channel.frames.append(refused)
    try:
        c.wait(corr2, 1.0)
        why = None
    except client.SendRefused as err:
        why = str(err)
    check("C4 a refused send ends the conversation as refused, in Loom's words",
          why is not None and "no grant" in why, why)
    corr3 = c.ask("zen.Ack", {})
    try:
        c.wait(corr3, 0.05)
        silent = False
    except client.NotAnswered:
        silent = True
    check("C5 silence is this client's own wait running out; the ask stays open",
          silent and c.open_asks() == [(corr3, "zen.Ack")])

    # ---- settlement: what `settle=True` requires of completion --------------------------------
    def settled_frame(corr):
        e = wire.Event("settled")
        e.correlation = corr
        return e

    s = connection()
    corr = s.ask("zen.Ack", {}, role="office", settle=True)
    check("S0 the request to settle goes out on the wire", s.channel.sent == [(corr, "office", True)],
          s.channel.sent)
    s.channel.frames.append(delivered(corr, True))
    s.poll(0.0)
    check("S1 an attested answer alone does NOT complete an ask that asked to be settled",
          not s.done(corr) and s.open_asks() == [(corr, "zen.Ack")], s.open_asks())
    try:
        s.wait(corr, 0.05)
        why = ""
    except client.NotAnswered as err:
        why = str(err)
    check("S2 a wait that runs out says which half is missing, and keeps the half that came",
          "settlement it asked for has not arrived" in why and s.open_asks() == [(corr,
                                                                                 "zen.Ack")], why)
    s.channel.frames.append(settled_frame(corr))
    a = s.wait(corr, 1.0)
    check("S3 the settlement completes it, and the answer that was held is the one returned",
          a.shape == "zen.Ack" and s.open_asks() == [])

    s2 = connection()
    corr = s2.ask("zen.Ack", {}, role="office", settle=True)
    s2.channel.frames.append(settled_frame(corr))
    s2.poll(0.0)
    settled_only = not s2.done(corr)
    s2.channel.frames.append(delivered(corr, True))
    a2 = s2.wait(corr, 1.0)
    check("S4 the other arrival order is the same rule: settlement first completes nothing, and "
          "the answer then completes it", settled_only and a2.shape == "zen.Ack")

    s3 = connection()
    corr = s3.ask("zen.Ack", {}, role="office", settle=True)
    refused = wire.Event("send-refused")
    refused.correlation = corr
    refused.reason = "no grant for zen.Ack"
    s3.channel.frames.append(refused)
    try:
        s3.wait(corr, 1.0)
        refusal = ""
    except client.SendRefused as err:
        refusal = str(err)
    check("S5 a refusal completes a settle-requested ask whatever was asked: nothing is in "
          "motion to settle", "no grant" in refusal, refusal)

    s4 = connection()
    corr = s4.ask("zen.Ack", {}, role="office")  # settlement NOT requested
    s4.channel.frames.append(delivered(corr, True))
    a4 = s4.wait(corr, 1.0)
    check("S6 without the request, the attested answer alone is still enough",
          a4.shape == "zen.Ack" and s4.open_asks() == [])

    # ---- polling: read(0) over a REAL socket pair ---------------------------------------------
    def pair(buffer_bytes=0):
        left, right = socket.socketpair()
        if buffer_bytes:
            # SMALL BUFFERS so backpressure is a certainty rather than a hope. How much a kernel
            # then absorbs is its own business: no check below depends on the number.
            left.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, buffer_bytes)
            right.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, buffer_bytes)
        ch = wire.Channel.__new__(wire.Channel)
        ch.sock = left
        ch.inbox = b""
        ch.closed = False
        return ch, left, right

    def settled_bytes(corr):
        body = struct.pack("<Q", corr)
        return struct.pack("<I", len(body)) + bytes([wire.OP_SETTLED]) + body

    def poll_until(ch, seconds=5.0):
        """ZERO-TIMEOUT polls only. That the kernel has the bytes is the harness's wait; that a
        poll SEES them is the claim, and a poll never waits for one more byte."""
        deadline = time.monotonic() + seconds
        while True:
            events = ch.read(0.0)
            if events or time.monotonic() >= deadline:
                return events
            time.sleep(0.01)

    ch, left, right = pair()
    right.sendall(settled_bytes(5))
    events = poll_until(ch)
    check("P1 read(0) services a whole frame that has already arrived",
          len(events) == 1 and events[0].kind == "settled" and events[0].correlation == 5,
          [e.kind for e in events])
    started = time.monotonic()
    nothing = ch.read(0.0)
    check("P2 read(0) on a socket with nothing on it returns at once, with nothing",
          nothing == [] and time.monotonic() - started < 1.0, time.monotonic() - started)
    whole = settled_bytes(9)
    right.sendall(whole[:3])                   # HALF A FRAME: not even its length is complete
    started = time.monotonic()
    partial = ch.read(0.0)
    check("P3 read(0) with half a frame on the socket waits for none of the rest",
          partial == [] and time.monotonic() - started < 1.0, time.monotonic() - started)
    right.sendall(whole[3:])
    rest = poll_until(ch)
    check("P4 the frame is completed by the bytes that follow, and read(0) then has it",
          len(rest) == 1 and rest[0].correlation == 9, [e.kind for e in rest])
    right.close()
    gone = None
    deadline = time.monotonic() + 5.0
    while gone is None and time.monotonic() < deadline:
        try:
            ch.read(0.0)
        except wire.Disconnected as err:
            gone = str(err)
    check("P5 a connection that ended is Disconnected at zero timeout too, not silence",
          gone is not None, gone)
    left.close()

    # ---- writing: a frame's bound is its own, whatever the last poll left behind --------------
    #
    # Reads and writes share one socket. `read(0)` deliberately makes it non-blocking, and a
    # write that INHERITED that mode turned ordinary backpressure from a live peer into a lost
    # frame and a closed channel. These four say the mode is the write's own, that a slow-but-
    # live peer gets the whole frame, and that a real write failure is still a real failure.
    big = b"y" * (1 << 20)               # 1 MiB: under MAX_FRAME, over any socket buffer
    frame_bytes = len(big) + 5

    ch, left, right = pair(4096)
    polled = ch.read(0.0)                # THE POLL: nothing there, socket left non-blocking
    mode_after_poll = left.gettimeout()
    taken = {"n": 0}

    def drain_slowly():
        """A peer that is BUSY, not gone: it takes nothing for a moment and then reads it all."""
        time.sleep(0.1)
        right.settimeout(10.0)
        while taken["n"] < frame_bytes:
            try:
                chunk = right.recv(1 << 16)
            except OSError:
                return
            if not chunk:
                return
            taken["n"] += len(chunk)

    reader = threading.Thread(target=drain_slowly)
    reader.start()
    wrote = ""
    try:
        ch.send(wire.OP_SEND, big)
    except Exception as err:
        wrote = "%s: %s" % (type(err).__name__, err)
    mode_after_write = left.gettimeout()
    reader.join(20)
    check("W1 a poll leaves the socket non-blocking, and the write that follows does not keep "
          "that mode", polled == [] and mode_after_poll == 0.0 and mode_after_write != 0.0,
          (mode_after_poll, mode_after_write))
    check("W2 the whole frame reaches a peer that is merely slow, and nothing reports a failure",
          taken["n"] == frame_bytes and wrote == "" and not ch.closed,
          (taken["n"], frame_bytes, wrote))
    ch.close()
    right.close()

    # A peer that STOPS taking, on every platform alike. How much a kernel buffers before it
    # pushes back is its own business and no check here depends on it; what the write does when
    # a peer takes no more is this client's, so that is staged directly under the real `_write`.
    class StallingSocket(object):
        def __init__(self, take_once):
            self.take_once = take_once
            self.taken = 0
            self.timeouts = []
            self.closed = False

        def settimeout(self, seconds):
            self.timeouts.append(seconds)

        def send(self, view):
            if self.taken >= self.take_once:
                time.sleep(max(0.0, self.timeouts[-1]))  # it waits, and still takes nothing
                return 0
            n = min(len(view), self.take_once - self.taken)
            self.taken += n
            return n

        def sendall(self, data):
            # A stand-in is a whole socket: a caller that reaches for `sendall` gets what one
            # does -- as much as this peer takes, and then the timeout it is waiting under.
            self.send(memoryview(data))
            raise socket.timeout("timed out")

        def close(self):
            self.closed = True

    stalling = StallingSocket(4096)
    ch = wire.Channel.__new__(wire.Channel)
    ch.sock = stalling
    ch.inbox = b""
    ch.closed = False
    ch.write_seconds = 0.5
    started = time.monotonic()
    stalled = ""
    try:
        ch.send(wire.OP_SEND, big)
    except wire.Disconnected as err:
        stalled = str(err)
    took = time.monotonic() - started
    check("W3 a peer that stops taking ends the write at the channel's own bound, says how much "
          "had already gone, and closes the channel rather than sending it again",
          "within 0.5s" in stalled and ("%d of this frame's %d bytes" % (4096, frame_bytes))
          in stalled and "nothing is sent again" in stalled and ch.closed and stalling.closed and
          took < 5.0 and stalling.timeouts and all(0.0 < t <= 0.5 for t in stalling.timeouts),
          (stalled[:170], round(took, 2), len(stalling.timeouts)))

    # ...and a GENUINE write failure is still one: the peer is gone before a byte is offered.
    ch, left, right = pair()
    right.close()
    left.close()                                  # the socket itself is no longer usable
    failed = ""
    try:
        ch.send(wire.OP_SEND, b"gone")
    except wire.Disconnected as err:
        failed = str(err)
    check("W4 a write that cannot happen at all is Disconnected, with nothing claimed to have "
          "been transmitted", failed != "" and "nothing of this" in failed and ch.closed,
          failed[:140])

    # ---- cancelling: the public property, and the cleanup phase -------------------------------
    from loom_session import tool as ltool

    text = values.TypeRef(values.TEXT)
    shapes = {
        ("zen.Ack", 1): values.Schema("zen.Ack", 1, []),
        ("loom.runs.Control", 1): values.Schema("loom.runs.Control", 1, []),
        ("loom.runs.Directive", 1): values.Schema(
            "loom.runs.Directive", 1, [values.Field("directive", True, text),
                                       values.Field("reason", True, text)]),
        ("loom.runs.Progress", 1): values.Schema(
            "loom.runs.Progress", 1, [values.Field("step", True, text),
                                      values.Field("note", True, text),
                                      values.Field("pending", True,
                                                   values.TypeRef(values.LIST, element=text))]),
    }

    def cancelled_context():
        """A context whose manager has answered its standing question with `cancel`, the answer
        sitting unread on the channel exactly as it would be mid-run."""
        c = connection()
        c._schemas = dict(shapes)
        control = c.ask("loom.runs.Control", {}, role="loom.runs")
        ctx = ltool.Context(c, {"name": "t", "lifetime": "L", "tool": "p/t", "revision": "r",
                                "inputs": {}, "out": "."}, ".", control)
        e = wire.Event("delivered")
        e.correlation = control
        e.answers_ask = True
        e.sender = 3
        e.payload = values.envelope("loom.runs.Directive", 1,
                                    {"directive": "cancel", "reason": "a client asked"})
        c.channel.frames.append(e)
        return c, ctx

    _, ctx = cancelled_context()
    first, second = ctx.cancel_requested, ctx.cancel_requested
    check("K1 cancel_requested takes the directive the manager sent and says so, without raising",
          first is True and second is True, (first, second))
    raised = None
    try:
        ctx._check_cancel()
    except ltool.Cancelled as err:
        raised = str(err)
    check("K2 the implicit check at a wait point still raises Cancelled, with the reason",
          raised == "a client asked", raised)

    _, ctx = cancelled_context()
    ctx._pump(0.0)
    ctx._take_cancel()
    attempted = []

    def close():
        attempted.append("asked")
        ctx.ask_async("zengine.input", "zen.Ack", {})

    def refuses():
        raise RuntimeError("the link is lost: session 1 is Workshop's to close")

    ctx.on_cleanup(refuses, "close the far thing")
    ctx.on_cleanup(close, "close input session 1")
    lines = ctx._run_cleanups()
    check("K3 a cleanup may still ask after a cancellation -- the whole point of the phase",
          attempted == ["asked"] and lines and lines[0] == "cleanup close input session 1: done",
          lines)
    check("K4 a cleanup that cannot finish is recorded as what it was, never as done",
          len(lines) == 2 and "RuntimeError" in lines[1] and "link is lost" in lines[1], lines)

    # ---- observing: a subscription's words, held, numbered, decoded and acknowledged ----------
    # A real relay cannot be asked to skip a number, forge a speaker or say a word before its own
    # answer has been read, so these rules are pinned against the shipped client here.
    import base64 as lbase64
    from loom_session import observe as lobserve

    int_t, bytes_t = values.TypeRef(values.INT), values.TypeRef(values.BYTES)

    def word_schema(name, fields):
        return values.Schema(name, 1, [values.Field(n, False, t) for n, t in fields])

    common = [("subscription", int_t), ("relay", text), ("seq", int_t), ("link", text),
              ("epoch", int_t), ("session", int_t)]
    watched = dict(shapes)
    watched[("loom.observe.Observed", 1)] = word_schema("loom.observe.Observed", common + [
        ("shape", text), ("version", int_t), ("payload", bytes_t), ("producer", int_t),
        ("incarnation", int_t), ("office", text), ("coalesced", int_t), ("cause", int_t),
        ("delivery", int_t), ("published_in", int_t)])
    watched[("loom.observe.Gap", 1)] = word_schema("loom.observe.Gap", common + [
        ("lost", int_t), ("reason", text)])
    watched[("loom.observe.Ended", 1)] = word_schema("loom.observe.Ended", common + [
        ("last", int_t), ("lost", int_t), ("kind", text), ("reason", text)])
    watched[("loom.observe.Acknowledge", 1)] = word_schema("loom.observe.Acknowledge", [
        ("subscription", int_t), ("relay", text), ("through", int_t)])
    watched[("loom.link.Ask", 1)] = word_schema("loom.link.Ask", [
        ("role", text), ("target", int_t), ("payload", bytes_t),
        ("settle", values.TypeRef(values.BOOL))])

    probe_desc = {"name": "Probe", "version": "1",
                  "fields": [{"name": "n", "required": True, "type": [{"kind": "0"}]}]}
    described = values.envelope("loom.observe.Shapes", 1, {"shapes": [probe_desc]})
    LINK = 41

    def watching_context():
        c = connection()
        c._schemas = dict(watched)
        control = c.ask("loom.runs.Control", {}, role="loom.runs")
        ctx = ltool.Context(c, {"name": "t", "lifetime": "L", "tool": "p/t", "revision": "r",
                                "inputs": {}, "out": "."}, ".", control)
        return c, ctx

    def subscription(ctx, window=8, max_pending=None, number=3, relay="R1"):
        answer = client.Answer("loom.observe.Subscribed", 1, {
            "subscription": number, "relay": relay, "producer": "far.ticker", "holder": 12,
            "incarnation": 1, "window": window, "encoding": "compat", "latest": [],
            "shapes": described, "next": 1}, LINK, 5, b"")
        return lobserve.Subscription(ctx, answer, "far", [{"name": "Probe", "version": 1}],
                                     max_pending)

    def word(shape, fields, sender=LINK):
        e = wire.Event("delivered")
        e.correlation = 0
        e.answers_ask = False
        e.sender = sender
        e.payload = values.envelope(shape, 1, fields)
        return e

    def observed(seq, n, sender=LINK, number=3, relay="R1", cause=0):
        return word("loom.observe.Observed", {
            "subscription": str(number), "relay": relay, "seq": str(seq), "shape": "Probe",
            "version": "1", "producer": "12", "cause": str(cause),
            "payload": lbase64.b64encode(values.envelope("Probe", 1, {"n": str(n)})).decode()},
            sender)

    c, ctx = watching_context()
    c.channel.frames.extend([observed(1, 10), observed(2, 11)])
    c.poll(0.0)
    held = c.stray_observations
    sub = subscription(ctx)
    c.watch(sub)
    first, second = sub.next(0.0), sub.next(0.0)
    check("O1 words said before this client registered the subscription (the relay's first words, "
          "read with its answer) are held and handed over in order, not lost",
          held == 2 and c.stray_observations == 0 and first.seq == 1 and second.seq == 2 and
          first.fields == {"n": 10} and second["n"] == 11, (held, first, second))
    check("O2 a payload is decoded with the descriptors the relay sent, for a shape this host "
          "never declared", first.shape == "Probe" and ("Probe", 1) not in c._schemas)
    c.channel.frames.extend([observed(5, 14), observed(5, 14)])
    c.poll(0.0)
    hole, after = sub.next(0.0), sub.next(0.0)
    check("O3 a hole in the numbering is handed over as a local Gap of exactly the missing count, "
          "before what follows it, and a repeated number is kept out",
          hole.kind == "gap" and hole.local and hole.lost == 2 and hole.through == 4 and
          after.seq == 5 and sub.next(0.0) is None and sub.repeated == 1, (hole, after))
    c.channel.frames.extend([observed(6, 1, sender=99), observed(6, 1, relay="R0"),
                             observed(6, 1, number=4)])
    c.poll(0.0)
    check("O4 a word from any speaker but the one that answered, or naming another relay lifetime "
          "or subscription, reaches nobody", sub.next(0.0) is None and c.stray_observations == 3,
          c.stray_observations)
    c.channel.frames.append(word("loom.observe.Gap", {"subscription": "3", "relay": "R1",
                                                       "seq": "6", "lost": "7",
                                                       "reason": "the window was shut"}))
    c.poll(0.0)
    said = sub.next(0.0)
    check("O5 the relay's own Gap is handed over as said, with its count",
          said.kind == "gap" and not said.local and said.lost == 7 and sub.lost_said == 7, said)
    c.channel.frames.append(word("loom.observe.Ended", {"subscription": "3", "relay": "R1",
                                                         "seq": "7", "last": "6",
                                                         "kind": "revoked", "reason": "stopped"}))
    c.channel.frames.append(observed(8, 99))
    c.poll(0.0)
    ended = sub.next(0.0)
    check("O6 an ending is handed over, nothing follows it, and the subscription stops listening",
          ended.kind == "ended" and ended.how == "revoked" and sub.next(0.0) is None and
          sub.key not in c._watching, ended)

    c, ctx = watching_context()
    sub = subscription(ctx, window=16, max_pending=2)
    c.watch(sub)
    c.channel.frames.extend([observed(i, i) for i in range(1, 6)])
    c.poll(0.0)
    got = [sub.next(0.0) for _ in range(3)]
    check("O7 past this client's own bound, observations are dropped HERE and the loss is handed "
          "over as a local Gap with its count, even when nothing follows it",
          [g.kind for g in got] == ["observed", "observed", "gap"] and got[2].local and
          got[2].lost == 3 and got[2].through == 5, got)

    c, ctx = watching_context()
    sub = subscription(ctx, window=4)
    c.watch(sub)
    c.channel.frames.extend([observed(1, 1, cause=17), observed(2, 2)])
    c.poll(0.0)
    before = len(c.channel.sent)
    one, two = sub.next(0.0), sub.next(0.0)
    asks = c.channel.sent[before:]
    through = None
    if asks:
        _, _, ask = values.open_envelope(c.channel.payloads[-1])
        _, _, inner = values.open_envelope(lbase64.b64decode(ask["payload"]))
        through = inner.get("through")
    check("O8 taking half a window acknowledges it, through the link, by the number taken",
          len(asks) == 1 and asks[0][1] == "loom.link.far" and through == "2", (asks, through))
    check("O9 cause is kept as the relay (or link) said it: this run's own correlation, or 0",
          one.cause == 17 and two.cause == 0)

    c, ctx = watching_context()
    sub = subscription(ctx, window=64)
    c.watch(sub)
    c.channel.frames.extend([observed(n, n) for n in range(1, 4)])
    c.poll(0.0)
    before = len(c.channel.sent)
    taken = [sub.next(0.0) for _ in range(3)] + [sub.next(0.0)]  # three, then nothing to read
    quiet = len(c.channel.sent) - before
    c.channel.frames.extend([observed(n, n) for n in range(4, 9)])
    c.poll(0.0)
    taken += [sub.next(0.0) for _ in range(5)] + [sub.next(0.0)]
    check("O10 an idle reader acknowledges an eighth of its window, not every word: three taken "
          "ask nothing, eight taken ask once",
          quiet == 0 and len(c.channel.sent) - before == 1 and
          [t.seq for t in taken if t is not None] == list(range(1, 9)),
          (quiet, len(c.channel.sent) - before, [getattr(t, "seq", None) for t in taken]))

    # O11: the first Observed a new worker meets is described by the host before it can be read,
    # and that wait reads on: words arriving meanwhile must queue behind it, never overtake it.
    # The host here answers each Describe after whatever is already on the wire (a real one does).
    def desc_of(schema):
        return {"name": schema.name, "version": str(schema.version),
                "fields": [{"name": f.name, "required": f.required,
                            "type": [{"kind": str(f.type.kind)}]} for f in schema.fields]}

    class DescribingChannel(StandInChannel):
        def __init__(self, batches):
            StandInChannel.__init__(self)
            self.batches = collections.deque(batches)
            self.described = 0

        def describe(self, name, version):
            self.described += 1
            e = wire.Event("schema")
            e.payload = values.envelope("zen.SchemaDesc", 1, desc_of(watched[(name, version)]))
            self.batches.append([e])

        def read(self, timeout):
            return self.batches.popleft() if self.batches else []

    c, ctx = watching_context()
    del c._schemas[("loom.observe.Observed", 1)]
    sub = subscription(ctx)
    c.watch(sub)
    c.channel = DescribingChannel([[observed(1, 1)], [observed(2, 2), observed(3, 3)]])
    c.poll(0.0)
    c.poll(0.0)
    c.poll(0.0)
    got = [sub.next(0.0) for _ in range(4)]
    check("O11 a word that arrives while an earlier one waits for its shape's description queues "
          "behind it: all three, in order, no hole, nothing repeated",
          [getattr(g, "seq", None) for g in got] == [1, 2, 3, None] and
          all(g is None or g.kind == "observed" for g in got) and sub.repeated == 0,
          ([repr(g) for g in got], sub.repeated, c.channel.described))

    failed = [name for name, ok in CHECKS if not ok]
    print("%d of %d checks held" % (len(CHECKS) - len(failed), len(CHECKS)), flush=True)
    return 0 if CHECKS and not failed else 1


if __name__ == "__main__":
    sys.exit(main())
