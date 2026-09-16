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

} // namespace loom

#endif // ZEN_CONTENT_ID_HPP
