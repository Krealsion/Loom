// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE STRANGER'S HISTORY — a host outside Loom's build tree that remembers what
// its bus did and keeps a durable record of the part that matters, reaching both
// halves only through `find_package(loom)`.
//
// RTH-1 exported and installed a recorder because its first consumer is a Zengine
// application that reaches this repository solely through the package — and then
// shipped without a witness that a stranger can actually reach it. This closes
// that debt, and it is not a formality: the build tree can satisfy a target the
// export set never published, and the difference only shows up in somebody else's
// project. RTH-1a widened the surface again (a second owner, a second header, a
// renamed target), so the claim is measured at exactly the moment it changed.
//
// It links `loom::history` and nothing that is not exported. If either half stops
// being reachable that way — a header left out of the install, a target dropped
// from the export set, a public type that needs an unexported one — this fails to
// configure or to compile, on the DEFAULT path.

#include <zen/history/dump.hpp>
#include <zen/history/logger.hpp>
#include <zen/history/recorder.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void ok(bool condition, const char* what) {
    std::printf("  %-5s %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

struct Beat {
    std::int64_t n;
    ZEN_SHAPE(Beat, 1, ZEN_FIELD(n));
};
struct Rare {
    std::string what;
    ZEN_SHAPE(Rare, 1, ZEN_FIELD(what));
};
struct Tally {
    std::int64_t heard;
    ZEN_SHAPE(Tally, 1, ZEN_FIELD(heard));
};

/// An ordinary participant the stranger also wrote. Nothing about it knows that a
/// history exists — which is the property being witnessed: observation is the
/// host's, not the participant's.
class Sink final
    : public loom::WeaveBase<Sink, Tally, loom::Accept<Beat, Rare>> {
public:
    void on(const Beat&, loom::Mail&) { ++heard_; }
    void on(const Rare&, loom::Mail&) { ++heard_; }
    Tally describe() const { return Tally{heard_}; }
    void restore(const Tally& t) { heard_ = t.heard; }

private:
    std::int64_t heard_ = 0;
};

} // namespace

int main() {
    std::printf("stranger history witness (RTH-1a: the two halves, through the package)\n");

    loom::Switchboard bus;

    // ---- the volatile half ------------------------------------------------
    loom::RecorderPolicy policy = loom::default_policy();
    policy.recent_capacity = 16;
    // The heartbeat policy this phase exists to make expressible: keep the last
    // one, take no recent context, keep no bytes.
    policy.rules.push_back(loom::RetentionRule{"Beat", /*last_n=*/1, /*in_recent=*/false,
                                               /*retain_payload=*/false});
    loom::Recorder recorder(bus, policy);

    // ---- the durable half -------------------------------------------------
    loom::LoggerSelection selection;
    selection.shapes.push_back(loom::LogRule{"Rare", 0});
    loom::Logger logger(bus, selection);
    const std::string path = "stranger-history-witness.log";
    std::string error;
    ok(logger.open(path, &error), "a stranger can open a durable stream");

    logger.info("stranger.host", "the witness is starting");

    const loom::WeaveId sink =
        bus.register_weave(std::make_unique<Sink>(), loom::Grant{}.allow_any());

    bus.send(sink, loom::Message(loom::to_value(Rare{"the thing worth keeping"})));
    for (int i = 0; i < 400; ++i) {
        bus.send(sink, loom::Message(loom::to_value(Beat{i})));
    }
    bus.pump();

    // 1. THE BEATS DID NOT FLOOD RECENT CONTEXT...
    bool beat_in_context = false;
    for (const loom::HistoryRecord& r : recorder.recent()) {
        if (r.shape == "Beat") {
            beat_in_context = true;
        }
    }
    ok(!beat_in_context, "400 beats took no place in recent context");

    // 2. ...AND ARE STILL DISCOVERABLE.
    const loom::Lookup last_beat = recorder.last_of("Beat");
    ok(last_beat.horizon == loom::Horizon::Retained && last_beat.record != nullptr,
       "the last beat is still there to be found");
    ok(last_beat.record != nullptr && loom::held_in(last_beat.record->held, loom::Held::LastCall),
       "...and it says which window holds it");
    ok(recorder.observed("Beat") && recorder.observed("Rare"),
       "every observed shape is discoverable by name");
    ok(!recorder.observed("NeverSent"), "a shape nobody sent is not claimed as observed");

    // 3. THE RARE FACT SURVIVED RECENT CONTEXT ENTIRELY.
    const loom::Lookup rare = recorder.last_of("Rare");
    ok(rare.horizon == loom::Horizon::Retained, "the one-off is still answerable");

    // 4. THE DURABLE HALF KEPT WHAT WAS NAMED, AND NOTHING ELSE.
    ok(logger.counters().selected == 1, "the logger selected exactly the named shape");
    ok(logger.appended_of("Beat") == 0, "unselected traffic never reached the stream");
    logger.close();

    std::vector<loom::LogRecord> back;
    ok(loom::Logger::read(path, &back, &error), "the stream reads back through the gate");
    ok(back.size() == 2, "a diagnostic and an observation, both durable");
    if (back.size() == 2) {
        ok(back[0].origin == loom::LogOrigin::Diagnostic &&
               back[0].observation.shape.empty(),
           "the host's own words carry no fake message");
        ok(back[1].origin == loom::LogOrigin::BusObservation &&
               back[1].observation.shape == "Rare",
           "the bus's fact is marked as the bus's");
    }

    // 5. THE FORMATTING SURFACE IS REACHABLE AND IS NOT THE READER.
    std::ostringstream out;
    loom::dump_history(recorder, out);
    loom::dump_log(back, out);
    const std::string text = out.str();
    ok(text.find("last-call") != std::string::npos, "the dump states the windows");
    ok(text.find("BusObservation") != std::string::npos, "...and every durable record's origin");

    std::remove(path.c_str());
    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
