// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The box reload demo: one live counter (counter.cpp) asked every turn while the Weave Manager is
// asked by message to reload it from its native build into its module and back, after two reloads
// the box refuses; the transcript says which build gave each answer and how long each reload held
// the bus, and `--check` exits 1 naming each requirement the run missed.
//   zen-box-reload <native counter> <counter.wasm> <counter that reaches for a file.wasm>
//                  [--turns <n>] [--asks <n>] [--check]   docs/reference/kernel.md#a-weave-built-as-a-webassembly-module

#include "counter_shapes.hpp"

#include <zen/host/grant_wiring.hpp>
#include <zen/kernel/admission.hpp>
#include <zen/kernel/control.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/kernel/manager.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace loom;
using namespace box_reload;

namespace {

// ---- the demo's own participants ----------------------------------------------------------------

struct Turn {
    std::int64_t n = 0;
    ZEN_SHAPE(Turn, 1, ZEN_FIELD(n));
};

struct Operate {
    std::string name;
    std::string path;
    ZEN_SHAPE(Operate, 1, ZEN_FIELD(name), ZEN_FIELD(path));
};

/// Asks the operator to read the counter's grant into the log.
struct Describe {
    std::int64_t n = 0;
    ZEN_SHAPE(Describe, 1, ZEN_FIELD(n));
};

struct Tally {
    std::int64_t asked = 0;
    std::int64_t heard = 0;
    ZEN_SHAPE(Tally, 1, ZEN_FIELD(asked), ZEN_FIELD(heard));
};

/// One answer as the client heard it.
struct Heard {
    std::int64_t turn = 0;
    WeaveId from{};
    bool attested = false;
    CountAnswer answer;
};

/// One reload's outcome as the operator heard it.
struct Outcome {
    bool acknowledged = false;
    std::string reason;
};

struct Log {
    std::int64_t turn = 0;
    std::vector<std::string> authority; ///< the counter's grant, each time it was read
    std::map<std::uint64_t, std::vector<Heard>> heard; ///< by the counter that answered
    std::vector<Outcome> outcomes;
};

/// Asks each counter every turn: `asks` asks of the first, one of the second.
class Client final : public WeaveBase<Client, Tally, Accept<Turn, CountAnswer>, Emit<CountAsk>> {
public:
    Client(Log& log, WeaveId first, WeaveId second, std::int64_t asks)
        : log_(log), first_(first), second_(second), asks_(asks) {}

    void on(const Turn&, Mail& mail) {
        for (std::int64_t i = 0; i < asks_; ++i) {
            (void)mail.send(first_, CountAsk{++first_seq_});
            ++state_.asked;
        }
        (void)mail.send(second_, CountAsk{++second_seq_});
        ++state_.asked;
    }

