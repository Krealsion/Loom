// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// Live delegation (GATE-05): a live mounted subject's real, Loom-enforced message authority can
// be widened and narrowed, without remounting it, by an ordinary weave holding a host-minted
// capability scoped to one subject and one ceiling. The cases are the failures it must catch: an
// administrator past its ceiling or reaching a subject it does not govern, another Loom's
// capability working here, a revocation that misses or strips the baseline, authorization at send
// time rather than delivery, and an administrator brokering what the subject should retry.

#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/host/grant_wiring.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace loom;
using namespace sbfx;

namespace {

// ---- the three shapes this suite's story needs ----------------------------
//
// Test-local, deliberately: this builds an enforcement primitive, and giving
// it a permanent `DriverCommand`/`RequestAuthority` vocabulary would be building
// the Weaver's policy interface a layer early.

std::shared_ptr<const Schema> work_schema() {
    static const auto s = SchemaBuilder("Work", 1).field("n", Kind::Int).build();
    return s;
}
std::shared_ptr<const Schema> ask_admin_schema() {
    static const auto s = SchemaBuilder("AskAdmin", 1).field("n", Kind::Int).build();
    return s;
}
std::shared_ptr<const Schema> other_work_schema() {
    static const auto s = SchemaBuilder("OtherWork", 1).field("n", Kind::Int).build();
    return s;
}

Value work(std::int64_t n) {
    Value v(work_schema());
    v.set("n", Cell::integer(n));
    return v;
}
Value ask_admin(std::int64_t n) {
    Value v(ask_admin_schema());
    v.set("n", Cell::integer(n));
    return v;
}
Value other_work(std::int64_t n) {
    Value v(other_work_schema());
    v.set("n", Cell::integer(n));
    return v;
}

bool tap_has_denied(const std::vector<TapRecord>& tap, WeaveId target, const std::string& shape) {
    for (const TapRecord& r : tap) {
        if (r.kind == EventKind::Refused && r.reason == RefusalReason::CapabilityDenied &&
            r.target == target && r.schema == shape) {
            return true;
        }
    }
    return false;
}

/// The ceiling this suite hands its administrator almost everywhere: exactly
/// "Work v1 may be spoken to the Service", and nothing else in the world.
LiveAuthority work_to(WeaveId service) {
    LiveAuthority ceiling;
    ceiling.allow("Work", 1, service);
    return ceiling;
}

// ---- compile-time proofs ---------------------------------------------------
//
// The strongest result for "live delegation cannot grant Network, SpawnProcess, filesystem reach
// or resource limits" is not a refusal a test observes: the delegation door's argument type HAS
// NO WORD for them. These detectors go red the day somebody adds one.

template <class T, class = void>
struct has_os_capabilities : std::false_type {};
template <class T>
struct has_os_capabilities<
    T, std::void_t<decltype(std::declval<T&>().with_os_capabilities(std::uint32_t{}))>>
    : std::true_type {};

template <class T, class = void>
struct has_filesystem : std::false_type {};
template <class T>
struct has_filesystem<
    T, std::void_t<decltype(std::declval<T&>().with_filesystem(loom::FsAccess::None, ""))>>
    : std::true_type {};

template <class T, class = void>
struct has_resources : std::false_type {};
template <class T>
struct has_resources<
    T, std::void_t<decltype(std::declval<T&>().with_resources(loom::ResourceLimits{}))>>
    : std::true_type {};

// The containment verbs exist on the admission envelope...
static_assert(has_os_capabilities<loom::Grant>::value, "Grant still names OS capabilities");
static_assert(has_filesystem<loom::Grant>::value, "Grant still names filesystem reach");
static_assert(has_resources<loom::Grant>::value, "Grant still names resource limits");
// ...and on the live-authority type they do not exist at all. An isolated child's
// namespace, mount view and cgroup leaf were built once, before it ran; no write
// in this process moves any of them, so no word here may pretend otherwise.
static_assert(!has_os_capabilities<loom::LiveAuthority>::value,
              "live authority must have no vocabulary for OS capabilities");
static_assert(!has_filesystem<loom::LiveAuthority>::value,
              "live authority must have no vocabulary for filesystem reach");
static_assert(!has_resources<loom::LiveAuthority>::value,
              "live authority must have no vocabulary for resource limits");
// And a whole Grant cannot be smuggled in through a conversion.
static_assert(!std::is_convertible_v<loom::Grant, loom::LiveAuthority>,
              "a Grant must not convert to a LiveAuthority");
static_assert(!std::is_constructible_v<loom::LiveAuthority, loom::Grant>,
              "a LiveAuthority must not be constructible from a Grant");

// An ordinary weave cannot mint one: the constructor is private and the one
// factory needs a Switchboard&, which a weave never holds.
static_assert(!std::is_constructible_v<loom::GrantAuthority, std::weak_ptr<const LoomIdentity>,
                                       WeaveId, loom::LiveAuthority>,
              "GrantAuthority must not be publicly constructible");

} // namespace

