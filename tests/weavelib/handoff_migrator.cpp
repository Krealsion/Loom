// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE MIGRATOR (HANDOFF-01): an ordinary, temporary weave that says "I know how to transform v1
// meaning into v2 meaning". A weave, because inspectable, testable, versioned, refusable and
// attributable are facts Loom already carries about one, and unloading it afterwards makes
// "temporary" a proven thing (docs/reference/handoff.md compares the alternatives). It holds no
// role, touches neither the incumbent nor the candidate, and answers one question through the
// ordinary answer rail, so readiness rests on an authenticated statement from a named author.

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
        // A REAL TRANSFORMATION, with real opinions, which is what makes a bad one provable.
        // Every field changes shape: next_id becomes ids.high_water (a "next" becomes a
        // "highest issued", so next_id - 1), total becomes totals.sum (with a count nobody had
        // before), and the free-text mode becomes a named flag in modes.
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
