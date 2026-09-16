// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// WHO DECIDES WHAT A LOADED ARTIFACT MAY DO (zen/kernel/admission.hpp).
//
// Until this suite existed the answer was "the Kernel, always the same way": a
// three-argument `load` minted `Grant{}.allow_any()` through three separate doors —
// the direct load, the message-driven control door, and `load_candidate`. Every case
// here pins one part of the replacement, and each is written so that removing the
// behaviour makes it RED rather than merely less thorough:
//
//   - a Kernel nobody configured admits NOTHING, and says so;
//   - the policy is asked twice, and the first ask happens before the file is opened;
//   - the verdict's grant IS the baseline, exactly, and nothing wider;
//   - a refusal leaves no artifact, no instance and no open library;
//   - all three doors ask, so closing one does not leave another open;
//   - a host naming the grant at the call site bypasses the policy, deliberately;
//   - a reload asks about the NEW bytes, and cannot re-grant (GATE-05).

#include <doctest.h>

#include "switchboard_fixtures.hpp"

#include <zen/content_id.hpp>
#include <zen/kernel/admission.hpp>
#include <zen/kernel/control.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/kernel/manager.hpp>
#include <zen/switchboard.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace loom;
using sbfx::register_probe;
using sbfx::Registered;

namespace {

/// Every question the policy was asked, in order. A recorder rather than a predicate,
/// because most of these cases are about WHICH questions were asked and when — a policy
/// that answered correctly but was never consulted at `open` would pass a predicate.
struct Recorder {
    std::vector<AdmissionRequest> seen;
    /// What to answer. Indexed by stage so a case can admit the open and refuse the
    /// speak, which is the shape of "it ran and then was not allowed to say anything".
    bool admit_open = true;
    bool admit_speak = true;
    Grant baseline{};
    std::string why = "the test said no";

    /// How many times to ask for the build's identity during each question — the policy's
    /// own choice, which is the whole point: 0 is a policy that decides without the bytes.
    int build_asks = 0;
    /// What each ask answered, in order, and how many libraries had been opened by then.
    std::vector<std::string> builds;
    std::vector<std::string> failures;
    std::vector<std::uint64_t> opened_when_asked;

    AdmissionPolicy policy() {
        return [this](const AdmissionRequest& req) {
            seen.push_back(req);
            for (int i = 0; i < build_asks; ++i) {
                builds.push_back(req.build.content_id());
                failures.push_back(req.build.failure());
                opened_when_asked.push_back(kernel_lifetime_counts().libraries_opened);
            }
            const bool yes = (req.stage == AdmissionStage::Open) ? admit_open : admit_speak;
            if (!yes) {
                return AdmissionVerdict::refuse(why);
            }
            return AdmissionVerdict::admit(req.stage == AdmissionStage::Speak ? baseline : Grant{});
        };
    }

    std::size_t asked(AdmissionStage s) const {
        std::size_t n = 0;
        for (const AdmissionRequest& r : seen) {
            if (r.stage == s) {
                ++n;
            }
        }
        return n;
    }
};

/// Copy a real artifact to a scratch path, so a case can change an artifact's BYTES
/// without changing the build. Returns the path, or empty on failure.
std::string copy_artifact(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) {
        return {};
    }
    out << in.rdbuf();
    return out ? to : std::string{};
}

