// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_KERNEL_HPP
#define ZEN_KERNEL_KERNEL_HPP

#include <zen/kernel/abi.h>
#include <zen/kernel/admission.hpp>
#include <zen/registry.hpp>
#include <zen/switchboard/switchboard.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace loom {

class HostAdapter;   // host-side Weave wrapping a loaded library instance
class LoadedLibrary; // one open dynamic library, closed when the last holder lets go

/// Thrown host-side when a library hands back bytes that fail the gate, or a thunk reports an
/// error; the Kernel turns it into a clean result.
class DllBoundaryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// What this Kernel can say about one artifact name (KERN-03). `is_loaded()` is true of a live
/// service, a sealed candidate, an incumbent sealed for retirement and a dead weave alike; only
/// the first is a participant. Derived from the Switchboard each time it is asked.
enum class ArtifactStatus : std::uint8_t {
    NotLoaded,    ///< this Kernel holds no artifact under that name
    Live,         ///< loaded, registered, alive, unsealed: an ordinary participant
    Sealed,       ///< loaded and alive, but outside the world — a prepared candidate,
                  ///< or an incumbent sealed for private retirement
    Dead,         ///< loaded and registered, but killed and awaiting revival
    /// Loaded here, but no longer on the Switchboard: a host took the adapter through
    /// `unregister_weave` and still holds it, so the library is open.
    Unregistered,
};

const char* name_of(ArtifactStatus s) noexcept;

/// The host's artifact lifetime ledger: every dynamic instance created and destroyed and every
/// library opened and closed, counted process-wide where each happens, so a test can assert
/// each happened exactly once by taking a difference across an operation. Diagnostics only:
/// nothing reads it to decide anything, and it is never reset.
struct KernelLifetimeCounts {
    std::uint64_t instances_created = 0;   ///< abi->create() calls that yielded an instance
    std::uint64_t instances_destroyed = 0; ///< abi->destroy() calls the host made
    std::uint64_t libraries_opened = 0;    ///< dlopen/LoadLibrary calls that succeeded
    std::uint64_t libraries_closed = 0;    ///< dlclose/FreeLibrary calls
};

KernelLifetimeCounts kernel_lifetime_counts() noexcept;

struct LoadResult {
    bool ok = false;
    loom::WeaveId id{};
    std::string error;
};

struct ReloadResult {
    bool ok = false;               ///< the operation completed without a hard error
    bool reloaded = false;         ///< the Weave is now running the new library, state restored
    bool version_mismatch = false; ///< the new library's state schema version differs (clean refusal)
    std::string error;
};

/// Loads Weaves from dynamic libraries and hosts them on a Switchboard: an owned object, not a
/// singleton. It adds only the library boundary: everything a library hands back crosses as
/// bytes and is re-admitted through the gate. The Switchboard must outlive the Kernel.
///
/// The ownership of one dynamic artifact (KERN-02), one owner per link:
///
///   LoadedLibrary    the open library, shared by this Kernel's record and the adapter, and
///                    closed once, when the last holder lets go, so it cannot close while its
///                    code could run.
///   HostAdapter      the loom::Weave wrapping the instance, owned by the Switchboard from
///                    registration; its destructor destroys the instance.
///   Kernel::Loaded   the record: name, library, ABI, and a non-owning pointer to the adapter.
///
/// A record never outlives its adapter: the adapter's destructor erases it, and dropping a
/// record detaches the adapter first. An adapter may outlive its record, when a host took it
/// through `unregister_weave`: detached, it reaps nothing and keeps its share of the library
/// (`ArtifactStatus::Unregistered`). The detach and the identity check in `adapter_destroyed`
/// each cover the other, and the namesake-load case pins the pair. The adapter's destructor is
/// the removal notification, whoever destroyed it, so the Switchboard needs no hook.
/// docs/reference/kernel.md
class Kernel {
public:
    explicit Kernel(loom::Switchboard& bus);
    ~Kernel();

    /// The containment this hosting mode actually provides. The in-process Kernel isolates
    /// nothing on any platform; out-of-process isolation is the isolation host's, on Linux only,
    /// and the Windows backend is an explicit development opt-in.
    static constexpr const char* containment_note() {
#if defined(_WIN32)
        // ASCII only, deliberately: this line prints before any console setup
        // (codepage, VT) exists, so it must render on the barest conhost.
        return "unisolated; process-level only; no sandbox (Windows development/demo "
               "backend - isolation and the OS sandbox are Linux-only)";
#else
        return "in-process; trusted; no OS sandbox (out-of-process isolation is the "
               "isolation host's job)";
#endif
    }

    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    /// As the one-argument constructor, installing `policy` at once: a Kernel that is a member
    /// cannot call `admit_with` from a member initializer.
    Kernel(loom::Switchboard& bus, AdmissionPolicy policy);

    /// Install the host's admission policy, which decides what a loaded artifact may do
    /// (`zen/kernel/admission.hpp`). Until one is installed every policy-mediated load refuses
    /// and says nobody has decided. An empty policy resets to `admit_nothing()`, so clearing the
    /// policy never means trusting everything.
    void admit_with(AdmissionPolicy policy);

