// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE SUPPLIED HOST'S DECIDING HALF — the two files a person writes by hand, and the
// admission policy over one of them (`src/host/`).
//
// It is here because everything in this suite reads TEXT A PERSON TYPED: a rule grammar
// and two schemas over hand-edited JSON. The rest of the host is a REPL, driven by hand
// and proved by using it; a grammar is not, and a grammar nothing tests is one that
// eventually accepts the wrong thing quietly.
//
// What each group pins:
//   - the rule grammar reads back exactly what `render_rule` prints, including the two
//     spellings it must REFUSE rather than guess at;
//   - a decision survives a write and a re-read, which is the whole of "remembered";
//   - the bare form a person writes and the enveloped form the host writes both read;
//   - the policy admits, pins, re-pins and refuses in the five cases it distinguishes,
//     and its baseline is the small one on purpose.

#include <doctest.h>

#include "authority.hpp"
#include "boot_plan.hpp"
#include "config_file.hpp"
#include "store_lock.hpp"

#include <zen/content_id.hpp>
#include <zen/kernel/admission.hpp>
#include <zen/switchboard/grant.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using namespace loom;
using namespace loom::host;

namespace {

/// A scratch path that cleans up after itself, so a failing case cannot poison the next.
struct Scratch {
    std::string path;
    explicit Scratch(const char* name) : path(std::string("zen-host-test-") + name + ".json") {
        std::remove(path.c_str());
    }
    ~Scratch() {
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
    }
    void write(const std::string& text) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
};

/// "May it send this shape to SOMEBODY?" asked with a concrete, non-default WeaveId on
/// purpose. `LiveAuthority::permits` compares `r.target == target`, and a ROLE rule
/// stores a default-constructed target — so asking with `WeaveId{}` would report a role
/// rule as a wildcard. The trap is in the query, not the rule, and this is where a test
/// gets to fall into it once.
bool permits_anyone(const LiveAuthority& a, const char* shape, std::uint32_t version) {
    return a.permits(shape, version, WeaveId{9999});
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

AuthorityRule rule_for(const char* name, bool may_run = true) {
    AuthorityRule r;
    r.artifact = name;
    r.may_run = may_run;
    return r;
}

/// A question about a build whose identity is already known — the store's decisions are about
/// that identity, not about how the Kernel read it. An empty `content_id` stands for a file that
/// could not be read, which is exactly what the Kernel's own identity reports for one.
AdmissionRequest open_of(const char* name, const std::string& content_id,
                         const char* path = "/somewhere/x.so") {
    AdmissionRequest q;
    q.stage = AdmissionStage::Open;
    q.kind = AdmissionKind::Load;
    q.name = name;
    q.path = path;
    q.build = content_id.empty() ? BuildIdentity(std::string("/no/such/dir/") + name + ".so")
                                 : BuildIdentity::known(content_id);
    return q;
}

} // namespace

TEST_SUITE("host_policy") {

// ---- the rule grammar -------------------------------------------------------

TEST_CASE("every rule `render_rule` can print, read back as the authority it printed") {
    // Exactly the spellings in weaver.hpp's render_rule. One grammar for what a person
    // reads and what a person writes was the point; this is where the two meet.
    LiveAuthority a;
    std::string why;

    REQUIRE_MESSAGE(apply_rule("Greet v1 -> any target", &a, &why), why);
    CHECK(permits_anyone(a, "Greet", 1));

    LiveAuthority b;
    REQUIRE_MESSAGE(apply_rule("Tick v2 -> role clock", &b, &why), why);
    CHECK(b.permits_role("Tick", 2, "clock"));
    CHECK_FALSE(b.permits_role("Tick", 2, "other"));
    CHECK_FALSE(permits_anyone(b, "Tick", 2)); // a role rule is not a wildcard

    LiveAuthority c;
    REQUIRE_MESSAGE(apply_rule("any shape -> any target", &c, &why), why);
    CHECK(permits_anyone(c, "AnythingAtAll", 7));

    LiveAuthority d;
    REQUIRE_MESSAGE(apply_rule("observe Tick v1", &d, &why), why);
    CHECK(d.permits_observe("Tick", 1));
    CHECK_FALSE(permits_anyone(d, "Tick", 1)); // reading is not sending

    LiveAuthority e;
    REQUIRE_MESSAGE(apply_rule("observe any shape", &e, &why), why);
    CHECK(e.permits_observe("Whatever", 3));
}

TEST_CASE("a rule naming a WeaveId is refused, because a WeaveId is minted per run") {
    LiveAuthority a;
    std::string why;
    CHECK_FALSE(apply_rule("Greet v1 -> weave #19", &a, &why));
    // The refusal has to teach, because the person writing it thought it would work.
    CHECK(why.find("minted fresh every run") != std::string::npos);
    CHECK(why.find("role") != std::string::npos);
}

TEST_CASE("a rule the bus has no word for is refused rather than approximated") {
    LiveAuthority a;
    std::string why;
    // LiveAuthority has no any-shape-to-a-role rule. Inventing one here would be
    // inventing authority the bus cannot express, so it says so instead.
    CHECK_FALSE(apply_rule("any shape -> role clock", &a, &why));
    CHECK(why.find("cannot be expressed") != std::string::npos);
}

TEST_CASE("every way of mistyping a rule is a named refusal, never a silent no-op") {
    LiveAuthority a;
    std::string why;
    const char* bad[] = {
        "",                       // empty
        "   ",                    // blank
        "Greet -> any target",    // no version
        "Greet vX -> any target", // unreadable version
        "Greet v1",               // no destination
        "Greet v1 -> somewhere",  // unreadable destination
        "Greet v1 -> role ",      // no office
        "observe Greet",          // no version on an observe
    };
    for (const char* text : bad) {
        why.clear();
        CAPTURE(text);
        CHECK_FALSE(apply_rule(text, &a, &why));
        CHECK_FALSE(why.empty()); // a refusal with no reason is not a refusal
    }
}

TEST_CASE("an observe list entry reads with or without its verb") {
    AuthorityRule r = rule_for("x");
    r.observe = {"Tick v1", "observe Tock v2"};
    LiveAuthority a;
    std::string why;
    REQUIRE_MESSAGE(to_live_authority(r, &a, &why), why);
    CHECK(a.permits_observe("Tick", 1));
    CHECK(a.permits_observe("Tock", 2));
}

TEST_CASE("one bad rule stops the whole decision, naming the artifact and the rule") {
    AuthorityRule r = rule_for("counter");
    r.send = {"Greet v1 -> any target", "Greet v1 -> weave #19"};
    LiveAuthority a;
    std::string why;
    // NOT a partial install. A person who believes they granted two things and got one
    // has a system that disagrees with their file, silently.
    CHECK_FALSE(to_live_authority(r, &a, &why));
    CHECK(why.find("counter") != std::string::npos);
    CHECK(why.find("weave #19") != std::string::npos);
}

// ---- the store, and remembering ---------------------------------------------

TEST_CASE("a missing decision file is an empty store, not an error") {
    Scratch f("missing");
    AuthorityStore s;
    std::string why;
    // Where everyone starts. A host that refused to run without this file is a host a
    // person cannot explore.
    CHECK(s.open(f.path, &why));
    CHECK(why.empty());
    CHECK(s.rules().empty());
}

TEST_CASE("a decision survives a write and a re-read, whole") {
    Scratch f("roundtrip");
    std::string why;
    {
        AuthorityStore s;
        REQUIRE(s.open(f.path, &why));
        AuthorityRule r = rule_for("counter");
        r.content_id = "0123456789abcdef0123456789abcdef";
        r.trust_rebuilds = true;
        r.send = {"Noted v1 -> role logbook"};
        r.observe = {"observe Tick v1"};
        r.note = "approved because I wrote it";
        REQUIRE_MESSAGE(s.put(std::move(r), &why), why);
    }
    AuthorityStore reopened;
    REQUIRE_MESSAGE(reopened.open(f.path, &why), why);
    const AuthorityRule* r = reopened.find("counter");
    REQUIRE(r != nullptr);
    CHECK(r->may_run);
    CHECK(r->trust_rebuilds);
    CHECK(r->content_id == "0123456789abcdef0123456789abcdef");
    REQUIRE(r->send.size() == 1);
    CHECK(r->send[0] == "Noted v1 -> role logbook");
    REQUIRE(r->observe.size() == 1);
    CHECK(r->observe[0] == "observe Tick v1");
    CHECK(r->note == "approved because I wrote it");
}

TEST_CASE("forgetting a decision removes it from the file, not just from memory") {
    Scratch f("forget");
    std::string why;
    AuthorityStore s;
    REQUIRE(s.open(f.path, &why));
    REQUIRE(s.put(rule_for("a"), &why));
    REQUIRE(s.put(rule_for("b"), &why));
    REQUIRE(s.forget("a", &why));
    CHECK_FALSE(s.forget("a", &why)); // and says so the second time
    CHECK(why.find("no standing decision") != std::string::npos);

    AuthorityStore reopened;
    REQUIRE(reopened.open(f.path, &why));
    CHECK(reopened.find("a") == nullptr);
    CHECK(reopened.find("b") != nullptr);
}

TEST_CASE("a decision the host could not carry out is never written") {
    Scratch f("unwritable");
    std::string why;
    AuthorityStore s;
    REQUIRE(s.open(f.path, &why));
    AuthorityRule r = rule_for("counter");
    r.send = {"Greet v1 -> weave #19"};
    CHECK_FALSE(s.put(std::move(r), &why));
    CHECK(s.find("counter") == nullptr);
    CHECK(s.rules().empty());
}

// ---- the file a person actually types ---------------------------------------

TEST_CASE("the BARE form a person writes is admitted, without the envelope") {
    Scratch f("bare");
    // The whole point of host/config_file.hpp. The envelope version, the schema's wire
    // name and its version are three facts a newcomer cannot guess, so the host supplies
    // them and admits the person's object inside.
    f.write(R"({"boot":[{"name":"logbook","path":"/x/liblogbook.so","role":"logbook"}]})");
    BootPlan plan;
    std::string why;
    REQUIRE_MESSAGE(read_boot_plan(f.path, &plan, &why), why);
    REQUIRE(plan.entries.size() == 1);
    CHECK(plan.entries[0].name == "logbook");
    CHECK(plan.entries[0].role == "logbook");
    CHECK(plan.entries[0].enabled);
    CHECK(plan.entries[0].on_failure == OnFailure::Continue);
}

TEST_CASE("the ENVELOPED form the host writes is admitted too") {
    Scratch f("enveloped");
    f.write(R"({"zen":1,"schema":"zen.HostBootPlan","version":2,)"
            R"("fields":{"boot":[{"name":"a","path":"/x/a.so"}]}})");
    BootPlan plan;
    std::string why;
    // Both forms, because the host writes one and a person writes the other, and a store
    // the host cannot re-read is a store that forgets.
    REQUIRE_MESSAGE(read_boot_plan(f.path, &plan, &why), why);
    REQUIRE(plan.entries.size() == 1);
    CHECK(plan.entries[0].name == "a");
}

TEST_CASE("the gate still refuses what it always refused") {
    std::string why;
    SUBCASE("a field the schema does not declare") {
        Scratch f("unknown-field");
        f.write(R"({"boot":[{"name":"a","path":"/x/a.so","colour":"blue"}]})");
        BootPlan plan;
        CHECK_FALSE(read_boot_plan(f.path, &plan, &why));
        CHECK(why.find(f.path) != std::string::npos); // the file is named
    }
    SUBCASE("a field of the wrong type") {
        Scratch f("wrong-type");
        f.write(R"({"boot":[{"name":"a","path":"/x/a.so","enabled":"yes"}]})");
        BootPlan plan;
        CHECK_FALSE(read_boot_plan(f.path, &plan, &why));
    }
    SUBCASE("text that is not JSON at all") {
        Scratch f("garbage");
        f.write("this is not a boot plan");
        BootPlan plan;
        CHECK_FALSE(read_boot_plan(f.path, &plan, &why));
    }
    SUBCASE("an authority file somebody hand-edited into nonsense") {
        Scratch f("bad-authority");
        f.write(R"({"rules":[{"artifact":"a","may_run":true,"send":["Greet v1 -> weave #2"],)"
                R"("observe":[]}]})");
        AuthorityStore s;
        // A RULE IS PARSED AT LOAD, not at first use: a person who hand-edits this learns
        // about the typo when the host starts, in a sentence naming the rule.
        CHECK_FALSE(s.open(f.path, &why));
        CHECK(why.find("weave #2") != std::string::npos);
    }
}

TEST_CASE("a plan's links and history read back whole, and a bare plan has none of either") {
    Scratch f("links-history");
    f.write(R"({"boot":[{"name":"probe","path":"/x/probe.so"}],)"
            R"("links":[{"name":"workshop","connect":"127.0.0.1:7654",)"
            R"("identity":"agent","credential":"open-sesame"}],)"
            R"("history":{"log":"run.log","recent":"64","keep_refusals":true,)"
            R"("retain":[{"shape":"SurfaceCaptured","last_n":"4","in_recent":false,)"
            R"("retain_payload":true}],)"
            R"("keep":[{"shape":"InputInjected"},{"shape":"SurfaceCaptureChunk","cap":"2"}]}})");
    BootPlan plan;
    std::string why;
    REQUIRE_MESSAGE(read_boot_plan(f.path, &plan, &why), why);
    REQUIRE(plan.links.size() == 1);
    CHECK(plan.links[0].name == "workshop");
    CHECK(plan.links[0].connect == "127.0.0.1:7654");
    CHECK(plan.links[0].identity == "agent");
    CHECK(plan.links[0].credential == "open-sesame");
    CHECK(plan.history.log == "run.log");
    CHECK(plan.history.recent == 64);
    CHECK(plan.history.keep_refusals);
    REQUIRE(plan.history.retain.size() == 1);
    CHECK(plan.history.retain[0].shape == "SurfaceCaptured");
    CHECK(plan.history.retain[0].last_n == 4);
    CHECK_FALSE(plan.history.retain[0].in_recent);
    CHECK(plan.history.retain[0].retain_payload);
    REQUIRE(plan.history.keep.size() == 2);
    CHECK(plan.history.keep[0].cap == 0);
    CHECK(plan.history.keep[1].shape == "SurfaceCaptureChunk");
    CHECK(plan.history.keep[1].cap == 2);

    Scratch bare("bare-no-links");
    bare.write(R"({"boot":[]})");
    BootPlan none;
    REQUIRE_MESSAGE(read_boot_plan(bare.path, &none, &why), why);
    CHECK(none.links.empty());
    CHECK(none.history.log.empty());
    CHECK(none.history.retain.empty());
    CHECK(none.history.keep.empty());
}

TEST_CASE("a link without a destination, or two links with one name, is refused by name") {
    std::string why;
    BootPlan plan;
    SUBCASE("no connect") {
        Scratch f("link-no-connect");
        f.write(R"({"boot":[],"links":[{"name":"workshop","connect":""}]})");
        CHECK_FALSE(read_boot_plan(f.path, &plan, &why));
        CHECK(why.find("link") != std::string::npos);
    }
    SUBCASE("two names") {
        Scratch f("link-twice");
        f.write(R"({"boot":[],"links":[{"name":"w","connect":"a:1"},{"name":"w","connect":"b:2"}]})");
        CHECK_FALSE(read_boot_plan(f.path, &plan, &why));
        CHECK(why.find("'w'") != std::string::npos);
    }
    SUBCASE("a negative window") {
        Scratch f("history-negative");
        f.write(R"({"boot":[],"history":{"recent":"-1"}})");
        CHECK_FALSE(read_boot_plan(f.path, &plan, &why));
        CHECK(why.find("negative") != std::string::npos);
    }
}

TEST_CASE("a boot plan's own vocabulary is checked, and a typo is refused not shrugged at") {
    std::string why;
    SUBCASE("an unreadable on_failure names both accepted values") {
        Scratch f("bad-onfailure");
        f.write(R"({"boot":[{"name":"a","path":"/x/a.so","on_failure":"halt"}]})");
        BootPlan plan;
        CHECK_FALSE(read_boot_plan(f.path, &plan, &why));
        CHECK(why.find("continue") != std::string::npos);
        CHECK(why.find("stop") != std::string::npos);
    }
    SUBCASE("an empty name or path is refused") {
        Scratch f("empty-name");
        f.write(R"({"boot":[{"name":"","path":"/x/a.so"}]})");
        BootPlan plan;
        CHECK_FALSE(read_boot_plan(f.path, &plan, &why));
    }
    SUBCASE("stop is read, and is not the default") {
        Scratch f("stop");
        f.write(R"({"boot":[{"name":"a","path":"/x/a.so","on_failure":"stop"},)"
                R"({"name":"b","path":"/x/b.so"}]})");
        BootPlan plan;
        REQUIRE_MESSAGE(read_boot_plan(f.path, &plan, &why), why);
        REQUIRE(plan.entries.size() == 2);
        CHECK(plan.entries[0].on_failure == OnFailure::Stop);
        CHECK(plan.entries[1].on_failure == OnFailure::Continue);
        // AUTHORED ORDER, preserved. The plan is a list because the order is the
        // person's to state; a reader that sorted it would be deciding for them.
        CHECK(plan.entries[0].name == "a");
        CHECK(plan.entries[1].name == "b");
    }
}

// ---- the admission policy ----------------------------------------------------

TEST_CASE("with no standing decision, nothing runs — and the refusal says what to type") {
    Scratch f("no-decision");
    AuthorityStore s;
    std::string why;
    REQUIRE(s.open(f.path, &why));

    const AdmissionVerdict v = s.policy()(open_of("counter", "aaaa1111bbbb2222cccc3333dddd4444"));

    CHECK_FALSE(v.admitted);
    CHECK(v.reason.find("authority trust counter") != std::string::npos);
    // ...and it is recorded, so the console can show a person the facts rather than
    // making them re-read a scrolled-away line.
    REQUIRE(s.pending().size() == 1);
    CHECK(s.pending()[0].artifact == "counter");
    CHECK(s.pending()[0].content_id == "aaaa1111bbbb2222cccc3333dddd4444");
    CHECK(s.pending()[0].pinned.empty());
}

TEST_CASE("a decision that says no is a different refusal from having made none") {
    Scratch f("denied");
    AuthorityStore s;
    std::string why;
    REQUIRE(s.open(f.path, &why));
    REQUIRE(s.put(rule_for("counter", /*may_run=*/false), &why));

    const AdmissionVerdict v = s.policy()(open_of("counter", "aaaa1111bbbb2222cccc3333dddd4444"));
    CHECK_FALSE(v.admitted);
    CHECK(v.reason.find("denied by a standing decision") != std::string::npos);
}

TEST_CASE("the first load under a fresh approval PINS the build that turned up") {
    Scratch f("pin");
    AuthorityStore s;
    std::string why;
    REQUIRE(s.open(f.path, &why));
    REQUIRE(s.put(rule_for("counter"), &why));
    REQUIRE(s.find("counter")->content_id.empty());

    const std::string build = "aaaa1111bbbb2222cccc3333dddd4444";
    const AdmissionVerdict v = s.policy()(open_of("counter", build));

    CHECK(v.admitted);
    // The person approved an ARTIFACT; this records which build that was, so the next
    // different one is a question rather than a silent substitution.
    CHECK(s.find("counter")->content_id == build);
    CHECK(s.notes().size() == 1);
    CHECK(s.notes()[0].find("pinned") != std::string::npos);

    SUBCASE("and the same build afterwards is admitted without a word") {
        const AdmissionVerdict again = s.policy()(open_of("counter", build));
        CHECK(again.admitted);
        CHECK(s.notes().size() == 1); // nothing new happened, so nothing new is said
        CHECK(s.pending().empty());
    }
}

TEST_CASE("a changed build re-asks, naming both, and its speech authority is untouched") {
    Scratch f("changed");
    AuthorityStore s;
    std::string why;
    REQUIRE(s.open(f.path, &why));
    AuthorityRule r = rule_for("counter");
    r.content_id = "aaaa1111bbbb2222cccc3333dddd4444";
    r.send = {"Noted v1 -> role logbook"};
    REQUIRE(s.put(std::move(r), &why));

    const AdmissionVerdict v = s.policy()(open_of("counter", "99998888777766665555444433332222"));

    CHECK_FALSE(v.admitted);
    CHECK(v.reason.find("aaaa1111") != std::string::npos); // what was approved
    CHECK(v.reason.find("99998888") != std::string::npos); // what turned up
    CHECK(v.reason.find("--rebuilds") != std::string::npos);
    REQUIRE(s.pending().size() == 1);
    CHECK(s.pending()[0].pinned == "aaaa1111bbbb2222cccc3333dddd4444");
    // THE PIN DID NOT MOVE and neither did the rules. A refusal changes nothing.
    CHECK(s.find("counter")->content_id == "aaaa1111bbbb2222cccc3333dddd4444");
    REQUIRE(s.find("counter")->send.size() == 1);
}

TEST_CASE("trust_rebuilds admits a changed build, re-pins it, and SAYS it did") {
    Scratch f("rebuilds");
    AuthorityStore s;
    std::string why;
    REQUIRE(s.open(f.path, &why));
    AuthorityRule r = rule_for("counter");
    r.content_id = "aaaa1111bbbb2222cccc3333dddd4444";
    r.trust_rebuilds = true;
    REQUIRE(s.put(std::move(r), &why));

    const AdmissionVerdict v = s.policy()(open_of("counter", "99998888777766665555444433332222"));

    CHECK(v.admitted);
    CHECK(s.find("counter")->content_id == "99998888777766665555444433332222");
    CHECK(s.pending().empty());
    // NOT SILENT. "Nothing changed" and "you are running code you have not seen, under
    // authority you granted earlier" are different facts, and this is the note that had
    // to be read to notice it was reporting the new build as the old one.
    REQUIRE(s.notes().size() == 1);
    CHECK(s.notes()[0].find("REBUILT") != std::string::npos);
    CHECK(s.notes()[0].find("aaaa1111") != std::string::npos);
    CHECK(s.notes()[0].find("99998888") != std::string::npos);
}

TEST_CASE("an artifact the host could not identify is refused, not guessed at") {
    Scratch f("unreadable");
    AuthorityStore s;
    std::string why;
    REQUIRE(s.open(f.path, &why));
    REQUIRE(s.put(rule_for("counter"), &why));

    const AdmissionVerdict v = s.policy()(open_of("counter", "")); // the file could not be read
    CHECK_FALSE(v.admitted);
    CHECK(v.reason.find("could not be identified") != std::string::npos);
    CHECK(s.find("counter")->content_id.empty()); // and nothing was pinned to nothing
}

TEST_CASE("the store reads the build at open and only there, and names what it cannot read") {
    // THE COST IS THE STORE'S, BECAUSE THE PIN IS. The Kernel works the identity out only when
    // a policy asks (zen/kernel/admission.hpp); this policy asks before the file is opened,
    // because that is where a pin means "no other build's code ran" — and never at speak,
    // where it decides the baseline without the bytes.
    Scratch f("identity-at-open");
    AuthorityStore s;
    std::string why;
    REQUIRE(s.open(f.path, &why));
    REQUIRE(s.put(rule_for("counter"), &why));
    Scratch build("identity-at-open-build");
    build.write("the bytes of the build that turned up");

    AdmissionRequest speak;
    speak.stage = AdmissionStage::Speak;
    speak.name = "counter";
    speak.path = build.path;
    speak.build = BuildIdentity(build.path);
    const std::uint64_t scans = loom::file_content_id_scans();
    REQUIRE(s.policy()(speak).admitted);
    CHECK(loom::file_content_id_scans() - scans == 0);

    AdmissionRequest open = speak;
    open.stage = AdmissionStage::Open;
    open.build = BuildIdentity(build.path);
    REQUIRE(s.policy()(open).admitted);
    CHECK(loom::file_content_id_scans() - scans == 1);
    CHECK(s.find("counter")->content_id == loom::file_content_id(build.path));

    AdmissionRequest gone = open;
    gone.path = "/no/such/dir/counter.so";
    gone.build = BuildIdentity(gone.path);
    const AdmissionVerdict v = s.policy()(gone);
    CHECK_FALSE(v.admitted);
    CHECK(v.reason.find("could not be identified") != std::string::npos);
    CHECK(v.reason.find("/no/such/dir/counter.so") != std::string::npos); // what it could not read
}

TEST_CASE("the baseline a policy mints is the small one, deliberately") {
    Scratch f("baseline");
    AuthorityStore s;
    std::string why;
    REQUIRE(s.open(f.path, &why));
    REQUIRE(s.put(rule_for("counter"), &why));

    AdmissionRequest speak = open_of("counter", "aaaa1111bbbb2222cccc3333dddd4444");
    speak.stage = AdmissionStage::Speak;
    const AdmissionVerdict v = s.policy()(speak);

    REQUIRE(v.admitted);
    // A BASELINE IS FROZEN FOR THE SUBJECT'S WHOLE LIFE (GATE-05), so everything a person
    // might want to take back has to live in the delegated half instead. What is left is
    // the one permission it would be perverse to make revocable: being inspectable.
    const LiveAuthority& live = v.grant.live();
    CHECK_FALSE(permits_anyone(live, "Noted", 1));
    CHECK_FALSE(live.permits_observe("Noted", 1));
    CHECK(permits_anyone(live, "zen.Result", 1)); // poke answers, and that is all
}

TEST_CASE("the policy never admits a Speak it would have refused at Open — it is not asked twice") {
    Scratch f("stages");
    AuthorityStore s;
    std::string why;
    REQUIRE(s.open(f.path, &why));
    // No rule at all. The Open stage refuses (above), so the Kernel never reaches Speak —
    // this pins the DIVISION OF LABOUR rather than a second opinion: Speak decides only
    // what the baseline is, and by then the right to run has already been settled.
    AdmissionRequest speak = open_of("nobody-approved-this", "aaaa1111bbbb2222cccc3333dddd4444");
    speak.stage = AdmissionStage::Speak;
    const AdmissionVerdict v = s.policy()(speak);
    CHECK(v.admitted);
    CHECK(s.pending().empty()); // and it recorded no question, because it asked none
}

// ---- the boot report ---------------------------------------------------------

TEST_CASE("a boot is complete when every row the person enabled started") {
    BootReport report;
    const auto row = [](BootState st, bool enabled = true) {
        BootOutcome o;
        o.entry.name = "x";
        o.entry.enabled = enabled;
        o.state = st;
        return o;
    };

    report.rows = {row(BootState::Started), row(BootState::Skipped, false)};
    // A deliberately disabled row does not make a boot incomplete: "complete" is about
    // the person's intent, not about the row count.
    CHECK(report.complete());

    report.rows.push_back(row(BootState::Refused));
    CHECK_FALSE(report.complete());
    CHECK(report.count(BootState::Started) == 1);
    CHECK(report.count(BootState::Refused) == 1);
    CHECK(report.count(BootState::Skipped) == 1);
}

TEST_CASE("the report distinguishes refused from failed, because they send a person elsewhere") {
    BootReport report;
    BootOutcome refused;
    refused.entry.name = "counter";
    refused.state = BootState::Refused;
    refused.detail = "admission refused at open: no standing decision";
    BootOutcome failed;
    failed.entry.name = "typo";
    failed.state = BootState::Failed;
    failed.detail = "open failed: no such file";
    report.rows = {refused, failed};

    const std::vector<std::string> lines = report.render();
    REQUIRE(lines.size() == 3); // two rows and a summary
    CHECK(lines[0].find("refused") != std::string::npos);
    CHECK(lines[1].find("failed") != std::string::npos);
    // The summary counts them apart: one is answered at the console with an authority
    // decision, the other in the build or the filesystem.
    CHECK(lines[2].find("refused by policy") != std::string::npos);
    CHECK(lines[2].find("1 failed") != std::string::npos);
    CHECK(lines[2].find("INCOMPLETE") != std::string::npos);
}

TEST_CASE("an empty plan renders as nothing requested, not as an empty success") {
    BootReport report;
    const std::vector<std::string> lines = report.render();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("nothing requested") != std::string::npos);
}

// ---- identity ----------------------------------------------------------------

TEST_CASE("the identity a decision is pinned to is the file's bytes, and says so when absent") {
    Scratch f("identity");
    f.write("the bytes of a file that is not a weave");
    const std::string id = loom::file_content_id(f.path);
    CHECK(id.size() == 32);
    CHECK(id == loom::file_content_id(f.path));

    f.write("different bytes entirely");
    CHECK(loom::file_content_id(f.path) != id);

    // And the non-throwing form answers empty rather than losing the caller's own error.
    CHECK(loom::file_content_id_or_empty("/no/such/file/anywhere.so").empty());
}


// ---- durability: a failed write must change NOTHING -------------------------
//
// The store used to change its map and then try to write it, so an unwritable file
// printed `cannot write` and changed what the host permitted anyway: `authority trust x`
// failed, `start x` then succeeded under the failed approval, and a restart lost the
// decision nobody had been told was not made. The candidate is written first now.
//
// THE INJECTION: the temp path is a DIRECTORY, which no open-for-write and no rename can
// replace. It is a real filesystem refusal on both platforms rather than a seam a caller
// could be given to stub out — this store's whole job is to be believed about the disk.

namespace {

/// Make `<path>.tmp` an unusable destination, and clean it up again.
struct BlockedTemp {
    std::string tmp;
    explicit BlockedTemp(const std::string& path) : tmp(path + ".tmp") {
        std::filesystem::remove(tmp);
        std::filesystem::create_directory(tmp);
    }
    ~BlockedTemp() {
        std::error_code ignored;
        std::filesystem::remove_all(tmp, ignored);
    }
};

} // namespace

TEST_CASE("a decision that could not be written did not happen, live or remembered") {
    Scratch f("failed-write");
    AuthorityStore store;
    std::string error;
    REQUIRE(store.open(f.path, &error));

    BlockedTemp blocked(f.path);
    REQUIRE_FALSE(store.put(rule_for("probe"), &error));
    CHECK(error.find("cannot write") != std::string::npos);

    // THE LIVE HALF. This is the assertion the old order could not make: the policy the
    // running host is deciding by must not have changed.
    CHECK(store.find("probe") == nullptr);
    CHECK(store.rules().empty());
    // ...and the durable half: nothing was created at all.
    CHECK_FALSE(std::filesystem::exists(f.path));

    // And the positive control, in the same case so "nothing was remembered" above is a
    // distinction rather than a store that never remembers anything.
    std::filesystem::remove_all(blocked.tmp);
    REQUIRE(store.put(rule_for("probe"), &error));
    REQUIRE(store.find("probe") != nullptr);
    CHECK(store.find("probe")->may_run);
}

TEST_CASE("a forget that could not be written leaves the decision in force") {
    Scratch f("failed-forget");
    AuthorityStore store;
    std::string error;
    REQUIRE(store.open(f.path, &error));
    REQUIRE(store.put(rule_for("probe"), &error));

    BlockedTemp blocked(f.path);
    REQUIRE_FALSE(store.forget("probe", &error));
    // Still permitted, here and after a restart — which is the only honest reading of a
    // command that reported it could not write.
    REQUIRE(store.find("probe") != nullptr);
    CHECK(store.find("probe")->may_run);

    AuthorityStore reread;
    REQUIRE(reread.open(f.path, &error));
    REQUIRE(reread.find("probe") != nullptr);
}

TEST_CASE("a build whose pin cannot be written does not run, because the pin is the approval") {
    // THE SEQUENCE THAT EXPOSED IT, not just its first step. An approval made without
    // `--rebuilds` means "this build, and ask me again when it changes", and the record of
    // WHICH build is the only thing that can notice a change. This store used to admit the
    // first build when that record could not be written, leave the rule unpinned, and then
    // admit a DIFFERENT build the same way — so a disk the person could not see turned their
    // "ask me again" into "run anything", announced in a note. A warning is not consent.
    Scratch f("failed-pin");
    AuthorityStore store;
    std::string error;
    REQUIRE(store.open(f.path, &error));
    REQUIRE(store.put(rule_for("probe"), &error)); // approved, unpinned, rebuilds re-ask

    Scratch a("failed-pin-build-a");
    a.write("build A");
    Scratch b("failed-pin-build-b");
    b.write("build B, which is different code");
    const AdmissionRequest build_a = open_of("probe", loom::file_content_id(a.path), a.path.c_str());
    const AdmissionRequest build_b = open_of("probe", loom::file_content_id(b.path), b.path.c_str());
    REQUIRE(build_a.build.content_id() != build_b.build.content_id());

    {
        BlockedTemp blocked(f.path);
        const AdmissionVerdict first = store.policy()(build_a);
        CHECK_FALSE(first.admitted);
        // WHY, and what to do — storage, not a decision. Nothing about the approval is
        // wrong, so the refusal must not send the person to approve anything again.
        CHECK(first.reason.find("could not record") != std::string::npos);
        CHECK(first.reason.find("cannot write") != std::string::npos);
        CHECK(first.reason.find("writable") != std::string::npos);
        // A RETRY is the same answer, and so is the different build that used to walk in.
        CHECK_FALSE(store.policy()(build_a).admitted);
        CHECK_FALSE(store.policy()(build_b).admitted);
        // Nothing was invented: no pin in memory, no widened posture, and no "decision
        // waiting on you" — the person's decision stands; the disk is what is waiting.
        REQUIRE(store.find("probe") != nullptr);
        CHECK(store.find("probe")->content_id.empty());
        CHECK_FALSE(store.find("probe")->trust_rebuilds);
        CHECK(store.pending().empty());
    }

    // THE RECOVERY PATH is the ordinary one: once the store can be written, the same approval
    // records the build that runs...
    const AdmissionVerdict recovered = store.policy()(build_a);
    CHECK(recovered.admitted);
    CHECK(store.find("probe")->content_id == build_a.build.content_id());
    // ...and a changed build asks again, which is what the person chose.
    CHECK_FALSE(store.policy()(build_b).admitted);

    // ...and so does a restarted host, which reads the pin from the file rather than memory.
    AuthorityStore restarted;
    REQUIRE(restarted.open(f.path, &error));
    CHECK(restarted.policy()(build_a).admitted);
    CHECK_FALSE(restarted.policy()(build_b).admitted);
}

TEST_CASE("`--rebuilds` is consent to any build, so a pin that cannot be written withdraws nothing") {
    // THE DISTINCTION the refusal above must not blur: a person who said `--rebuilds` has
    // already answered "may a different build of this run?" with yes. Not knowing WHICH build
    // ran changes nothing they asked for — so it runs, and the record that could not be made
    // is said, rather than presented as a failure of their consent.
    Scratch f("failed-pin-rebuilds");
    AuthorityStore store;
    std::string error;
    REQUIRE(store.open(f.path, &error));
    AuthorityRule r = rule_for("probe");
    r.trust_rebuilds = true;
    REQUIRE(store.put(std::move(r), &error));

    Scratch a("failed-pin-rebuilds-a");
    a.write("build A");
    Scratch b("failed-pin-rebuilds-b");
    b.write("build B");

    BlockedTemp blocked(f.path);
    CHECK(store.policy()(open_of("probe", loom::file_content_id(a.path), a.path.c_str())).admitted);
    CHECK(store.policy()(open_of("probe", loom::file_content_id(b.path), b.path.c_str())).admitted);
    REQUIRE_FALSE(store.notes().empty());
    const std::string& note = store.notes().back();
    CHECK(note.find("trust_rebuilds") != std::string::npos);
    CHECK(note.find("could not be recorded") != std::string::npos);
    CHECK(store.find("probe")->content_id.empty()); // said, not pretended
}

TEST_CASE("replacement preserves the previous record when it cannot happen") {
    // `write_gated_file` removed the destination and THEN renamed, so an interruption or a
    // failed rename between the two lost the last good record. The header claimed atomic
    // replacement; the code did not implement one. Both platforms replace in ONE operation
    // now, and this is the observable consequence: a failed write leaves the old file
    // exactly as it was, byte for byte.
    Scratch f("atomic-replace");
    AuthorityStore store;
    std::string error;
    REQUIRE(store.open(f.path, &error));
    REQUIRE(store.put(rule_for("first"), &error));
    REQUIRE(std::filesystem::exists(f.path));
    const std::string before = read_file(f.path);
    REQUIRE(before.find("first") != std::string::npos);

    {
        BlockedTemp blocked(f.path);
        REQUIRE_FALSE(store.put(rule_for("second"), &error));
    }
    CHECK(read_file(f.path) == before); // untouched, not deleted and not half-written

    AuthorityStore reread;
    REQUIRE(reread.open(f.path, &error));
    CHECK(reread.find("first") != nullptr);
    CHECK(reread.find("second") == nullptr);
}


TEST_CASE("replacement never removes the destination as a separate step") {
    // THE PROPERTY, NOT A STORY ABOUT A CRASH. `write_gated_file` used to `remove()` the
    // destination and then `rename()` onto it, which is two operations with a gap between
    // them: a process that died in the gap, or a rename that then failed, left NO record at
    // all. The header called that atomic replacement. Both platforms replace in one
    // operation now -- POSIX rename(2), Windows MoveFileEx(MOVEFILE_REPLACE_EXISTING).
    //
    // A gap cannot be observed by racing it, so the test observes the DELETE instead. The
    // destination here is something a single-operation replace cannot replace (a
    // directory), so:
    //   one operation  -> it fails, and whatever was at the destination is still there;
    //   remove + rename -> the remove succeeds, the rename then succeeds onto the freed
    //                      name, and the thing that was there is gone.
    // The second is reported as SUCCESS by the old code, which is the sharper half of it.
    Scratch f("replace-destination");
    std::filesystem::remove(f.path);
    std::filesystem::create_directory(f.path);

    const auto schema =
        loom::SchemaBuilder("zen.HostTestDoc", 1).field("x", loom::Kind::Text).build();
    loom::Value doc(schema);
    doc.set("x", loom::Cell::text("the new record"));

    std::string error;
    CHECK_FALSE(write_gated_file(f.path, doc, &error));
    CHECK_FALSE(error.empty());
    // Still exactly what was there before, not removed and not replaced by a file.
    CHECK(std::filesystem::exists(f.path));
    CHECK(std::filesystem::is_directory(f.path));
    // ...and the candidate was not left lying about for a later reader to mistake for one.
    CHECK_FALSE(std::filesystem::exists(f.path + ".tmp"));

    std::error_code ignored;
    std::filesystem::remove_all(f.path, ignored);
}

// ---- repeated approval -------------------------------------------------------

TEST_CASE("one permission written twice is one permission, and one revoke takes it back") {
    AuthorityRule r = rule_for("probe");
    r.send.push_back("Greet v1 -> any target");
    r.send.push_back("Greet v1 -> any target");
    // An observe rule written both ways it may legally be written: the same permission,
    // spelled twice, which a naive string compare would keep as two.
    r.observe.push_back("Tick v1");
    r.observe.push_back("observe Tick v1");

    CHECK(collapse_duplicates(&r) == 2);
    REQUIRE(r.send.size() == 1);
    REQUIRE(r.observe.size() == 1);
    CHECK(r.observe[0] == "observe Tick v1"); // one stored spelling, the one show prints

    // ...and the store applies it on the way in, so a decision cannot be stored doubled.
    Scratch f("duplicates");
    AuthorityStore store;
    std::string error;
    REQUIRE(store.open(f.path, &error));
    AuthorityRule doubled = rule_for("probe");
    doubled.send.push_back("Greet v1 -> any target");
    doubled.send.push_back("Greet v1 -> any target");
    REQUIRE(store.put(std::move(doubled), &error));
    REQUIRE(store.find("probe") != nullptr);
    CHECK(store.find("probe")->send.size() == 1);
}

TEST_CASE("a hand-edited store with a repeated permission reads as one, and says so") {
    Scratch f("duplicates-file");
    f.write(R"({"rules":[{"artifact":"probe","content_id":"","may_run":true,
                "trust_rebuilds":false,
                "send":["Greet v1 -> any target","Greet v1 -> any target"],
                "observe":[],"note":""}]})");
    AuthorityStore store;
    std::string error;
    REQUIRE(store.open(f.path, &error));
    REQUIRE(store.find("probe") != nullptr);
    CHECK(store.find("probe")->send.size() == 1);
    // Collapsed, and NOT silently: a person who wrote it twice is told which reading they
    // got, because the alternative reading (two things, needing two revokes) is the one
    // the file's shape suggests.
    bool said = false;
    for (const std::string& n : store.notes()) {
        said = said || n.find("repeated permission") != std::string::npos;
    }
    CHECK(said);
}

