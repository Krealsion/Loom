// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// A SECOND HOST, FROM THE PACKAGE ALONE, KEEPS ITS PERSON'S DECISIONS IN LOOM'S AUTHORITY STORE
// and answers Loom's `authority` command over that file: refused with the command that answers
// it, decided through the installed command, loaded on the retry, and a decision installed on the
// running bus by the installed warden. Its operator seat is its own, an ordinary participant with
// an AskBook, so every answer it hands the command is one Loom attributed to the question asked.
// The load half needs loom::kernel; without it that half is declared absent and the rest runs.

#include "witness_protocol.hpp"

#include <zen/gate.hpp>
#include <zen/host/authority.hpp>
#include <zen/host/authority_command.hpp>
#include <zen/host/grant_wiring.hpp>
#include <zen/host/store_lock.hpp>
#include <zen/host/warden.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/weave/ask_book.hpp>

#if defined(ZEN_WITNESS_WEAVE)
#include <zen/kernel/kernel.hpp>
#endif

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %-6s %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) {
        ++failures;
    }
}

bool says(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

struct Turn {
    ZEN_SHAPE(Turn, 1);
};

struct SeatState {
    std::int64_t answers;
    ZEN_SHAPE(SeatState, 1, ZEN_FIELD(answers));
};

// The operator seat. A send it makes from inside its own delivery is a participant's send, gated
// and stamped with its id, which is what the warden checks; a host's root send would be neither.
class Seat final
    : public loom::WeaveBase<Seat, SeatState,
                             loom::Accept<Turn, loom::Result, loom::Refused,
                                          loom::AuthorityDescription>> {
public:
    void set_self(loom::WeaveId id) { me_ = id; }

    template <class Question>
    loom::host::WardenAnswer ask(loom::Switchboard& bus, loom::WeaveId warden,
                                 const Question& question) {
        reply_.reset();
        errand_ = [this, warden, question](loom::Mail& mail) {
            const loom::AskOpened opened =
                book_.open(warden, Question::zen_name, Question::zen_version);
            if (opened.ok) {
                (void)mail.send(warden, question, opened.correlation);
            }
        };
        bus.send(me_, loom::Message(loom::to_value(Turn{})));
        bus.drain_until_idle();
        errand_ = nullptr;
        loom::host::WardenAnswer a;
        a.reply = reply_;
        if (!a.reply) {
            a.trouble = "no attributed answer from the warden";
        }
        return a;
    }

    void on(const Turn&, loom::Mail& mail) {
        if (errand_) {
            errand_(mail);
        }
    }
    void on(const loom::Result& r, loom::Mail& mail) { keep(r, mail); }
    void on(const loom::Refused& r, loom::Mail& mail) { keep(r, mail); }
    void on(const loom::AuthorityDescription& d, loom::Mail& mail) { keep(d, mail); }

private:
    template <class Answer>
    void keep(const Answer& answer, loom::Mail& mail) {
        if (book_.settle(mail.correlation(), mail.sender())) {
            ++state_.answers;
            reply_ = loom::to_value(answer);
        }
    }

    loom::AskBook book_{4};
    std::function<void(loom::Mail&)> errand_;
    std::optional<loom::Value> reply_;
    loom::WeaveId me_{};
};

class SeatDesk final : public loom::host::AuthorityDesk {
public:
    SeatDesk(loom::Switchboard& bus, Seat& seat, loom::WeaveId warden)
        : bus_(bus), seat_(seat), warden_(warden) {}
    loom::host::WardenAnswer sync(const std::string& artifact) override {
        return seat_.ask(bus_, warden_, loom::host::HostAuthoritySync{artifact});
    }
    loom::host::WardenAnswer describe(const std::string& artifact) override {
        return seat_.ask(bus_, warden_, loom::host::HostDescribeAuthority{artifact});
    }

private:
    loom::Switchboard& bus_;
    Seat& seat_;
    loom::WeaveId warden_;
};

struct Pongs {
    std::int64_t count;
    ZEN_SHAPE(Pongs, 1, ZEN_FIELD(count));
};

class Collector final : public loom::WeaveBase<Collector, Pongs, loom::Accept<witness::Pong>> {
public:
    void on(const witness::Pong&, loom::Mail&) { ++state_.count; }
};

std::int64_t pongs_at(loom::Switchboard& bus, loom::WeaveId collector) {
    loom::Admission a =
        loom::admit(loom::parse(bus.snapshot_bytes(collector)), loom::schema_of<Pongs>());
    return a.ok() ? loom::from_value<Pongs>(std::move(a).value()).count : -1;
}

} // namespace