    void on(const CountAnswer& a, Mail& mail) {
        ++state_.heard;
        log_.heard[mail.sender().value].push_back(
            Heard{log_.turn, mail.sender(), mail.answers_ask(), a});
    }

private:
    Log& log_;
    WeaveId first_;
    WeaveId second_;
    std::int64_t asks_;
    std::int64_t first_seq_ = 0;
    std::int64_t second_seq_ = 0;
};

/// The send rules of a grant as read, as one comparable line: base, then delegated.
std::string authority_line(const AuthorityView& v) {
    std::ostringstream out;
    out << (v.available ? "" : "unavailable ") << "base " << v.base.rules().size() << " send, "
        << v.base.observe_rules().size() << " observe; delegated " << v.delegated.rules().size();
    for (const SendRule& r : v.base.rules()) {
        out << "; " << (r.any_shape ? std::string("*") : r.shape_name) << "->"
            << (r.any_target ? std::string("*")
                             : (r.target_role.empty() ? std::to_string(r.target.value)
                                                      : "@" + r.target_role));
    }
    return out.str();
}

/// Asks the Weave Manager for one reload per Operate, and keeps what it answered. It holds the
/// right to read the counter's grant, which the host minted it, and reads it on each Describe.
class Operator final : public WeaveBase<Operator, Tally, Accept<Operate, Describe, Ack, Refused>,
                                        Emit<ReloadWeave>> {
public:
    Operator(Log& log, WeaveId manager, GrantAuthority reader)
        : log_(log), manager_(manager), reader_(std::move(reader)) {}

    void on(const Describe&, Mail& mail) {
        log_.authority.push_back(authority_line(mail.bus().describe_authority(reader_)));
    }

    void on(const Operate& op, Mail& mail) {
        ++state_.asked;
        (void)mail.send(manager_, ReloadWeave{op.name, op.path});
    }
    void on(const Ack&, Mail&) {
        ++state_.heard;
        log_.outcomes.push_back(Outcome{true, ""});
    }
    void on(const Refused& r, Mail&) {
        ++state_.heard;
        log_.outcomes.push_back(Outcome{false, r.reason});
    }

private:
    Log& log_;
    WeaveId manager_;
    GrantAuthority reader_;
};

// ---- the run ------------------------------------------------------------------------------------

struct Args {
    std::string native;
    std::string module;
    std::string reaching;
    std::int64_t turns = 40;
    std::int64_t asks = 4;
    bool check = false;
};

bool parse_args(int argc, char** argv, Args& a) {
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--check") {
            a.check = true;
        } else if ((arg == "--turns" || arg == "--asks") && i + 1 < argc) {
            const std::int64_t v = std::atoll(argv[++i]);
            if (v < 1) {
                return false;
            }
            (arg == "--turns" ? a.turns : a.asks) = v;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() != 3 || a.turns < 40) {
        return false;
    }
    a.native = positional[0];
    a.module = positional[1];
    a.reaching = positional[2];
    return true;
}

