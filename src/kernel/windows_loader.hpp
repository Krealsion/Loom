// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#pragma once

#if defined(_WIN32)

#include <string>

namespace loom::detail {

/// Opens the weave at `path`, read in the program's code page as every narrow path is (UTF-8 in a
/// program that runs in the UTF-8 code page), resolved against the current folder. The libraries
/// it needs are looked for beside it, in the program's folder, in the system folder and in any
/// folder the host added; never in the current folder or on PATH (docs/reference/kernel.md).
/// Returns the module, or nullptr with `error` saying, in one line of UTF-8, why it was refused.
void* windows_open_library(const std::string& path, std::string& error);

} // namespace loom::detail

#endif
