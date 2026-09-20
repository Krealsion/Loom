// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_RUNS_CATALOG_HPP
#define ZEN_RUNS_CATALOG_HPP

// THE TOOL CATALOG: which packages exist, what each tool says it is, and which ones may run.
//
// Two files, two owners:
//
//   loom-tools.json   THE OPERATOR'S, in the session directory. Which package directories this
//                     manager knows, and for each one whether it may run: `"approve":
//                     "any-revision"` (every edit may run -- the author's own tools), a revision
//                     digest (exactly that content), or nothing (listed and described, never run).
//                     Optionally which Python interpreter to use and where `loom_session` lives.
//   loom-tool.json    THE PACKAGE AUTHOR'S, in the package directory. What each tool is for, its
//                     inputs and outputs, what it needs, the bus rules it asks its worker to be
//                     granted, the shapes it speaks, an example and what its refusals mean.
//
// READING NEVER RUNS ANYTHING. Listing and describing parse these two files and hash the package's
// bytes; no tool code is loaded or executed until a run of an approved package starts. A path is
// never approval: a package is approved by name in the operator's file, pinned by content unless
// the operator said otherwise.
//
// A PACKAGE'S REVISION is the SHA-256 over its files -- every regular file under its directory,
// by relative path, except Python's own caches -- so an edit anywhere in it is a new revision.
// A run executes a SNAPSHOT copied into its own directory, and its revision is the snapshot's
// digest: an edit made while a run is going changes the next run, never that one.
//
// Both files are read strictly: a key neither file defines is a problem the catalog reports,
// never a key it silently ignores.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace loom::runs {

struct InputSpec {
    std::string name;
    std::string type;         ///< text / int / bool / number
    bool required = false;
    bool has_default = false;
    std::string default_json; ///< the default as a JSON literal
    std::string help;
};

struct OutputSpec {
    std::string name;
    std::string help;
};

struct ToolSpec {
    std::string name;
    std::string script;       ///< the Python file, relative to the package directory
    std::string summary;
    std::string description;
    std::vector<InputSpec> inputs;
    std::vector<OutputSpec> outputs;
    std::vector<std::string> requires_;
    std::vector<std::string> asks;
    std::vector<std::string> vocabulary;
    std::vector<std::string> terms; ///< search words beyond the name and summary
    std::string example;
    std::string recovery;
};

struct Package {
    std::string declared;     ///< the path as the operator wrote it
    std::string dir;          ///< absolute
    std::string name;
    std::string version;
    std::string summary;
    std::vector<ToolSpec> tools;
    std::string revision;     ///< the content digest as it stands now
    std::string approve;      ///< "" / "any-revision" / a 64-hex digest
    std::string problem;      ///< why it could not be read, when it could not
};

struct Catalog {
    std::string file;
    bool present = false;
    std::string python;
    std::string runtime;
    std::vector<Package> packages;
    std::vector<std::string> problems;

    /// "<package>/<tool>" -> the package and the tool, or nullptrs.
    const Package* find(const std::string& id, const ToolSpec** tool) const;
};

/// The most packages a catalog names, the most tools a package holds, and the most files and
/// bytes a package may contain: a tool package is a small thing, and a snapshot is a copy.
inline constexpr std::size_t kMaxPackages = 64;
inline constexpr std::size_t kMaxToolsPerPackage = 32;
inline constexpr std::size_t kMaxPackageFiles = 512;
inline constexpr std::uintmax_t kMaxPackageBytes = 16u * 1024u * 1024u;

/// Read `file` and every package it names. Never throws: what could not be read is a problem.
Catalog read_catalog(const std::string& file);

/// The package's content digest (64 hex), or empty with `*why` set.
std::string package_revision(const std::filesystem::path& dir, std::string* why);

/// Copy the package's files (exactly the ones its digest covers) into `to`, which must not exist.
bool snapshot_package(const std::filesystem::path& from, const std::filesystem::path& to,
                      std::string* why);

/// May a run of `p` start at `revision`? `*why` says what the operator would have to decide.
bool approved(const Package& p, const std::string& revision, std::string* why);

/// A run's inputs, checked against the tool's declaration: `json` must be one object whose keys
/// the tool declares, of the declared types; defaults are filled in. Returns the normalized JSON
/// object (declared order), or empty with `*why` set.
std::string check_inputs(const ToolSpec& tool, const std::string& json, std::string* why);

/// SHA-256 (64 hex) of one file's bytes, or empty when it cannot be read.
std::string file_sha256(const std::filesystem::path& file);

} // namespace loom::runs

#endif // ZEN_RUNS_CATALOG_HPP
