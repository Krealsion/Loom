// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// `loom-runs` -- the run manager, as the loadable artifact a session host boots.
//
//   { "name": "runs", "path": "<prefix>/lib/loom/loom-runs.<so|dll>", "role": "loom.runs" }
//
// The operator decides what it may run (the catalog, loom-tools.json) and what it may say and pass
// on to its workers (`authority allow runs ...`); docs/guides/sessions.md walks through both.

#include "run_manager.hpp"

#include <zen/kernel/export.hpp>

ZEN_EXPORT_WEAVE(loom::runs::RunManager)
