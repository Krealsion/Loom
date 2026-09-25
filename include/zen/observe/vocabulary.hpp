// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_OBSERVE_VOCABULARY_HPP
#define ZEN_OBSERVE_VOCABULARY_HPP

// WHAT A SUBSCRIBER MAY ASK AN OBSERVATION RELAY, AND WHAT IT IS TOLD.
//
// A producer PUBLISHES what happened in its own vocabulary; whoever declares that shape hears it.
// A participant somewhere else -- another host's agent, a monitor -- often wants the same
// sentences as they happen, without being a listener the producer's host installed, and without a
// tap on that host's whole bus. A RELAY (`zen/observe/relay.hpp`) is how a host offers that: a
// subscriber asks the relay's office for one producer's publications of named shapes, the host's
// policy says yes or no, and from then on the relay tells the subscriber each such publication, in
// order, numbered, until the subscription ends. docs/reference/observation.md is the contract.
//
// OBSERVATION IS ITS OWN AUTHORITY. Asking needs a send rule for `Subscribe` to the relay's office,
// like any ask; being ANSWERED yes is the relay's host policy, and it admits nothing unless the host
// said otherwise. Neither a send rule, nor knowing a shape, nor being able to inject input or take a
// picture on that host is permission to observe. And observing grants nothing: a subscriber that
// sees a producer's words may still say nothing to it.
//
// READY MEANS READY, AND NOTHING EARLIER IS REPLAYED. A subscription begins at the moment the relay
// handles `Subscribe` on the producer's bus. Every publication ENQUEUED after that moment is told;
// one enqueued before it is not, and no history is invented to cover the time before. `Subscribed`
// is enqueued at that same moment, so it reaches the subscriber before the first observation.
//
// ORDER AND IDENTITY. The relay numbers everything it says about one subscription -- observations,
// gaps and the ending -- from `next` upward with no holes. A hole a subscriber finds is a loss
// somewhere between the relay and itself; a `Gap` is a loss the relay chose and counted. The order
// is the producer bus's delivery order to the relay; it says nothing about any other bus. A
// subscription number is minted once per relay lifetime (`relay`), so a restarted host's
// subscription 1 is never the one before it, and across a link each session is a new `epoch`.
//
// NOT AN ANSWER. `Observed`, `Gap` and an `Ended` the relay SAYS are ordinary words, never an
// answer to anything the subscriber asked (only `Subscribed`, and the `Ended` that answers
// `Release`, are answers). A producer's publication is what it published; that the subscriber saw
// it proves nothing about what anybody did next.

#include <zen/serialize.hpp>
#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace loom::observe {

/// The office a relay holds.
inline constexpr const char* kObserveRole = "loom.observe";

/// The two payload encodings a subscriber may ask for: Zen's canonical binary, or Zen's
/// self-describing JSON envelope (docs/reference/bridge.md#the-payload-encoding-a-session-speaks).
inline constexpr const char* kEncodingNative = "native";
inline constexpr const char* kEncodingCompat = "compat";

/// How a subscription ended (`Ended::kind`).
inline constexpr const char* kEndedReleased = "released"; ///< its subscriber asked
inline constexpr const char* kEndedRevoked = "revoked";   ///< the relay's host withdrew it
inline constexpr const char* kEndedLost = "lost";         ///< the way it crossed ended (a link's session)
inline constexpr const char* kEndedGone = "gone";         ///< the relay itself is ending

/// One shape by its exact identity.
struct ShapeRef {
    std::string name;
    std::int64_t version = 0;
    using ZenSelf = ShapeRef;
    static constexpr const char* zen_name = "loom.observe.ShapeRef";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(version)); }
};

/// ASK TO OBSERVE `producer`'s publications of `shapes`, from now on. Answered `Subscribed` or
/// `zen.Refused` (with the relay's or its host's reason).
///
/// `producer` is an OFFICE: what is told is what the office's holder at each delivery published,
/// and each observation names that holder and its incarnation, so a replaced holder is visible.
/// `latest` names shapes among `shapes` that are STATE rather than occurrences: while the window is
/// shut the relay keeps only the newest of each, and says how many it stood for. `window` is how
/// many observations the subscriber lets stand unacknowledged (0: the relay's default).
struct Subscribe {
    std::string producer;
    std::vector<ShapeRef> shapes;
    std::vector<std::string> latest;
    std::string encoding; ///< `native` (also when empty) or `compat`
    std::int64_t window = 0;
    std::string label; ///< the subscriber's own words, kept for evidence
    using ZenSelf = Subscribe;
    static constexpr const char* zen_name = "loom.observe.Subscribe";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(producer), ZEN_FIELD(shapes), ZEN_FIELD(latest),
                               ZEN_FIELD(encoding), ZEN_FIELD(window), ZEN_FIELD(label));
    }
};

