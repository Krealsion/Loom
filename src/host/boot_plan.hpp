// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_BOOT_PLAN_HPP
#define ZEN_HOST_BOOT_PLAN_HPP

// WHAT BOOTS, IN WHAT ORDER, AND WHAT HAPPENED TO EACH.
//
// The supplied host's boot plan: a file the person writes, listing the artifacts they
// want started and the order they want them started in. It is deliberately a LIST and
// not a graph — order is the person's to state, and a host that inferred it would be
// making a decision the person cannot see. Nothing here is a dependency solver.
//
// IT IS NOT AUTHORITY. Being in this file means "the person wants this started"; it
// does not mean the artifact may run or may say anything. That is the authority store's
// question and the admission policy's answer (host/authority.hpp), asked separately and
// able to refuse every row in this file. Two files, because they answer two questions a
// person makes at different times and with different care.
//
// THE REPORT IS THE POINT. A boot that half-worked is the normal case in development,
// so every row ends in a state with a name and, when it failed, the refusal's own
// words. "What was requested, what started, what is ready, what failed, what remains
// pending" is one table, printed at boot and re-readable from the console afterwards.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace loom::host {

/// What a row asks the host to do when it fails.
enum class OnFailure {
    Continue, ///< note it and go on to the next row (the default)
    Stop,     ///< leave every later row Pending, and say so
};

/// One authored row.
struct BootEntry {
    std::string name;  ///< the artifact name the host will know it by
    std::string path;  ///< the file
    std::string role;  ///< the office it should hold, or empty
    bool enabled = true;
    OnFailure on_failure = OnFailure::Continue;
};

/// A LINK TO ANOTHER RUNNING HOST, as the person wrote it: a name (the office local weaves
/// address is `loom.link.<name>`), where to connect, and what this host says about itself when
/// it does. The far host's policy decides what that is worth; nothing here is authority.
struct LinkEntry {
    std::string name;
    std::string connect;    ///< host:port
    std::string identity;   ///< the name this host CLAIMS; the far host may establish another
    std::string credential; ///< presented at Hello; a passphrase, a token, or nothing
};

/// ONE RETENTION RULE FOR THE HOST'S WORKING MEMORY, as `loom::RetentionRule` spells it: how
/// many of the last observations of a shape to keep, whether it takes recent context, and
/// whether its bytes are kept.
struct HistoryRetain {
    std::string shape;
    std::int64_t last_n = 1;
    bool in_recent = true;
    bool retain_payload = true;
};

/// ONE DURABLE SELECTION, as `loom::LogRule` spells it: a shape worth keeping for good, and a
/// per-shape cap (0 = uncapped).
struct HistoryKeep {
    std::string shape;
    std::int64_t cap = 0;
};

/// WHAT THIS HOST REMEMBERS AND WHAT IT KEEPS -- the `history` section of the boot plan. Both
/// halves are Loom's own (`zen/history/`); this is only their configuration, written where the
/// person writes everything else about a run. Absent, the host keeps Loom's defaults: a small
/// bounded memory of everything, and a durable file of nothing until `log` names one.
struct HistoryConfig {
    std::string log;                   ///< the durable stream's path; empty = write nothing
    std::int64_t recent = 0;           ///< the recent window's capacity; 0 = Loom's default
    std::int64_t payload_budget = 0;   ///< bytes of payload held at once; 0 = Loom's default
    bool keep_refusals = false;        ///< durable: every refusal (measured noisy; off by default)
    std::vector<HistoryRetain> retain; ///< per-shape working-memory rules
    std::vector<HistoryKeep> keep;     ///< per-shape durable selection, beside Loom's default
};

/// A boot plan, in the person's stated order.
struct BootPlan {
    std::vector<BootEntry> entries;
    std::vector<LinkEntry> links;
    HistoryConfig history;
    /// Where it came from, for the console to be able to say so. Empty when no file
    /// was read (the host booted with nothing, which is a legitimate and useful state).
    std::string source;
};

/// Read and gate a boot plan from `path`. A missing file is NOT an error: it yields an
/// empty plan, because "I have not written a plan yet" is where every new person
/// starts, and a host that refuses to run without one is a host they cannot explore.
/// A file that exists but is malformed IS an error — a person who wrote a plan is owed
/// the parse error rather than a silent empty boot.
///
/// `*error` carries why on a false return.
bool read_boot_plan(const std::string& path, BootPlan* out, std::string* error);

/// The states a row can end in. Every one of them is a distinct thing a person does
/// something different about, which is why they are not collapsed into ok/failed.
enum class BootState {
    Pending,  ///< not attempted — an earlier Stop row ended the walk
    Skipped,  ///< deliberately not attempted (disabled, or --no-boot)
    Started,  ///< loaded and registered: a live participant
    Refused,  ///< the host's admission policy said no, and said why
    Failed,   ///< the load itself failed (missing file, bad ABI, held role, …)
};

const char* name_of(BootState s) noexcept;

/// One row's outcome.
struct BootOutcome {
    BootEntry entry;
    BootState state = BootState::Pending;
    std::string detail;        ///< the refusal's or failure's own words
    std::uint64_t weave = 0;   ///< the WeaveId, when Started
};

/// The whole walk's outcome, in authored order.
struct BootReport {
    std::vector<BootOutcome> rows;

    std::size_t count(BootState s) const;
    /// Did every enabled row start? "Complete" is about the person's intent, so a
    /// deliberately disabled row does not make a boot incomplete and a refused one does.
    bool complete() const;
    /// The table, as a person reads it — one row per line, plus a summary line.
    std::vector<std::string> render() const;
};

} // namespace loom::host

#endif // ZEN_HOST_BOOT_PLAN_HPP