    /// The policy currently installed, for a host that wants to hand the same one
    /// to a second Kernel. Never null.
    const AdmissionPolicy& admission_policy() const noexcept { return admit_; }

    /// Load `path`, mount its Weave under `name`, and return its id, asking the installed
    /// admission policy what it may do. A non-empty `role` binds the Weave to that role, and
    /// load is the only moment one can be bound; a role already held is a clean failure and the
    /// incumbent keeps it.
    ///
    /// The policy is asked twice: before the library is opened (`AdmissionStage::Open`: may this
    /// file's code run here?), and after its manifest crosses the gate, before it is registered
    /// (`AdmissionStage::Speak`: what may it say?). Either refusal is a `LoadResult` failure
    /// with the policy's reason; after an `Open` refusal no code from the file has run. A load
    /// asked for by message through the control door lands here.
    /// to load something is asking the host's policy, not the Kernel's good nature.
    LoadResult load(const std::string& name, const std::string& path,
                    const std::string& role = "");

    /// As `load`, with the host naming the grant and bypassing the policy: the call site is the
    /// decision the policy would make, in code the host owns. The policy governs the loads a host
    /// did not write, the ones that arrive as messages. The grant's floor is empty, so a loaded
    /// weave that should observe Senses is granted that here.
    LoadResult load(const std::string& name, const std::string& path, const std::string& role,
                    Grant grant);

    /// Hot-reload `name` from `new_path`: snapshot the live Weave, swap the library behind the
    /// same WeaveId, and revive from the snapshot through the gate.
    ///
    /// The whole contract must match, or the reload is refused before the incumbent is touched:
    /// the state schema exactly, and the accepted-message schemas as an exact set, since the bus
    /// keeps routing by the accept-set it recorded at registration. Changing a contract is
    /// replacement's business. The candidate's closure is checked against the bus's live
    /// vocabulary first too, and a refused candidate's shapes leave with it (LIFE-08): nothing
    /// about the incumbent, its routing or the Loom's vocabulary changes.
    ///
    /// Not transactional past that point: the incumbent's instance is destroyed before revival
    /// is known to succeed, so a candidate whose revive() fails leaves it unavailable. Prepared
    /// replacement is the transactional path (PR-01..09).
    ///
    /// Reloading a weave a prepared replacement bound as its candidate ends that transaction,
    /// which discards the artifact; this reports `reloaded == false` with the reason, and closes
    /// what it opened.
    ///
    /// The admission policy is asked about the new bytes at both stages with
    /// `AdmissionKind::Reload`. Only its yes or no counts: the WeaveId and its baseline grant are
    /// kept (GATE-05), so a policy that will not let the new code run under the old authority
    /// means the artifact must be replaced, not reloaded.
    ReloadResult reload_from(const std::string& name, const std::string& new_path);

    /// Load an artifact as a prepared candidate (PR-01): everything an ordinary load does, then
    /// sealed, so it receives no publication, ordinary send or role traffic and may speak only to
    /// `coordinator`. Every refusal an ordinary load can give happens here first, before the
    /// live world is touched. It holds no role until `commit_candidate`. Asks the admission
    /// policy as `load` does, with `AdmissionKind::Candidate`.
    LoadResult load_candidate(const std::string& name, const std::string& path,
                              loom::WeaveId coordinator);

    /// As `load_candidate`, with the host's grant and no policy, as the four-argument `load`.
    LoadResult load_candidate(const std::string& name, const std::string& path,
                              loom::WeaveId coordinator, Grant grant);

    /// Unseal `candidate_name` and move `role` to it, as one change to what delivery can see
    /// (PR-07). False, changing nothing, unless the candidate is a live sealed weave and
    /// `incumbent_name` holds the role.
    bool commit_candidate(const std::string& incumbent_name, const std::string& candidate_name,
                          const std::string& role);

    /// Release this artifact: its Weave leaves the bus, its instance is destroyed, then its
    /// library is closed, and no call order can invert that, since the close happens when the
    /// last share goes. False if this Kernel holds no such name, including one a transaction
    /// already discarded. If a host took the adapter through `unregister_weave` and still holds
    /// it, this gives up the name and this Kernel's share and closes nothing: the adapter
    /// closes the library when it goes.
    bool unload(const std::string& name);

    /// Unload whichever loaded library holds `role` now; false if none, or if the holder is a
    /// native weave. Resolved from the Switchboard's role table, so a role moved by admission
    /// selects its actual holder. The role is released for a successor.
    bool unload_role(const std::string& role);

    loom::WeaveId weave_id(const std::string& name) const;

    /// Does this Kernel hold an artifact under that name? One coarse bit; `status()` says what
    /// kind. False once released, including when a transaction discarded it.
    bool is_loaded(const std::string& name) const;
    std::vector<std::string> loaded() const;

    /// What kind of thing this artifact is now, derived from the Switchboard. `Dead` outranks
    /// `Sealed`, which outranks `Live`: a dead weave receives nothing whatever its seal.
    ArtifactStatus status(const std::string& name) const;