/// THE SUBSCRIPTION EXISTS, and this is where it begins. `shapes` is the subscribed shapes'
/// descriptors -- `loom.observe.Shapes` (relay.hpp), serialized in the subscription's encoding:
/// `zen.SchemaDesc` v1 for each shape and the closure it nests, exactly as self-description carries
/// them -- so a subscriber decodes a shape its own host never declared.
struct Subscribed {
    std::int64_t subscription = 0;
    std::string relay;       ///< the relay's lifetime: minted when it was made, never reused
    std::string producer;
    std::int64_t holder = 0; ///< who held the office as the subscription began; 0 when nobody did
    std::int64_t incarnation = 0;
    std::int64_t window = 0; ///< the window granted
    std::string encoding;
    std::vector<std::string> latest;
    loom::Bytes shapes;
    std::int64_t next = 1; ///< the number the first thing said about this subscription carries
    using ZenSelf = Subscribed;
    static constexpr const char* zen_name = "loom.observe.Subscribed";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(subscription), ZEN_FIELD(relay), ZEN_FIELD(producer),
                               ZEN_FIELD(holder), ZEN_FIELD(incarnation), ZEN_FIELD(window),
                               ZEN_FIELD(encoding), ZEN_FIELD(latest), ZEN_FIELD(shapes),
                               ZEN_FIELD(next));
    }
};

/// ONE PUBLICATION, AS THE RELAY HEARD IT. Said to the subscriber; never an answer.
///
/// `producer` and `incarnation` are the bus-stamped publisher and its incarnation at delivery;
/// `office` is the office it deliberately spoke as, if it did. `cause` is the SUBSCRIBER'S OWN
/// correlation of the settle-requested send whose synchronous dispatch set this publication in
/// motion, when there was one; it is never another participant's. `delivery` and `published_in` are
/// the relay bus's sequence numbers of this publication's delivery to the relay and of the delivery
/// during which it was published -- references into that host's history, not identities here.
/// `link`, `epoch` and `session` are filled by a link that carried it; empty on the relay's own bus.
struct Observed {
    std::int64_t subscription = 0;
    std::string relay;
    std::int64_t seq = 0;
    std::string shape;
    std::int64_t version = 0;
    loom::Bytes payload; ///< the published value, in the subscription's encoding
    std::int64_t producer = 0;
    std::int64_t incarnation = 0;
    std::string office;
    std::int64_t coalesced = 0; ///< earlier publications of a `latest` shape this one stands for
    std::int64_t cause = 0;
    std::int64_t delivery = 0;
    std::int64_t published_in = 0;
    std::string link;
    std::int64_t epoch = 0;
    std::int64_t session = 0;
    using ZenSelf = Observed;
    static constexpr const char* zen_name = "loom.observe.Observed";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(subscription), ZEN_FIELD(relay), ZEN_FIELD(seq),
                               ZEN_FIELD(shape), ZEN_FIELD(version), ZEN_FIELD(payload),
                               ZEN_FIELD(producer), ZEN_FIELD(incarnation), ZEN_FIELD(office),
                               ZEN_FIELD(coalesced), ZEN_FIELD(cause), ZEN_FIELD(delivery),
                               ZEN_FIELD(published_in), ZEN_FIELD(link), ZEN_FIELD(epoch),
                               ZEN_FIELD(session));
    }
};

/// PUBLICATIONS THE RELAY HEARD AND DID NOT TELL: `lost` occurrences dropped while the subscriber's
/// window was shut, counted, and said in their place before anything later. Never silent.
struct Gap {
    std::int64_t subscription = 0;
    std::string relay;
    std::int64_t seq = 0;
    std::int64_t lost = 0;
    std::string reason;
    std::string link;
    std::int64_t epoch = 0;
    std::int64_t session = 0;
    using ZenSelf = Gap;
    static constexpr const char* zen_name = "loom.observe.Gap";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(subscription), ZEN_FIELD(relay), ZEN_FIELD(seq),
                               ZEN_FIELD(lost), ZEN_FIELD(reason), ZEN_FIELD(link),
                               ZEN_FIELD(epoch), ZEN_FIELD(session));
    }
};