TEST_SUITE("grant") {

// =========================================================================
// The whole delegation, in one test.
// =========================================================================

TEST_CASE("a live session gains and loses real message authority, and stays its own sender") {
    Switchboard bus;

    // Service: accepts Work, and records who it heard from.
    Registered service = register_probe(bus, {work_schema()});
    std::vector<WeaveId> heard_from;
    service.weave->on_handle = [&heard_from](const Message& in, Bus&, ProbeWeave&) {
        heard_from.push_back(in.sender);
    };

    // Administrator: an ordinary weave. Its own grant is nothing at all — it
    // never speaks. Its power is the capability, handed to it by the host.
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());

    // Session: baseline authority is "may ask the administrator", and NOTHING
    // else. It may not speak Work to the Service.
    Grant session_base;
    session_base.allow("AskAdmin", 1, admin.id);
    Registered session = register_probe(bus, {ping_schema()}, 2, true, session_base);

    const GrantAuthority cap = host_grant_authority(bus, session.id, work_to(service.id));

    // The administrator's handler — an ORDINARY handler, reached by an ordinary
    // message — installs the delegated authority. It sends nothing.
    GrantChange last{};
    LiveAuthority to_install;
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        last = b.delegate_authority(cap, to_install);
    };

    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    // 1. Session -> Work: CapabilityDenied.
    session.weave->on_handle = [sid = service.id](const Message&, Bus& b, ProbeWeave&) {
        b.send(sid, Message(work(1)));
    };
    bus.send(session.id, Message(ping(1)));
    bus.drain_until_idle();
    CHECK(heard_from.empty());
    CHECK(tap_has_denied(tap, service.id, "Work"));

    // 2. Session -> AskAdmin: delivered (baseline authority).
    // 3. ...and the administrator's handler installs Work -> Service.
    to_install = LiveAuthority{}.allow("Work", 1, service.id);
    session.weave->on_handle = [aid = admin.id](const Message&, Bus& b, ProbeWeave&) {
        b.send(aid, Message(ask_admin(1)));
    };
    bus.send(session.id, Message(ping(2)));
    bus.drain_until_idle();
    REQUIRE(admin.weave->handled_names.size() == 1); // the ask arrived
    CHECK(last.outcome == GrantOutcome::Installed);
    CHECK(last.subject == session.id);
    CHECK(last.previous.empty()); // it held no delegated authority before
    CHECK_FALSE(last.installed.empty());

    // 4. Session -> Work again, explicitly: delivered.
    //    NOTE this is a NEW send. Nothing resurrected the message from step 1 —
    //    that one was discarded at refusal and no dead-letter path exists.
    session.weave->on_handle = [sid = service.id](const Message&, Bus& b, ProbeWeave&) {
        b.send(sid, Message(work(2)));
    };
    bus.send(session.id, Message(ping(3)));
    bus.drain_until_idle();
    REQUIRE(service.weave->handled_values.size() == 1);
    CHECK(service.weave->handled_values[0] == 2); // the RETRY, not the refused work(1)

    // 5. THE PROVENANCE ASSERTION. The Service heard the SESSION, not the
    //    administrator. If the implementation ever brokered the retry through
    //    the administrator, this is the line that fails.
    REQUIRE(heard_from.size() == 1);
    CHECK(heard_from[0] == session.id);
    CHECK(heard_from[0] != admin.id);

    // 6. The administrator revokes, through the same door: install nothing.
    to_install = LiveAuthority::nothing();
    session.weave->on_handle = [aid = admin.id](const Message&, Bus& b, ProbeWeave&) {
        b.send(aid, Message(ask_admin(2)));
    };
    bus.send(session.id, Message(ping(4)));
    bus.drain_until_idle();
    CHECK(last.outcome == GrantOutcome::Installed);
    CHECK_FALSE(last.previous.empty()); // it HAD Work
    CHECK(last.installed.empty());      // and now holds nothing delegated

    // 7. Session -> Work: CapabilityDenied again.
    tap.clear();
    session.weave->on_handle = [sid = service.id](const Message&, Bus& b, ProbeWeave&) {
        b.send(sid, Message(work(3)));
    };
    bus.send(session.id, Message(ping(5)));
    bus.drain_until_idle();
    CHECK(service.weave->handled_values.size() == 1); // still just the one from step 4
    CHECK(tap_has_denied(tap, service.id, "Work"));

    // 8. ...and the BASELINE survived the revocation untouched.
    const std::size_t asks_before = admin.weave->handled_names.size();
    session.weave->on_handle = [aid = admin.id](const Message&, Bus& b, ProbeWeave&) {
        b.send(aid, Message(ask_admin(3)));
    };
    bus.send(session.id, Message(ping(6)));
    bus.drain_until_idle();
    CHECK(admin.weave->handled_names.size() == asks_before + 1);
}

// =========================================================================
// Delivery-time enforcement.
// =========================================================================

TEST_CASE("a message queued while authorized is refused when delivery finds the authority gone") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    const GrantAuthority cap = host_grant_authority(bus, session.id, work_to(service.id));

    // Grant, host-side, before anything is queued.
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());
    LiveAuthority to_install = LiveAuthority{}.allow("Work", 1, service.id);
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.delegate_authority(cap, to_install);
    };
    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();

    // Sanity: while the rule is held, this exact send lands.
    session.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.send(service.id, Message(work(1)));
    };
    bus.send(session.id, Message(ping(0)));
    bus.drain_until_idle();
    REQUIRE(service.weave->handled_names.size() == 1);

    // THE WITNESS, and its shape is the point. `pump_pending()` dispatches exactly what was
    // queued at ENTRY, so in one turn the session's handler AUTHORS Work (authorized at that
    // instant, and only enqueued), the administrator's handler revokes, and the turn ends with
    // Work still queued. The next turn delivers it into a world where the authority is gone.
    session.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.send(service.id, Message(work(42)));
    };
    to_install = LiveAuthority::nothing();
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });

    const std::size_t queued_before = bus.pending();
    bus.send(session.id, Message(ping(1)));   // dispatched this turn: authors Work
    bus.send(admin.id, Message(ask_admin(2))); // dispatched this turn: revokes
    bus.pump_pending();
    REQUIRE(bus.pending() == queued_before + 1);       // the Work envelope, in flight
    REQUIRE(service.weave->handled_names.size() == 1); // and NOT yet delivered

    bus.drain_until_idle();

    // Refused. What was true when the message was authored bought nothing:
    // nothing on the envelope remembered it, and the check read the record as it
    // stands at the moment of delivery. This is the property live revocation is
    // built on — a mutation that cached authorization at send time makes exactly
    // these two lines go red.
    CHECK(service.weave->handled_names.size() == 1);
    CHECK(tap_has_denied(tap, service.id, "Work"));
}

TEST_CASE("granting after a denial does not resurrect the message that was refused") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    const GrantAuthority cap = host_grant_authority(bus, session.id, work_to(service.id));
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.delegate_authority(cap, LiveAuthority{}.allow("Work", 1, service.id));
    };

    session.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.send(service.id, Message(work(7)));
    };
    bus.send(session.id, Message(ping(1)));
    bus.drain_until_idle();
    REQUIRE(service.weave->handled_names.empty()); // refused and DISCARDED

    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();

    // Authority changed. History did not: nothing was replayed, no dead letter
    // was delivered, and the target has still never seen that Work.
    CHECK(service.weave->handled_names.empty());

    // Only an explicit retry gets through.
    bus.send(session.id, Message(ping(2)));
    bus.drain_until_idle();
    REQUIRE(service.weave->handled_values.size() == 1);
    CHECK(service.weave->handled_values[0] == 7);
}

// =========================================================================
// Baseline vs delegated.
// =========================================================================

TEST_CASE("revoking delegated authority leaves the admission baseline untouched") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());

    Grant base;
    base.allow("AskAdmin", 1, admin.id); // the host's baseline: may ask
    Registered session = register_probe(bus, {ping_schema()}, 2, true, base);

    const GrantAuthority cap = host_grant_authority(bus, session.id, work_to(service.id));
    REQUIRE(bus.weave(session.id) != nullptr);

    // Install, then revoke, both host-driven through a probe holding the capability.
    LiveAuthority to_install = LiveAuthority{}.allow("Work", 1, service.id);
    AuthorityView view{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.delegate_authority(cap, to_install);
        view = b.describe_authority(cap);
    };

    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();
    REQUIRE(view.available);
    CHECK(view.permits("Work", 1, service.id));    // delegated
    CHECK(view.permits("AskAdmin", 1, admin.id));  // baseline
    CHECK(view.base.permits("AskAdmin", 1, admin.id));
    CHECK_FALSE(view.base.permits("Work", 1, service.id)); // never was baseline
    CHECK(view.delegated.permits("Work", 1, service.id));
    CHECK_FALSE(view.delegated.permits("AskAdmin", 1, admin.id));

    to_install = LiveAuthority::nothing();
    bus.send(admin.id, Message(ask_admin(2)));
    bus.drain_until_idle();
    REQUIRE(view.available);
    CHECK_FALSE(view.permits("Work", 1, service.id)); // delegated gone
    CHECK(view.permits("AskAdmin", 1, admin.id));     // baseline intact
    CHECK(view.base.permits("AskAdmin", 1, admin.id));
    CHECK(view.delegated.empty());

    // ...and enforcement agrees with the picture.
    session.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.send(admin.id, Message(ask_admin(3)));
    };
    const std::size_t before = admin.weave->handled_names.size();
    bus.send(session.id, Message(ping(1)));
    bus.drain_until_idle();
    CHECK(admin.weave->handled_names.size() == before + 1);
}

