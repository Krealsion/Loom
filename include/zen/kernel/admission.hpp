// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_ADMISSION_HPP
#define ZEN_KERNEL_ADMISSION_HPP

// Who decides what a loaded artifact may do: the host's `AdmissionPolicy`, which the Kernel
// asks at every door that loads (a direct load, the control door, a candidate, a reload) and
// which admits nothing until a host installs one. An artifact is loaded only with a grant named
// at the call site or a policy installed to name one. docs/reference/capabilities.md
//
// The policy is asked at two stages:
//
//   AdmissionStage::Open   Before the library is opened: no static initializer has run. A
//                          refusal means the file's native code never executes here. The
//                          verdict's `grant` is ignored.
//   AdmissionStage::Speak  After the manifest has crossed the gate, before the weave is
//                          registered. The verdict's `grant` becomes its baseline authority,
//                          what it may send and observe, for good (GATE-05).
//
// Neither is OS containment. An in-process loaded weave shares this address space
// (docs/guides/dynamic-weaves.md): refusing at Open is a decision about a file, and a grant
// at Speak bounds speech only. OS containment is the out-of-process isolation host's (Linux).
//
// `AdmissionRequest::declared` is the manifest's capability ask (zen.CapabilityAsk: network,
// filesystem, roles): what the artifact says it would like, shown as advice and never used to
// make a grant. The policy is shown nothing else the artifact declares, its emit-set included,
// and the ask has no send section, so send authority comes from the policy's own
// configuration.

#include <zen/content_id.hpp>           // BuildIdentity
#include <zen/kernel/schema_codec.hpp> // CapabilityAsk
#include <zen/switchboard/grant.hpp>

#include <functional>
#include <string>
#include <utility>

namespace loom {

/// Which question is being asked: two decisions with different consequences.
enum class AdmissionStage {
    Open,  ///< may this file's native code run in this process? (`grant` ignored)
    Speak, ///< what may the loaded weave say? (`grant` becomes its baseline)
};

/// Which operation is asking, so a policy can treat a reload of approved code differently from
/// a first load without keeping its own notes.
enum class AdmissionKind {
    Load,      ///< an ordinary load: a new participant enters the world
    Candidate, ///< a prepared candidate (PR-01): loaded sealed, outside the world
    Reload,    ///< reload-in-place: same WeaveId, new code behind it
};

const char* name_of(AdmissionStage s) noexcept;
const char* name_of(AdmissionKind k) noexcept;

/// What the host is asked to admit. Every field is a fact the Kernel established, except
/// `declared`, which is the artifact's own claim and labelled as one.
struct AdmissionRequest {
    AdmissionStage stage = AdmissionStage::Open;
    AdmissionKind kind = AdmissionKind::Load;

    /// The artifact name the operation used: the host's word for it, not the library's.
    std::string name;

    /// The file, as given to the Kernel. A policy that cares where an artifact lives
    /// canonicalizes this itself.
    std::string path;

    /// The role it would bind, or empty; load is the only moment a role can be bound.
    std::string role;

    /// Which build this is, by its bytes, computed only if the policy asks. `build.content_id()`
    /// is SHA-256 of the file truncated to 128 bits, lowercase hex, the identity
    /// `so_content_hash` computes for the isolation ledger. `build.identified()` is false when the
    /// file could not be read, with `build.failure()` saying why; a policy should then say no.
    ///
    /// A policy that never asks makes the Kernel read nothing. The first ask reads and hashes
    /// the file; one operation has one identity shared by both stages, so every answer describes
    /// one reading, and the next operation reads again. A copy of the request shares the reading.
    ///
    /// Check a pin at `Open`: there it is the bytes on disk before the library is opened, so a
    /// refusal means no code from the file ran. First asked at `Speak`, it describes the file
    /// after its code has run. It names a build and vouches for nobody: a rebuild is a new
    /// identity, and nothing here is a signature.
    BuildIdentity build;

    /// The manifest's capability ask: advice, never authority. Only at Speak, once the manifest
    /// has crossed the gate.
    bool declared_present = false;
    CapabilityAsk declared{};
};

/// The host's answer. A refusal says why, since that is what a person and the asking
/// participant are shown.
struct AdmissionVerdict {
    bool admitted = false;

    /// The baseline, at Speak. Ignored at Open, and at both stages of a Reload, which keeps the
    /// incumbent's WeaveId and so its baseline (GATE-05); a policy that wants reloaded code to
    /// hold different authority refuses the reload, and the host replaces the artifact.
    Grant grant{};

    /// Why: on a refusal, the sentence a person reads; on an admission, an optional note.
    std::string reason;

    static AdmissionVerdict admit(Grant g, std::string note = {}) {
        AdmissionVerdict v;
        v.admitted = true;
        v.grant = std::move(g);
        v.reason = std::move(note);
        return v;
    }
    static AdmissionVerdict refuse(std::string why) {
        AdmissionVerdict v;
        v.admitted = false;
        v.reason = std::move(why);
        return v;
    }
};

/// The host's decision procedure, called on the Kernel's thread inside the load with no delivery
/// in progress. It may block on ordinary I/O but cannot reach the bus, and must not wait on a
/// person: a policy that needs someone's decision refuses now, and the person approves into a
/// record the next attempt reads.
using AdmissionPolicy = std::function<AdmissionVerdict(const AdmissionRequest&)>;

/// The explicit permissive policy, which a host asks for by name with a reason, so "this host
/// trusts every artifact it loads" is something a reader can find. Nothing installs it by
/// default; `why` comes back in every verdict's note. For hosts whose artifacts are all their
/// own build output, such as a test harness or a fixed demo; a host that loads a file a person
/// could have put there and installs this trusts a stranger with everything the process can say.
AdmissionPolicy trust_every_artifact(std::string why);

/// The policy a Kernel has until a host installs one: it admits nothing at either stage, and its
/// reason names the missing decision rather than the artifact.
AdmissionPolicy admit_nothing();

} // namespace loom

#endif // ZEN_KERNEL_ADMISSION_HPP