struct Failures {
    std::vector<std::string> said;
    void require(bool ok, const std::string& what) {
        if (!ok) {
            said.push_back(what);
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        std::cerr << "usage: zen-box-reload <native counter> <counter.wasm> <counter that reaches "
                     "for a file.wasm> [--turns <n, at least 40>] [--asks <n>] [--check]\n";
        return 2;
    }

    Switchboard bus;
    Kernel kernel(bus, trust_every_artifact("the box reload demo loads its own build's counter"));
    const WeaveId control = mount_control(kernel, bus);
    const WeaveId manager = mount_manager(control, bus);

    // The counter, and a second one granted filesystem reach a box cannot carry.
    const LoadResult counter = kernel.load("counter", args.native);
    const LoadResult counter_fs = kernel.load(
        "counter-fs", args.native, "", Grant{}.allow_any().with_filesystem(FsAccess::ReadOnly, "."));
    if (!counter.ok || !counter_fs.ok) {
        std::cerr << "zen-box-reload: the native counter did not load: " << counter.error
                  << counter_fs.error << '\n';
        return 1;
    }

    Log log;
    const WeaveId client = mount<Client>(bus, log, counter.id, counter_fs.id, args.asks);
    const WeaveId operator_id =
        mount<Operator>(bus, log, manager, host_describe_only_authority(bus, counter.id));
    auto read_authority = [&]() {
        bus.send(operator_id, Message(to_value(Describe{})));
        bus.drain_until_idle();
        return log.authority.empty() ? std::string("not read") : log.authority.back();
    };

    // Each reload: how long the control door's handler held the one thread of the bus.
    std::vector<double> held_ms;
    std::int64_t refused_asks = 0;
    std::int64_t failed_counter = 0;
    bus.add_observer([&](const BusEvent& ev) {
        if ((ev.kind == EventKind::Delivered || ev.kind == EventKind::HandlerFailed) &&
            ev.target == control && ev.schema_name == ReloadLibrary::zen_name) {
            held_ms.push_back(static_cast<double>(ev.handler_elapsed_ns) / 1e6);
        }
        if (ev.kind == EventKind::Refused &&
            (ev.schema_name == "CountAsk" || ev.schema_name == "CountAnswer")) {
            ++refused_asks;
        }
        if (ev.kind == EventKind::HandlerFailed &&
            (ev.target == counter.id || ev.target == counter_fs.id)) {
            ++failed_counter;
        }
    });

    const std::string authority_before = read_authority();
    const std::int64_t q = args.turns / 4;
    // The reloads, by the turn they are asked in: the two the box refuses, each offered to a
    // native weave that must keep answering; then into the box, and back.
    const std::map<std::int64_t, Operate> plan = {
        {q, Operate{"counter", args.reaching}},
        {q + q / 2, Operate{"counter-fs", args.module}},
        {2 * q, Operate{"counter", args.module}},
        {3 * q, Operate{"counter", args.native}},
    };

    std::cout << "loom box reload demo: " << args.turns << " turns, " << args.asks
              << " asks a turn to `counter` and 1 to `counter-fs`\n"
              << "  counter     id " << counter.id.value << ", from " << args.native << '\n'
              << "  counter-fs  id " << counter_fs.id.value
              << ", from the same file, granted filesystem read-only\n"
              << "  counter's grant: " << authority_before << "\n\n";

    std::size_t said_outcomes = 0;
    std::size_t said_held = 0;
    for (std::int64_t t = 1; t <= args.turns; ++t) {
        log.turn = t;
        const std::size_t first_heard = log.heard[counter.id.value].size();
        const std::size_t fs_heard = log.heard[counter_fs.id.value].size();
        bus.send(client, Message(to_value(Turn{t})));
        const auto op = plan.find(t);
        if (op != plan.end()) {
            bus.send(operator_id, Message(to_value(op->second)));
        }
        bus.send(client, Message(to_value(Turn{t})));
        const auto started = std::chrono::steady_clock::now();
        bus.drain_until_idle();
        const double turn_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();

        const auto& mine = log.heard[counter.id.value];
        const auto& fs = log.heard[counter_fs.id.value];
        std::ostringstream line;
        line << "turn " << t << ":";
        if (mine.size() > first_heard) {
            std::map<std::string, std::int64_t> by_build;
            for (std::size_t i = first_heard; i < mine.size(); ++i) {
                ++by_build[mine[i].answer.build];
            }
            line << " counter seq " << mine[first_heard].answer.seq << "-" << mine.back().answer.seq
                 << " answered by";
            for (const auto& b : by_build) {
                line << " " << b.first << " x" << b.second;
            }
            line << " (count " << mine.back().answer.count << ", sum " << mine.back().answer.sum
                 << ");";
        }
        if (fs.size() > fs_heard) {
            line << " counter-fs seq " << fs.back().answer.seq << " by " << fs.back().answer.build
                 << " (count " << fs.back().answer.count << ")";
        }
        line << "  [" << turn_ms << " ms]";
        std::cout << line.str() << '\n';
        if (op != plan.end()) {
            std::cout << "  reload `" << op->second.name << "` from " << op->second.path << ": ";
            if (log.outcomes.size() > said_outcomes) {
                const Outcome& o = log.outcomes.back();
                std::cout << (o.acknowledged ? "zen.Ack" : "zen.Refused: " + o.reason);
                said_outcomes = log.outcomes.size();
            } else {
                std::cout << "no answer yet";
            }
            if (held_ms.size() > said_held) {
                std::cout << "; the bus was held " << held_ms.back() << " ms";
                said_held = held_ms.size();
            }
            std::cout << '\n';
        }
    }

    const std::string authority_after = read_authority();
    const auto& mine = log.heard[counter.id.value];
    const auto& fs = log.heard[counter_fs.id.value];

    // The judgement, said whether or not --check asked for it.
    Failures f;
    const auto expected = args.turns * args.asks * 2;
    f.require(static_cast<std::int64_t>(mine.size()) == expected,
              "every ask of `counter` answered exactly once: " + std::to_string(mine.size()) +
                  " answers to " + std::to_string(expected) + " asks");
    std::int64_t sum = 0;
    std::vector<std::string> phases;
    bool in_order = true;
    bool counted = true;
    bool attested = true;
    bool one_sender = true;
    for (std::size_t i = 0; i < mine.size(); ++i) {
        const CountAnswer& a = mine[i].answer;
        const auto n = static_cast<std::int64_t>(i) + 1;
        sum += n;
        in_order = in_order && a.seq == n;
        counted = counted && a.count == n && a.sum == sum;
        attested = attested && mine[i].attested;
        one_sender = one_sender && mine[i].from == counter.id;
        if (phases.empty() || phases.back() != a.build) {
            phases.push_back(a.build);
        }
    }
    f.require(in_order, "the answers in the order asked, none twice and none missing");
    f.require(counted, "the state carried through both swaps: count and sum unbroken");
    f.require(attested, "every answer attested by Loom as the answer to its ask");
    f.require(one_sender && kernel.weave_id("counter") == counter.id,
              "one WeaveId throughout: every answer from " + std::to_string(counter.id.value));
    f.require(authority_after == authority_before,
              "one grant throughout: " + authority_before + " | " + authority_after);
    std::string seen;
    for (const auto& p : phases) {
        seen += (seen.empty() ? "" : ", ") + p;
    }
    f.require(phases == std::vector<std::string>{"native", "wasm32", "native"},
              "the builds native, then wasm32, then native: " + seen);
    f.require(refused_asks == 0 && failed_counter == 0,
              "no ask refused and no handler failed: " + std::to_string(refused_asks) +
                  " refused, " + std::to_string(failed_counter) + " failed");
    f.require(static_cast<std::int64_t>(fs.size()) == args.turns * 2,
              "every ask of `counter-fs` answered: " + std::to_string(fs.size()));
    bool fs_native = true;
    for (const Heard& h : fs) {
        fs_native = fs_native && h.answer.build == "native";
    }
    f.require(fs_native, "`counter-fs` answered by its native build throughout");
    f.require(log.outcomes.size() == 4, "four reload answers: " +
                                            std::to_string(log.outcomes.size()));
    if (log.outcomes.size() == 4) {
        f.require(!log.outcomes[0].acknowledged &&
                      log.outcomes[0].reason.find("wasi_snapshot_preview1.path_open") !=
                          std::string::npos,
                  "the module importing path_open refused, naming it: " +
                      log.outcomes[0].reason);
        f.require(!log.outcomes[1].acknowledged &&
                      log.outcomes[1].reason.find("filesystem") != std::string::npos,
                  "the grant the box cannot carry refused, naming it: " + log.outcomes[1].reason);
        f.require(log.outcomes[2].acknowledged, "the reload into the box acknowledged");
        f.require(log.outcomes[3].acknowledged, "the reload back to native acknowledged");
    }
    f.require(held_ms.size() == 4, "four reloads timed: " + std::to_string(held_ms.size()));

    std::cout << "\nanswers from `counter`: " << mine.size() << " of " << expected
              << ", builds in order: " << seen << "; from `counter-fs`: " << fs.size() << '\n'
              << "counter's grant after: " << authority_after << '\n';
    if (held_ms.size() == 4) {
        std::cout << "the bus held: refused (import) " << held_ms[0] << " ms, refused (grant) "
                  << held_ms[1] << " ms, into the box " << held_ms[2] << " ms, back to native "
                  << held_ms[3] << " ms\n";
    }
    if (f.said.empty()) {
        std::cout << "every requirement met\n";
        return 0;
    }
    for (const std::string& s : f.said) {
        std::cout << "NOT MET: " << s << '\n';
    }
    return args.check ? 1 : 0;
}
