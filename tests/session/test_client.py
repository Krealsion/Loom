# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""THE PYTHON CLIENT'S ANSWER RULE, without a host.

A conversation settles on the delivery Loom attests as ITS answer (``answers_ask`` under the
conversation's own correlation), or on Loom's refusal of the ask's send or dispatch -- and on
nothing else: not an ordinary word under the same number, not an answer under a number this
client never minted, not silence. The journey cannot stage a forged answer (no participant on a
session host may say one to a client), so the rule is pinned here, against the shipped client
with a stand-in channel that records what was sent and hands over the frames a case queues.

    test_client.py --runtime <dir holding loom_session>

Exit 0 only when every check held.
"""

import argparse
import collections
import sys

CHECKS = []


def check(name, condition, detail=""):
    CHECKS.append((name, bool(condition)))
    print("%s  %s%s" % ("ok  " if condition else "FAIL", name,
                        ("  -- %s" % (detail,)) if detail != "" else ""), flush=True)


class StandInChannel(object):
    """What the client writes to and reads from: nothing crosses a socket."""

    def __init__(self):
        self.sent = []
        self.frames = collections.deque()

    def send_message(self, payload, correlation, role=None, target=0, settle=False):
        self.sent.append((correlation, role, settle))

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
    failed = [name for name, ok in CHECKS if not ok]
    print("%d of %d checks held" % (len(CHECKS) - len(failed), len(CHECKS)), flush=True)
    return 0 if CHECKS and not failed else 1


if __name__ == "__main__":
    sys.exit(main())