TEST_CASE("a rule the baseline already carries survives revoking the delegated copy of it") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});

    Grant base;
    base.allow("Work", 1, service.id); // the host ALREADY permits this
    Registered session = register_probe(bus, {ping_schema()}, 2, true, base);
    const GrantAuthority cap = host_grant_authority(bus, session.id, work_to(service.id));

    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());
    LiveAuthority to_install = LiveAuthority{}.allow("Work", 1, service.id); // the same rule
    AuthorityView view{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.delegate_authority(cap, to_install);
        view = b.describe_authority(cap);
    };
    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();
    CHECK(view.permits("Work", 1, service.id));

    to_install = LiveAuthority::nothing();
    bus.send(admin.id, Message(ask_admin(2)));
    bus.drain_until_idle();

    // Effective authority is a UNION, so removing the delegated copy cannot
    // remove the host's. The inspection explains why: base still carries it.
    CHECK(view.permits("Work", 1, service.id));
    CHECK(view.base.permits("Work", 1, service.id));
    CHECK(view.delegated.empty());

    session.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.send(service.id, Message(work(5)));
    };
    bus.send(session.id, Message(ping(1)));
    bus.drain_until_idle();
    REQUIRE(service.weave->handled_values.size() == 1);
    CHECK(service.weave->handled_values[0] == 5);
}

// =========================================================================
// The ceiling.
// =========================================================================

TEST_CASE("an administrator cannot install authority outside its ceiling") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered other = register_probe(bus, {work_schema()});
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());

    // The ceiling is exactly: Work v1 -> service.
    const GrantAuthority cap = host_grant_authority(bus, session.id, work_to(service.id));
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());

    LiveAuthority attempt;
    GrantChange out{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        out = b.delegate_authority(cap, attempt);
    };
    auto try_install = [&](LiveAuthority a) {
        attempt = std::move(a);
        out = GrantChange{};
        bus.send(admin.id, Message(ask_admin(1)));
        bus.drain_until_idle();
        return out;
    };

    // The one legal request.
    CHECK(try_install(LiveAuthority{}.allow("Work", 1, service.id)).outcome ==
          GrantOutcome::Installed);

    // Every widening, refused — and each leaves the subject exactly as it was.
    CHECK(try_install(LiveAuthority{}.allow("OtherWork", 1, service.id)).outcome ==
          GrantOutcome::ExceedsCeiling); // different shape
    CHECK(try_install(LiveAuthority{}.allow("Work", 1, other.id)).outcome ==
          GrantOutcome::ExceedsCeiling); // different target
    CHECK(try_install(LiveAuthority{}.allow("Work", 2, service.id)).outcome ==
          GrantOutcome::ExceedsCeiling); // different VERSION of the same name
    CHECK(try_install(LiveAuthority{}.allow_to_any("Work", 1)).outcome ==
          GrantOutcome::ExceedsCeiling); // any target
    CHECK(try_install(LiveAuthority{}.allow_any_to(service.id)).outcome ==
          GrantOutcome::ExceedsCeiling); // any shape
    CHECK(try_install(LiveAuthority{}.allow_any()).outcome ==
          GrantOutcome::ExceedsCeiling); // the broadest dangerous rule of all
    CHECK(try_install(LiveAuthority{}.allow_observe("Work", 1)).outcome ==
          GrantOutcome::ExceedsCeiling); // observation is not licensed by a send ceiling
    CHECK(try_install(LiveAuthority{}.allow_observe_any()).outcome == GrantOutcome::ExceedsCeiling);
    // A legal rule PLUS an illegal one is refused whole — no partial install.
    LiveAuthority mixed;
    mixed.allow("Work", 1, service.id).allow("OtherWork", 1, service.id);
    CHECK(try_install(std::move(mixed)).outcome == GrantOutcome::ExceedsCeiling);

    // After every refusal the subject's delegated authority is still the one
    // legal thing installed at the top — nothing partially landed.
    AuthorityView view{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        view = b.describe_authority(cap);
    };
    bus.send(admin.id, Message(ask_admin(2)));
    bus.drain_until_idle();
    REQUIRE(view.available);
    CHECK(view.delegated.permits("Work", 1, service.id));
    CHECK_FALSE(view.delegated.permits("OtherWork", 1, service.id));
    CHECK_FALSE(view.delegated.permits("Work", 1, other.id));
    CHECK_FALSE(view.delegated.permits_observe("Work", 1));
}

TEST_CASE("a broad ceiling contains narrower rules, in every direction that is real") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());

    // Ceiling: any shape, to anyone. The host chose to hand out a wide one.
    const GrantAuthority wide =
        host_grant_authority(bus, session.id, LiveAuthority{}.allow_any());
    LiveAuthority attempt;
    GrantChange out{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        out = b.delegate_authority(wide, attempt);
    };
    auto try_install = [&](LiveAuthority a) {
        attempt = std::move(a);
        bus.send(admin.id, Message(ask_admin(1)));
        bus.drain_until_idle();
        return out;
    };

    CHECK(try_install(LiveAuthority{}.allow("Work", 1, service.id)).outcome ==
          GrantOutcome::Installed);
    CHECK(try_install(LiveAuthority{}.allow_to_any("Work", 1)).outcome == GrantOutcome::Installed);
    CHECK(try_install(LiveAuthority{}.allow_any_to(service.id)).outcome ==
          GrantOutcome::Installed);
    CHECK(try_install(LiveAuthority{}.allow_to_role("Work", 1, "svc")).outcome ==
          GrantOutcome::Installed);
    CHECK(try_install(LiveAuthority{}.allow_any()).outcome == GrantOutcome::Installed);
    // ...but `allow_any()` is a SEND wildcard and licenses no observation.
    CHECK(try_install(LiveAuthority{}.allow_observe("Work", 1)).outcome ==
          GrantOutcome::ExceedsCeiling);

    // An observe-wide ceiling is the mirror image.
    const GrantAuthority seer =
        host_grant_authority(bus, session.id, LiveAuthority{}.allow_observe_any());
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        out = b.delegate_authority(seer, attempt);
    };
    CHECK(try_install(LiveAuthority{}.allow_observe("Work", 1)).outcome ==
          GrantOutcome::Installed);
    CHECK(try_install(LiveAuthority{}.allow_observe_any()).outcome == GrantOutcome::Installed);
    CHECK(try_install(LiveAuthority{}.allow("Work", 1, service.id)).outcome ==
          GrantOutcome::ExceedsCeiling);

    // Revocation is inside every ceiling, however narrow — including an empty one.
    const GrantAuthority barren =
        host_grant_authority(bus, session.id, LiveAuthority::nothing());
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        out = b.delegate_authority(barren, attempt);
    };
    CHECK(try_install(LiveAuthority::nothing()).outcome == GrantOutcome::Installed);
    CHECK(try_install(LiveAuthority{}.allow("Work", 1, service.id)).outcome ==
          GrantOutcome::ExceedsCeiling);
}

