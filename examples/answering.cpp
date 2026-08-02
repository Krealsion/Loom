// Answers, immediate and deferred — the compiled twin of docs/guides/messaging.md.
//
// The Chef asks a Station to prepare. If the oven is already hot the Station
// answers at once; otherwise it CONVERTS its one answer right into a deferred
// one and spends it when the oven catches up. The answer is delivered to the
// asker — the Chef — who checks `answers_ask()`, Loom's own word, rather than
// trusting shapes or correlations.

#include <zen/switchboard.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <iostream>

using namespace loom;

namespace {

struct Go { // the host's nudge: "chef, place your order for this heat"
    std::int64_t heat;
    ZEN_SHAPE(Go, 1, ZEN_FIELD(heat));
};
struct PrepareStation {
    std::int64_t heat;
    ZEN_SHAPE(PrepareStation, 1, ZEN_FIELD(heat));
};
struct OvenHot {
    std::int64_t heat;
    ZEN_SHAPE(OvenHot, 1, ZEN_FIELD(heat));
};
struct StationReady {
    std::int64_t heat;
    ZEN_SHAPE(StationReady, 1, ZEN_FIELD(heat));
};
struct Nothing {
    std::int64_t n = 0;
    ZEN_SHAPE(Nothing, 1, ZEN_FIELD(n));
};

class Station final
    : public WeaveBase<Station, Nothing, Accept<PrepareStation, OvenHot>, Emit<StationReady>> {
public:
    void on(const PrepareStation& ask, Mail& mail) {
        if (ask.heat <= warmth_) {
            mail.answer(StationReady{warmth_}); // THE authorized answer, immediately
            return;
        }
        needed_ = ask.heat;
        pending_ = mail.defer_answer(); // the same one right, retained for later
    }
    void on(const OvenHot& oven, Mail& mail) {
        warmth_ = oven.heat;
        if (pending_.valid() && warmth_ >= needed_) {
            (void)answer_deferred(pending_, mail, StationReady{warmth_});
        }
    }

private:
    std::int64_t warmth_ = 0;
    std::int64_t needed_ = 0;
    DeferredAnswer pending_{};
};

class Chef final
    : public WeaveBase<Chef, Nothing, Accept<Go, StationReady>, Emit<PrepareStation>> {
public:
    explicit Chef(WeaveId station) : station_(station) {}

    void on(const Go& g, Mail& mail) {
        mail.send(station_, PrepareStation{g.heat}); // the ask: MY speech, so the
    }                                                // answer right binds to me

    void on(const StationReady& r, Mail& mail) {
        // Loom's word that this delivery answers MY ask — not a lookalike.
        std::cout << (mail.answers_ask() ? "answered" : "unsolicited") << ": station at heat "
                  << r.heat << "\n";
    }

private:
    WeaveId station_;
};

} // namespace

int main() {
    Switchboard bus;
    const WeaveId station = mount<Station>(bus);
    const WeaveId chef = mount<Chef>(bus, station);

    std::cout << "immediate (the station is already warm enough for 0):\n";
    bus.send(chef, Message(to_value(Go{0})));
    bus.pump();

    std::cout << "deferred (heat 200 is not there yet; the answer follows the oven):\n";
    bus.send(chef, Message(to_value(Go{200})));
    bus.pump();
    bus.send(station, Message(to_value(OvenHot{250})));
    bus.pump();
    return 0;
}