/// THE SUBSCRIPTION IS OVER; nothing more will be said about it. The answer to `Release`, or said
/// on its own when the relay's host revoked it, the relay is ending, or (said by a link) the session
/// it crossed ended. `last` is the last number said before this; `lost` counts occurrences still
/// held back and never told. Ending an observation stops nothing the producer was doing.
struct Ended {
    std::int64_t subscription = 0;
    std::string relay;
    std::int64_t seq = 0;
    std::int64_t last = 0;
    std::int64_t lost = 0;
    std::string kind; ///< kEndedReleased / kEndedRevoked / kEndedLost / kEndedGone
    std::string reason;
    std::string link;
    std::int64_t epoch = 0;
    std::int64_t session = 0;
    using ZenSelf = Ended;
    static constexpr const char* zen_name = "loom.observe.Ended";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(subscription), ZEN_FIELD(relay), ZEN_FIELD(seq),
                               ZEN_FIELD(last), ZEN_FIELD(lost), ZEN_FIELD(kind),
                               ZEN_FIELD(reason), ZEN_FIELD(link), ZEN_FIELD(epoch),
                               ZEN_FIELD(session));
    }
};

/// END ONE OF YOUR SUBSCRIPTIONS. Answered with its `Ended` (kind `released`), or `zen.Refused`
/// when it is not the asker's, not this relay lifetime's, or already over.
struct Release {
    std::int64_t subscription = 0;
    std::string relay;
    using ZenSelf = Release;
    static constexpr const char* zen_name = "loom.observe.Release";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(subscription), ZEN_FIELD(relay)); }
};

/// EVERYTHING THROUGH `through` HAS ARRIVED: the window opens by that much, and anything the relay
/// held back is said. Answered `zen.Ack`, or `zen.Refused` for a subscription that is not the
/// asker's or a number the relay never said.
struct Acknowledge {
    std::int64_t subscription = 0;
    std::string relay;
    std::int64_t through = 0;
    using ZenSelf = Acknowledge;
    static constexpr const char* zen_name = "loom.observe.Acknowledge";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(subscription), ZEN_FIELD(relay), ZEN_FIELD(through));
    }
};

/// WHAT THE RELAY HOLDS FOR THE ASKER -- its own subscriptions only. Answered `Status`.
struct StatusRequested {
    using ZenSelf = StatusRequested;
    static constexpr const char* zen_name = "loom.observe.StatusRequested";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// One subscription, as the relay keeps it.
struct StatusRow {
    std::int64_t subscription = 0;
    std::int64_t subscriber = 0;
    std::string producer;
    std::vector<ShapeRef> shapes;
    std::string label;
    std::int64_t window = 0;
    std::int64_t said = 0;      ///< the last number said
    std::int64_t acked = 0;     ///< the last number acknowledged
    std::int64_t heard = 0;     ///< publications of the subscribed shapes by the producer, heard
    std::int64_t lost = 0;      ///< occurrences dropped and counted, all told
    std::int64_t coalesced = 0; ///< `latest` publications another stood for
    std::int64_t foreign = 0;   ///< the same shapes published by someone else: not told
    using ZenSelf = StatusRow;
    static constexpr const char* zen_name = "loom.observe.StatusRow";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(subscription), ZEN_FIELD(subscriber), ZEN_FIELD(producer),
                               ZEN_FIELD(shapes), ZEN_FIELD(label), ZEN_FIELD(window),
                               ZEN_FIELD(said), ZEN_FIELD(acked), ZEN_FIELD(heard),
                               ZEN_FIELD(lost), ZEN_FIELD(coalesced), ZEN_FIELD(foreign));
    }
};

struct Status {
    std::string relay;
    std::vector<StatusRow> rows;
    using ZenSelf = Status;
    static constexpr const char* zen_name = "loom.observe.Status";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(relay), ZEN_FIELD(rows)); }
};

} // namespace loom::observe

#endif // ZEN_OBSERVE_VOCABULARY_HPP
