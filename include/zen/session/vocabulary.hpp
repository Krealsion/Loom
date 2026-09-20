// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SESSION_VOCABULARY_HPP
#define ZEN_SESSION_VOCABULARY_HPP

// WHAT A CLIENT OF A PERSISTENT SESSION HOST MAY SAY, AND WHAT IT IS TOLD.
//
// `loom-host --serve <dir>` is the supplied host kept alive on purpose: it does not end when a
// console closes, and it listens -- on loopback only -- for CLIENTS that attach, do some work and
// leave again, and for the RUN WORKERS a run manager starts (docs/guides/sessions.md). Two host
// offices answer them, and this header is their whole vocabulary:
//
//   loom.session   the session's door: what this host lifetime is, who is attached, the links it
//                  holds, and the one operation that ends it. A run manager also registers here
//                  the one connection it expects for each run it starts.
//   loom.history   a SCOPED READER over the host's own Recorder: what became of the deliveries to
//                  one participant, and the link crossing each one descends from.
//
// NONE OF IT IS AUTHORITY BY BEING KNOWN. Knowing a lifetime, a run name, a session number or
// these shapes grants nothing: a client may say these things because the door admitted it with a
// grant naming them, and a run worker may say only what its registrar was itself allowed to say
// (below). A name in a payload is data.
//
// THE LIFETIME IS THE SESSION'S IDENTITY, AND IT IS NEVER REUSED. A host started again is a new
// lifetime, whatever directory it serves: a client that attached to the old one learns that the
// lifetime changed, and every run handle names the lifetime that minted it, so an old handle can
// never be applied to new work.
//
// RUN REGISTRATION CARRIES NO SECRET. The worker presents a one-time credential in its Hello; the
// run manager tells the door only its SHA-256 (`digest`), so the bus -- and therefore the host's
// history -- never holds anything a process could connect with. The door admits the first
// connection whose credential hashes to an expected digest, once.
//
// ATTENUATION, NOT A RUN LABEL. A run is admitted with the rules its registrar asked for, and the
// door grants a rule only when the registrar's OWN approved authority (the operator's decision
// in the authority store) already contains it -- or when the rule addresses an office the
// registrar itself holds, which lets a worker report to its manager and nothing else. A run name
// narrows nothing on its own.

#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace loom::session {

inline constexpr const char* kSessionRole = "loom.session";
inline constexpr const char* kHistoryRole = "loom.history";

/// The prefix of the name the door establishes for a run worker's session: `run:<name>`.
inline constexpr const char* kRunSessionPrefix = "run:";
/// The name the door establishes for the owner's client sessions.
inline constexpr const char* kClientSessionName = "client";

// ---- the door ----------------------------------------------------------------------------------

/// WHAT THIS SESSION HOST IS, RIGHT NOW. Answered `Description`.
struct Describe {
    using ZenSelf = Describe;
    static constexpr const char* zen_name = "loom.session.Describe";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// One connection as the door sees it: a client, a run worker, or one still being decided.
struct Connection {
    std::int64_t connection = 0; ///< the door's own number for the socket
    std::string kind;            ///< "client" / "run" / "pending"
    std::string name;            ///< the name the door established (empty until admitted)
    std::string claimed;         ///< what the peer called itself -- its word, never an identity
    std::string state;           ///< awaiting-hello / admitted / refused / closed
    std::int64_t session = 0;    ///< the session's WeaveId on this bus, when admitted
    std::string encoding;        ///< "compat" / "native"
    using ZenSelf = Connection;
    static constexpr const char* zen_name = "loom.session.Connection";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(connection), ZEN_FIELD(kind), ZEN_FIELD(name),
                               ZEN_FIELD(claimed), ZEN_FIELD(state), ZEN_FIELD(session),
                               ZEN_FIELD(encoding));
    }
};

/// One link this host holds, as the link itself reports it (see `zen/bridge/link.hpp`).
struct LinkRow {
    std::string name;
    std::string endpoint;
    std::string state;            ///< connecting / admitted / denied / lost / closed
    std::string established;      ///< the far host's name for this host's session
    std::int64_t far_session = 0; ///< the far host's session id, when admitted
    std::int64_t epoch = 0;       ///< how many sessions this link has opened
    std::int64_t open = 0;        ///< crossings submitted and not yet answered
    std::string detail;
    using ZenSelf = LinkRow;
    static constexpr const char* zen_name = "loom.session.LinkRow";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(endpoint), ZEN_FIELD(state),
                               ZEN_FIELD(established), ZEN_FIELD(far_session), ZEN_FIELD(epoch),
                               ZEN_FIELD(open), ZEN_FIELD(detail));
    }
};

