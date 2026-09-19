// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_SESSION_DOOR_HPP
#define ZEN_HOST_SESSION_DOOR_HPP

// THE SESSION'S DOOR: who may attach to a persistent host, as whom, and under what grant.
//
// `loom-host --serve <dir>` keeps a host alive for clients that come and go. Everything a client
// or a run worker does reaches this host through ONE loopback listener, and this participant owns
// it: the bridge server (`zen/bridge/server.hpp`), the admission decision over every Hello, the
// lifetime id, and the office `loom.session` (`zen/session/vocabulary.hpp`) that describes the
// session and ends it. It is host wiring, like the warden and the links -- minimal hosting and
// authority, never task policy. What runs exist, which tools they execute and what a run is doing
// belong to a run manager, an ordinary artifact the operator approved; this door only admits the
// one connection that manager asked for, under no more than the manager itself may say.
//
// THREE KINDS OF CONNECTION, DECIDED BY WHAT THEY PRESENT, NEVER BY WHAT THEY CLAIM:
//
//   the owner's key     a CLIENT: may describe and end the session, speak the run manager's client
//                       vocabulary, and read the scoped history -- exactly those shapes, to exactly
//                       those offices. No tap: a client is not given the whole bus to watch.
//   a run credential    a RUN WORKER: one-time, expected by a registrar that told this door the
//                       credential's SHA-256 and the rules to grant. Admitted once, as `run:<name>`.
//   anything else       refused, in one sentence that does not say which guess came closest.
//
// ATTENUATION IS THE DOOR'S, AND IT IS SEMANTIC. A registrar may ask for a rule only when its own
// approved authority -- the operator's standing decision for the artifact the warden administers
// it as -- already CONTAINS that rule (`LiveAuthority::contains`), or when the rule addresses an
// office the registrar itself holds (a worker reporting to its manager threatens no third party).
// Anything else refuses the whole registration, naming the rule and the decision that would allow
// it: a run is never quietly granted less than it was started for, and never more than its
// manager holds. A run label is not attenuation; this is.
//
// A GRANT IS NOT A LEASE, AND THIS DOOR SAYS WHAT THAT MEANS HERE. A worker's session keeps its
// admission grant until its socket ends. What the door does add: when the registrar of a run is no
// longer on the bus, every session it registered is severed at the next service -- a manager that
// dies does not leave workers speaking on its authority -- and a registrar's `ForgetRun` severs one.
//
// ENCODING. Every session this door admits speaks Zen's compat JSON envelope (`PayloadEncoding::
// Compat`), so a client or a worker written in Python needs no copy of the canonical binary.

#include "authority.hpp"
#include "secure_random.hpp"
#include "warden.hpp"

#include <zen/bridge/channel.hpp>
#include <zen/bridge/server.hpp>
#include <zen/runs/vocabulary.hpp>
#include <zen/session/vocabulary.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/standard_shapes.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace loom::host {

struct SessionDoorState {
    std::int64_t clients = 0;  ///< client sessions admitted, all time
    std::int64_t runs = 0;     ///< run sessions admitted, all time
    std::int64_t refused = 0;  ///< connections refused
    std::int64_t expected = 0; ///< run registrations accepted, all time
    std::int64_t severed = 0;  ///< run sessions ended by the door (forgotten, or registrar gone)
    ZEN_SHAPE(SessionDoorState, 1, ZEN_FIELD(clients), ZEN_FIELD(runs), ZEN_FIELD(refused),
              ZEN_FIELD(expected), ZEN_FIELD(severed));
};

/// What the host tells the door about itself, once, at boot.
struct SessionFacts {
    std::string lifetime;
    std::string host;
    std::int64_t abi = 0;
    std::int64_t started_ms = 0;
    std::int64_t pid = 0;
    std::string directory;
    std::string containment;
};