    /// The role this artifact's weave holds now, asked of the Switchboard, or empty. Not the role
    /// it was loaded under: an admission moves a role with no Kernel call.
    std::string role_of(const std::string& name) const;

    /// A role's holder, as far as this Kernel can say.
    struct RoleQuery {
        loom::WeaveId holder{}; ///< the kernel-loaded holder, or 0 — see below
        bool accepts = false;   ///< holder declares (shape_name, shape_version) in its accept-set
    };

    /// Whether the holder of `role` accepts a shape, read from the bus's role table and published
    /// accept-set. `holder == 0` means no weave this Kernel loaded holds the role: it is unheld,
    /// or a native weave holds it. A caller that needs to tell those apart asks
    /// `Switchboard::role_holder`.
    RoleQuery query_role(const std::string& role, const std::string& shape_name,
                         std::uint32_t shape_version) const;

    /// Does `id` declare (shape_name, shape_version) in the accept-set the bus published for it?
    /// Read from the bus, never cached, so it cannot differ from what delivery matches; an
    /// unknown id is false. What to do with the answer is the caller's.
    bool accepts(loom::WeaveId id, const std::string& shape_name,
                 std::uint32_t shape_version) const;

private:
    friend class HostAdapter;

    struct Loaded {
        std::string name;
        /// Shared with the adapter, so the library outlives any code that could still run from
        /// it and closes once.
        std::shared_ptr<LoadedLibrary> lib;
        const ZenWeaveAbi* abi = nullptr;
        /// Non-owning (the Switchboard owns it), and never dangling: nothing destroys the
        /// adapter without erasing this record. The reverse does not hold.
        HostAdapter* adapter = nullptr;
        loom::WeaveId id{};
        /// This artifact's claim on the decoding registry (LIFE-08): its manifest's shapes, held
        /// while it is loaded and released by erasing this record, on every path out.
        loom::SchemaClaimScope schemas;
    };

    struct Manifest {
        std::vector<std::shared_ptr<const Schema>> accepted;
        std::shared_ptr<const Schema> state;
        /// The declared claim-set (SENSE-04); empty when none is declared.
        std::vector<std::shared_ptr<const Schema>> claims;
        /// The declared emit-set, claimed through the same wall as the accept-set so a divergent
        /// emitter refuses at load, and registered with the bus for discovery. Vocabulary, never
        /// authority. Empty when none is declared.
        std::vector<std::shared_ptr<const Schema>> emits;
        /// The manifest's `requests` section, the artifact's CapabilityAsk if it made one: shown
        /// to the admission policy as advice, and consulted by nothing here.
        bool declared_present = false;
        CapabilityAsk declared{};
        /// The claim that keeps these shapes resolvable (LIFE-08), owned by the caller of
        /// `reconstruct`: a candidate refused afterwards takes its vocabulary with it.
        /// docs/laws/lifecycle-laws.md
        loom::SchemaClaimScope schemas;
    };

    Manifest reconstruct(const ZenWeaveAbi* abi, void* instance);

    /// Called from `~HostAdapter` once the instance is destroyed, whoever destroyed it. `who` is
    /// checked against the record's adapter, so a late destructor never reaps a namesake that
    /// reused the name.
    void adapter_destroyed(const std::string& name, const HostAdapter* who) noexcept;

    /// Drop the record for `name`, detaching its adapter first. Idempotent; releases this
    /// Kernel's share of the library, which closes it unless an adapter still holds one.
    void forget(const std::string& name) noexcept;

    /// Is this id one of ours: loaded by this Kernel rather than native?
    const Loaded* record_for(loom::WeaveId id) const;

    /// The one loading machine. `explicit_grant == nullptr` asks the installed policy at both
    /// stages; non-null is the host's grant and the policy is not consulted.
    LoadResult load_impl(const std::string& name, const std::string& path,
                         const std::string& role, AdmissionKind kind,
                         const Grant* explicit_grant);

    /// `load_impl` plus the seal.
    LoadResult candidate_impl(const std::string& name, const std::string& path,
                              loom::WeaveId coordinator, const Grant* explicit_grant);

    /// Put the admission question to the installed policy, in one place for all three doors.
    /// True when admitted; on a refusal `*why` gets the sentence to report. `build` is the
    /// operation's one identity, shared by its two stages and read only if the policy asks.
    bool ask_admission(AdmissionStage stage, AdmissionKind kind, const std::string& name,
                       const std::string& path, const std::string& role,
                       const BuildIdentity& build, const CapabilityAsk* declared,
                       Grant* granted, std::string* why) const;

    loom::Switchboard& bus_;
    /// The decoding registry: the loaded artifacts' manifests, against which a manifest's nested
    /// references are resolved and a library's emitted bytes decoded. It compares content, but
    /// the one agreement wall every participant registers into is the Switchboard's; native
    /// weaves never enter this one, and it decides nothing about authority.
    loom::Registry registry_;
    std::map<std::string, Loaded> libs_;
    /// The host's decision procedure; never null, and `admit_nothing()` until a host installs one.
    AdmissionPolicy admit_;
};

} // namespace loom

#endif // ZEN_KERNEL_KERNEL_HPP