TEST_CASE("an office rule and a weave rule never contain one another, whoever holds the office") {
    Switchboard bus;
    // The service holds the role — so "Work -> role svc" and "Work -> service.id"
    // pick out the same weave TODAY. They must still not contain one another:
    // otherwise a routing decision would become permanent authority.
    auto owned = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{
        work_schema()});
    ProbeWeave* raw = owned.get();
    const WeaveId service = bus.register_weave(std::move(owned), Grant{}.allow_any(), "svc");
    (void)raw;
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());

    LiveAuthority attempt;
    GrantChange out{};
    GrantAuthority cap;
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        out = b.delegate_authority(cap, attempt);
    };
    auto try_with = [&](GrantAuthority c, LiveAuthority a) {
        cap = std::move(c);
        attempt = std::move(a);
        bus.send(admin.id, Message(ask_admin(1)));
        bus.drain_until_idle();
        return out.outcome;
    };

    // Ceiling names the OFFICE; the request names the weave that holds it.
    CHECK(try_with(host_grant_authority(bus, session.id,
                                        LiveAuthority{}.allow_to_role("Work", 1, "svc")),
                   LiveAuthority{}.allow("Work", 1, service)) == GrantOutcome::ExceedsCeiling);
    // Ceiling names the WEAVE; the request names the office it currently holds.
    CHECK(try_with(host_grant_authority(bus, session.id, LiveAuthority{}.allow("Work", 1, service)),
                   LiveAuthority{}.allow_to_role("Work", 1, "svc")) ==
          GrantOutcome::ExceedsCeiling);
    // Each contains itself, and a different office is a different destination.
    CHECK(try_with(host_grant_authority(bus, session.id,
                                        LiveAuthority{}.allow_to_role("Work", 1, "svc")),
                   LiveAuthority{}.allow_to_role("Work", 1, "svc")) == GrantOutcome::Installed);
    CHECK(try_with(host_grant_authority(bus, session.id,
                                        LiveAuthority{}.allow_to_role("Work", 1, "svc")),
                   LiveAuthority{}.allow_to_role("Work", 1, "other")) ==
          GrantOutcome::ExceedsCeiling);
}

TEST_CASE("containment does not depend on the order rules were added") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered other = register_probe(bus, {work_schema()});
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());

    // A ceiling whose covering rule is LAST, and a request whose rules arrive in
    // both orders. Nothing here may depend on vector position.
    LiveAuthority ceiling;
    ceiling.allow("OtherWork", 1, other.id).allow_any_to(service.id);
    const GrantAuthority cap = host_grant_authority(bus, session.id, std::move(ceiling));

    LiveAuthority attempt;
    GrantChange out{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        out = b.delegate_authority(cap, attempt);
    };
    auto try_install = [&](LiveAuthority a) {
        attempt = std::move(a);
        bus.send(admin.id, Message(ask_admin(1)));
        bus.drain_until_idle();
        return out.outcome;
    };

    LiveAuthority forward;
    forward.allow("Work", 1, service.id).allow("OtherWork", 1, other.id);
    CHECK(try_install(std::move(forward)) == GrantOutcome::Installed);

    LiveAuthority backward;
    backward.allow("OtherWork", 1, other.id).allow("Work", 1, service.id);
    CHECK(try_install(std::move(backward)) == GrantOutcome::Installed);

    // And an uncovered rule is uncovered wherever it sits.
    LiveAuthority sneaky;
    sneaky.allow("Work", 1, service.id).allow("Work", 1, other.id).allow("OtherWork", 1, other.id);
    CHECK(try_install(std::move(sneaky)) == GrantOutcome::ExceedsCeiling);
}

// =========================================================================
// The capability itself.
// =========================================================================

TEST_CASE("a capability for one subject does nothing to another") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered a = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered b_sub = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());

    // The subject is IN the capability. There is no parameter by which this call
    // could name `b_sub`, so "administrator for A quietly administers B" is not a
    // check that could be removed — it is a sentence with nowhere to put B.
    const GrantAuthority cap_a = host_grant_authority(bus, a.id, work_to(service.id));

    AuthorityView view_a{}, view_b{};
    admin.weave->on_handle = [&](const Message&, Bus& bs, ProbeWeave&) {
        bs.delegate_authority(cap_a, LiveAuthority{}.allow("Work", 1, service.id));
        view_a = bs.describe_authority(cap_a);
        // The only way to look at B at all is a capability for B, which the host
        // never minted. A host-minted one proves B is untouched.
        view_b = bs.describe_authority(host_grant_authority(bus, b_sub.id, LiveAuthority{}));
    };
    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();

    REQUIRE(view_a.available);
    REQUIRE(view_b.available);
    CHECK(view_a.delegated.permits("Work", 1, service.id));
    CHECK(view_b.delegated.empty()); // B gained nothing
    CHECK_FALSE(view_b.permits("Work", 1, service.id));

    // ...and enforcement agrees: B still cannot speak Work.
    b_sub.weave->on_handle = [&](const Message&, Bus& bs, ProbeWeave&) {
        bs.send(service.id, Message(work(1)));
    };
    bus.send(b_sub.id, Message(ping(1)));
    bus.drain_until_idle();
    CHECK(service.weave->handled_names.empty());
}

TEST_CASE("a capability minted by another Loom has no standing here") {
    Switchboard board_a;
    Switchboard board_b;

    // The subject and the service live on board B.
    Registered service = register_probe(board_b, {work_schema()});
    Registered session = register_probe(board_b, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(board_b, {ask_admin_schema()}, 2, true, Grant::nothing());

    // The capability is minted by board A — for an id that board A does not even
    // have. It is entirely genuine; it is genuine SOMEWHERE ELSE.
    const GrantAuthority foreign =
        host_grant_authority(board_a, session.id, LiveAuthority{}.allow_any());

    GrantChange out{};
    AuthorityView view{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        out = b.delegate_authority(foreign, LiveAuthority{}.allow("Work", 1, service.id));
        view = b.describe_authority(foreign);
    };
    board_b.send(admin.id, Message(ask_admin(1)));
    board_b.drain_until_idle();

    CHECK(out.outcome == GrantOutcome::ForeignBoard);
    CHECK_FALSE(view.available); // and it learns nothing about board B either

    // No mutation happened: the session still cannot speak Work.
    session.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.send(service.id, Message(work(1)));
    };
    board_b.send(session.id, Message(ping(1)));
    board_b.drain_until_idle();
    CHECK(service.weave->handled_names.empty());

    // The same capability IS real on its own board — the point is domain, not
    // forgery. A subject with that id on board A would be administrable.
    Registered a_service = register_probe(board_a, {work_schema()});
    Registered a_sub = register_probe(board_a, {ping_schema()}, 2, true, Grant::nothing());
    Registered a_admin = register_probe(board_a, {ask_admin_schema()}, 2, true, Grant::nothing());
    const GrantAuthority home =
        host_grant_authority(board_a, a_sub.id, LiveAuthority{}.allow_any());
    GrantChange home_out{};
    a_admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        home_out = b.delegate_authority(home, LiveAuthority{}.allow("Work", 1, a_service.id));
    };
    board_a.send(a_admin.id, Message(ask_admin(1)));
    board_a.drain_until_idle();
    CHECK(home_out.outcome == GrantOutcome::Installed);
}