TEST_CASE("canonical_rule is the spelling `authority show` prints") {
    CHECK(canonical_rule("Greet v1 -> any target", /*observe=*/false) == "Greet v1 -> any target");
    CHECK(canonical_rule("Tick v1", /*observe=*/true) == "observe Tick v1");
    CHECK(canonical_rule("observe Tick v1", /*observe=*/true) == "observe Tick v1");
    CHECK(canonical_rule("  observe Tick v1  ", /*observe=*/true) == "observe Tick v1");
}

// ---- ownership of a decision store -------------------------------------------

TEST_CASE("a decision store has one owner at a time, and the claim is released with it") {
    // Two hosts writing one store do not merely lose an approval: the second one's stale
    // whole-file write RESTORES what the first one revoked. The mechanism is an OS claim
    // rather than a PID file precisely so nothing has to be cleaned up after a host that
    // died — and a claim taken through a SECOND open of the same path conflicts with the
    // first whether the two opens are in one process or two (flock binds to the open file
    // description; a Windows share-mode-0 handle excludes every other open).
    Scratch f("owned-store");
    std::string error;
    {
        StoreLock first;
        REQUIRE(first.claim(f.path, &error));
        CHECK(first.held());

        StoreLock second;
        bool taken = false;
        REQUIRE_FALSE(second.claim(f.path, &error, &taken));
        CHECK(taken);
        CHECK(error.find("already owns the decision store") != std::string::npos);
        CHECK_FALSE(second.held());
    }
    // ...and once the owner is gone the store is free again.
    StoreLock later;
    CHECK(later.claim(f.path, &error));
    CHECK(later.held());
    // Released before the sidecar is removed: Windows refuses to delete a file that an
    // open handle still names, which is the same fact that makes the claim work at all.
    later.release();
    CHECK_FALSE(later.held());
    std::error_code ignored;
    std::filesystem::remove(StoreLock::lock_path_for(f.path), ignored);
}

TEST_CASE("an in-memory store has nothing to own") {
    // No --authority means no file, so there is no store to race over and refusing to
    // start would be refusing over nothing.
    StoreLock lock;
    std::string error;
    CHECK(lock.claim("", &error));
    CHECK_FALSE(lock.held());
}

} // TEST_SUITE("host_policy")
