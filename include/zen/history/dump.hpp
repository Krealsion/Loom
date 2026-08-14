// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HISTORY_DUMP_HPP
#define ZEN_HISTORY_DUMP_HPP

// THE SMALLEST WITNESS THAT A MEMORY AND A RECORD ARE WORTH QUERYING.
//
// A development and test instrument, not a user-facing surface, and it lives in
// its own header for one reason: FORMATTING IS NOT THE API. Everything below reads
// the structured records `Recorder` and `Logger` return and turns them into text;
// nothing in `recorder.hpp` or `logger.hpp` returns a line, so a Terminal, a
// Workshop panel, a debugger and a test each format for themselves and none of
// them is downstream of this file's choices.
//
// Deliberately NOT here: filtering, a query grammar, colour, paging, a widget tree,
// or anything that would make this a UI. It prints what is retained, in order, and
// says what is missing.

#include <zen/history/logger.hpp>
#include <zen/history/recorder.hpp>

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace loom {

/// How much a dump says.
struct DumpOptions {
    /// Print the retained payload bytes for each record that still has one. Off by
    /// default: a payload is the expensive half and a dump of an interactive
    /// session is mostly canvases.
    bool payloads = false;
    /// Print at most this many records, newest last. 0 = everything retained.
    std::size_t limit = 0;
    /// Print the per-shape tallies after the records.
    bool tallies = true;
};

/// Dump a live recorder: its bounds, its retained records in order, and what it
/// has forgotten. The header line is what makes the dump honest — a reader is told
/// the size of the windows and the count of what fell out of them before they read
/// a single record.
void dump_history(const Recorder& rec, std::ostream& out, const DumpOptions& opts = {});

/// Dump records without a live recorder behind them.
void dump_records(const std::vector<HistoryRecord>& records, std::ostream& out,
                  const DumpOptions& opts = {});

/// Dump durable records read back from a Logger stream. It renders the ORIGIN of
/// every record, because a durable stream mixes what the bus said with what the
/// host said and a reader must never have to guess which is which.
void dump_log(const std::vector<LogRecord>& records, std::ostream& out,
              const DumpOptions& opts = {});

/// One record as one line, for a caller that is doing its own layout.
std::string render_record(const HistoryRecord& rec);
std::string render_log_record(const LogRecord& rec);

} // namespace loom

#endif // ZEN_HISTORY_DUMP_HPP
