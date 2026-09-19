// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_LINK_HPP
#define ZEN_HOST_LINK_HPP

// A LINK TO ANOTHER HOST, AS ONE ORDINARY PARTICIPANT OF THIS ONE.
//
// The supplied host can hold a connection to another running Loom -- a Workshop, another
// loom-host -- and put an office in front of it: `loom.link.<name>`. A local weave that wants
// something from the far side sends the link a `loom.link.Ask` naming the far office and
// carrying its message as bytes; the link ships it across under the asker's correlation, and
// hands back whatever the far host delivers to this session as an ORDINARY local message --
// re-admitted through THIS bus's gate, stamped as the link's own speech, under the same
// correlation -- so the asker settles it exactly as it settles any answer.
//
// WHAT THE LINK IS NOT. It is not authority: the far host admitted THIS SESSION under a grant of
// the far host's choosing, and every send is checked there against that grant. It is not a
// mirror of the far bus: it holds no tap (a guest is not given one), no far registry, no far
// history. It is not a retry engine: a lost link tells every open asker `lost` and never
// resends, because a send whose outcome is unknown may well have acted. And it is not a
// participant a loaded weave can replace -- it is host wiring, like the warden, holding a socket
// the host opened.
//
// THE HOST SERVICES IT. A loaded weave has nothing to wake it; the host loop calls `service()`
// every turn, exactly as it reads a line, so the socket is read between bus turns and a far
// answer is delivered into the next one.

#include <zen/bridge/client.hpp>
#include <zen/bridge/link.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/dispatch_refusal.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace loom::host {

/// Counters, and deliberately nothing else in the state shape: a link's open conversations are
/// plain members, because a reloaded link is a new session and inherits no open ask.
struct LinkState {
    std::int64_t submitted = 0;
    std::int64_t answered = 0;
    std::int64_t outcomes = 0;
    ZEN_SHAPE(LinkState, 1, ZEN_FIELD(submitted), ZEN_FIELD(answered), ZEN_FIELD(outcomes));
};

