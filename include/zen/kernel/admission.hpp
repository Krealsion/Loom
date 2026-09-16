// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_ADMISSION_HPP
#define ZEN_KERNEL_ADMISSION_HPP

// WHO DECIDES WHAT A LOADED ARTIFACT MAY DO.
//
// Until now the Kernel answered that question itself, and always the same way: a
// three-argument `load` minted `Grant{}.allow_any()` for every library it opened.
// That default had three doors — the direct load, the message-driven control door,
// and `load_candidate` — so it could not be closed at any one of them. It is closed
// here instead, at the artifact door itself, where all three pass.
//
// THE KERNEL STILL HAS NO OPINION. It does not decide; it ASKS, and it refuses when
// nobody is there to answer. An `AdmissionPolicy` is the host's — a person's, through
// whatever surface the host gives them. The Kernel's contribution is that there is now
// no way to load an artifact without either naming its grant at the call site or
// having installed somebody to name it.
//
// ---- THE THREE THINGS THIS DOES NOT CONFLATE -------------------------------
//
// A policy is asked at two STAGES, because "may this code run here" and "what may it
// say" are different questions with different answers and different enforcement:
//
//   AdmissionStage::Open   Before the library is opened. No static initializer has
//                          run, no symbol has been resolved. A refusal here means the
//                          file's native code never executes in this process. The
//                          verdict's `grant` is IGNORED.
//
//   AdmissionStage::Speak  After the manifest has been read and crossed the gate, and
//                          before the weave is registered on the bus. The verdict's
//                          `grant` becomes the artifact's BASELINE authority — what it
//                          may send and observe, checked at every delivery, forever
//                          (GATE-05: a baseline is admission-time and never changes).
//
// And the third thing, which is NOT here at all: OS containment. An in-process loaded
// weave shares this address space and is trusted at that level — see
// docs/guides/dynamic-weaves.md. Refusing at Open is the only containment the
// in-process kernel has, and it is a decision about a FILE, not a sandbox. What a
// policy grants at Speak bounds SPEECH and nothing else. The out-of-process isolation
// host (Linux) is where OS containment lives, and it has its own ledger. A host that
// tells a person otherwise is lying to them.
//
// ---- WHAT A DECLARATION IS WORTH -------------------------------------------
//
// AdmissionRequest::declared is the manifest's zen.CapabilityAsk — what the artifact
// SAYS it would like. It is advice: gated data, carried across the same wall as the
// accept-set, useful for showing a person what they are being asked for. It is never
// consulted to produce a grant, here or anywhere. A plan, a manifest or a filename
// does not grant itself power; the policy does, and the policy answers to a person.
//
// Note what the ask CANNOT say today: there is no send-side section in the manifest
// (schema_codec.hpp's encode_manifest emits referenced / accepted / state / requests /
// claims, and requests is network / filesystem / roles). So a host cannot learn from
// the artifact what shapes it wants to send, and send authority comes from the
// policy's own knowledge — a configuration a person wrote. That is the correct source
// either way; it does mean a console can show a person only what the artifact asked
// for in OS terms, not in message terms. Widening CapabilityAsk to carry a send-ask is
// an ABI change and is deliberately not made here.

#include <zen/kernel/schema_codec.hpp> // CapabilityAsk
#include <zen/switchboard/grant.hpp>

#include <functional>
#include <string>
#include <utility>

namespace loom {

/// Which question is being asked. See the header note: these are not two halves of
/// one decision, they are two decisions with different consequences.
enum class AdmissionStage {
    Open,  ///< may this file's native code run in this process? (`grant` ignored)
    Speak, ///< what may the loaded weave say? (`grant` becomes its baseline)
};

/// Which operation is asking. A policy that wants to treat a hot-reload of code a
/// person already approved differently from a first load has the fact available
/// without having to keep its own notes about what it has seen.
enum class AdmissionKind {
    Load,      ///< an ordinary load: a new participant enters the world
    Candidate, ///< a prepared candidate (PR-01): loaded sealed, outside the world
    Reload,    ///< reload-in-place: same WeaveId, new code behind it
};

const char* name_of(AdmissionStage s) noexcept;
const char* name_of(AdmissionKind k) noexcept;

/// What the host is being asked to admit. Every field is a fact the Kernel
/// established, never something the artifact claimed about itself — except
/// `declared`, which is exactly a claim and is labelled as one.
struct AdmissionRequest {
    AdmissionStage stage = AdmissionStage::Open;
    AdmissionKind kind = AdmissionKind::Load;

