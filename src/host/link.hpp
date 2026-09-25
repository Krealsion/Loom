// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_LINK_HPP
#define ZEN_HOST_LINK_HPP

// A LINK TO ANOTHER HOST, AS ONE ORDINARY PARTICIPANT OF THIS ONE.
//
// The supplied host can hold a connection to another running Loom -- a Workshop, another
// loom-host -- and put an office in front of it: `loom.link.<name>`. A local weave that wants
// something from the far side sends the link a `loom.link.Ask` naming the far office and
// carrying its message as bytes (zen/bridge/link.hpp says what an asker is told, and when).
//
// ONE CROSSING PER ASK, AND IT IS THE LINK'S OWN. The asker's correlation is the asker's: two
// askers each keep a book whose first conversation is 1, so it cannot be what crosses. The link
// mints an `attempt` for every ask it submits -- monotonic over the link's life, never reused --
// puts THAT on the wire, and keeps the crossing: which asker, which of the asker's
// conversations, which session of the link (its `epoch`), and the asker's answer right. A far
// reply names the attempt, and the attempt names exactly one crossing; a reply for a session
// that has ended names none.
//
// THE ANSWER IS LOOM'S, NOT THE LINK'S SAY-SO. Taking an ask, the link converts the delivery's
// answer opportunity into a deferred answer (ANS-02), which Loom binds to the asker, the
// asker's incarnation and life, and its correlation. The far owner's attested answer -- and only
// that -- is spent through it, so the asker's `mail.answers_ask()` is this bus's word; the
// link's own `Outcome` is spent the same way. A far participant's ordinary speech to the
// session is recorded and settles nothing; an asker that was replaced or died before its
// answer came is refused by Loom at the spend, and its successor is answered nothing that was
// earned by its predecessor.
//
// WHY THE LINK SPEAKS TO ITSELF. A deferred answer is spent from a delivery to the weave that
// holds it, and a far frame is read on the HOST's turn, outside every delivery. So `service()`
// turns each far frame into a `loom.link.Crossed` record said to the link itself -- which is also
// how this host's Recorder and Logger come to hold the far session, the far stamp and office,
// the attempt and the bytes -- and the link acts on it when it is delivered. Only a `Crossed`
// the link said acts: anyone else's is ignored.
//
// WHAT THE LINK IS NOT. It is not authority: the far host admitted THIS SESSION under a grant of
// the far host's choosing, and every send is checked there against that grant. It is not a
// mirror of the far bus: it holds no tap (a guest is not given one), no far registry, no far
// history. It is not a retry engine: a session that ends tells every crossing still open on it
// `lost` and never resends, because a send whose outcome is unknown may well have acted. And it
// is not a participant a loaded weave can replace -- it is host wiring, like the warden, holding
// a socket the host opened.
//
// THE HOST SERVICES IT. A loaded weave has nothing to wake it; the host loop calls `service()`
// every turn, exactly as it reads a line, so the socket is read between bus turns and what it
// read is delivered in the next one.
//
// A FAR RELAY'S OBSERVATIONS REACH THE LOCAL PARTICIPANT THAT SUBSCRIBED, AND NOBODY ELSE. When a
// far `loom.observe.Subscribed` crosses as the answer to a local asker's ask, the link keeps the
// subscription's custody here: which far relay lifetime and far sender answered, on which epoch,
// for which local asker. From then on the far relay's own `Observed`, `Gap` and `Ended` for that
// subscription -- ordinary far speech, which the link otherwise ignores -- are re-admitted through
// this bus's gate and said by the link to that asker, with the link, epoch and far session filled
// in, and `cause` translated from the far attempt to the asker's own correlation when that attempt
// was the asker's (and emptied when it was not). Words about a subscription the link does not hold
// for that relay, sender and epoch are ignored; a session that ends ends its subscriptions `lost`;
// an asker that is gone has its subscription released at the far relay. zen/observe/vocabulary.hpp
// says what each word means.

#include <zen/bridge/client.hpp>
#include <zen/bridge/link.hpp>
#include <zen/observe/vocabulary.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/dispatch_refusal.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace loom::host {

