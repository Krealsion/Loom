// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_TESTS_HANDOFF_PROTOCOL_HPP
#define ZEN_TESTS_HANDOFF_PROTOCOL_HPP

// THE HANDOFF GARDEN'S VOCABULARY (HANDOFF-01..03).
//
// Two ledger implementations with GENUINELY INCOMPATIBLE state, one temporary
// migrator that knows how to turn the first meaning into the second, and the
// old/new protocol pair that makes queued traffic around a replacement a real
// question rather than a hypothetical one.
//
// The point of the shapes below is that NOTHING here revives trivially:
//
//   LedgerV1   next_id: Int          flat, scalar, one mode as free text
//              total:   Int
//              mode:    Text
//
//   LedgerV2   ids:     NamespaceState{high_water}      nested message
//              totals:  Metrics{count, sum}             one Int became two
//              modes:   List<ModeFlag{name, on}>        one Text became a list
//                                                       of messages
//
// Feeding a LedgerV1 value to LedgerV2's gate fails on every field. That is the
// premise: different schema identities stay different, and only an explicit
// authored transformation produces a valid new-schema value.

#include <zen/schema.hpp>
#include <zen/value.hpp>
#include <zen/weave/shape.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace hg {

// ---- state: v1 --------------------------------------------------------------

struct LedgerV1 {
    std::int64_t next_id = 1;   ///< the identity namespace: the next id to mint
    std::int64_t total = 0;     ///< one flat running total
    std::string mode = "plain"; ///< one mode, as free text
    ZEN_SHAPE(LedgerV1, 1, ZEN_FIELD(next_id), ZEN_FIELD(total), ZEN_FIELD(mode));
};

// ---- state: v2 (incompatible, deliberately) ---------------------------------

struct NamespaceState {
    std::int64_t high_water = 0; ///< the highest id ever minted; the next is +1
    ZEN_SHAPE(NamespaceState, 1, ZEN_FIELD(high_water));
};

struct Metrics {
    std::int64_t count = 0; ///< how many additions
    std::int64_t sum = 0;   ///< their sum
    ZEN_SHAPE(Metrics, 1, ZEN_FIELD(count), ZEN_FIELD(sum));
};

struct ModeFlag {
    std::string name;
    bool on = false;
    ZEN_SHAPE(ModeFlag, 1, ZEN_FIELD(name), ZEN_FIELD(on));
};

struct LedgerV2 {
    NamespaceState ids;
    Metrics totals;
    std::vector<ModeFlag> modes;
    ZEN_SHAPE(LedgerV2, 1, ZEN_FIELD(ids), ZEN_FIELD(totals), ZEN_FIELD(modes));
};

// ---- production protocol: old and new ---------------------------------------

/// Mint the next identity. Both versions accept it — this is the shape whose
/// CONTINUITY across the replacement the namespace witness is about.
struct Issue {
    std::int64_t token = 0;
    ZEN_SHAPE(Issue, 1, ZEN_FIELD(token));
};

/// The reply carrying whichever id was minted.
struct Issued {
    std::int64_t id = 0;
    ZEN_SHAPE(Issued, 1, ZEN_FIELD(id));
};

/// THE OLD PROTOCOL. v1 accepts it; v2 deliberately does NOT — the successor no
/// longer speaks this shape, and Loom will not translate it. What happens to
/// traffic carrying it around the replacement is the domain's decision, and the
/// suite makes each case visible rather than smoothing it over.
struct AddV1 {
    std::int64_t amount = 0;
    ZEN_SHAPE(AddV1, 1, ZEN_FIELD(amount));
};

/// THE NEW PROTOCOL. Only v2 accepts it.
struct AddV2 {
    std::int64_t amount = 0;
    std::string currency;
    ZEN_SHAPE(AddV2, 1, ZEN_FIELD(amount), ZEN_FIELD(currency));
};

// ---- the FIFO handoff boundary ----------------------------------------------