TEST_CASE("a capability whose board has died administers nothing") {
    // The subject's board outlives the ISSUING board, so `issued_here` has a
    // live registry to be asked about and an expired issuer to refuse.
    Switchboard living;
    Registered service = register_probe(living, {work_schema()});
    Registered session = register_probe(living, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(living, {ask_admin_schema()}, 2, true, Grant::nothing());

    GrantAuthority ghost;
    {
        Switchboard doomed;
        ghost = host_grant_authority(doomed, session.id, LiveAuthority{}.allow_any());
    } // the issuing board is destroyed; its identity's control block goes with it

    GrantChange out{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        out = b.delegate_authority(ghost, LiveAuthority{}.allow("Work", 1, service.id));
    };
    living.send(admin.id, Message(ask_admin(1)));
    living.drain_until_idle();
    CHECK(out.outcome == GrantOutcome::ForeignBoard);
}

TEST_CASE("a default-constructed capability is inert, and a wide grant is not one") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());

    // An ordinary weave with the most permissive grant in the system. It may say
    // ANYTHING to ANYONE — and that is speech authority, which is not
    // authority-administration authority. The only capability it can produce is
    // a default-constructed one, which names nothing.
    Registered loud = register_probe(bus, {ask_admin_schema()}, 2, true, Grant{}.allow_any());

    GrantChange out{};
    AuthorityView view{};
    loud.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        GrantAuthority forged; // everything a weave can build for itself
        out = b.delegate_authority(forged, LiveAuthority{}.allow("Work", 1, service.id));
        view = b.describe_authority(forged);
    };
    bus.send(loud.id, Message(ask_admin(1)));
    bus.drain_until_idle();

    CHECK(out.outcome == GrantOutcome::NoAuthority);
    CHECK_FALSE(view.available);

    // ...and nothing moved: the session still cannot say Work. A weave that may
    // emit anything is not thereby a weave that may decide what others emit —
    // the same wall LIFE-04 puts between emitting lifecycle-shaped bytes and
    // attesting a lifecycle.
    session.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.send(service.id, Message(work(1)));
    };
    bus.send(session.id, Message(ping(1)));
    bus.drain_until_idle();
    CHECK(service.weave->handled_names.empty());
}

TEST_CASE("administering a subject that is gone fails explicitly and touches nobody else") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered bystander = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());

    const GrantAuthority cap = host_grant_authority(bus, session.id, work_to(service.id));
    const WeaveId departed = session.id;

    // Install first, so there is real state to look for afterwards.
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.delegate_authority(cap, LiveAuthority{}.allow("Work", 1, service.id));
    };
    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();

    std::unique_ptr<Weave> handed_back = bus.unregister_weave(departed);
    CHECK(handed_back != nullptr);

    GrantChange out{};
    AuthorityView view{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        out = b.delegate_authority(cap, LiveAuthority{}.allow("Work", 1, service.id));
        view = b.describe_authority(cap);
    };
    bus.send(admin.id, Message(ask_admin(2)));
    bus.drain_until_idle();

    CHECK(out.outcome == GrantOutcome::NoSuchSubject);
    CHECK(out.subject == departed);
    CHECK_FALSE(view.available);

    // The bystander gained nothing from the failed act, and a NEW weave mounted
    // afterwards does not inherit the departed subject's delegation: ids are
    // never reused, so the old capability names nothing that exists.
    Registered fresh = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    CHECK(fresh.id != departed);
    for (Registered* r : {&bystander, &fresh}) {
        r->weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
            b.send(service.id, Message(work(1)));
        };
        bus.send(r->id, Message(ping(1)));
        bus.drain_until_idle();
    }
    CHECK(service.weave->handled_names.empty());
}

TEST_CASE("a subject's delegated authority dies with the subject") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());
    const GrantAuthority cap = host_grant_authority(bus, session.id, work_to(service.id));

    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.delegate_authority(cap, LiveAuthority{}.allow("Work", 1, service.id));
    };
    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();

    // It worked while the subject lived.
    session.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.send(service.id, Message(work(1)));
    };
    bus.send(session.id, Message(ping(1)));
    bus.drain_until_idle();
    CHECK(service.weave->handled_names.size() == 1);

    // The record's destruction is the whole of the cleanup: there is no delegated
    // authority left anywhere for a later weave to inherit.
    (void)bus.unregister_weave(session.id);
    CHECK(bus.weave(session.id) == nullptr);
    AuthorityView view{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        view = b.describe_authority(cap);
    };
    bus.send(admin.id, Message(ask_admin(2)));
    bus.drain_until_idle();
    CHECK_FALSE(view.available);
}

TEST_CASE("delegated authority outlives the administrator that installed it") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());

    {
        // The capability is a local, destroyed at the end of this scope. A
        // capability authorizes operations; it is not a lease, so its destruction
        // must not silently revoke what it established.
        const GrantAuthority cap = host_grant_authority(bus, session.id, work_to(service.id));
        admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
            b.delegate_authority(cap, LiveAuthority{}.allow("Work", 1, service.id));
        };
        bus.send(admin.id, Message(ask_admin(1)));
        bus.drain_until_idle();
    }
    // ...and the administrator weave itself is removed.
    (void)bus.unregister_weave(admin.id);

    session.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.send(service.id, Message(work(11)));
    };
    bus.send(session.id, Message(ping(1)));
    bus.drain_until_idle();
    REQUIRE(service.weave->handled_values.size() == 1);
    CHECK(service.weave->handled_values[0] == 11);
}

// =========================================================================
// Observation authority.
// =========================================================================

TEST_CASE("observe authority is delegable and revocable, and is read at the moment of the read") {
    Switchboard bus;

    // A claimant that declares and claims a Counter.
    class Claimer : public ProbeWeave {
    public:
        using ProbeWeave::ProbeWeave;
        std::vector<std::shared_ptr<const Schema>> claimed_schemas() const override {
            return {counter_schema()};
        }
    };
    auto owned = std::make_unique<Claimer>(std::vector<std::shared_ptr<const Schema>>{
        ping_schema()});
    Claimer* claimer_raw = owned.get();
    const WeaveId claimer = bus.register_weave(std::move(owned), Grant{}.allow_any());
    claimer_raw->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        Value v(counter_schema());
        v.set("count", Cell::integer(7));
        b.claim(std::move(v));
    };
    bus.send(claimer, Message(ping(1)));
    bus.drain_until_idle();

    // The reader is admitted with NO observe authority at all.
    Registered reader = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    SenseRefusal seen = SenseRefusal::None;
    bool had_value = false;
    reader.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        SenseReading r = b.observe(claimer, counter_schema());
        seen = r.refusal;
        had_value = r.value.has_value();
    };
    bus.send(reader.id, Message(ping(1)));
    bus.drain_until_idle();
    CHECK(seen == SenseRefusal::NotAuthorized);
    CHECK_FALSE(had_value);

    // Delegate observe authority.
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());
    const GrantAuthority cap =
        host_grant_authority(bus, reader.id, LiveAuthority{}.allow_observe("Counter", 1));
    LiveAuthority to_install = LiveAuthority{}.allow_observe("Counter", 1);
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.delegate_authority(cap, to_install);
    };
    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();

    bus.send(reader.id, Message(ping(2)));
    bus.drain_until_idle();
    CHECK(seen == SenseRefusal::None);
    CHECK(had_value);

    // Revoke — and the very next read is refused. Nothing cached the answer.
    to_install = LiveAuthority::nothing();
    bus.send(admin.id, Message(ask_admin(2)));
    bus.drain_until_idle();
    bus.send(reader.id, Message(ping(3)));
    bus.drain_until_idle();
    CHECK(seen == SenseRefusal::NotAuthorized);
    CHECK_FALSE(had_value);
}