/// Counters, and deliberately nothing else in the state shape: a link's open crossings are
/// plain members, because a reloaded link is a new session and inherits no open ask.
struct LinkState {
    std::int64_t submitted = 0;
    std::int64_t answered = 0;
    std::int64_t outcomes = 0;
    std::int64_t ignored = 0;
    ZEN_SHAPE(LinkState, 1, ZEN_FIELD(submitted), ZEN_FIELD(answered), ZEN_FIELD(outcomes),
              ZEN_FIELD(ignored));
};

class LinkWeave final
    : public WeaveBase<LinkWeave, LinkState,
                       Accept<link::Ask, link::StatusRequested, link::Crossed>,
                       Emit<link::Outcome, link::Status, link::Crossed, observe::Subscribe,
                            observe::Subscribed, observe::Observed, observe::Gap, observe::Ended,
                            observe::Release, observe::Acknowledge, observe::StatusRequested,
                            observe::Status>> {
public:
    /// The most crossings one link holds open at once. Each holds a deferred answer, which is
    /// host-side state the whole bus shares (`Switchboard::kMaxDeferredAnswers`), so a link
    /// keeps well inside it. Past it a new Ask is told `refused` at once, and every open one is
    /// untouched.
    static constexpr std::size_t kMaxOpenAsks = 16;

    /// The most far subscriptions one link carries for its askers at once, counting subscribe
    /// asks still open. Past it a `loom.observe.Subscribe` is told `refused` before it crosses.
    static constexpr std::size_t kMaxWatches = 32;

    LinkWeave(std::string name, std::string endpoint, std::string identity, std::string credential)
        : name_(std::move(name)), endpoint_(std::move(endpoint)), identity_(std::move(identity)),
          credential_(std::move(credential)) {}

    /// Host wiring: the bus the link speaks on, as itself. Set once, out of band.
    void attach(Switchboard& bus) { bus_ = &bus; }

    const std::string& name() const noexcept { return name_; }
    const std::string& endpoint() const noexcept { return endpoint_; }
    const std::string& state() const noexcept { return link_state_; }
    const std::string& detail() const noexcept { return detail_; }
    std::uint64_t session() const noexcept { return client_ ? client_->session() : 0; }
    std::string established_name() const {
        return client_ ? client_->established_name() : std::string();
    }
    std::size_t open() const noexcept { return open_.size(); }
    std::uint64_t epoch() const noexcept { return epoch_; }

    /// OPEN THE SOCKET AND SAY HELLO -- A NEW SESSION, A NEW EPOCH. Bounded: it waits at most
    /// `timeout_ms` for the far host's Welcome or Denied. A failure is a state and a sentence,
    /// never an exception -- the host prints it and keeps running. Every crossing still open on
    /// the session this replaces is told `lost` on the next turn; none is carried over.
    bool connect(int timeout_ms, std::string* why) {
        end_session("reconnecting: the session this ask went out on was replaced");
        client_.reset();
        ++epoch_;
        std::string host = "127.0.0.1";
        std::uint16_t port = 0;
        if (!split_endpoint(endpoint_, &host, &port)) {
            link_state_ = "closed";
            detail_ = "endpoint '" + endpoint_ + "' is not host:port";
            *why = detail_;
            return false;
        }
        std::string err;
        const socket_t sock = bridge_connect_tcp(host, port, &err);
        if (sock == kInvalidSocket) {
            link_state_ = "closed";
            detail_ = "connect " + endpoint_ + " failed: " + err;
            *why = detail_;
            return false;
        }
        client_ = std::make_unique<BridgeClient>(sock);
        link_state_ = "connecting";
        detail_.clear();
        if (!client_->hello(identity_, credential_)) {
            link_state_ = "lost";
            detail_ = "the connection closed before Hello";
            *why = detail_;
            return false;
        }
        if (!client_->await_admission(timeout_ms)) {
            if (client_->denied()) {
                link_state_ = "denied";
                detail_ = client_->denial();
            } else if (client_->disconnected()) {
                link_state_ = "lost";
                detail_ = "the far host closed the connection before answering";
            } else {
                link_state_ = "connecting";
                detail_ = "no answer from the far host within " + std::to_string(timeout_ms) + " ms";
            }
            *why = detail_;
            return false;
        }
        link_state_ = "admitted";
        return true;
    }

    /// READ THE SOCKET, once per host turn: every far frame becomes a `Crossed` record the link
    /// says to itself, acted on when it is delivered.
    void service() {
        if (!client_ || bus_ == nullptr) {
            return;
        }
        std::vector<BridgeEvent> events;
        client_->poll(events);
        for (const BridgeEvent& e : events) {
            switch (e.kind) {
            case BridgeEvent::Kind::Welcome:
                link_state_ = "admitted";
                break;
            case BridgeEvent::Kind::Denied:
                link_state_ = "denied";
                detail_ = e.reason;
                end_session("denied: " + e.reason);
                break;
            case BridgeEvent::Kind::Delivered: {
                link::Crossed x = record(e.dispatch_refused ? link::kCrossedDispatchRefused
                                         : e.answers_ask    ? link::kCrossedAnswer
                                                            : link::kCrossedMessage);
                x.attempt = static_cast<std::int64_t>(e.correlation);
                x.far_sender = static_cast<std::int64_t>(e.sender);
                x.far_role = e.authored_role;
                loom::Unverified u = loom::parse(e.payload);
                x.shape = u.claimed_name();
                x.version = static_cast<std::int64_t>(u.claimed_version());
                x.payload.assign(e.payload.begin(), e.payload.end());
                tell_self(std::move(x));
                break;
            }
            case BridgeEvent::Kind::SendRefused: {
                link::Crossed x = record(link::kCrossedSendRefused);
                x.attempt = static_cast<std::int64_t>(e.correlation);
                x.reason = e.reason;
                tell_self(std::move(x));
                break;
            }
            case BridgeEvent::Kind::Settled: {
                link::Crossed x = record(link::kCrossedSettled);
                x.attempt = static_cast<std::int64_t>(e.correlation);
                tell_self(std::move(x));
                break;
            }
            case BridgeEvent::Kind::Disconnected:
                if (link_state_ == "admitted" || link_state_ == "connecting") {
                    link_state_ = "lost";
                    detail_ = "the far host closed the connection";
                }
                end_session(detail_.empty() ? std::string("the link closed") : detail_);
                break;
            case BridgeEvent::Kind::Weaves:
            case BridgeEvent::Kind::Schema:
            case BridgeEvent::Kind::SchemaNone:
            case BridgeEvent::Kind::Tap:
                break; // a link keeps no far registry and is given no tap
            }
        }
    }

    // ---- the doors ------------------------------------------------------------------------

    void on(const link::Ask& ask, Mail& mail) {
        const std::uint64_t correlation = mail.correlation();
        const std::string_view payload(reinterpret_cast<const char*>(ask.payload.data()),
                                       ask.payload.size());
        loom::Unverified asked = loom::parse(payload);
        // A COMPAT ENVELOPE IS ADMITTED HERE, AND WHAT CROSSES IS STILL NATIVE. An asker that does
        // not speak the canonical binary (a Python tool, over a compat session) may hand the link
        // Zen's JSON envelope instead. The link admits it through THIS bus's gate against the shape
        // this host resolves -- the one gate, the one decode budget -- and puts the canonical bytes
        // of the admitted value on the wire, so the far host sees exactly what a C++ asker's
        // `ask_role` would have sent. A shape nothing here declares cannot be encoded, and is
        // refused before anything is submitted: the far vocabulary must be known on this host,
        // exactly as it must be for the far ANSWER to be re-admitted (below).
        std::string compat_native;
        std::string compat_refusal;
        if (!asked.well_formed()) {
            loom::Unverified json = loom::compat::parse(payload);
            if (json.well_formed()) {
                asked = json;
                std::shared_ptr<const Schema> door =
                    bus_ != nullptr ? bus_->resolve_schema(json.claimed_name(),
                                                           json.claimed_version())
                                    : nullptr;
                if (!door) {
                    compat_refusal = json.claimed_name() + " v" +
                                     std::to_string(json.claimed_version()) +
                                     " is a shape nothing on this host declares, so the link cannot "
                                     "encode it for the far host; load a participant that declares "
                                     "it";
                } else {
                    loom::Admission a = loom::admit(json, door);
                    if (!a.ok()) {
                        compat_refusal = "the ask's payload did not pass this bus's gate: " +
                                         a.first_error().message();
                    } else {
                        compat_native = loom::serialize(a.value());
                    }
                }
            }
        }
        const std::string shape = asked.claimed_name();
        const std::int64_t version = static_cast<std::int64_t>(asked.claimed_version());
        // REFUSED OR UNLINKED BEFORE ANYTHING IS SUBMITTED: said at once, as this delivery's one
        // answer.
        const auto tell_now = [&](const char* state, std::string why) {
            ++state_.outcomes;
            (void)mail.answer(outcome_of(state, std::move(why), shape, version, 0));
        };
        if (correlation == 0) {
            tell_now(link::kOutcomeRefused, "an ask across a link needs a correlation of its own, "
                                            "so its answer can come back under it");
            return;
        }
        if (!compat_refusal.empty()) {
            tell_now(link::kOutcomeRefused, compat_refusal);
            return;
        }
        if (!client_ || !client_->admitted() || client_->disconnected()) {
            tell_now(link::kOutcomeUnlinked, link_state_ == "denied"
                                                 ? "this link was denied: " + detail_
                                                 : "this link is " + link_state_);
            return;
        }
        if (open_.size() >= kMaxOpenAsks) {
            tell_now(link::kOutcomeRefused, "this link already holds " +
                                                std::to_string(kMaxOpenAsks) +
                                                " open asks; wait for one to be answered");
            return;
        }
        const bool subscribing = shape == observe::Subscribe::zen_name;
        if (subscribing && watches_.size() + subscribing_now() >= kMaxWatches) {
            tell_now(link::kOutcomeRefused, "this link already carries " +
                                                std::to_string(kMaxWatches) +
                                                " subscriptions; release one first");
            return;
        }
        // THE ASKER'S ANSWER RIGHT, KEPT FOR THE FAR SIDE TO EARN. A refused deferral leaves the
        // immediate opportunity intact, so the refusal below is still this ask's answer.
        DeferredAnswer due = mail.defer_answer();
        if (!due.valid()) {
            tell_now(link::kOutcomeRefused, "this host is holding as many unfinished conversations "
                                            "as it can; nothing was sent");
            return;
        }
        const std::uint64_t attempt = ++next_attempt_;
        const std::string_view bytes =
            compat_native.empty() ? payload : std::string_view(compat_native);
        if (ask.role.empty()) {
            client_->send(static_cast<std::uint64_t>(ask.target), attempt, bytes, ask.settle);
        } else {
            client_->send_to_role(ask.role, attempt, bytes, ask.settle);
        }
        client_->flush();
        if (client_->disconnected()) {
            // Submitted into a socket that then closed: the far host may or may not have read
            // it. That is `lost`, told now rather than on the next service.
            ++state_.outcomes;
            (void)loom::answer_deferred(
                due, mail,
                outcome_of(link::kOutcomeLost, "the link closed while this ask was being submitted",
                           shape, version, static_cast<std::int64_t>(attempt)));
            link_state_ = "lost";
            return;
        }
        ++state_.submitted;
        Crossing c;
        c.attempt = attempt;
        c.epoch = epoch_;
        c.asker = mail.sender();
        c.correlation = correlation;
        c.due = std::move(due);
        c.shape = shape;
        c.version = version;
        c.settle = ask.settle;
        c.subscribe = subscribing;
        open_.push_back(std::move(c));
    }

    void on(const link::StatusRequested&, Mail& mail) {
        link::Status s;
        s.name = name_;
        s.endpoint = endpoint_;
        s.state = link_state_;
        s.session = static_cast<std::int64_t>(session());
        s.established_name = established_name();
        s.detail = detail_;
        s.submitted = state_.submitted;
        s.answered = state_.answered;
        s.outcomes = state_.outcomes;
        s.open = static_cast<std::int64_t>(open_.size());
        s.ignored = state_.ignored;
        (void)mail.answer(s);
    }

    /// WHAT ARRIVED, ACTED ON -- the link's own records only.
    void on(const link::Crossed& x, Mail& mail) {
        if (mail.sender() != this->self_) {
            ++state_.ignored; // somebody else's account of a crossing is not the link's
            return;
        }
        if (x.kind == link::kCrossedMessage && observation(x.shape)) {
            observed(x);
            return;
        }
        if (x.kind == link::kCrossedEnded) {
            const std::uint64_t epoch = static_cast<std::uint64_t>(x.epoch);
            end_watches(epoch, x.session, x.reason);
            for (std::size_t i = 0; i < open_.size();) {
                if (open_[i].epoch != epoch) {
                    ++i;
                    continue;
                }
                const std::string why =
                    open_[i].held.has_value() || open_[i].held_outcome.has_value()
                        ? "the far owner had answered, but the session ended before the far host "
                          "said what the ask set in motion had settled; the answer is in this "
                          "host's history: " + x.reason
                        : x.reason;
                finish_with(i, outcome_of(link::kOutcomeLost, why, open_[i].shape,
                                          open_[i].version,
                                          static_cast<std::int64_t>(open_[i].attempt)),
                            mail);
            }
            return;
        }
        const std::size_t i = find(static_cast<std::uint64_t>(x.attempt),
                                   static_cast<std::uint64_t>(x.epoch));
        if (i == open_.size()) {
            ++state_.ignored; // late, duplicate, from an ended session, or never ours
            return;
        }
        Crossing& c = open_[i];
        if (x.kind == link::kCrossedMessage) {
            ++state_.ignored; // ordinary far speech is not the answer, whatever it names
            return;
        }
        if (x.kind == link::kCrossedSendRefused) {
            finish_with(i, outcome_of(link::kOutcomeRefused, x.reason, c.shape, c.version,
                                      x.attempt),
                        mail);
            return;
        }
        if (x.kind == link::kCrossedSettled) {
            c.settled = true;
            complete_if_ready(i, mail);
            return;
        }
        if (c.held.has_value() || c.held_outcome.has_value()) {
            ++state_.ignored; // one answer per crossing: a second attested word is not another
            return;
        }
        if (x.kind == link::kCrossedDispatchRefused) {
            c.held_outcome = outcome_of(link::kOutcomeDispatchRefused,
                                        refusal_reason(x.payload), c.shape, c.version, x.attempt);
            complete_if_ready(i, mail);
            return;
        }
        if (x.kind != link::kCrossedAnswer) {
            ++state_.ignored;
            return;
        }
        // THE FAR OWNER'S ANSWER, RE-ADMITTED THROUGH THIS BUS'S GATE: only against a shape this
        // bus knows (the asker declared it), and never in the link's own vocabulary.
        loom::Unverified u = loom::parse(std::string_view(
            reinterpret_cast<const char*>(x.payload.data()), x.payload.size()));
        if (u.claimed_name().rfind("loom.link.", 0) == 0) {
            c.held_outcome = outcome_of(link::kOutcomeRefused,
                                        "the far host answered with " + u.claimed_name() +
                                            ", which is this link's own vocabulary",
                                        c.shape, c.version, x.attempt);
            complete_if_ready(i, mail);
            return;
        }
        std::shared_ptr<const Schema> door =
            bus_->resolve_schema(u.claimed_name(), u.claimed_version());
        if (!door) {
            c.held_outcome = outcome_of(
                link::kOutcomeRefused,
                "the far host answered with " + u.claimed_name() + " v" +
                    std::to_string(u.claimed_version()) + ", a shape nothing here declares",
                c.shape, c.version, x.attempt);
            complete_if_ready(i, mail);
            return;
        }
        loom::Admission a = loom::admit(u, door);
        if (!a.ok()) {
            c.held_outcome = outcome_of(link::kOutcomeRefused,
                                        "the far host's answer did not pass this bus's gate: " +
                                            a.first_error().message(),
                                        c.shape, c.version, x.attempt);
            complete_if_ready(i, mail);
            return;
        }
        c.held = std::move(a).value();
        custody(c, x);
        complete_if_ready(i, mail);
    }

    /// How many far subscriptions this link carries for its askers now.
    std::size_t watches() const noexcept { return watches_.size(); }
    /// Far observation words that named no subscription this link holds, or came from the wrong
    /// far sender or epoch: ignored, and counted here as well as in `ignored`.
    std::int64_t stray_observations() const noexcept { return strays_; }

private:
    struct Crossing {
        std::uint64_t attempt = 0;     ///< the link's number on the wire
        std::uint64_t epoch = 0;       ///< the session it went out on
        WeaveId asker{};
        std::uint64_t correlation = 0; ///< the asker's, for reading; Loom binds the answer to it
        DeferredAnswer due;            ///< the asker's answer right
        std::string shape;
        std::int64_t version = 0;
        bool settle = false;
        bool settled = false;
        bool subscribe = false; ///< a `loom.observe.Subscribe`: counts against kMaxWatches
        std::optional<loom::Value> held;           ///< a far answer waiting on settlement
        std::optional<link::Outcome> held_outcome; ///< a refusal waiting on settlement
    };

    /// ONE FAR SUBSCRIPTION IN THIS LINK'S CUSTODY: the far relay lifetime and far sender that
    /// answered it, the session (epoch) it was made on, and the local asker it is for.
    struct Watch {
        std::int64_t subscription = 0;
        std::string relay;
        std::uint64_t epoch = 0;
        std::int64_t far_sender = 0;
        WeaveId subscriber{};
        std::int64_t last = 0; ///< the last number forwarded
    };

    static link::Outcome outcome_of(const char* state, std::string reason, std::string shape,
                                    std::int64_t version, std::int64_t attempt) {
        link::Outcome o;
        o.state = state;
        o.reason = std::move(reason);
        o.shape = std::move(shape);
        o.version = version;
        o.attempt = attempt;
        return o;
    }

    static bool split_endpoint(const std::string& endpoint, std::string* host, std::uint16_t* port) {
        const std::size_t colon = endpoint.rfind(':');
        if (colon == std::string::npos || colon + 1 >= endpoint.size()) {
            return false;
        }
        unsigned long p = 0;
        try {
            p = std::stoul(endpoint.substr(colon + 1));
        } catch (...) {
            return false;
        }
        if (p == 0 || p > 65535) {
            return false;
        }
        *host = colon == 0 ? std::string("127.0.0.1") : endpoint.substr(0, colon);
        *port = static_cast<std::uint16_t>(p);
        return true;
    }

    /// The far bus's own words for a dispatch refusal, from its notice.
    static std::string refusal_reason(const loom::Bytes& payload) {
        std::string reason = "the far bus refused the delivery";
        loom::Unverified u = loom::parse(
            std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
        loom::Admission a = loom::admit(u, schema_of<DispatchRefused>());
        if (a.ok()) {
            if (const loom::Cell* r = a.value().get("reason")) {
                reason = r->as_text();
            }
        }
        return reason;
    }

    /// A `Crossed` for the session as it stands now.
    link::Crossed record(const char* kind) const {
        link::Crossed x;
        x.link = name_;
        x.epoch = static_cast<std::int64_t>(epoch_);
        x.session = static_cast<std::int64_t>(session());
        x.established = established_name();
        x.kind = kind;
        return x;
    }

    /// Said by the link to itself, from outside any delivery (the host's turn).
    void tell_self(link::Crossed x) {
        (void)bus_->send_as(this->self_, this->self_,
                            Message(to_value(x), this->self_, WeaveId{}, 0));
    }

    /// The session this link holds is over: every crossing still open on it is told `lost` when
    /// the record is delivered. Said once per session, and only when something is open on it.
    void end_session(const std::string& why) {
        if (bus_ == nullptr || ended_epoch_ == epoch_) {
            return;
        }
        ended_epoch_ = epoch_;
        bool any = false;
        for (const Crossing& c : open_) {
            any = any || c.epoch == epoch_;
        }
        for (const Watch& w : watches_) {
            any = any || w.epoch == epoch_; // a subscription carried on it ends `lost` too
        }
        if (!any) {
            return;
        }
        link::Crossed x = record(link::kCrossedEnded);
        x.reason = why;
        tell_self(std::move(x));
    }

    std::size_t find(std::uint64_t attempt, std::uint64_t epoch) const {
        for (std::size_t i = 0; i < open_.size(); ++i) {
            if (open_[i].attempt == attempt && open_[i].epoch == epoch) {
                return i;
            }
        }
        return open_.size();
    }

    /// A crossing's answer goes out once it has one AND, when it asked, the far host has said
    /// its settlement.
    void complete_if_ready(std::size_t i, Mail& mail) {
        Crossing& c = open_[i];
        if (c.settle && !c.settled) {
            return;
        }
        if (c.held.has_value()) {
            loom::Value v = std::move(*c.held);
            DeferredAnswer due = std::move(c.due);
            open_.erase(open_.begin() + static_cast<std::ptrdiff_t>(i));
            ++state_.answered;
            (void)mail.bus().spend_deferred(due, Message(std::move(v)));
            return;
        }
        if (c.held_outcome.has_value()) {
            link::Outcome o = std::move(*c.held_outcome);
            finish_with(i, std::move(o), mail);
        }
    }

    void finish_with(std::size_t i, link::Outcome o, Mail& mail) {
        DeferredAnswer due = std::move(open_[i].due);
        open_.erase(open_.begin() + static_cast<std::ptrdiff_t>(i));
        ++state_.outcomes;
        (void)loom::answer_deferred(due, mail, o);
    }

    // ---- far subscriptions ------------------------------------------------------------------

    static bool observation(const std::string& shape) {
        return shape == observe::Observed::zen_name || shape == observe::Gap::zen_name ||
               shape == observe::Ended::zen_name;
    }

    std::size_t subscribing_now() const {
        return static_cast<std::size_t>(
            std::count_if(open_.begin(), open_.end(), [](const Crossing& c) { return c.subscribe; }));
    }

    Watch* watch_for(std::int64_t subscription, const std::string& relay, std::uint64_t epoch) {
        for (Watch& w : watches_) {
            if (w.subscription == subscription && w.relay == relay && w.epoch == epoch) {
                return &w;
            }
        }
        return nullptr;
    }

    void unwatch(const Watch* w) {
        watches_.erase(std::remove_if(watches_.begin(), watches_.end(),
                                      [&](const Watch& o) { return &o == w; }),
                       watches_.end());
    }

    /// A far answer about a subscription changes this link's custody: a `Subscribed` for this
    /// crossing's asker begins one; the `Ended` that answers a release finishes one.
    void custody(const Crossing& c, const link::Crossed& x) {
        const loom::Value& v = *c.held;
        const std::string name(v.schema().name());
        const loom::Cell* sub = v.get("subscription");
        const loom::Cell* relay = v.get("relay");
        if (sub == nullptr || relay == nullptr) {
            return;
        }
        if (name == observe::Subscribed::zen_name && c.subscribe) {
            Watch w;
            w.subscription = sub->as_int();
            w.relay = relay->as_text();
            w.epoch = c.epoch;
            w.far_sender = x.far_sender;
            w.subscriber = c.asker;
            watches_.push_back(std::move(w));
        } else if (name == observe::Ended::zen_name) {
            if (Watch* w = watch_for(sub->as_int(), relay->as_text(), c.epoch)) {
                if (w->subscriber == c.asker) {
                    unwatch(w);
                }
            }
        }
    }

    /// A far relay's word about a subscription: forwarded to the local asker it is for, or not at
    /// all. Only the relay that answered, on the session it answered on, about a subscription this
    /// link holds, is heard.
    void observed(const link::Crossed& x) {
        loom::Unverified u = loom::parse(std::string_view(
            reinterpret_cast<const char*>(x.payload.data()), x.payload.size()));
        std::shared_ptr<const Schema> door =
            bus_->resolve_schema(u.claimed_name(), u.claimed_version());
        if (!door) {
            ++state_.ignored;
            ++strays_;
            return;
        }
        loom::Admission a = loom::admit(u, door);
        if (!a.ok()) {
            ++state_.ignored;
            ++strays_;
            return;
        }
        loom::Value v = std::move(a).value();
        const loom::Cell* sub = v.get("subscription");
        const loom::Cell* relay = v.get("relay");
        Watch* w = (sub != nullptr && relay != nullptr)
                       ? watch_for(sub->as_int(), relay->as_text(),
                                   static_cast<std::uint64_t>(x.epoch))
                       : nullptr;
        if (w == nullptr || w->far_sender != x.far_sender) {
            ++state_.ignored; // not a subscription this link holds, or not its relay speaking
            ++strays_;
            return;
        }
        if (!bus_->alive(w->subscriber)) {
            // THE ASKER IS GONE: nobody here will read another word, so the far relay is asked
            // to stop saying them. Its answer arrives as a crossing nobody waits for.
            release_far(*w);
            unwatch(w);
            ++state_.ignored;
            return;
        }
        v.set("link", loom::Cell::text(name_));
        v.set("epoch", loom::Cell::integer(x.epoch));
        v.set("session", loom::Cell::integer(x.session));
        if (const loom::Cell* seq = v.get("seq")) {
            w->last = seq->as_int();
        }
        if (const loom::Cell* cause = v.get("cause")) {
            // THE FAR CAUSE IS AN ATTEMPT OF THIS LINK'S; the asker knows its own correlation.
            std::int64_t local = 0;
            const std::size_t i = find(static_cast<std::uint64_t>(cause->as_int()),
                                       static_cast<std::uint64_t>(x.epoch));
            if (cause->as_int() != 0 && i < open_.size() && open_[i].asker == w->subscriber) {
                local = static_cast<std::int64_t>(open_[i].correlation);
            }
            v.set("cause", loom::Cell::integer(local));
        }
        const WeaveId to = w->subscriber;
        if (std::string(v.schema().name()) == observe::Ended::zen_name) {
            unwatch(w);
        }
        (void)bus_->send_as(this->self_, to, Message(std::move(v), this->self_, WeaveId{}, 0));
    }

    /// The session a subscription crossed on ended: each of its subscriptions is over, and its
    /// asker is told so, `lost` -- what the far relay said after the last word forwarded is unknown.
    void end_watches(std::uint64_t epoch, std::int64_t session, const std::string& why) {
        std::vector<Watch> ending;
        for (const Watch& w : watches_) {
            if (w.epoch == epoch) {
                ending.push_back(w);
            }
        }
        watches_.erase(std::remove_if(watches_.begin(), watches_.end(),
                                      [&](const Watch& w) { return w.epoch == epoch; }),
                       watches_.end());
        for (const Watch& w : ending) {
            observe::Ended e;
            e.subscription = w.subscription;
            e.relay = w.relay;
            e.seq = w.last + 1;
            e.last = w.last;
            e.kind = observe::kEndedLost;
            e.reason = "the link's session ended: " + why;
            e.link = name_;
            e.epoch = static_cast<std::int64_t>(w.epoch);
            e.session = session;
            (void)bus_->send_as(this->self_, w.subscriber,
                                Message(to_value(e), this->self_, WeaveId{}, 0));
        }
    }

    /// Ask the far relay to end a subscription whose local asker is gone. Not a crossing anyone
    /// waits on: its answer is recorded as it arrives and settles nothing.
    void release_far(const Watch& w) {
        if (!client_ || !client_->admitted() || client_->disconnected() || w.epoch != epoch_) {
            return;
        }
        observe::Release r;
        r.subscription = w.subscription;
        r.relay = w.relay;
        const std::string bytes = loom::serialize(loom::to_value(r));
        client_->send_to_role(observe::kObserveRole, ++next_attempt_, bytes, false);
        client_->flush();
    }

    std::string name_;
    std::string endpoint_;
    std::string identity_;
    std::string credential_;
    std::string link_state_ = "closed";
    std::string detail_;
    Switchboard* bus_ = nullptr;
    std::unique_ptr<BridgeClient> client_;
    std::vector<Crossing> open_;
    std::vector<Watch> watches_;     ///< far subscriptions in this link's custody (kMaxWatches)
    std::int64_t strays_ = 0;        ///< far observation words that named none of them
    std::uint64_t next_attempt_ = 0; ///< the last attempt minted; never reused
    std::uint64_t epoch_ = 0;        ///< sessions opened; 0 before the first
    std::uint64_t ended_epoch_ = 0;  ///< the last epoch told `ended`
};

} // namespace loom::host

#endif // ZEN_HOST_LINK_HPP
