// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE SUPPLIED HOST'S PROBE — an ordinary loadable weave that reports what the host
// actually did to it, and that behaves the two ways a host has to survive.
//
// WHY IT EXISTS. Every defect in the host's lifecycle/authority glue was invisible to
// the policy tests, because none of them is about a grammar or a schema: they are about
// WHICH reply answered which question, WHEN an approved permission became effective, and
// WHETHER the operator could still be heard. Nothing but a real weave in a real host
// process can answer those, so this is that weave — the review's own reproduction
// fixture, maintained instead of thrown away.
//
// WHAT IT REPORTS, and each number is a defect this host had:
//
//   activations  authenticated `zen.Activated` deliveries (`mail.lifecycle_attested()`
//                only — an unattested one proves nothing). A boot walk that called
//                `Kernel::load` directly left this at 0 while the host printed COMPLETE.
//   startups     messages this weave sent TO ITSELF on activation and received. It is
//                the "an approved permission is effective for the first breath" number:
//                it is 0 and `refusals` is 1 when authority arrives one turn too late.
//   refusals     authenticated `zen.DispatchRefused` notices — Loom's own word that a
//                send of ours was refused before any handler saw it.
//   ticks        `Spin` deliveries. Each one re-arms itself, which is the whole of the
//                responsiveness probe: a cooperative handler that always returns and
//                still keeps the bus non-empty forever.
//
// AND ONE CONVERSATION IT ANSWERS LATE. `Countdown{turns}` is answered only after that many
// more bus turns, by an ordinary send carrying the asker's correlation — so the answer is
// attributable (the bus stamps this weave as its author) and arrives after the host has
// stopped waiting for it. It is what "the host kept turning while a person was typing" is
// measured with, and what makes a held answer DELAYED rather than immediate.
//
// TWO VARIANTS, ONE SOURCE.
//
//   (default)        honest: announces itself on activation and answers when asked.
//   ZEN_PROBE_FORGE  also authors a `zen.Result` AT THE CONSOLE, unasked, timed to land
//                    after the genuine answer to the operator's own load. It needs NO
//                    extra approval to do it: `loom::host::admitted_baseline()` is
//                    `allow_poke_answers`, which is `allow_to_any` for `zen.Result`, and
//                    a console is registered `AcceptMode::AnyRegistered`. So "the newest
//                    thing in the reply window" is a value any admitted artifact can
//                    author — which is exactly how an unrelated `zen.Result{"1"}` once
//                    became the subject of a host-minted administration capability.
//                    The staging hop (Result to our own role first) is what puts the
//                    forgery BEHIND the genuine answer in the queue; a forgery that
//                    arrived first would prove nothing about which one the host picks.

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>
#include <zen/weave/dispatch_refusal.hpp>
#include <zen/weave/lifecycle.hpp>

#include <cstdint>
#include <string>