// =========================================================================
// Atomicity, inspection, reload.
// =========================================================================

TEST_CASE("replacement is one transition: the old rule is never live beside the new one") {
    Switchboard bus;
    Registered a = register_probe(bus, {work_schema()});
    Registered b_svc = register_probe(bus, {other_work_schema()});
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());

    LiveAuthority ceiling;
    ceiling.allow("Work", 1, a.id).allow("OtherWork", 1, b_svc.id);
    const GrantAuthority cap = host_grant_authority(bus, session.id, std::move(ceiling));

    LiveAuthority to_install = LiveAuthority{}.allow("Work", 1, a.id);
    AuthorityView view{};
    admin.weave->on_handle = [&](const Message&, Bus& bus_, ProbeWeave&) {
        bus_.delegate_authority(cap, to_install);
        // Read back inside the SAME handler, immediately after the write: there
        // is no turn, no pump and no weave code between them, so if any state
        // existed where both old and new were live this is where it would show.
        view = bus_.describe_authority(cap);
    };
    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();
    CHECK(view.delegated.permits("Work", 1, a.id));
    CHECK_FALSE(view.delegated.permits("OtherWork", 1, b_svc.id));

    to_install = LiveAuthority{}.allow("OtherWork", 1, b_svc.id);
    bus.send(admin.id, Message(ask_admin(2)));
    bus.drain_until_idle();
    // A allowed/B denied became A denied/B allowed, with nothing in between:
    // the board is single-threaded and the transition is one assignment, so no
    // observer — weave, tap or handler — can be scheduled inside it.
    CHECK_FALSE(view.delegated.permits("Work", 1, a.id));
    CHECK(view.delegated.permits("OtherWork", 1, b_svc.id));

    // ...and enforcement matches the report, both ways round.
    session.weave->on_handle = [&](const Message&, Bus& bus_, ProbeWeave&) {
        bus_.send(a.id, Message(work(1)));
        bus_.send(b_svc.id, Message(other_work(2)));
    };
    bus.send(session.id, Message(ping(1)));
    bus.drain_until_idle();
    CHECK(a.weave->handled_names.empty());
    CHECK(b_svc.weave->handled_names.size() == 1);
}

TEST_CASE("inspection reports the state delivery will actually apply") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());
    const GrantAuthority cap = host_grant_authority(bus, session.id, work_to(service.id));

    LiveAuthority to_install;
    AuthorityView view{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.delegate_authority(cap, to_install);
        view = b.describe_authority(cap);
    };
    session.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.send(service.id, Message(work(1)));
    };
    auto delivered_after = [&](LiveAuthority a) {
        to_install = std::move(a);
        bus.send(admin.id, Message(ask_admin(1)));
        bus.drain_until_idle();
        const std::size_t before = service.weave->handled_names.size();
        bus.send(session.id, Message(ping(1)));
        bus.drain_until_idle();
        return service.weave->handled_names.size() > before;
    };

    // read / grant / read / revoke / read — and each report matches reality.
    CHECK_FALSE(delivered_after(LiveAuthority::nothing()));
    CHECK(view.available);
    CHECK_FALSE(view.permits("Work", 1, service.id));

    CHECK(delivered_after(LiveAuthority{}.allow("Work", 1, service.id)));
    CHECK(view.permits("Work", 1, service.id));

    CHECK_FALSE(delivered_after(LiveAuthority::nothing()));
    CHECK_FALSE(view.permits("Work", 1, service.id));
}

TEST_CASE("delegated authority survives a reload of the same subject") {
    Switchboard bus;
    Registered service = register_probe(bus, {work_schema()});
    // max_reloads=2, revive-from-last-known-good.
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());
    const GrantAuthority cap = host_grant_authority(bus, session.id, work_to(service.id));

    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.delegate_authority(cap, LiveAuthority{}.allow("Work", 1, service.id));
    };
    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();

    // Kill and revive: the same WeaveId, a new life, the same mounted subject.
    const std::string bytes = bus.snapshot_bytes(session.id);
    bus.kill(session.id);
    CHECK_FALSE(bus.alive(session.id));
    const ReviveOutcome out = bus.reload(session.id, bytes);
    CHECK(out.revived);
    CHECK(bus.alive(session.id));

    // The BASELINE grant survives a reload (it is a property of the mounted
    // subject, not of a life), so delegated authority — scoped to the same
    // WeaveId, by the same argument — survives it too. One identity rule, not two.
    auto* revived = static_cast<ProbeWeave*>(bus.weave(session.id));
    REQUIRE(revived != nullptr);
    revived->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        b.send(service.id, Message(work(3)));
    };
    bus.send(session.id, Message(ping(1)));
    bus.drain_until_idle();
    REQUIRE(service.weave->handled_values.size() == 1);
    CHECK(service.weave->handled_values[0] == 3);
}

// =========================================================================
// The refusal vocabulary itself.
// =========================================================================

TEST_CASE("every administration outcome is reachable and named") {
    CHECK(std::string(name_of(GrantOutcome::Installed)) == "Installed");
    CHECK(std::string(name_of(GrantOutcome::NoAuthority)) == "NoAuthority");
    CHECK(std::string(name_of(GrantOutcome::ForeignBoard)) == "ForeignBoard");
    CHECK(std::string(name_of(GrantOutcome::NoSuchSubject)) == "NoSuchSubject");
    CHECK(std::string(name_of(GrantOutcome::ExceedsCeiling)) == "ExceedsCeiling");
    CHECK(std::string(name_of(GrantOutcome::NoLiveDelivery)) == "NoLiveDelivery");
    CHECK(std::string(name_of(GrantOutcome::DescribeOnly)) == "DescribeOnly");

    // A Bus that is not a live participating context truthfully says so rather
    // than pretending to have administered anything.
    struct InertBus : Bus {
        Ticket send(WeaveId, Message) override { return Ticket{}; }
        std::size_t publish(Message) override { return 0; }
        Ticket send_to_role(std::string_view, Message) override { return Ticket{}; }
    } inert;
    Switchboard bus;
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    const GrantAuthority cap =
        host_grant_authority(bus, session.id, LiveAuthority{}.allow_any());
    const GrantChange out = inert.delegate_authority(cap, LiveAuthority{}.allow_any());
    CHECK(out.outcome == GrantOutcome::NoLiveDelivery);
    CHECK(out.installed.empty());
    CHECK_FALSE(inert.describe_authority(cap).available);

    // A refused change reports the state unchanged, on every refusal.
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());
    const GrantAuthority narrow =
        host_grant_authority(bus, session.id, LiveAuthority::nothing());
    GrantChange refused{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        refused = b.delegate_authority(narrow, LiveAuthority{}.allow_any());
    };
    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();
    CHECK(refused.outcome == GrantOutcome::ExceedsCeiling);
    CHECK(refused.previous.empty());
    CHECK(refused.installed.empty());
    CHECK_FALSE(static_cast<bool>(refused));
}

// =========================================================================
// Doors: a name on a send rule to an office, which that office judges.
// =========================================================================