/// THE BOUNDARY MESSAGE. An ordinary domain message delivered at an exact FIFO
/// position. Loom gives it no special standing whatsoever: what makes it a
/// boundary is that the incumbent's handler for it deliberately enters a
/// quiescing state and authors its final value there.
struct Quiesce {
    std::int64_t token = 0;
    ZEN_SHAPE(Quiesce, 1, ZEN_FIELD(token));
};

/// Ask the incumbent to describe itself. Answered with `LedgerV1` as the payload.
/// Used twice, for two DIFFERENT things the suite must keep apart:
///   before the boundary — an ordinary snapshot, which may go stale (H1);
///   after  the boundary — the incumbent's final authored value (H2).
struct Describe {
    std::int64_t token = 0;
    ZEN_SHAPE(Describe, 1, ZEN_FIELD(token));
};

/// The incumbent's answer to Describe: its v1 meaning, plus whether it had
/// already quiesced when it answered. THAT FLAG IS THE HONEST LABEL — it is how
/// a consumer tells an exact final value from a snapshot of a still-running
/// service, and the suite refuses to let the two look alike.
struct LedgerV1Report {
    LedgerV1 state;
    bool quiesced = false;
    ZEN_SHAPE(LedgerV1Report, 1, ZEN_FIELD(state), ZEN_FIELD(quiesced));
};

// ---- the migrator's conversation --------------------------------------------

/// "I know how to turn v1 meaning into v2 meaning." The migrator is an ORDINARY
/// weave: inspectable (a real artifact), testable (ordinary tests), versioned
/// (its own artifact), refusable (it answers Refused), and attributable (the bus
/// stamps who transformed it).
struct MigrateV1ToV2 {
    LedgerV1 from;
    /// The domain's explicit choice about the identity namespace. True carries
    /// the high-water mark across; false deliberately does not — the control
    /// case, so the difference is demonstrated rather than asserted.
    bool carry_namespace = true;
    ZEN_SHAPE(MigrateV1ToV2, 1, ZEN_FIELD(from), ZEN_FIELD(carry_namespace));
};

/// The migrator's answer. `ok == false` with a reason is a REFUSAL: a migrator
/// that does not understand its input says so instead of inventing a value.
struct MigrationResult {
    bool ok = false;
    std::string reason;
    LedgerV2 to;
    ZEN_SHAPE(MigrationResult, 1, ZEN_FIELD(ok), ZEN_FIELD(reason), ZEN_FIELD(to));
};

// ---- the candidate's preparation --------------------------------------------

/// The preparation ask, carrying the MIGRATED v2 value. The candidate adopts it
/// (through its own gate, as its own state) and answers for itself.
struct AdoptMigrated {
    LedgerV2 state;
    ZEN_SHAPE(AdoptMigrated, 1, ZEN_FIELD(state));
};

/// The candidate's authenticated answer: it adopted the value, or it did not.
struct Adopted {
    bool ready = false;
    std::string why;
    ZEN_SHAPE(Adopted, 1, ZEN_FIELD(ready), ZEN_FIELD(why));
};

// ---- the Sense both versions claim ------------------------------------------

/// What the ledger currently claims about itself. Both v1 and v2 declare it, so
/// the role-bound view is continuous across the replacement — which is exactly
/// what makes "the predecessor's claim is not the successor's" testable here.
struct LedgerStatus {
    std::int64_t issued_high_water = 0;
    bool quiesced = false;
    std::string version;
    /// How much production this ledger declined after its handoff boundary. The
    /// domain's post-boundary policy, made observable — and carried on the SENSE
    /// rather than in the ledger's own state, because "how many I refused" is not
    /// part of what a ledger means. This is the category doing its job.
    std::int64_t refused_after_boundary = 0;
    ZEN_SHAPE(LedgerStatus, 1, ZEN_FIELD(issued_high_water), ZEN_FIELD(quiesced),
              ZEN_FIELD(version), ZEN_FIELD(refused_after_boundary));
};

/// The production role both versions hold in turn.
inline constexpr const char* kLedgerRole = "handoff.ledger";

} // namespace hg

#endif // ZEN_TESTS_HANDOFF_PROTOCOL_HPP
