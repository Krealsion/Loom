// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_RECORDER_DUMP_HPP
#define ZEN_RECORDER_DUMP_HPP

// THE SMALLEST WITNESS THAT A MEMORY IS WORTH QUERYING (RTH-1 § 14).
//
// It is a development and test instrument, not a user-facing surface, and it
// lives in its own header for one reason: FORMATTING IS NOT THE API. Everything
// below reads `Recorder`'s structured records and turns them into text; nothing
// in `recorder.hpp` returns a line, so a Terminal, a Workshop panel, a debugger
// and a test each format for themselves and none of them is downstream of this
// file's choices.
//
// Deliberately NOT here: filtering, a query grammar, colour, paging, a widget
// tree, or anything that would make this a UI. It prints what is retained, in
// order, and says what is missing.

#include <zen/recorder/recorder.hpp>

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace loom {

/// How much a dump says.
struct DumpOptions {
    /// Print the retained payload bytes for each record that still has one. Off
    /// by default: a payload is the expensive half and a dump of an interactive
    /// session is mostly canvases.
    bool payloads = false;
    /// Print at most this many records, newest last. 0 = everything retained.
    std::size_t limit = 0;
    /// Print the per-shape tallies after the records.
    bool tallies = true;
};

/// Dump a live recorder: its bounds, its retained records in order, and what it
/// has forgotten. The header line is what makes the dump honest — a reader is
/// told the size of the window and the count of what fell out of it before they
/// read a single record.
void dump_history(const Recorder& rec, std::ostream& out, const DumpOptions& opts = {});

/// Dump records read back from a persistent log. The same rendering, so a log
/// and a live recorder are comparable line for line — which is how the phase
/// proves the log carries what the recorder promised.
void dump_records(const std::vector<HistoryRecord>& records, std::ostream& out,
                  const DumpOptions& opts = {});

/// One record as one line, for a caller that is doing its own layout.
std::string render_record(const HistoryRecord& rec);

} // namespace loom

#endif // ZEN_RECORDER_DUMP_HPP