class LinkWeave final
    : public WeaveBase<LinkWeave, LinkState, Accept<link::Ask, link::StatusRequested>,
                       Emit<link::Outcome, link::Status>> {
public:
    /// The most asks one link holds open at once. Past it a new Ask is told `refused` at once,
    /// and every open one is untouched.
    static constexpr std::size_t kMaxOpenAsks = 64;

    LinkWeave(std::string name, std::string endpoint, std::string identity, std::string credential)
        : name_(std::move(name)), endpoint_(std::move(endpoint)), identity_(std::move(identity)),
          credential_(std::move(credential)) {}

    /// Host wiring: the bus the link delivers far answers into, as itself. Set once, out of band.
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

    /// OPEN THE SOCKET AND SAY HELLO. Bounded: it waits at most `timeout_ms` for the far host's
    /// Welcome or Denied. A failure is a state and a sentence, never an exception -- the host
    /// prints it and keeps running.
    bool connect(int timeout_ms, std::string* why) {
        lose("reconnecting"); // any open ask on an older session is lost, not carried over
        client_.reset();
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

    /// READ THE SOCKET, once per host turn. Far deliveries become local deliveries here.
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
                lose("denied: " + e.reason);
                break;
            case BridgeEvent::Kind::Delivered:
                on_delivered(e);
                break;
            case BridgeEvent::Kind::SendRefused:
                settle(e.correlation, link::kOutcomeRefused, e.reason);
                break;
            case BridgeEvent::Kind::Disconnected:
                if (link_state_ == "admitted" || link_state_ == "connecting") {
                    link_state_ = "lost";
                    detail_ = "the far host closed the connection";
                }
                lose(detail_.empty() ? std::string("the link closed") : detail_);
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
        const std::string shape = claimed_shape(ask.payload);
        const auto tell = [&](const char* state, std::string why) {
            ++state_.outcomes;
            link::Outcome o;
            o.state = state;
            o.reason = std::move(why);
            o.shape = shape;
            (void)mail.answer(o);
        };
        if (correlation == 0) {
            tell(link::kOutcomeRefused, "an ask across a link needs a correlation of its own, so "
                                        "its answer can come back under it");
            return;
        }
        if (!client_ || !client_->admitted() || client_->disconnected()) {
            tell(link::kOutcomeUnlinked, link_state_ == "denied" ? "this link was denied: " + detail_
                                                             : "this link is " + link_state_);
            return;
        }
        if (open_.size() >= kMaxOpenAsks) {
            tell(link::kOutcomeRefused, "this link already holds " + std::to_string(kMaxOpenAsks) +
                                            " open asks; wait for one to settle");
            return;
        }
        for (const Open& o : open_) {
            if (o.asker == mail.sender() && o.correlation == correlation) {
                tell(link::kOutcomeRefused, "correlation " + std::to_string(correlation) +
                                                " is already open on this link for this asker");
                return;
            }
        }
        const std::string_view bytes(reinterpret_cast<const char*>(ask.payload.data()),
                                     ask.payload.size());
        if (ask.role.empty()) {
            client_->send(static_cast<std::uint64_t>(ask.target), correlation, bytes);
        } else {
            client_->send_to_role(ask.role, correlation, bytes);
        }
        client_->flush();
        if (client_->disconnected()) {
            // Submitted into a socket that then closed: the far host may or may not have read
            // it. That is `lost`, told now rather than on the next service.
            tell(link::kOutcomeLost, "the link closed while this ask was being submitted");
            link_state_ = "lost";
            return;
        }
        ++state_.submitted;
        open_.push_back(Open{mail.sender(), correlation, shape});
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
        (void)mail.answer(s);
    }

private:
    struct Open {
        WeaveId asker{};
        std::uint64_t correlation = 0;
        std::string shape;
    };

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

    static std::string claimed_shape(const Bytes& payload) {
        const std::string_view bytes(reinterpret_cast<const char*>(payload.data()),
                                     payload.size());
        return loom::parse(bytes).claimed_name();
    }

    /// The link's own word to one asker, delivered as ordinary speech under the ask's
    /// correlation, from outside any delivery (a far frame arrived on the host's turn).
    void say_outcome(const Open& o, const char* state, std::string reason) {
        link::Outcome out;
        out.state = state;
        out.reason = std::move(reason);
        out.shape = o.shape;
        ++state_.outcomes;
        (void)bus_->send_as(this->self_, o.asker,
                            Message(to_value(out), this->self_, WeaveId{}, o.correlation));
    }

    void settle(std::uint64_t correlation, const char* state, const std::string& reason) {
        for (auto it = open_.begin(); it != open_.end(); ++it) {
            if (it->correlation == correlation) {
                Open o = *it;
                open_.erase(it);
                say_outcome(o, state, reason);
                return;
            }
        }
        // A correlation nobody is waiting on: a duplicate, a late word about a forgotten ask,
        // or a far host's invention. It settles nothing.
    }

    void lose(const std::string& why) {
        std::vector<Open> gone;
        gone.swap(open_);
        for (const Open& o : gone) {
            if (bus_ != nullptr) {
                say_outcome(o, link::kOutcomeLost, why);
            }
        }
    }

    void on_delivered(const BridgeEvent& e) {
        // WHICH ASK. The far correlation is the one this link put on the wire, and it names
        // exactly one open conversation; anything else is data the far host chose to send this
        // session and is told to nobody, because nobody asked for it.
        auto it = open_.begin();
        for (; it != open_.end(); ++it) {
            if (it->correlation == e.correlation) {
                break;
            }
        }
        if (it == open_.end()) {
            return;
        }
        if (e.dispatch_refused) {
            // The far bus refused the delivery (MSG-12). The notice's own reason is the word.
            Open o = *it;
            open_.erase(it);
            std::string reason = "the far bus refused the delivery";
            loom::Unverified u = loom::parse(e.payload);
            loom::Admission a = loom::admit(u, schema_of<DispatchRefused>());
            if (a.ok()) {
                if (const loom::Cell* r = a.value().get("reason")) {
                    reason = r->as_text();
                }
            }
            say_outcome(o, link::kOutcomeDispatchRefused, reason);
            return;
        }
        // RE-ADMIT THROUGH THIS BUS'S GATE. The far payload claims a shape; it is admitted only
        // against a shape THIS bus knows (the asker declared it), and delivered as the link's
        // ordinary speech under the ask's correlation. An attested far answer and a far
        // participant's unsolicited word both arrive this way; the asker's book settles on the
        // correlation and the link's stamp, and `answers_ask` rides the link's own ledger.
        loom::Unverified u = loom::parse(e.payload);
        std::shared_ptr<const Schema> door =
            bus_->resolve_schema(u.claimed_name(), u.claimed_version());
        if (!door) {
            Open o = *it;
            open_.erase(it);
            say_outcome(o, link::kOutcomeRefused,
                        "the far host answered with " + u.claimed_name() + " v" +
                            std::to_string(u.claimed_version()) + ", a shape nothing here declares");
            return;
        }
        loom::Admission a = loom::admit(u, door);
        if (!a.ok()) {
            Open o = *it;
            open_.erase(it);
            say_outcome(o, link::kOutcomeRefused,
                        "the far host's answer did not pass this bus's gate: " +
                            a.first_error().message());
            return;
        }
        const Open o = *it;
        if (e.answers_ask) {
            open_.erase(it); // an attested answer closes the conversation
        }
        ++state_.answered;
        (void)bus_->send_as(this->self_, o.asker,
                            Message(std::move(a).value(), this->self_, WeaveId{}, o.correlation));
    }

    std::string name_;
    std::string endpoint_;
    std::string identity_;
    std::string credential_;
    std::string link_state_ = "closed";
    std::string detail_;
    Switchboard* bus_ = nullptr;
    std::unique_ptr<BridgeClient> client_;
    std::vector<Open> open_;
};

} // namespace loom::host

#endif // ZEN_HOST_LINK_HPP
