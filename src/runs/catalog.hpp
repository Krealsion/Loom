// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_RUNS_CATALOG_HPP
#define ZEN_RUNS_CATALOG_HPP

// The tool catalog: which packages exist, what each tool says it is, and which may run. Two
// files, two owners: `loom-tools.json` is the operator's (which package directories, and
// whether each may run: "any-revision", a revision digest, or nothing), `loom-tool.json` the
// package author's (each tool's inputs, outputs, needs and requested rules). Reading never runs
// anything; a revision is the SHA-256 over the package's files; a run executes a snapshot; a
// package may build on one level of others. Both are read strictly. docs/guides/sessions.md

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
    std::vector<std::string> uses; ///< packages of this catalog its tools import, by name
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

    /// A package this catalog read, by its name; nullptr when none (or it could not be read).
    const Package* package(const std::string& name) const;
};

/// The most packages a catalog names, the most tools a package holds, and the most files and
/// bytes a package may contain: a tool package is a small thing, and a snapshot is a copy.
inline constexpr std::size_t kMaxPackages = 64;
inline constexpr std::size_t kMaxToolsPerPackage = 32;
inline constexpr std::size_t kMaxPackageFiles = 512;
inline constexpr std::uintmax_t kMaxPackageBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kMaxUses = 4; ///< packages one package builds on

/// Read `file` and every package it names. Never throws: what could not be read is a problem.
Catalog read_catalog(const std::string& file);

/// The package's content digest (64 hex), or empty with `*why` set.
std::string package_revision(const std::filesystem::path& dir, std::string* why);

/// Copy the package's files (exactly the ones its digest covers) into `to`, which must not exist.
bool snapshot_package(const std::filesystem::path& from, const std::filesystem::path& to,
                      std::string* why);

/// May a run of `p` start at `revision`? `*why` says what the operator would have to decide.
bool approved(const Package& p, const std::string& revision, std::string* why);

/// THE PACKAGES `p` BUILDS ON, in its order: each listed in `c`, readable and using nothing
/// itself -- or false, with `*why` naming the first that is not. Approval is not judged here: a run
/// judges each one's snapshot, a description each one as it stands (`approved`).
bool uses_of(const Catalog& c, const Package& p, std::vector<const Package*>* used,
             std::string* why);

/// A run's inputs, checked against the tool's declaration: `json` must be one object whose keys
/// the tool declares, of the declared types; defaults are filled in. Returns the normalized JSON
/// object (declared order), or empty with `*why` set.
std::string check_inputs(const ToolSpec& tool, const std::string& json, std::string* why);

/// SHA-256 (64 hex) of one file's bytes, or empty when it cannot be read.
std::string file_sha256(const std::filesystem::path& file);

} // namespace loom::runs

#endif // ZEN_RUNS_CATALOG_HPP