TEST_CASE("a door is read by its grammar, and every other spelling is refused in words") {
    for (const char* text : {"zengine.files/*", "zengine.files/files", "zengine.files/files:*",
                             "zengine.files/files:files.open",
                             "zengine.builder-pane/builder:builder.*",
                             "zengine.builder-pane/builder:builder.build-realize", "arrange:*",
                             "arrange:split_1", "workshop:quit"}) {
        CAPTURE(text);
        std::string why;
        const std::optional<Door> door = Door::parse(text, &why);
        REQUIRE_MESSAGE(door.has_value(), why);
        CHECK(door->text() == text); // one spelling per door: what was written is what is held
    }

    // `*` stands only as a whole last segment or as an id's tail after a dot; anything else is
    // malformed, and the refusal says which rule it breaks.
    const std::string long_door = "o/p:" + std::string(kMaxDoorBytes, 'x');
    for (const std::string& text :
         {std::string("zengine.files*"), std::string("builder.build*"), std::string("*"),
          std::string("zengine.files*/x"), std::string("zengine.files/files*"),
          std::string("zengine.builder-pane/builder:builder.build*"), std::string("o/*:x"),
          std::string("o/p:*.x"), std::string("o/p:a.*.*"), std::string("o/p:.*"),
          std::string("*/p"), std::string("*:x"), std::string("o/p:a..b"), std::string("o./p"),
          std::string(".o/p"), std::string("o/p."), std::string("o/"), std::string("o/p:"),
          std::string("o/p:a:b"), std::string("o/p/q"), std::string("zengine.files"),
          std::string("o p/x"), std::string("o/p:x\x1b"), std::string(""), long_door}) {
        CAPTURE(text);
        std::string why;
        CHECK_FALSE(Door::parse(text, &why).has_value());
        CHECK_FALSE(why.empty());
    }
    std::string why;
    CHECK_FALSE(Door::parse("builder.build*", &why).has_value());
    CHECK(why.find("is not a door") != std::string::npos);
    CHECK_FALSE(Door::parse("o/p:builder.build*", &why).has_value());
    CHECK(why.find("'*' that is not a whole last segment") != std::string::npos);
    CHECK_FALSE(Door::parse(long_door, &why).has_value());
    CHECK(why.find(std::to_string(kMaxDoorBytes)) != std::string::npos);
}

TEST_CASE("a door contains whole segments, never a prefix of one") {
    const auto contains = [](const char* outer, const char* inner) {
        const std::optional<Door> o = Door::parse(outer);
        const std::optional<Door> i = Door::parse(inner);
        REQUIRE(o.has_value());
        REQUIRE(i.has_value());
        return o->contains(*i);
    };
    // An office's `*` contains every door beneath the office, and no other office's.
    CHECK(contains("zengine.files/*", "zengine.files/files:files.open"));
    CHECK(contains("zengine.files/*", "zengine.files/files"));
    CHECK(contains("zengine.files/*", "zengine.files/files:*"));
    CHECK(contains("zengine.files/*", "zengine.files/*"));
    CHECK_FALSE(contains("zengine.files/*", "zengine.files-evil/x"));
    CHECK_FALSE(contains("zengine.files/*", "zengine.files.x/files"));
    // A pane's `*` contains its controls, not the pane's own door, and no other pane's.
    CHECK(contains("o/p:*", "o/p:x"));
    CHECK(contains("o/p:*", "o/p:x.y"));
    CHECK(contains("o/p:*", "o/p:x.*"));
    CHECK_FALSE(contains("o/p:*", "o/p2:x"));
    CHECK_FALSE(contains("o/p:*", "o/p"));
    CHECK_FALSE(contains("o/p:*", "o/*"));
    // The pane's own door contains only itself, and none of its controls.
    CHECK(contains("o/p", "o/p"));
    CHECK_FALSE(contains("o/p", "o/p:x"));
    CHECK_FALSE(contains("o/p", "o/p:*"));
    // An id's tail contains the ids beneath it, and not itself without the star.
    CHECK(contains("zengine.builder-pane/builder:builder.*",
                   "zengine.builder-pane/builder:builder.build"));
    CHECK(contains("zengine.builder-pane/builder:builder.*",
                   "zengine.builder-pane/builder:builder.build-realize"));
    CHECK_FALSE(contains("zengine.builder-pane/builder:builder.*",
                         "zengine.builder-pane/builder:builder"));
    CHECK_FALSE(contains("zengine.builder-pane/builder:builder.*",
                         "zengine.builder-pane/builder:*"));
    CHECK_FALSE(contains("o/p:a.b.*", "o/p:a.*"));
    // An exact id is itself and nothing longer.
    CHECK(contains("zengine.builder-pane/builder:builder.build",
                   "zengine.builder-pane/builder:builder.build"));
    CHECK_FALSE(contains("zengine.builder-pane/builder:builder.build",
                         "zengine.builder-pane/builder:builder.build-realize"));
    CHECK_FALSE(contains("zengine.builder-pane/builder:builder.build",
                         "zengine.builder-pane/builder:builder.build.x"));
    // A root an office declares is its own tree.
    CHECK(contains("arrange:*", "arrange:split_1"));
    CHECK_FALSE(contains("arrange:*", "arrange-evil:split_1"));
    CHECK_FALSE(contains("arrange:*", "arrange/p"));
    CHECK_FALSE(contains("o/*", "o:x"));
    CHECK_FALSE(contains("o:*", "o/p:x"));
}

TEST_CASE("a door rule permits its door and no other") {
    const Door open = *Door::parse("zengine.files/files:files.open");
    LiveAuthority a;
    a.allow_to_role("ControlRequested", 1, "zengine.workshop", open);

    CHECK(a.permits_role("ControlRequested", 1, "zengine.workshop", open));
    CHECK_FALSE(a.permits_role("ControlRequested", 1, "zengine.workshop",
                               *Door::parse("zengine.files/files:files.delete")));
    CHECK_FALSE(a.permits_role("ControlRequested", 1, "zengine.workshop",
                               *Door::parse("zengine.files/files")));
    CHECK_FALSE(a.permits_role("ControlRequested", 1, "zengine.workshop",
                               *Door::parse("zengine.files/*")));
    // The rest of the rule still counts: another office, shape or version is another rule.
    CHECK_FALSE(a.permits_role("ControlRequested", 1, "zengine.other", open));
    CHECK_FALSE(a.permits_role("OtherRequested", 1, "zengine.workshop", open));
    CHECK_FALSE(a.permits_role("ControlRequested", 2, "zengine.workshop", open));

    // A wide door rule permits each door beneath it, through a Grant as through a view.
    Grant g;
    g.allow_to_role("ControlRequested", 1, "zengine.workshop", *Door::parse("zengine.files/*"));
    CHECK(g.permits_role("ControlRequested", 1, "zengine.workshop", open));
    CHECK_FALSE(g.permits_role("ControlRequested", 1, "zengine.workshop",
                               *Door::parse("zengine.builder-pane/builder:builder.build")));
    REQUIRE(g.rules().size() == 1);
    REQUIRE(g.rules().front().door.has_value());
    CHECK(g.rules().front().door->text() == "zengine.files/*");
}

