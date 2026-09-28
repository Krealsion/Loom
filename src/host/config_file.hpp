// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_CONFIG_FILE_HPP
#define ZEN_HOST_CONFIG_FILE_HPP

// Reading a file a person wrote, through the gate. The compat JSON is an envelope
// (`{"zen":1,"schema":...,"version":1,"fields":{...}}`) so a reader can challenge bytes of
// unknown schema; a configuration file's schema is known by name, and a person writing one
// cannot guess the envelope's three facts. So the host supplies the envelope and admits the
// person's object inside it, through the same `admit()`: an unknown field or a wrong type is
// still refused. A file that does carry an envelope still reads, as the host writes one.

#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <memory>
#include <optional>
#include <string>

namespace loom::host {

/// Read `path` and admit its contents against `schema`: a Value when the file parsed and
/// conformed; nullopt with `*missing` when there is no such file (not an error for either of
/// this host's files); nullopt with `*error` when the file is wrong, in the gate's own words.
std::optional<loom::Value> read_gated_file(const std::string& path,
                                           const std::shared_ptr<const loom::Schema>& schema,
                                           bool* missing, std::string* error);

/// Write `value` to `path` through a temp file and one replace (POSIX `rename(2)`, Windows
/// `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`), so a reader sees the previous complete record or
/// the new one and never neither. Not promised: a write barrier. There is no fsync, so a machine
/// crash can still lose the latest decision. One writer at a time is the caller's business: the
/// supplied host holds `loom::host::StoreLock` for its whole session (host/store_lock.hpp).
bool write_gated_file(const std::string& path, const loom::Value& value, std::string* error);

} // namespace loom::host

#endif // ZEN_HOST_CONFIG_FILE_HPP