struct Description {
    std::string lifetime;          ///< this host run's identity: minted at start, never reused
    std::string host;              ///< the host's version line
    std::int64_t abi = 0;          ///< the weave ABI it hosts
    std::int64_t started_ms = 0;   ///< wall clock at start, for a person; never an identity
    std::int64_t pid = 0;          ///< the host process, for a person or a supervisor
    std::string directory;         ///< the session directory it serves
    std::string endpoint;          ///< where clients attach (loopback)
    std::string containment;       ///< the kernel's own sentence about what it contains
    std::vector<Connection> connections;
    std::vector<LinkRow> links;
    std::int64_t expected = 0;     ///< run connections registered and not yet admitted
    bool ending = false;           ///< a Shutdown was accepted; the host ends after this turn
    using ZenSelf = Description;
    static constexpr const char* zen_name = "loom.session.Description";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(lifetime), ZEN_FIELD(host), ZEN_FIELD(abi),
                               ZEN_FIELD(started_ms), ZEN_FIELD(pid), ZEN_FIELD(directory),
                               ZEN_FIELD(endpoint), ZEN_FIELD(containment),
                               ZEN_FIELD(connections), ZEN_FIELD(links), ZEN_FIELD(expected),
                               ZEN_FIELD(ending));
    }
};

/// END THIS HOST LIFETIME. Answered `zen.Ack`; the host finishes the turn, says so in its log, and
/// exits 0. It is not a cancellation of anything: every run still active ends with the host and
/// is recorded as interrupted by its manager, and every link's far session simply closes.
struct Shutdown {
    std::string reason; ///< one line, for the log
    using ZenSelf = Shutdown;
    static constexpr const char* zen_name = "loom.session.Shutdown";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(reason)); }
};

/// EXPECT ONE RUN WORKER. Said by a run manager before it starts the worker process. Answered
/// `RunExpected` (with the rules actually granted) or `zen.Refused` (naming the rule its sender
/// may not pass on, and the decision that would allow it).
struct ExpectRun {
    std::string run;                   ///< the registrar's name for the run
    std::string digest;                ///< SHA-256 (hex) of the worker's one-time credential
    std::vector<std::string> may_say;  ///< rules, spelled as `authority show` prints them
    using ZenSelf = ExpectRun;
    static constexpr const char* zen_name = "loom.session.ExpectRun";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(run), ZEN_FIELD(digest), ZEN_FIELD(may_say));
    }
};

struct RunExpected {
    std::string run;
    std::string established;           ///< the name the worker's session will carry
    std::vector<std::string> granted;  ///< exactly the rules the session will be admitted with
    using ZenSelf = RunExpected;
    static constexpr const char* zen_name = "loom.session.RunExpected";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(run), ZEN_FIELD(established), ZEN_FIELD(granted));
    }
};

/// FORGET AN EXPECTATION, AND SEVER ITS SESSION IF IT IS CONNECTED. Only the registrar that made
/// it may. Answered `zen.Ack`, or `zen.Refused` when there is no such run of the sender's.
struct ForgetRun {
    std::string run;
    using ZenSelf = ForgetRun;
    static constexpr const char* zen_name = "loom.session.ForgetRun";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(run)); }
};

/// WHAT BECAME OF AN EXPECTED CONNECTION, told by the door to the registrar that expected it:
/// `admitted` (with the session's WeaveId) once, then `closed` once when the socket ends. A
/// registrar that is gone takes its runs' sessions with it: the door severs them.
struct RunConnection {
    std::string run;
    std::int64_t session = 0;
    std::string state;  ///< "admitted" / "closed"
    std::string detail;
    using ZenSelf = RunConnection;
    static constexpr const char* zen_name = "loom.session.RunConnection";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(run), ZEN_FIELD(session), ZEN_FIELD(state),
                               ZEN_FIELD(detail));
    }
};

// ---- the scoped history reader -----------------------------------------------------------------
//
// It answers from the host's own Recorder (`zen/history/recorder.hpp`) and adds no memory of its
// own. Its words for a fact are the Recorder's: RETAINED (here it is), FORGOTTEN (it was held and
// the window moved past it), NOT-RECORDED (seen, and policy kept nothing), UNOBSERVED (beyond what
// this recorder has seen). A crossing's fields are the link's own account of what ARRIVED -- the
// far session, the name the far host established, the far bus's stamp and office, the attempt --
// never an observation of the far execution. Its own questions and answers are declared to the
// Recorder's structural blacklist, so reading history never writes history.