TEST_CASE("the door axis is disjoint both ways, and delivery asks only the door-less question") {
    const Door files = *Door::parse("zengine.files/*");
    LiveAuthority plain;
    plain.allow_to_role("ControlRequested", 1, "zengine.workshop");
    plain.allow_to_any("ControlRequested", 1);
    plain.allow_any();
    // A door-less rule, `any` rules included, answers no door question...
    CHECK(plain.permits_role("ControlRequested", 1, "zengine.workshop"));
    CHECK_FALSE(plain.permits_role("ControlRequested", 1, "zengine.workshop", files));

    // ...and a door rule no door-less one: holding a door is not being able to speak.
    LiveAuthority door_only;
    door_only.allow_to_role("ControlRequested", 1, "zengine.workshop", files);
    CHECK_FALSE(door_only.permits_role("ControlRequested", 1, "zengine.workshop"));
    CHECK_FALSE(door_only.permits("ControlRequested", 1, WeaveId{}));

    // The same at the real boundary: a subject holding only a door rule to an office is refused
    // when it speaks to that office, since delivery asks the door-less question.
    Switchboard bus;
    auto owned = std::make_unique<ProbeWeave>(std::vector<std::shared_ptr<const Schema>>{
        work_schema()});
    ProbeWeave* service = owned.get();
    const WeaveId service_id = bus.register_weave(std::move(owned), Grant::nothing(), "svc");
    Grant session_base;
    session_base.allow_to_role("Work", 1, "svc", *Door::parse("svc/desk:*"));
    Registered session = register_probe(bus, {ping_schema()}, 2, true, session_base);
    std::vector<TapRecord> tap;
    bus.add_observer([&tap](const BusEvent& e) { tap.push_back(to_record(e)); });
    session.weave->on_handle = [](const Message&, Bus& b, ProbeWeave&) {
        b.send_to_role("svc", Message(work(1)));
    };
    bus.send(session.id, Message(ping(1)));
    bus.drain_until_idle();
    CHECK(service->handled_names.empty());
    bool denied = false;
    for (const TapRecord& r : tap) {
        denied = denied || (r.kind == EventKind::Refused &&
                            r.reason == RefusalReason::CapabilityDenied && r.schema == "Work");
    }
    CHECK(denied);
    (void)service_id;
}

TEST_CASE("a ceiling of doors delegates the doors it contains and nothing beside them") {
    Switchboard bus;
    Registered session = register_probe(bus, {ping_schema()}, 2, true, Grant::nothing());
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());

    LiveAuthority attempt;
    GrantChange out{};
    GrantAuthority cap;
    AuthorityView view{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        out = b.delegate_authority(cap, attempt);
        view = b.describe_authority(cap);
    };
    auto try_with = [&](const GrantAuthority& c, LiveAuthority a) {
        cap = c;
        attempt = std::move(a);
        bus.send(admin.id, Message(ask_admin(1)));
        bus.drain_until_idle();
        return out.outcome;
    };
    const auto door_rule = [](const char* door) {
        return LiveAuthority{}.allow_to_role("ControlRequested", 1, "zengine.workshop",
                                             *Door::parse(door));
    };

    const GrantAuthority files = host_grant_authority(bus, session.id,
                                                      door_rule("zengine.files/*"));
    CHECK(try_with(files, door_rule("zengine.files/files:files.open")) ==
          GrantOutcome::Installed);
    // What was installed is the door asked for, and the view answers the door question with it.
    REQUIRE(view.delegated.rules().size() == 1);
    CHECK(view.delegated.rules().front().door->text() == "zengine.files/files:files.open");
    CHECK(view.permits_role("ControlRequested", 1, "zengine.workshop",
                            *Door::parse("zengine.files/files:files.open")));
    CHECK_FALSE(view.permits_role("ControlRequested", 1, "zengine.workshop",
                                  *Door::parse("zengine.files/files:files.delete")));
    CHECK_FALSE(view.permits_role("ControlRequested", 1, "zengine.workshop"));

    CHECK(try_with(files, door_rule("zengine.files/*")) == GrantOutcome::Installed);
    CHECK(try_with(files, door_rule("zengine.builder-pane/builder:builder.build")) ==
          GrantOutcome::ExceedsCeiling);
    CHECK(try_with(files, door_rule("zengine.files-evil/x")) == GrantOutcome::ExceedsCeiling);
    // A door ceiling holds no door-less rule, and a door-less ceiling, `any` included, no door.
    CHECK(try_with(files, LiveAuthority{}.allow_to_role("ControlRequested", 1,
                                                        "zengine.workshop")) ==
          GrantOutcome::ExceedsCeiling);
    const GrantAuthority plain = host_grant_authority(
        bus, session.id, LiveAuthority{}.allow_to_role("ControlRequested", 1, "zengine.workshop"));
    CHECK(try_with(plain, door_rule("zengine.files/*")) == GrantOutcome::ExceedsCeiling);
    const GrantAuthority wide = host_grant_authority(bus, session.id, LiveAuthority{}.allow_any());
    CHECK(try_with(wide, door_rule("zengine.files/*")) == GrantOutcome::ExceedsCeiling);
    // The office still counts: the same door to another office is another rule.
    CHECK(try_with(files, LiveAuthority{}.allow_to_role("ControlRequested", 1, "zengine.other",
                                                        *Door::parse("zengine.files/*"))) ==
          GrantOutcome::ExceedsCeiling);
}

TEST_CASE("a describe-only capability reads its subject's authority and installs nothing") {
    Switchboard bus;
    Registered session = register_probe(bus, {ping_schema()}, 2, true,
                                         Grant{}.allow_to_role("Work", 1, "svc"));
    Registered admin = register_probe(bus, {ask_admin_schema()}, 2, true, Grant::nothing());
    const Door files = *Door::parse("zengine.files/*");
    const GrantAuthority full = host_grant_authority(
        bus, session.id, LiveAuthority{}.allow_to_role("ControlRequested", 1, "zengine.workshop",
                                                       files));
    const GrantAuthority reader = host_describe_only_authority(bus, session.id);
    CHECK(reader.valid());
    CHECK(reader.describe_only());
    CHECK_FALSE(full.describe_only());
    CHECK(reader.ceiling().empty());

    GrantChange installed{}, revoked{}, widened{};
    AuthorityView seen{};
    admin.weave->on_handle = [&](const Message&, Bus& b, ProbeWeave&) {
        installed = b.delegate_authority(
            full, LiveAuthority{}.allow_to_role("ControlRequested", 1, "zengine.workshop", files));
        revoked = b.delegate_authority(reader, LiveAuthority::nothing());
        widened = b.delegate_authority(reader, LiveAuthority{}.allow_any());
        seen = b.describe_authority(reader);
    };
    bus.send(admin.id, Message(ask_admin(1)));
    bus.drain_until_idle();

    REQUIRE(installed.outcome == GrantOutcome::Installed);
    // Its revoke is refused, and what was delegated stands.
    CHECK(revoked.outcome == GrantOutcome::DescribeOnly);
    CHECK_FALSE(static_cast<bool>(revoked));
    CHECK(widened.outcome == GrantOutcome::DescribeOnly);
    // It reads what the bus will apply, the door included.
    REQUIRE(seen.available);
    CHECK(seen.subject == session.id);
    CHECK(seen.permits_role("Work", 1, "svc"));
    CHECK(seen.permits_role("ControlRequested", 1, "zengine.workshop",
                            *Door::parse("zengine.files/files:files.open")));
    REQUIRE(seen.delegated.rules().size() == 1);
    CHECK(seen.delegated.rules().front().door->text() == "zengine.files/*");
}

} // TEST_SUITE