int main(int argc, char** argv) {
    std::puts("package witness: a second host's decisions, in Loom's authority store");
    if (argc != 2) {
        std::puts("usage: witness-authority <work directory>");
        return 2;
    }
    std::error_code ec;
    const std::filesystem::path work(argv[1]);
    std::filesystem::create_directories(work, ec);
    const std::string file = (work / "decisions.json").string();
    std::filesystem::remove(file, ec);

    // ---- one host owns the store ----------------------------------------------------
    loom::host::StoreLock lock;
    std::string why;
    const bool claimed = lock.claim(file, &why);
    check(claimed, "the host claimed its decision store" +
                       (why.empty() ? std::string() : " -- " + why));
    {
        loom::host::StoreLock second;
        bool held_by_another = false;
        std::string refusal;
        check(!second.claim(file, &refusal, &held_by_another) && held_by_another,
              "a second claim on the same store is refused, as held by another");
    }

    loom::host::AuthorityStore store;
    check(store.open(file, &why), "an absent store opens empty");

    // ---- the seat and the warden, as host wiring --------------------------------------
    loom::Switchboard bus;
    auto seat_owner = std::make_unique<Seat>();
    Seat* seat = seat_owner.get();
    loom::Grant seat_grant;
    seat_grant.allow_to_any(loom::host::HostAuthoritySync::zen_name,
                            loom::host::HostAuthoritySync::zen_version);
    seat_grant.allow_to_any(loom::host::HostDescribeAuthority::zen_name,
                            loom::host::HostDescribeAuthority::zen_version);
    const loom::WeaveId seat_id = bus.register_weave(std::move(seat_owner), seat_grant);
    seat->set_self(seat_id);
    auto warden_owner = std::make_unique<loom::host::HostWarden>(store, seat_id);
    loom::host::HostWarden* warden = warden_owner.get();
    const loom::WeaveId warden_id =
        bus.register_weave(std::move(warden_owner), loom::host::warden_capability(seat_id));
    warden->zen_set_self(warden_id);
    SeatDesk desk(bus, *seat, warden_id);

    const auto run = [&](const std::vector<std::string>& words) {
        std::ostringstream out;
        loom::host::run_authority_command(store, *warden, desk, words, out);
        std::printf("    > authority%s\n", [&] {
            std::string line;
            for (std::size_t i = 1; i < words.size(); ++i) {
                line += " " + words[i];
            }
            return line;
        }().c_str());
        std::fputs(out.str().c_str(), stdout);
        return out.str();
    };

    check(says(run({"authority"}), "Nothing may run"), "an empty store says nothing may run");
    check(says(loom::host::authority_help(), "authority pending"),
          "the grammar's help names its commands");

#if defined(ZEN_WITNESS_WEAVE)
    // ---- refused, with the command that answers it ------------------------------------
    loom::Kernel kernel(bus, store.policy());
    const loom::WeaveId collector = loom::mount<Collector>(bus);
    const loom::LoadResult refused = kernel.load("witness", ZEN_WITNESS_WEAVE);
    check(!refused.ok && says(refused.error, "authority trust witness"),
          "an undecided artifact is refused, naming the command that answers it");
    check(store.pending().size() == 1 && store.pending()[0].artifact == "witness",
          "...and it is pending in the store");
    const std::string pending = run({"authority", "pending"});
    check(says(pending, "witness  ") && says(pending, "no standing decision for 'witness'") &&
              says(pending, "build presented: "),
          "`authority pending` lists it, why it was refused and the build presented");

    // ---- decided through the installed command, loaded on the retry --------------------
    check(says(run({"authority", "trust", "witness"}), "'witness' may run"),
          "`authority trust` decides it");
    const loom::LoadResult loaded = kernel.load("witness", ZEN_WITNESS_WEAVE);
    check(loaded.ok, "the retry loads it" + (loaded.ok ? std::string() : " -- " + loaded.error));
    if (!loaded.ok) {
        std::printf("package witness: authority FAILED (%d)\n", failures);
        return 1;
    }
    // A host that loads through Kernel::load itself puts the subject under its warden here;
    // loom-host does it inside the load (LifecycleAdoption), where every load passes one door.
    warden->govern("witness",
                   loom::host_grant_authority(
                       bus, loaded.id, loom::LiveAuthority{}.allow_any().allow_observe_any()));

    // ---- a decision, installed on the running bus by the warden ------------------------
    const auto ping = [&](std::int64_t seq) {
        bus.send(loaded.id,
                 loom::Message(loom::to_value(witness::Ping{seq}), loom::WeaveId{}, collector));
        bus.drain_until_idle();
        return pongs_at(bus, collector);
    };
    check(ping(1) == 0, "before any decision about its speech, its reply is refused");
    const std::string allowed = run({"authority", "allow", "witness", "Pong", "v1", "->", "any",
                                     "target"});
    check(says(allowed, "remembered.") &&
              says(allowed, "weave " + std::to_string(loaded.id.value) +
                                " may now say: Pong v1 -> any target"),
          "`authority allow` is remembered and the warden installs it on the bus");
    check(ping(2) == 1, "...and the same reply is delivered now");
    const std::string shown = run({"authority", "show", "witness"});
    check(says(shown, "LIVE, weave " + std::to_string(loaded.id.value)) &&
              says(shown, "delegated (revocable): Pong v1 -> any target"),
          "`authority show` reads the live authority back through the warden");

    // ---- and the decisions are in the file -----------------------------------------------
    loom::host::AuthorityStore reread;
    check(reread.open(file, &why), "the store reads back from its file");
    const loom::host::AuthorityRule* rule = reread.find("witness");
    check(rule != nullptr && rule->may_run && !rule->content_id.empty() &&
              rule->send == std::vector<std::string>{"Pong v1 -> any target"},
          "the file holds the decision, the build it pinned and the permission allowed");

    check(says(run({"authority", "revoke", "witness"}), "revoked, for this run and the next."),
          "`authority revoke` takes it back");
    check(ping(3) == 1, "...and the reply is refused again");
    warden->release("witness");
    check(kernel.unload("witness"), "the artifact unloads");
#else
    std::puts("  loom::kernel is absent from this install: the load half is DECLARED ABSENT");
    check(says(run({"authority", "trust", "witness"}), "'witness' may run"),
          "`authority trust` decides an artifact");
    check(says(run({"authority", "allow", "witness", "Pong", "v1", "->", "any", "target"}),
               "nothing loaded under 'witness' to administer"),
          "`authority allow` remembers it, and says nothing is loaded to administer");
    loom::host::AuthorityStore reread;
    check(reread.open(file, &why) && reread.find("witness") != nullptr &&
              reread.find("witness")->may_run,
          "the file holds the decision");
#endif

    if (failures != 0) {
        std::printf("package witness: authority FAILED (%d)\n", failures);
        return 1;
    }
    std::puts("package witness: authority PASSED");
    return 0;
}