/// THE DELIVERIES TO ONE PARTICIPANT THIS HOST STILL REMEMBERS, newest first, each with the
/// crossing it descends from when its dispatch parent is a link's `loom.link.Crossed`.
struct DeliveriesTo {
    std::int64_t participant = 0; ///< a WeaveId on this bus
    std::string shape;            ///< only this shape; empty for every shape
    std::int64_t limit = 0;       ///< at most this many rows (0 = the reader's own bound)
    using ZenSelf = DeliveriesTo;
    static constexpr const char* zen_name = "loom.history.DeliveriesTo";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(participant), ZEN_FIELD(shape), ZEN_FIELD(limit));
    }
};

/// WHAT BECAME OF ONE BUS DELIVERY, and the crossing it descends from.
struct Delivery {
    std::int64_t seq = 0;
    using ZenSelf = Delivery;
    static constexpr const char* zen_name = "loom.history.Delivery";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(seq)); }
};

/// One remembered delivery, and -- flattened beside it -- what the reader could say about its
/// dispatch parent. `parent_horizon` is "none" when the delivery has no parent.
struct Record {
    std::int64_t record = 0;       ///< the Recorder's own number for the record
    std::int64_t seq = 0;          ///< the bus delivery seq
    std::int64_t sender = 0;
    std::int64_t target = 0;
    std::string shape;
    std::int64_t version = 0;
    std::int64_t correlation = 0;
    std::int64_t parent = 0;       ///< the dispatch parent's seq, 0 for none
    std::string outcome;           ///< delivered / refused / handler-failed
    std::string refusal;           ///< why Loom refused it (CapabilityDenied, NoSuchTarget, ...)
    std::string payload;           ///< the payload's state: retained / evicted / declined / absent
    std::string parent_horizon;    ///< none / retained / forgotten / not-recorded / unobserved
    std::string parent_shape;
    bool crossing = false;         ///< the parent is a link's own `loom.link.Crossed` record
    std::string crossing_payload;  ///< that record's payload state; its fields need `retained`
    std::string link;
    std::int64_t epoch = 0;
    std::int64_t far_session = 0;
    std::string established;
    std::string kind;              ///< the link's word for the frame: answer / dispatch-refused / ...
    std::int64_t attempt = 0;
    std::int64_t far_sender = 0;
    std::string far_role;
    std::string far_shape;
    std::int64_t far_version = 0;
    using ZenSelf = Record;
    static constexpr const char* zen_name = "loom.history.Record";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(
            ZEN_FIELD(record), ZEN_FIELD(seq), ZEN_FIELD(sender), ZEN_FIELD(target),
            ZEN_FIELD(shape), ZEN_FIELD(version), ZEN_FIELD(correlation), ZEN_FIELD(parent),
            ZEN_FIELD(outcome), ZEN_FIELD(refusal), ZEN_FIELD(payload), ZEN_FIELD(parent_horizon),
            ZEN_FIELD(parent_shape), ZEN_FIELD(crossing), ZEN_FIELD(crossing_payload),
            ZEN_FIELD(link), ZEN_FIELD(epoch), ZEN_FIELD(far_session), ZEN_FIELD(established),
            ZEN_FIELD(kind), ZEN_FIELD(attempt), ZEN_FIELD(far_sender), ZEN_FIELD(far_role),
            ZEN_FIELD(far_shape), ZEN_FIELD(far_version));
    }
};

/// The reader's answer to either question: the recorder's own horizon, the one asked about (for
/// `Delivery`), and the rows it could still show.
struct Records {
    std::int64_t newest_seq = 0;         ///< the highest bus seq the recorder has observed
    std::int64_t oldest_retained_seq = 0;
    std::int64_t forgotten_horizon = 0;  ///< the highest seq a released record carried
    std::int64_t forgotten = 0;          ///< records released to keep the windows bounded
    std::int64_t asked_seq = 0;          ///< `Delivery`'s seq; 0 for `DeliveriesTo`
    std::string asked_horizon;           ///< retained / forgotten / not-recorded / unobserved
    bool truncated = false;              ///< more rows matched than the bound allowed
    std::vector<Record> rows;
    using ZenSelf = Records;
    static constexpr const char* zen_name = "loom.history.Records";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(newest_seq), ZEN_FIELD(oldest_retained_seq),
                               ZEN_FIELD(forgotten_horizon), ZEN_FIELD(forgotten),
                               ZEN_FIELD(asked_seq), ZEN_FIELD(asked_horizon),
                               ZEN_FIELD(truncated), ZEN_FIELD(rows));
    }
};

/// The most rows one history answer carries.
inline constexpr std::int64_t kMaxHistoryRows = 256;

} // namespace loom::session

#endif // ZEN_SESSION_VOCABULARY_HPP