/// DOES THE LOADED WEAVE'S BASELINE LET IT ANSWER `who`?
///
/// Asked by making it actually try: `who` sends it a Ping naming itself as the reply
/// address, and the fixture answers with a Pong. Whether that Pong arrives is the bus's
/// own verdict at delivery — which is the only verdict that means anything. Reading a
/// rendered authority would prove what the record SAYS; this proves what happens.
bool can_answer(Switchboard& bus, WeaveId loaded, Registered& who) {
    const std::size_t before = who.weave->handled_names.size();
    Value ping(sbfx::ping_schema());
    ping.set("seq", Cell::integer(1));
    bus.send_as(who.id, loaded, Message(std::move(ping), WeaveId{}, who.id, 0));
    bus.drain_until_idle();
    for (std::size_t i = before; i < who.weave->handled_names.size(); ++i) {
        if (who.weave->handled_names[i] == "Pong") {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_SUITE("admission") {

TEST_CASE("a Kernel nobody configured admits nothing, and its refusal names the gap") {
    Switchboard bus;
    Kernel kernel(bus); // deliberately NOT the fixture posture

    const LoadResult lr = kernel.load("t", ZEN_SO_WEAVE);

    CHECK_FALSE(lr.ok);
    CHECK_FALSE(kernel.is_loaded("t"));
    // The sentence has to send a host somewhere, so it names the call it is missing
    // rather than blaming the artifact.
    CHECK(lr.error.find("no admission policy") != std::string::npos);
    CHECK(lr.error.find("admit_with") != std::string::npos);
    // And it is a REFUSAL, not a silent weakening: nothing was opened or constructed.
    const KernelLifetimeCounts c = kernel_lifetime_counts();
    CHECK(c.libraries_opened == c.libraries_closed);
    CHECK(c.instances_created == c.instances_destroyed);
}

TEST_CASE("clearing the policy goes back to refusing, never to permitting") {
    Switchboard bus;
    Kernel kernel(bus, sbfx::fixture_admission());
    REQUIRE(kernel.load("t", ZEN_SO_WEAVE).ok);

    kernel.admit_with({}); // an empty std::function
    const LoadResult after = kernel.load("u", ZEN_SO_WEAVE_B);

    CHECK_FALSE(after.ok);
    CHECK(after.error.find("no admission policy") != std::string::npos);
}

TEST_CASE("the policy is asked twice, and the first ask is before the file is opened") {
    Switchboard bus;
    Recorder rec;
    rec.admit_open = false;
    rec.build_asks = 1;
    Kernel kernel(bus, rec.policy());

    const std::uint64_t opened_before = kernel_lifetime_counts().libraries_opened;
    const LoadResult lr = kernel.load("t", ZEN_SO_WEAVE, "some.office");

    CHECK_FALSE(lr.ok);
    // EXACTLY ONE question. That is the load-bearing assertion: a `speak` question
    // could only exist if the library had been opened and its manifest read, so its
    // ABSENCE is how this case knows no code from the file ran.
    CHECK(rec.seen.size() == 1);
    REQUIRE(rec.asked(AdmissionStage::Open) == 1);
    CHECK(rec.asked(AdmissionStage::Speak) == 0);

    const AdmissionRequest& q = rec.seen.front();
    CHECK(q.kind == AdmissionKind::Load);
    CHECK(q.name == "t");
    CHECK(q.path == ZEN_SO_WEAVE);
    CHECK(q.role == "some.office");
    // The identity is the file's bytes, and it is the SAME identity the isolation
    // ledger keys by — one answer to "is this the build I approved", not two. It was
    // in the policy's hands at `open`, before any library had been opened.
    REQUIRE(rec.builds.size() == 1);
    CHECK(rec.builds[0] == loom::file_content_id(ZEN_SO_WEAVE));
    CHECK(rec.builds[0].size() == 32);
    CHECK(rec.opened_when_asked[0] == opened_before);
    // Nothing is declared yet, because no manifest exists yet.
    CHECK_FALSE(q.declared_present);

    CHECK(lr.error.find("admission refused at open") != std::string::npos);
    CHECK(lr.error.find(rec.why) != std::string::npos);
    const KernelLifetimeCounts c = kernel_lifetime_counts();
    CHECK(c.libraries_opened == c.libraries_closed);
    CHECK(c.instances_created == c.instances_destroyed);
}

TEST_CASE("the second ask carries the artifact's declaration, and the declaration grants nothing") {
    Switchboard bus;
    Recorder rec;
    Kernel kernel(bus, rec.policy());

    REQUIRE(kernel.load("asker", ZEN_SO_ASKS).ok);

    REQUIRE(rec.asked(AdmissionStage::Open) == 1);
    REQUIRE(rec.asked(AdmissionStage::Speak) == 1);
    const AdmissionRequest& speak = rec.seen.back();
    REQUIRE(speak.stage == AdmissionStage::Speak);
    // The ask reached the policy...
    REQUIRE(speak.declared_present);
    CHECK(speak.declared.network);
    CHECK(speak.declared.filesystem == "write-scoped");
    REQUIRE(speak.declared.roles.size() == 1);
    CHECK(speak.declared.roles.front() == "storage");
    // ...and the policy answered with the empty baseline it was going to answer with
    // anyway. A declaration is advice; what the artifact holds is what the HOST said.
    // `Recorder` never reads `declared`, so this is the whole of "it granted nothing":
    // the weave that asked for the network, a writable filesystem and a broker role
    // cannot so much as answer a Pong.
    Registered peer = register_probe(bus, {sbfx::pong_schema()});
    CHECK_FALSE(can_answer(bus, kernel.weave_id("asker"), peer));
}

TEST_CASE("the verdict's grant becomes the baseline, exactly") {
    Switchboard bus;
    Registered allowed = register_probe(bus, {sbfx::pong_schema()});
    Registered forbidden = register_probe(bus, {sbfx::pong_schema()});

    Recorder rec;
    rec.baseline = Grant{}.allow("Pong", 1, allowed.id);
    Kernel kernel(bus, rec.policy());
    REQUIRE(kernel.load("t", ZEN_SO_WEAVE).ok);
    const WeaveId t = kernel.weave_id("t");

    CHECK(can_answer(bus, t, allowed));
    // Narrow means narrow in BOTH directions: the same shape to another target is not
    // permitted, which is what distinguishes a real grant from a wildcard wearing one.
    // Before this phase the second line was false for every loaded artifact in Zen.
    CHECK_FALSE(can_answer(bus, t, forbidden));
}

TEST_CASE("a refusal at the second stage leaves no artifact, no instance and no open library") {
    Switchboard bus;
    Recorder rec;
    rec.admit_speak = false;
    Kernel kernel(bus, rec.policy());

    const KernelLifetimeCounts before = kernel_lifetime_counts();
    const LoadResult lr = kernel.load("t", ZEN_SO_WEAVE);
    const KernelLifetimeCounts after = kernel_lifetime_counts();

    CHECK_FALSE(lr.ok);
    CHECK_FALSE(kernel.is_loaded("t"));
    CHECK(lr.error.find("admission refused at speak") != std::string::npos);
    // The library really was opened this time — that is the difference from an `open`
    // refusal, and it is why the balance below is the assertion that matters.
    CHECK(after.libraries_opened > before.libraries_opened);
    CHECK(after.libraries_opened - before.libraries_opened ==
          after.libraries_closed - before.libraries_closed);
    CHECK(after.instances_created - before.instances_created ==
          after.instances_destroyed - before.instances_destroyed);
}

TEST_CASE("naming the grant at the call site bypasses the policy, and the policy is not asked") {
    Switchboard bus;
    Recorder rec;
    rec.admit_open = false; // a policy that refuses everything
    Kernel kernel(bus, rec.policy());

    const LoadResult lr = kernel.load("t", ZEN_SO_WEAVE, "", Grant{}.allow_any());

    CHECK(lr.ok);
    CHECK(kernel.is_loaded("t"));
    // NOT ASKED AT ALL. The bypass is the host having already decided at a call site a
    // reviewer can read; asking and ignoring the answer would be a different, worse
    // thing.
    CHECK(rec.seen.empty());
}

TEST_CASE("the prepared-candidate door asks too, and says which door it is") {
    Switchboard bus;
    Registered coordinator = register_probe(bus, {sbfx::pong_schema()});
    Recorder rec;
    Kernel kernel(bus, rec.policy());

    REQUIRE(kernel.load_candidate("cand", ZEN_SO_WEAVE, coordinator.id).ok);

    REQUIRE(rec.seen.size() == 2);
    CHECK(rec.seen[0].kind == AdmissionKind::Candidate);
    CHECK(rec.seen[1].kind == AdmissionKind::Candidate);
    // A candidate holds no role by construction, and the question says so rather than
    // making a policy guess.
    CHECK(rec.seen[0].role.empty());

    SUBCASE("and a refusing policy stops a candidate before the world is touched") {
        Recorder strict;
        strict.admit_open = false;
        Kernel k2(bus, strict.policy());
        const LoadResult lr = k2.load_candidate("cand2", ZEN_SO_WEAVE, coordinator.id);
        CHECK_FALSE(lr.ok);
        CHECK_FALSE(k2.is_loaded("cand2"));
        CHECK(strict.asked(AdmissionStage::Speak) == 0);
    }
}

TEST_CASE("the candidate door also takes an explicit grant, and then does not ask") {
    Switchboard bus;
    Registered coordinator = register_probe(bus, {sbfx::pong_schema()});
    Recorder rec;
    rec.admit_open = false;
    Kernel kernel(bus, rec.policy());

    CHECK(kernel.load_candidate("cand", ZEN_SO_WEAVE, coordinator.id, Grant{}.allow_any()).ok);
    CHECK(rec.seen.empty());
}

TEST_CASE("a reload asks about the NEW bytes, and a refusal leaves the incumbent untouched") {
    Switchboard bus;
    Recorder rec;
    rec.build_asks = 1;
    Kernel kernel(bus, rec.policy());
    REQUIRE(kernel.load("t", ZEN_SO_WEAVE).ok);
    const WeaveId before = kernel.weave_id("t");
    rec.seen.clear();

    // A byte-identical copy at a different path: the reload contract is satisfied, so
    // the only thing that can refuse it is the policy.
    const std::string copy = std::string(ZEN_SO_WEAVE) + ".admission-copy.so";
    REQUIRE_FALSE(copy_artifact(ZEN_SO_WEAVE, copy).empty());

    rec.admit_open = false;
    rec.builds.clear();
    const ReloadResult r = kernel.reload_from("t", copy);

    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.reloaded);
    CHECK(r.error.find("admission refused at open") != std::string::npos);
    REQUIRE(rec.seen.size() == 1);
    CHECK(rec.seen[0].kind == AdmissionKind::Reload);
    CHECK(rec.seen[0].path == copy);
    REQUIRE(rec.builds.size() == 1);
    CHECK(rec.builds[0] == loom::file_content_id(copy));
    // The incumbent kept its life and its id.
    CHECK(kernel.is_loaded("t"));
    CHECK(kernel.weave_id("t") == before);
    CHECK(kernel.status("t") == ArtifactStatus::Live);

    std::remove(copy.c_str());
}

TEST_CASE("a reload the policy allows cannot re-grant: the baseline is admission-time (GATE-05)") {
    Switchboard bus;
    Registered target = register_probe(bus, {sbfx::pong_schema()});
    Recorder rec;
    Kernel kernel(bus, rec.policy()); // admits with an EMPTY baseline
    REQUIRE(kernel.load("t", ZEN_SO_WEAVE).ok);
    const GrantAuthority cap =
        host_grant_authority(bus, kernel.weave_id("t"), LiveAuthority{}.allow_any());
    REQUIRE_FALSE(bus.describe_authority(cap).permits("Pong", 1, target.id));

    const std::string copy = std::string(ZEN_SO_WEAVE) + ".admission-regrant.so";
    REQUIRE_FALSE(copy_artifact(ZEN_SO_WEAVE, copy).empty());

    // The policy now tries to hand out a wide grant on the reload...
    rec.baseline = Grant{}.allow_any();
    const ReloadResult r = kernel.reload_from("t", copy);
    REQUIRE(r.reloaded);

    // ...and it changed nothing. A reload keeps the WeaveId, and a baseline belongs to
    // the admission that minted it. The only way to give reloaded code more authority
    // is the delegated half, which a person can also take back.
    CHECK_FALSE(bus.describe_authority(cap).permits("Pong", 1, target.id));

    std::remove(copy.c_str());
}

TEST_CASE("the message-driven door spends the policy, and the asker hears the policy's own words") {
    Switchboard bus;
    Recorder rec;
    rec.admit_open = false;
    rec.why = "this host does not run artifacts it was not told about";
    Kernel kernel(bus, rec.policy());
    const WeaveId control = mount_control(kernel, bus);
    const WeaveId manager = mount_manager(control, bus);

    // An ordinary participant holding nothing but the right to drive the steward.
    Registered asker =
        register_probe(bus, {schema_of<Refused>()}, 2, true,
                       Grant{}.allow(LoadWeave::zen_name, LoadWeave::zen_version, manager));
    std::vector<std::string> reasons;
    asker.weave->on_handle = [&reasons](const Message& in, Bus&, sbfx::ProbeWeave&) {
        if (const Cell* why = in.payload.get("reason")) {
            reasons.push_back(why->as_text());
        }
    };
    Value ask(schema_of<LoadWeave>());
    ask.set("name", Cell::text("t"));
    ask.set("path", Cell::text(ZEN_SO_WEAVE));
    ask.set("role", Cell::text(""));
    bus.send_as(asker.id, manager, Message(std::move(ask), WeaveId{}, asker.id, 1));
    bus.drain_until_idle();

    CHECK_FALSE(kernel.is_loaded("t"));
    REQUIRE(rec.asked(AdmissionStage::Open) == 1);
    CHECK(rec.seen[0].kind == AdmissionKind::Load);

    // THE ANSWER REACHED THE ASKER, carrying the reason the policy wrote — which is
    // what lets whoever asked connect the outcome to the request they made.
    bool heard = false;
    for (const std::string& why : reasons) {
        if (why.find(rec.why) != std::string::npos) {
            heard = true;
        }
    }
    CHECK(heard);
    CHECK(reasons.size() == 1); // one ask, one answer — not a broadcast
}

TEST_CASE("two different files have two different identities, and the same file one identity") {
    // The pin a remembered approval hangs on. If this were ever a path or an mtime, a
    // rebuild would be invisible and a move would look like a new artifact.
    const std::string a = loom::file_content_id(ZEN_SO_WEAVE);
    const std::string b = loom::file_content_id(ZEN_SO_WEAVE_B);
    CHECK(a != b);
    CHECK(a == loom::file_content_id(ZEN_SO_WEAVE));

    const std::string copy = std::string(ZEN_SO_WEAVE) + ".admission-identity.so";
    REQUIRE_FALSE(copy_artifact(ZEN_SO_WEAVE, copy).empty());
    CHECK(loom::file_content_id(copy) == a); // same bytes at another path: same artifact
    std::remove(copy.c_str());
}

TEST_CASE("an unreadable file still reaches the policy, named as unidentifiable") {
    Switchboard bus;
    Recorder rec;
    rec.build_asks = 2;
    Kernel kernel(bus, rec.policy());

    const std::uint64_t scans = file_content_id_scans();
    const LoadResult lr = kernel.load("gone", "/nonexistent/not-a-weave.so");

    CHECK_FALSE(lr.ok);
    // Asked, not skipped: a policy is entitled to refuse a thing it cannot identify, and
    // the identity says that it cannot — never an id, and never silence.
    REQUIRE(rec.asked(AdmissionStage::Open) == 1);
    REQUIRE(rec.builds.size() == 2);
    CHECK(rec.builds[0].empty());
    CHECK_FALSE(rec.seen[0].build.identified());
    CHECK(rec.failures[0].find("/nonexistent/not-a-weave.so") != std::string::npos);
    // A FAILED READING IS STILL ONE READING: the second ask did not try the file again.
    CHECK(rec.failures[1] == rec.failures[0]);
    CHECK(file_content_id_scans() - scans == 1);
}

// ---- the cost of identity: paid by the policy that asks, once per operation ---------
//
// The Kernel used to hash the whole image before asking any policy — including one that never
// read the answer — at every door, and Zengine's replacement-timing tests went red under
// `trust_every_artifact` for that work alone. These cases count the work itself
// (`file_content_id_scans()`, the one place a file is hashed) rather than a stopwatch, so they
// say whether identity work HAPPENED, on any machine.

namespace {

/// Copy `from` to `to` and give the copy `from`'s modification time.
bool copy_with_time(const std::string& from, const std::string& to) {
    if (copy_artifact(from, to).empty()) {
        return false;
    }
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(from, ec);
    if (ec) {
        return false;
    }
    std::filesystem::last_write_time(to, t, ec);
    return !ec;
}

} // namespace

TEST_CASE("a policy that never asks which build it is makes no door read the file") {
    Switchboard bus;
    Kernel kernel(bus, trust_every_artifact("this case counts identity work, and wants none"));
    Registered coordinator = register_probe(bus, {sbfx::pong_schema()});
    const WeaveId control = mount_control(kernel, bus);
    const WeaveId manager = mount_manager(control, bus);
    const std::string copy = std::string(ZEN_SO_WEAVE) + ".admission-noscan.so";
    REQUIRE_FALSE(copy_artifact(ZEN_SO_WEAVE, copy).empty());

    const std::uint64_t scans = file_content_id_scans();

    // The direct door, the candidate door and a reload — every door a policy is asked at...
    REQUIRE(kernel.load("t", ZEN_SO_WEAVE).ok);
    REQUIRE(kernel.load_candidate("cand", ZEN_SO_WEAVE_B, coordinator.id).ok);
    REQUIRE(kernel.reload_from("t", copy).reloaded);
    // ...and the message-driven door, which spends the same load.
    Registered asker =
        register_probe(bus, {schema_of<Refused>()}, 2, true,
                       Grant{}.allow(LoadWeave::zen_name, LoadWeave::zen_version, manager));
    Value ask(schema_of<LoadWeave>());
    ask.set("name", Cell::text("by-message"));
    ask.set("path", Cell::text(ZEN_SO_WEAVE_B));
    ask.set("role", Cell::text(""));
    bus.send_as(asker.id, manager, Message(std::move(ask), WeaveId{}, asker.id, 1));
    bus.drain_until_idle();
    REQUIRE(kernel.is_loaded("by-message"));
    // A Kernel nobody configured refuses without reading anything either.
    Switchboard other_bus;
    Kernel unconfigured(other_bus);
    CHECK_FALSE(unconfigured.load("u", ZEN_SO_WEAVE).ok);

    CHECK(file_content_id_scans() - scans == 0);
    std::remove(copy.c_str());
}

TEST_CASE("a policy that asks gets the bytes before the file is opened, read once per operation") {
    Switchboard bus;
    Recorder rec;
    rec.build_asks = 2; // twice at each stage: four asks, one reading
    Kernel kernel(bus, rec.policy());

    const std::uint64_t opened_before = kernel_lifetime_counts().libraries_opened;
    const std::uint64_t scans = file_content_id_scans();
    REQUIRE(kernel.load("t", ZEN_SO_WEAVE).ok);
    CHECK(file_content_id_scans() - scans == 1);

    REQUIRE(rec.asked(AdmissionStage::Open) == 1);
    REQUIRE(rec.asked(AdmissionStage::Speak) == 1);
    REQUIRE(rec.builds.size() == 4);
    // Every answer describes the one reading — at both stages, and through the copy of the
    // request the recorder kept.
    const std::string expected = loom::file_content_id(ZEN_SO_WEAVE);
    for (const std::string& b : rec.builds) {
        CHECK(b == expected);
    }
    CHECK(rec.seen[0].build.content_id() == expected);
    CHECK(file_content_id_scans() - scans == 2); // the reference above; the kept copy read nothing
    // The `open` answers were in hand before any library had been opened.
    CHECK(rec.opened_when_asked[0] == opened_before);
    CHECK(rec.opened_when_asked[1] == opened_before);
}

TEST_CASE("the candidate door and a reload each read their own bytes, once") {
    Switchboard bus;
    Registered coordinator = register_probe(bus, {sbfx::pong_schema()});
    Recorder rec;
    rec.build_asks = 1;
    Kernel kernel(bus, rec.policy());
    REQUIRE(kernel.load("t", ZEN_SO_WEAVE).ok);
    const std::string copy = std::string(ZEN_SO_WEAVE) + ".admission-once.so";
    REQUIRE_FALSE(copy_artifact(ZEN_SO_WEAVE, copy).empty());

    rec.builds.clear();
    std::uint64_t scans = file_content_id_scans();
    REQUIRE(kernel.load_candidate("cand", ZEN_SO_WEAVE_B, coordinator.id).ok);
    CHECK(file_content_id_scans() - scans == 1);
    REQUIRE(rec.builds.size() == 2);
    CHECK(rec.builds[0] == rec.builds[1]);

    rec.builds.clear();
    scans = file_content_id_scans();
    REQUIRE(kernel.reload_from("t", copy).reloaded);
    CHECK(file_content_id_scans() - scans == 1);
    REQUIRE(rec.builds.size() == 2);
    CHECK(rec.builds[0] == rec.builds[1]);
    CHECK(rec.builds[0] == loom::file_content_id(copy));
    CHECK(rec.builds[0] != loom::file_content_id(ZEN_SO_WEAVE_B)); // not the candidate's reading

    std::remove(copy.c_str());
}

TEST_CASE("each attempt reads the bytes again: a changed build of the same size and time is new") {
    // THE PIN'S WHOLE PREMISE. A cache keyed by path, size or modification time — or an
    // earlier attempt's reading reused — would call this the same build. The policy refuses
    // at `open`, so the altered file is never opened and may be any bytes at all.
    Switchboard bus;
    Recorder rec;
    rec.build_asks = 1;
    rec.admit_open = false;
    Kernel kernel(bus, rec.policy());
    const std::string copy = std::string(ZEN_SO_WEAVE) + ".admission-retry.so";
    REQUIRE(copy_with_time(ZEN_SO_WEAVE, copy));
    const auto size_before = std::filesystem::file_size(copy);
    const auto time_before = std::filesystem::last_write_time(copy);

    std::uint64_t scans = file_content_id_scans();
    CHECK_FALSE(kernel.load("t", copy).ok);
    CHECK(file_content_id_scans() - scans == 1);

    {
        std::fstream f(copy, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(f);
        const auto middle = static_cast<std::streamoff>(size_before / 2);
        f.seekg(middle);
        const int c = f.get();
        f.seekp(middle);
        f.put(static_cast<char>(c ^ 0x5a));
    }
    std::filesystem::last_write_time(copy, time_before);
    REQUIRE(std::filesystem::file_size(copy) == size_before);
    REQUIRE(std::filesystem::last_write_time(copy) == time_before);

    scans = file_content_id_scans();
    CHECK_FALSE(kernel.load("t", copy).ok);
    CHECK(file_content_id_scans() - scans == 1);

    REQUIRE(rec.builds.size() == 2);
    CHECK(rec.builds[0] != rec.builds[1]);
    CHECK(rec.builds[1] == loom::file_content_id(copy));
    std::remove(copy.c_str());
}

TEST_CASE("a build identity nobody asks for is never read, and copies share one reading") {
    const std::string copy = std::string(ZEN_SO_WEAVE) + ".admission-identity-object.so";
    REQUIRE_FALSE(copy_artifact(ZEN_SO_WEAVE, copy).empty());
    const std::uint64_t scans = file_content_id_scans();

    const BuildIdentity original(copy);
    const BuildIdentity kept = original;
    CHECK(file_content_id_scans() - scans == 0); // made and copied, never asked

    const std::string first = kept.content_id();
    CHECK(file_content_id_scans() - scans == 1);
    CHECK(original.identified());
    CHECK(original.content_id() == first); // the copy's reading, not a second one
    CHECK(original.failure().empty());
    CHECK(file_content_id_scans() - scans == 1);

    // Once read, the answer IS that reading, whatever happens to the file afterwards.
    std::remove(copy.c_str());
    CHECK(original.content_id() == first);
    CHECK(file_content_id_scans() - scans == 1);

    // Naming no file, or supplied whole: answered without touching a disk, and never confused.
    const BuildIdentity nothing;
    CHECK_FALSE(nothing.identified());
    CHECK_FALSE(nothing.failure().empty());
    const BuildIdentity known = BuildIdentity::known(first);
    CHECK(known.identified());
    CHECK(known.content_id() == first);
    CHECK_FALSE(BuildIdentity::known("").identified());
    CHECK(file_content_id_scans() - scans == 1);
}

TEST_CASE("the explicit permissive policy has to be asked for by name, and carries its reason") {
    Switchboard bus;
    Kernel kernel(bus, trust_every_artifact("because this suite says so"));

    const LoadResult lr = kernel.load("t", ZEN_SO_WEAVE);
    REQUIRE(lr.ok);

    // It is the old default's authority, and nothing more: permissive SENDS, and no
    // Sense read authority, because "I compiled it" is not "it may read everything
    // anyone publishes".
    Registered anyone = register_probe(bus, {sbfx::pong_schema()});
    Registered somebody_else = register_probe(bus, {sbfx::pong_schema()});
    CHECK(can_answer(bus, lr.id, anyone));
    CHECK(can_answer(bus, lr.id, somebody_else));
}

TEST_CASE("the trusting policy still grants no observation: sends are not reads") {
    Switchboard bus;
    Kernel kernel(bus, trust_every_artifact("this suite's own fixture"));
    const LoadResult lr = kernel.load("t", ZEN_SO_WEAVE);
    REQUIRE(lr.ok);

    // The wildcard is a SEND wildcard. `Grant`'s observe floor is empty and stays
    // empty — Senses did not change it, and neither does trusting an artifact.
    Registered claimant = register_probe(bus, {sbfx::pong_schema()});
    const SenseReading r = bus.observe_as(lr.id, claimant.id, "Pong", 1);
    // NotAuthorized, not NoClaim: the two send a person to opposite places, and the
    // one this pins is "your grant does not permit reading", which is the claim.
    CHECK(r.refusal == SenseRefusal::NotAuthorized);
}

} // TEST_SUITE("admission")
