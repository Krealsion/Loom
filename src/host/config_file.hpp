// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_CONFIG_FILE_HPP
#define ZEN_HOST_CONFIG_FILE_HPP

// READING A FILE A PERSON WROTE, THROUGH THE GATE.
//
// The supplied host has two files a person edits by hand. Both are Values and both go
// through `admit()` like anything else that crosses a boundary — that part was never in
// question. What was in question is what the person has to TYPE.
//
// Zen's JSON is an ENVELOPE: `{"zen":1,"schema":"zen.HostBootPlan","version":1,
// "fields":{...}}`. The envelope exists so a reader can challenge bytes whose schema it
// does not already know — which is exactly right for something arriving off a socket,
// and exactly wrong for a configuration file whose schema the host knows by name and
// whose location the host chose. Asked to write one from scratch, a person cannot: the
// envelope version, the schema's wire name and its version are three facts they have no
// way to guess, and the refusal they get for guessing wrong is about an "envelope
// member", which names nothing they wrote.
//
// So the host supplies the envelope it already knows and admits the person's object
// inside it. Nothing about the gate is softened: the same `admit()` runs against the
// same declared schema, an unknown field is still refused, and a wrong type is still
// refused. The only thing that changed is who types the three facts the host already
// had. A file that DOES carry an envelope still reads — which matters, because the host
// writes the authority store itself and `compat::serialize` emits one.

#include <zen/schema.hpp>
#include <zen/value.hpp>

#include <memory>
#include <optional>
#include <string>

namespace loom::host {

/// Read `path` and admit its contents against `schema`.
///
/// Returns:
///   - a Value            the file parsed and conformed;
///   - nullopt, *missing  no such file (the caller decides whether that is an error;
///                        for both of this host's files it is not — it is where
///                        everyone starts);
///   - nullopt, *error    the file exists and is wrong, with the gate's own words.
std::optional<loom::Value> read_gated_file(const std::string& path,
                                           const std::shared_ptr<const loom::Schema>& schema,
                                           bool* missing, std::string* error);

/// Write `value` to `path` by way of a temp file and a rename, so a host killed
/// mid-write leaves the previous contents rather than a half-file.
///
/// The durability claimed is ATOMIC REPLACEMENT, not a write barrier: there is no fsync
/// here, because this host runs on Windows too and the isolation ledger's POSIX
/// fsync+fsync-parent path is not portable. A crash of the machine (as opposed to the
/// process) can therefore still lose the most recent decision. Said out loud rather
/// than implied by the word "atomic".
///
/// AND IT IS LAST-WRITER-WINS ACROSS PROCESSES. Two hosts run from one directory share
/// one authority file, each holding its own copy in memory, and each write replaces the
/// whole file — so an approval made in one is lost the next time the other writes. There
/// is no lock, and there deliberately is not one yet: a per-install decision file is a
/// single-operator thing, and a lock would be a mechanism claiming to solve a problem
/// (two people deciding at once) that it does not solve. Give a second host its own
/// `--authority` file.
bool write_gated_file(const std::string& path, const loom::Value& value, std::string* error);

} // namespace loom::host

#endif // ZEN_HOST_CONFIG_FILE_HPP