namespace {

using namespace loom;

/// "Tell me what happened to you." Answered, so the answer is attributed by Loom.
struct Inspect {
    ZEN_SHAPE(Inspect, 1);
};

/// A self-addressed message that never stops: the cooperative busy weave.
struct Spin {
    ZEN_SHAPE(Spin, 1);
};

/// What this weave sends itself when it is told it is live — the send that must be
/// permitted by then, or the approval the person already made was installed too late.
struct Startup {
    ZEN_SHAPE(Startup, 1);
};

/// "Answer me after this many turns."
struct Countdown {
    std::int64_t turns = 0;
    ZEN_SHAPE(Countdown, 1, ZEN_FIELD(turns));
};

/// One turn of a countdown, addressed to this weave's own office, carrying who asked and the
/// conversation they named. The person approves `Tock v1 -> role probe` for it to run.
struct Tock {
    std::int64_t remaining = 0;
    std::int64_t asker = 0;
    std::int64_t correlation = 0;
    ZEN_SHAPE(Tock, 1, ZEN_FIELD(remaining), ZEN_FIELD(asker), ZEN_FIELD(correlation));
};

struct ProbeState {
    std::int64_t activations = 0;
    std::int64_t startups = 0;
    std::int64_t refusals = 0;
    std::int64_t ticks = 0;
    ZEN_SHAPE(ProbeState, 1, ZEN_FIELD(activations), ZEN_FIELD(startups), ZEN_FIELD(refusals),
              ZEN_FIELD(ticks));
};

class HostProbe
    : public WeaveBase<HostProbe, ProbeState,
                       Accept<Activated, Inspect, Spin, Startup, Countdown, Tock, DispatchRefused,
                              Result>,
                       Emit<Result, Spin, Startup, Tock>> {
public:
    void on(const Activated&, Mail& mail) {
        // ATTESTED ONLY. Any weave granted the shape could send a `zen.Activated`; only
        // Loom can attest one (LIFE-04), and an unattested one is not a first breath.
        if (mail.lifecycle_attested()) {
            ++state_.activations;
        }
#ifdef ZEN_PROBE_FORGE
        // Stage: bounce a Result off our own role so the forgery below is authored one
        // turn LATER than the genuine answer to whatever the operator asked.
        (void)mail.send_to_role("probe", Result{"stage"});
#else
        // The startup send. If the person has approved `Startup v1 -> role probe`, this
        // arrives and `startups` becomes 1. If the host installs that approval after the
        // activation, Loom refuses it and `refusals` becomes 1 instead. The two numbers
        // are the finding, stated as arithmetic.
        (void)mail.send_to_role("probe", Startup{});
#endif
    }

    void on(const Inspect&, Mail& mail) {
        (void)mail.answer(Result{"activations=" + std::to_string(state_.activations) +
                                 " startups=" + std::to_string(state_.startups) +
                                 " refusals=" + std::to_string(state_.refusals) +
                                 " ticks=" + std::to_string(state_.ticks)});
    }

    void on(const Startup&, Mail&) { ++state_.startups; }

    void on(const DispatchRefused&, Mail& mail) {
        if (mail.dispatch_refused()) {
            ++state_.refusals;
        }
    }

    void on(const Spin&, Mail& mail) {
        ++state_.ticks;
        // THE COOPERATIVE BUSY WEAVE. This handler returns immediately and leaves the bus
        // non-empty, which is precisely the case an unbounded drain cannot survive and a
        // bounded turn handles without noticing.
        (void)mail.send_to_role("probe", Spin{});
    }

    void on(const Countdown& c, Mail& mail) {
        // Not answered here: the asker and its conversation travel with the countdown, and the
        // answer is authored when it reaches zero.
        (void)mail.send_to_role("probe",
                                Tock{c.turns, static_cast<std::int64_t>(mail.reply_to().value),
                                     static_cast<std::int64_t>(mail.correlation())});
    }

    void on(const Tock& t, Mail& mail) {
        if (t.remaining > 0) {
            (void)mail.send_to_role("probe", Tock{t.remaining - 1, t.asker, t.correlation});
            return;
        }
        // An ORDINARY send naming the asker's conversation — `zen.Result` is in the baseline
        // every admitted artifact gets. The bus stamps this weave as the sender, which is the
        // other half of what lets the console attribute it.
        (void)mail.send(WeaveId{static_cast<std::uint64_t>(t.asker)}, Result{"counted down"},
                        static_cast<std::uint64_t>(t.correlation));
    }

    void on(const Result&, Mail& mail) {
#ifdef ZEN_PROBE_FORGE
        // Weave 1 is the operator console in the supplied host's construction order. An
        // attacker guesses exactly this, which is the point: guessing a target and a
        // shape must not be enough to be mistaken for somebody's answer.
        (void)mail.send(WeaveId{1}, Result{"1"});
#else
        (void)mail;
#endif
    }
};

} // namespace

ZEN_EXPORT_WEAVE(HostProbe)
