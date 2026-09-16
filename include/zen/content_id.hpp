// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_CONTENT_ID_HPP
#define ZEN_CONTENT_ID_HPP

// NAMING A FILE BY ITS BYTES.
//
// One function, in the core, because two different security-relevant records key
// themselves by exactly this identity and must not drift: the isolation host's
// persisted grant ledger (`so_content_hash`, zen/isolation/grant_record.hpp) and the
// in-process admission policy's view of an artifact (zen/kernel/admission.hpp). A
// second implementation of "hash the file" is a second answer to "is this the build I
// approved", and the two would eventually disagree about a truncation, an encoding or
// an error case.
//
// IT IS REAL WORK, AND ONLY THE ONE WHO NEEDS THE ANSWER PAYS FOR IT. The whole file is
// read and hashed — for a multi-megabyte Debug image, a large fraction of a second. The
// Kernel once did that for every load it put to a policy, including a policy that never
// looks at the answer, and that alone failed Zengine's replacement-timing tests. So an
// admission question carries a `BuildIdentity`, below, which does the work the first time
// somebody asks for it and never otherwise.

#include <cstdint>
#include <memory>
#include <string>

namespace loom {

/// SHA-256 of the file's bytes, truncated to 128 bits, as 32 lowercase hex
/// characters. Deterministic across runs and machines, so a persisted key survives a
/// host restart.
///
/// COLLISION-RESISTANT ON PURPOSE. This keys decisions about authority, so a fast
/// non-cryptographic name is not enough: FNV-1a's ~2^32 birthday resistance was cheap
/// enough for a determined attacker to forge a second artifact onto an existing
/// approval (audit F-1). Truncated SHA-256 raises that to ~2^128 second-preimage and
/// ~2^64 collision.
///
/// IT IS CONTENT-ADDRESSING, NOT AUTHENTICATION. It names a build by its bytes; it
/// vouches for nobody. It is not a MAC, it is not constant-time, and a signed author
/// identity remains the identity phase's job.
///
/// Throws `std::runtime_error` if the file cannot be read.
std::string file_content_id(const std::string& path);

/// As `file_content_id`, but answers with an empty string instead of throwing when
/// the file cannot be read — for the callers whose next step is to fail on the file
/// anyway, and who want to report *what they could not identify* rather than lose the
/// original error to an exception from the identifier.
std::string file_content_id_or_empty(const std::string& path) noexcept;

/// Process-wide count of whole-file identity scans — every `file_content_id` call, whether
/// or not the file could be read. For observability: a test takes a difference across an
/// operation to prove whether the operation hashed anything. Never reset; never an input.
std::uint64_t file_content_id_scans() noexcept;

/// WHICH BUILD, BY ITS BYTES — WORKED OUT WHEN SOMEBODY ASKS, AND THEN ONLY ONCE.
///
/// One observation of one file's `file_content_id`, made the first time `content_id()`,
/// `identified()` or `failure()` is called on this object or ANY COPY of it, and returned
/// unchanged from then on. Copies share the observation (and keep it alive), so asking
/// twice, at two stages or through a copy a policy kept, never reads the file twice and
/// never describes two different readings. An identity nobody asks for is never computed —
/// and there is no way to see one that was not asked for, so it cannot pass for a build
/// that was identified or for one that could not be.
///
/// ITS LIFETIME IS THE OBJECT'S, and whoever makes one decides what it spans. The Kernel
/// makes a fresh one for every admission operation (zen/kernel/admission.hpp), so no earlier
/// attempt's reading ever stands in for the bytes a later one is about to open. It records
/// nothing about the file but its bytes: a path, a size or a modification time is never
/// taken as proof that the bytes are the ones seen before.
class BuildIdentity {
public:
    /// Names no file: every observation is a failure that says so.
    BuildIdentity() = default;

    /// The file at `path`, read and hashed when first asked.
    explicit BuildIdentity(std::string path);

    /// An identity somebody already established — for a policy's own tests, or a host that
    /// identified the bytes itself. Asking reads nothing. An empty `content_id` is the failure
    /// "no identity was supplied".
    static BuildIdentity known(std::string content_id);

    /// The build's `file_content_id`, or empty when it could not be established.
    const std::string& content_id() const;

    /// True when `content_id()` names a build.
    bool identified() const;

    /// Why there is no identity (the file could not be read, or none was named); empty when
    /// there is one.
    const std::string& failure() const;

private:
    struct Observation; ///< in content_id.cpp: the path, a lock, and the one result
    const Observation& observe() const;

    std::shared_ptr<Observation> observation_;
};

} // namespace loom

#endif // ZEN_CONTENT_ID_HPP
