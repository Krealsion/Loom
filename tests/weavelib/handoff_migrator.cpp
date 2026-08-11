// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE MIGRATOR (HANDOFF-01): an ordinary, temporary weave that says
//
//     "I know how to transform v1 meaning into v2 meaning."
//
// WHY A WEAVE, and not the alternatives it was compared against:
//
//   host callback registry   a C++ lambda: not inspectable, not versioned, no
//                            natural refusal, and no bus identity — "who
//                            transformed this?" would be unanswerable
//   candidate-owned          the candidate would have to accept old-schema
//                            input, which is precisely the two schemas ceasing
//                            to be different (law 1)
//   gate migration hook      invisible coercion inside the gate: forbidden
//   plain library function   testable, but with no bus identity and no refusal
//                            rail — attribution again unanswerable
//
// A weave wins because every property the phase requires of a migration —
// inspectable, testable, versioned, refusable, attributable — is a fact Loom
// ALREADY CARRIES about a weave, rather than a convention the domain has to
// maintain. And it can be unloaded afterwards, which is what makes "temporary"
// a proven thing rather than an intention.
//
// It holds no role, needs no lifecycle standing, and touches neither the
// incumbent nor the candidate. It answers one question, through the ordinary
// answer rail, so the coordinator's readiness verdict rests on an authenticated
// statement from a named author.

#include "handoff_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/switchboard.hpp>
#include <zen/weave.hpp>
#include <zen/zen.hpp>

#include <string>

using namespace loom;
using namespace hg;

namespace {

/// What the migrator remembers: how many transformations it performed and how
/// many it refused. Its only window on itself, read through the ordinary
/// snapshot path — no back channel invented for the test.
struct MigratorState {
    std::int64_t migrated = 0;
    std::int64_t refused = 0;
    ZEN_SHAPE(MigratorState, 1, ZEN_FIELD(migrated), ZEN_FIELD(refused));
};

class Migrator : public WeaveBase<Migrator, MigratorState, Accept<MigrateV1ToV2>> {
public:
    void on(const MigrateV1ToV2& ask, Mail& mail) {
        // A REAL TRANSFORMATION, with real opinions — which is what makes a bad
        // one provable. Every field changes shape:
        //
        //   next_id  -> ids.high_water     (a "next" becomes a "highest issued",
        //                                   so it is next_id - 1, not next_id)
        //   total    -> totals.sum         (and a count nobody had before)
        //   mode     -> modes:List<Flag>   (free text becomes a named flag)
        MigrationResult out;

        // REFUSABLE. A migrator that does not understand its input says so
        // rather than inventing a value. An empty mode is meaningless in v2's
        // vocabulary — v2 flags are named — so this is a refusal, not a guess.
        if (ask.from.mode.empty()) {
            out.ok = false;
            out.reason = "v1 mode is empty; v2 flags must be named";
            ++state_.refused;
            mail.answer(out);
            return;
        }
        if (ask.from.next_id < 1) {
            out.ok = false;
            out.reason = "v1 next_id below 1; no valid namespace to carry";
            ++state_.refused;
            mail.answer(out);
            return;
        }

        // THE DOMAIN'S EXPLICIT NAMESPACE DECISION, made here and nowhere else.
        // Carrying it means the successor continues the identity sequence;
        // not carrying it means the successor starts over — legitimate for a
        // domain whose identities are scoped to an incarnation, and a defect for
        // one whose identities outlive it. Loom has no allocator and no opinion;
        // this weave has the opinion, and the suite proves both outcomes.
        out.to.ids.high_water = ask.carry_namespace ? (ask.from.next_id - 1) : 0;

        out.to.totals.count = ask.from.total == 0 ? 0 : 1;
        out.to.totals.sum = ask.from.total;
        out.to.modes.push_back(ModeFlag{ask.from.mode, true});
        out.ok = true;
        ++state_.migrated;
        mail.answer(out);
    }

    LifecyclePolicy policy_config() const { return LifecyclePolicy{0, false}; }
};

} // namespace

ZEN_EXPORT_WEAVE(Migrator)