class SessionDoor final
    : public WeaveBase<SessionDoor, SessionDoorState,
                       Accept<session::Describe, session::Shutdown, session::ExpectRun,
                              session::ForgetRun>,
                       Emit<session::Description, session::RunExpected, session::RunConnection,
                            Ack, Refused>> {
public:
    /// The most run registrations held at once (expected, or admitted and still connected).
    static constexpr std::size_t kMaxExpectations = 32;
    /// The most rules one run may ask for.
    static constexpr std::size_t kMaxRulesPerRun = 32;
    /// The longest run name, and the only characters it may use, so an established name is a
    /// plain token in every log and inventory that prints it.
    static constexpr std::size_t kMaxRunNameBytes = 64;

    SessionDoor(SessionFacts facts, std::string client_key, const AuthorityStore& store,
                const HostWarden& warden)
        : facts_(std::move(facts)), client_key_(std::move(client_key)), store_(&store),
          warden_(&warden) {}

    /// HOST WIRING: open the loopback listener and serve it. `port` 0 asks the OS for one.
    bool listen(Switchboard& bus, std::uint16_t port, std::string* why) {
        bus_ = &bus;
        std::string err;
        const socket_t listener = bridge_listen_tcp(port, &err);
        if (listener == kInvalidSocket) {
            *why = "cannot listen on 127.0.0.1:" + std::to_string(port) + ": " + err;
            return false;
        }
        port_ = bridge_socket_port(listener);
        server_ = std::make_unique<BridgeServer>(
            bus, listener, [this](const ConnectionRequest& r) { return decide(r); });
        server_->on_connection([this](const loom::Connection& c) { observed(c); });
        return true;
    }

    /// The link inventory the host holds, read at each Describe (the links are host wiring too).
    void describe_links_with(std::function<std::vector<session::LinkRow>()> links) {
        links_ = std::move(links);
    }

    std::uint16_t port() const noexcept { return port_; }
    std::string endpoint() const { return "127.0.0.1:" + std::to_string(port_); }
    const std::string& lifetime() const noexcept { return facts_.lifetime; }
    bool ending() const noexcept { return ending_; }
    const std::string& ending_reason() const noexcept { return ending_reason_; }
    std::size_t expectations() const noexcept { return expectations_.size(); }
    BridgeServer* server() noexcept { return server_.get(); }

    /// THE I/O HALF, once per host turn: accept, read, dispatch, flush -- and sever the sessions
    /// of any registrar that is no longer on the bus.
    void service() {
        if (!server_) {
            return;
        }
        server_->service();
        if (expectations_.empty() || bus_ == nullptr) {
            return;
        }
        std::set<std::uint64_t> live;
        for (const WeaveId id : bus_->list_weaves()) {
            live.insert(id.value);
        }
        for (std::size_t i = 0; i < expectations_.size();) {
            if (live.count(expectations_[i].registrar.value) != 0) {
                ++i;
                continue;
            }
            if (expectations_[i].connection != 0 && server_->disconnect(expectations_[i].connection)) {
                ++state_.severed;
            }
            expectations_.erase(expectations_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    // ---- the office ----------------------------------------------------------------------------

    void on(const session::Describe&, Mail& mail) { (void)mail.answer(description()); }

    void on(const session::Shutdown& s, Mail& mail) {
        // Only a client holds a grant naming this shape; the bus has already checked that. The
        // host reads `ending()` after the turn, flushes this answer, and exits 0.
        ending_ = true;
        ending_reason_ = s.reason.empty() ? std::string("a client asked this session to end")
                                          : s.reason;
        (void)mail.answer(Ack{});
    }

    void on(const session::ExpectRun& r, Mail& mail) {
        const WeaveId registrar = mail.sender();
        std::string why;
        if (!valid_run_name(r.run, &why)) {
            (void)mail.answer(Refused{why});
            return;
        }
        if (r.digest.size() != 64 ||
            r.digest.find_first_not_of("0123456789abcdef") != std::string::npos) {
            (void)mail.answer(Refused{"a run's credential digest is 64 lowercase hex characters "
                                      "(its SHA-256); the credential itself never crosses the bus"});
            return;
        }
        for (const Expectation& e : expectations_) {
            if (e.run == r.run) {
                (void)mail.answer(Refused{"run '" + r.run + "' is already registered with this door"});
                return;
            }
        }
        if (expectations_.size() >= kMaxExpectations) {
            (void)mail.answer(Refused{"this door already holds " +
                                      std::to_string(kMaxExpectations) +
                                      " run registrations; forget a finished run first"});
            return;
        }
        if (r.may_say.size() > kMaxRulesPerRun) {
            (void)mail.answer(Refused{"a run may ask for at most " + std::to_string(kMaxRulesPerRun) +
                                      " rules"});
            return;
        }
        // THE CEILING: what the operator approved for the artifact this registrar IS, read from
        // the warden's capability and the store -- never from anything the message says.
        const std::string artifact = warden_ != nullptr ? warden_->artifact_of(registrar)
                                                        : std::string();
        LiveAuthority ceiling;
        if (!artifact.empty()) {
            if (const AuthorityRule* rule = store_->find(artifact)) {
                std::string ignored;
                (void)to_live_authority(*rule, &ceiling, &ignored);
            }
        }
        Grant grant;
        std::vector<std::string> granted;
        for (const std::string& text : r.may_say) {
            LiveAuthority one;
            if (!apply_rule(text, &one, &why)) {
                (void)mail.answer(Refused{"run '" + r.run + "': " + why});
                return;
            }
            if (!own_office(one, registrar) && !ceiling.contains(one)) {
                const std::string who = artifact.empty() ? std::string("its registrar") : "'" + artifact + "'";
                (void)mail.answer(Refused{
                    "run '" + r.run + "' asks for '" + text + "', which " + who +
                    " may not say itself, so it cannot pass it on" +
                    (artifact.empty() ? std::string(" (the registrar is not an artifact this host "
                                                    "administers)")
                                      : "; allow it with:  authority allow " + artifact + " " +
                                            text)});
                return;
            }
            add_to(grant, one);
            granted.push_back(canonical_rule(text, text.rfind("observe", 0) == 0));
        }
        Expectation e;
        e.run = r.run;
        e.digest = r.digest;
        e.grant = std::move(grant);
        e.registrar = registrar;
        expectations_.push_back(std::move(e));
        ++state_.expected;
        (void)mail.answer(session::RunExpected{r.run, std::string(session::kRunSessionPrefix) + r.run,
                                               granted});
    }

    void on(const session::ForgetRun& f, Mail& mail) {
        for (std::size_t i = 0; i < expectations_.size(); ++i) {
            if (expectations_[i].run != f.run) {
                continue;
            }
            if (expectations_[i].registrar != mail.sender()) {
                (void)mail.answer(Refused{"run '" + f.run + "' was registered by another participant"});
                return;
            }
            if (expectations_[i].connection != 0 && server_ &&
                server_->disconnect(expectations_[i].connection)) {
                ++state_.severed;
            }
            expectations_.erase(expectations_.begin() + static_cast<std::ptrdiff_t>(i));
            (void)mail.answer(Ack{});
            return;
        }
        (void)mail.answer(Ack{}); // nothing held under that name: forgetting it again changes nothing
    }

    /// The description, as the office answers it (the host's own `status` reads the same).
    session::Description description() const {
        session::Description d;
        d.lifetime = facts_.lifetime;
        d.host = facts_.host;
        d.abi = facts_.abi;
        d.started_ms = facts_.started_ms;
        d.pid = facts_.pid;
        d.directory = facts_.directory;
        d.endpoint = endpoint();
        d.containment = facts_.containment;
        if (server_) {
            for (const loom::Connection& c : server_->connections()) {
                session::Connection row;
                row.connection = static_cast<std::int64_t>(c.connection);
                row.kind = kind_of(c);
                row.name = c.established_name;
                row.claimed = c.claimed_name;
                row.state = name_of(c.state);
                row.session = static_cast<std::int64_t>(c.session.value);
                row.encoding = name_of(c.encoding);
                d.connections.push_back(std::move(row));
            }
        }
        if (links_) {
            d.links = links_();
        }
        for (const Expectation& e : expectations_) {
            d.expected += e.connection == 0 ? 1 : 0;
        }
        d.ending = ending_;
        return d;
    }

    /// Exactly the shapes a client may say, to exactly the offices that answer them.
    static Grant client_grant() {
        Grant g;
        g.allow_to_role(session::Describe::zen_name, session::Describe::zen_version,
                        session::kSessionRole);
        g.allow_to_role(session::Shutdown::zen_name, session::Shutdown::zen_version,
                        session::kSessionRole);
        g.allow_to_role(session::DeliveriesTo::zen_name, session::DeliveriesTo::zen_version,
                        session::kHistoryRole);
        g.allow_to_role(session::Delivery::zen_name, session::Delivery::zen_version,
                        session::kHistoryRole);
        for (const std::string& shape : runs::client_request_shapes()) {
            g.allow_to_role(shape, 1, runs::kRunsRole);
        }
        return g;
    }

private:
    struct Expectation {
        std::string run;
        std::string digest;
        Grant grant;
        WeaveId registrar{};
        bool consumed = false;
        std::uint64_t connection = 0; ///< the server's number, once a credential matched
        std::int64_t session = 0;
        bool told_admitted = false;
        bool told_closed = false;
    };

    static bool valid_run_name(const std::string& name, std::string* why) {
        if (name.empty() || name.size() > kMaxRunNameBytes) {
            *why = "a run name is 1.." + std::to_string(kMaxRunNameBytes) + " characters";
            return false;
        }
        for (const char c : name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '-' || c == '_' || c == '.';
            if (!ok) {
                *why = "a run name uses only letters, digits, '-', '_' and '.'; '" + name +
                       "' does not";
                return false;
            }
        }
        return true;
    }

    /// Does `one` (a single parsed rule) address only an office `registrar` holds right now?
    bool own_office(const LiveAuthority& one, WeaveId registrar) const {
        if (bus_ == nullptr || !one.observe_rules().empty() || one.rules().size() != 1) {
            return false;
        }
        const SendRule& r = one.rules().front();
        return !r.any_shape && !r.any_target && !r.target_role.empty() &&
               bus_->role_holder(r.target_role) == registrar;
    }

    /// Carry parsed rules into an admission grant, rule for rule.
    static void add_to(Grant& g, const LiveAuthority& a) {
        for (const SendRule& r : a.rules()) {
            if (r.any_shape && r.any_target) {
                g.allow_any();
            } else if (r.any_target) {
                g.allow_to_any(r.shape_name, r.shape_version);
            } else if (!r.target_role.empty()) {
                g.allow_to_role(r.shape_name, r.shape_version, r.target_role);
            } else if (r.any_shape) {
                g.allow_any_to(r.target);
            } else {
                g.allow(r.shape_name, r.shape_version, r.target);
            }
        }
        for (const ObserveRule& o : a.observe_rules()) {
            if (o.any_shape) {
                g.allow_observe_any();
            } else {
                g.allow_observe(o.shape_name, o.shape_version);
            }
        }
    }

    /// THE ADMISSION DECISION, over what the peer PRESENTED. The claimed name is only recorded.
    ConnectionVerdict decide(const ConnectionRequest& r) {
        if (!client_key_.empty() && constant_time_equal(r.credential, client_key_)) {
            ConnectionAdmitted a;
            a.grant = client_grant();
            a.accept = AcceptMode::AnyRegistered;
            a.established_name = session::kClientSessionName;
            a.observe = false;
            a.encoding = PayloadEncoding::Compat;
            clients_.insert(r.connection);
            ++state_.clients;
            return ConnectionVerdict::admit(std::move(a));
        }
        if (!r.credential.empty()) {
            const std::string digest = credential_digest(r.credential);
            for (Expectation& e : expectations_) {
                if (!e.consumed && constant_time_equal(digest, e.digest)) {
                    e.consumed = true; // ONE-TIME: a second presentation is refused below
                    e.connection = r.connection;
                    ConnectionAdmitted a;
                    a.grant = e.grant;
                    a.accept = AcceptMode::AnyRegistered;
                    a.established_name = std::string(session::kRunSessionPrefix) + e.run;
                    a.observe = false;
                    a.encoding = PayloadEncoding::Compat;
                    ++state_.runs;
                    return ConnectionVerdict::admit(std::move(a));
                }
            }
        }
        ++state_.refused;
        return ConnectionVerdict::refuse(
            "this session admits its owner's client key and the one-time credential of a run it "
            "started; what was presented is neither");
    }

    /// Every state change of every connection: a run's `admitted` and `closed` go to its registrar.
    void observed(const loom::Connection& c) {
        if (c.state == ConnectionState::Closed) {
            clients_.erase(c.connection);
        }
        for (std::size_t i = 0; i < expectations_.size(); ++i) {
            Expectation& e = expectations_[i];
            if (e.connection != c.connection) {
                continue;
            }
            if (c.state == ConnectionState::Admitted && !e.told_admitted) {
                e.told_admitted = true;
                e.session = static_cast<std::int64_t>(c.session.value);
                notify(e, "admitted", "");
            } else if (c.state == ConnectionState::Closed && !e.told_closed) {
                e.told_closed = true;
                notify(e, "closed", "the worker's connection ended");
                // One-time and now over: the registration's place is free again.
                expectations_.erase(expectations_.begin() + static_cast<std::ptrdiff_t>(i));
            }
            return;
        }
    }

    void notify(const Expectation& e, const char* state, std::string detail) {
        if (bus_ == nullptr) {
            return;
        }
        session::RunConnection n{e.run, e.session, state, std::move(detail)};
        (void)bus_->send_as(this->self_, e.registrar,
                            Message(to_value(n), this->self_, WeaveId{}, 0));
    }

    std::string kind_of(const loom::Connection& c) const {
        if (clients_.count(c.connection) != 0) {
            return "client";
        }
        for (const Expectation& e : expectations_) {
            if (e.connection == c.connection) {
                return "run";
            }
        }
        return c.state == ConnectionState::Admitted ? "run" : "pending";
    }

    SessionFacts facts_;
    std::string client_key_;
    const AuthorityStore* store_ = nullptr;
    const HostWarden* warden_ = nullptr;
    Switchboard* bus_ = nullptr;
    std::unique_ptr<BridgeServer> server_;
    std::uint16_t port_ = 0;
    std::function<std::vector<session::LinkRow>()> links_;
    std::vector<Expectation> expectations_;
    std::set<std::uint64_t> clients_;
    bool ending_ = false;
    std::string ending_reason_;
};

} // namespace loom::host

#endif // ZEN_HOST_SESSION_DOOR_HPP
