// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/host/terminal_wiring.hpp>

#include <stdexcept>
#include <utility>

namespace loom {

namespace {

/// THE ONE IMPLEMENTATION OF `ParticipantChannel`, and the only place in the
/// terminal stack that names a `Switchboard`.
///
/// Each verb is the `*_as` form of the corresponding ordinary send: the sender is
/// stamped from `self_`, which was fixed when the host built this object, and the
/// delivery is GATED — authorized at delivery against that weave's own effective
/// authority. This is the same door a weave's own `WeaveBus` goes through; what
/// differs is only that this one outlives a single delivery, which is the whole
/// reason it exists.
///
/// It holds `self_` by value and the bus by reference, and there is no setter for
/// either. A terminal handed one of these has exactly one identity, forever.
class HostParticipantChannel final : public ParticipantChannel {
public:
    HostParticipantChannel(Switchboard& bus, WeaveId self) : bus_(bus), self_(self) {}

    WeaveId self() const noexcept override { return self_; }

    Ticket send(WeaveId target, Value payload, std::uint64_t correlation) override {
        return bus_.send_as(self_, target,
                            Message(std::move(payload), self_, WeaveId{}, correlation));
    }

    Ticket send_to_role(std::string_view office, Value payload,
                        std::uint64_t correlation) override {
        return bus_.send_as_to_role(self_, office,
                                    Message(std::move(payload), self_, WeaveId{}, correlation));
    }

    std::size_t publish(Value payload, std::uint64_t correlation) override {
        return bus_.publish_as(self_, Message(std::move(payload), self_, WeaveId{}, correlation));
    }

private:
    /// NO `reply_to` IS EVER SET, deliberately. A reply address is a request to be
    /// answered by an ordinary send, which carries no provenance; a terminal's
    /// asks are answered through Loom's own answer authority, which the bus binds
    /// to the stamped sender. Setting one would offer a second, weaker way for an
    /// answer to arrive, and a consumer that accepted either would have thrown
    /// away the distinction this core is built on.
    Switchboard& bus_;
    const WeaveId self_;
};

MountedTerminal mount(Switchboard& bus, std::unique_ptr<TerminalSession> session, Grant grant,
                      const std::string* role) {
    if (session == nullptr) {
        throw std::invalid_argument("loom::host_mount_terminal: no session");
    }
    TerminalSession* raw = session.get();
    const WeaveId id = role == nullptr
                           ? bus.register_weave(std::move(session), std::move(grant))
                           : bus.register_weave(std::move(session), std::move(grant), *role);
    // ...and only now can the door exist, because only now is there an identity for it to carry.
    raw->attach(host_participant_channel(bus, id));
    return MountedTerminal{id, raw};
}

} // namespace

std::unique_ptr<ParticipantChannel> host_participant_channel(Switchboard& bus, WeaveId who) {
    if (!who.valid()) {
        throw std::invalid_argument(
            "loom::host_participant_channel: a door must carry a real identity; the invalid "
            "WeaveId is what a root has, and a root is not a participant");
    }
    return std::make_unique<HostParticipantChannel>(bus, who);
}

MountedTerminal host_mount_terminal(Switchboard& bus, std::unique_ptr<TerminalSession> session,
                                    Grant grant) {
    return mount(bus, std::move(session), std::move(grant), nullptr);
}

MountedTerminal host_mount_terminal(Switchboard& bus, std::unique_ptr<TerminalSession> session,
                                    Grant grant, std::string role) {
    return mount(bus, std::move(session), std::move(grant), &role);
}

} // namespace loom