    /// The artifact name the operation used — the host's word for this artifact, not
    /// the library's. Two different files can be loaded under two names; one file can
    /// be loaded under two names.
    std::string name;

    /// The file, as given to the Kernel. A policy that cares about where an artifact
    /// lives should canonicalize this itself and say that it did.
    std::string path;

    /// The role it would bind, or empty. Load is the only moment a role can be bound,
    /// so a policy that cares which office an artifact may take has to care here.
    std::string role;

    /// SHA-256 of the file's bytes, truncated to 128 bits, lowercase hex — the same
    /// identity `so_content_hash` computes for the isolation ledger, so one host can
    /// key one record by both. Empty when the file could not be read at all (the
    /// Kernel will then fail the open anyway; a policy shown an empty id is being
    /// asked about something it cannot identify, and should say no).
    ///
    /// IT NAMES A BUILD BY ITS BYTES AND VOUCHES FOR NOBODY. A rebuild is a new
    /// identity — the honest default, and the reason a policy needs an answer to "the
    /// bytes changed" that is not merely "ask again". Authorship is the identity
    /// phase's; nothing here is a signature.
    std::string content_id;

    /// The manifest's ask — ADVICE, never authority (see the header note). Only
    /// populated at Speak, because it does not exist until the artifact has been
    /// opened and its manifest has crossed the gate.
    bool declared_present = false;
    CapabilityAsk declared{};
};

/// The host's answer. A refusal must say why, because the refusal is what a person
/// and the asking participant will both be shown.
struct AdmissionVerdict {
    bool admitted = false;

    /// The baseline, at Speak. Ignored at Open, and ignored for AdmissionKind::Reload
    /// at both stages: a reload keeps the incumbent's WeaveId and therefore its
    /// baseline, which GATE-05 says never changes. A policy that wants reloaded code
    /// to hold different authority must refuse the reload and have the host replace
    /// the artifact instead.
    Grant grant{};

    /// Why. On a refusal this is the sentence a person reads; on an admission it is
    /// an optional note (which rule matched, say) that a host may show or log.
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

/// The host's decision procedure. Called on the Kernel's thread, inside the load, with
/// no delivery in progress — so it may block on ordinary I/O (reading a policy file)
/// but must not expect to reach the bus, and MUST NOT WAIT ON A HUMAN: a policy that
/// needs a person's decision refuses now, and the person approves into a record that
/// the next attempt reads. That keeps a load out of the business of holding a delivery
/// open across somebody's coffee break, and it is why a refusal carries a reason
/// rather than a promise.
using AdmissionPolicy = std::function<AdmissionVerdict(const AdmissionRequest&)>;

/// THE EXPLICIT PERMISSIVE POLICY — what the Kernel used to do invisibly, now
/// something a host has to ask for BY NAME and explain in one string.
///
/// It exists so that "this host trusts every artifact it loads" is a sentence a
/// reader can grep for, instead of an absence they have to notice. It is not a
/// default, nothing installs it for you, and it is not a compatibility shim: a host
/// that installs it has made a real decision, and `why` is where it says so out loud —
/// it comes back in every verdict's note.
///
/// Legitimate holders are hosts whose artifacts are all their own build output: a test
/// harness (which holds the Switchboard, so it is inside the boundary by construction —
/// see zen/host/grant_wiring.hpp) and a fixed-cast demo. A host that loads a file a
/// person could have put there and installs this has decided to trust a stranger with
/// everything this process can say.
AdmissionPolicy trust_every_artifact(std::string why);

/// The policy a Kernel has until a host installs one: it admits nothing, at either
/// stage, and its reason names the missing decision rather than the artifact. A host
/// that has not decided has not thereby decided yes.
AdmissionPolicy admit_nothing();

} // namespace loom

#endif // ZEN_KERNEL_ADMISSION_HPP
