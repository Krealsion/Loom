// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_KERNEL_HPP
#define ZEN_KERNEL_KERNEL_HPP

#include <zen/kernel/abi.h>
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

/// Thrown host-side when a library hands back bytes that fail the gate, or a
/// thunk reports an error. The Kernel turns these into clean results.
class DllBoundaryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// WHAT THIS KERNEL CAN HONESTLY SAY ABOUT ONE ARTIFACT NAME (R2B-3b-3a).
///
/// `is_loaded()` answers one bit — *do I hold this artifact* — and that bit is
/// true of things an operator must not confuse: a live service, a prepared
/// candidate nobody may address, an incumbent sealed for private retirement, and
/// a weave that is registered but dead. They are all "loaded"; only one of them
/// is a participant, and reporting the other three as live is exactly the stale
/// bookkeeping this phase exists to end.
///
/// Every state below is derived from the Switchboard at the moment it is asked.
/// The Kernel caches none of it.
enum class ArtifactStatus : std::uint8_t {
    NotLoaded,    ///< this Kernel holds no artifact under that name
    Live,         ///< loaded, registered, alive, unsealed: an ordinary participant
    Sealed,       ///< loaded and alive, but outside the world — a prepared candidate,
                  ///< or an incumbent sealed for private retirement
    Dead,         ///< loaded and registered, but killed and awaiting revival
    /// Loaded here, and the Switchboard no longer has this participant: somebody
    /// took ownership of the adapter through `unregister_weave` and still holds
    /// it. The artifact is real and its library is open — it simply is not on the
    /// bus. Named rather than folded into `Dead`, because "not a participant" and
    /// "not even registered" send an operator to different places.
    Unregistered,
};

const char* name_of(ArtifactStatus s) noexcept;

/// HOST-SIDE ARTIFACT LIFETIME LEDGER — diagnostics only, process-wide, monotonic.
///
/// Every dynamic instance this host creates and destroys, and every library it
/// opens and closes, counted at the one place each act happens. It exists because
/// *exactly once* is the whole ownership law of the dynamic seam, and a law
/// nobody can count is a law nobody can test: a caller takes a delta across an
/// operation and asserts the balance.
///
/// A LEDGER AND NEVER AN INPUT — no code in the Kernel reads it to decide
/// anything. Never reset, so a reader takes differences rather than absolutes,
/// and it exposes no pointer, handle or generation value.
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

/// Loads Weaves from dynamic libraries and hosts them on a Switchboard. An owned
/// object, not a singleton. It reuses loom (gate, serialize, schema) and
/// zen-switchboard (routing, lifecycle) and adds only the library boundary:
/// everything a library hands back crosses as bytes and is re-admitted through
/// the same gate. The Switchboard must outlive the Kernel.
///
/// ---- THE OWNERSHIP LAW OF ONE DYNAMIC ARTIFACT (R2B-3b-3a) -----------------
///
/// Two layers own different truths about the same artifact, and until this phase
/// they could disagree: the Switchboard owns live participation and destroys a
/// weave's adapter when a prepared replacement aborts; the Kernel owns the
/// library handle and the artifact name and was never told. One explicit chain,
/// now, with one owner per link:
///
///   LoadedLibrary        the open dynamic library. Held by SHARED ownership —
///                        by this Kernel's record AND by the adapter — and closed
///                        exactly once, when the last holder lets go. The adapter
///                        holding one is what makes "the library closed while code
///                        from it could still run" unrepresentable rather than
///                        merely avoided.
///
///   HostAdapter          the loom::Weave wrapping the library instance. Owned by
///                        the SWITCHBOARD from registration onward, exactly as
///                        every other weave is. Its destructor destroys the
///                        library instance — once, because a destructor runs once.
///
///   Kernel::Loaded       the artifact record: the name, the library, the ABI, and
///                        a NON-OWNING pointer to the adapter.
///
/// The invariant that ties them, and it is deliberately ONE-DIRECTIONAL: **a
/// Loaded record never outlives its HostAdapter.** Creating the record attaches
/// the adapter to it; the adapter's destructor erases the record; and dropping a
/// record detaches its adapter first. So the raw pointer in a record can never
/// dangle — not because callers are careful, but because nothing removes the
/// adapter without removing the record in the same breath.
///
/// THE CONVERSE IS FALSE, and saying so is the point. An adapter may outlive its
/// record: a host that takes ownership through `unregister_weave` and keeps it
/// still holds a working weave after this Kernel has let the name go. It is
/// detached when that happens, so it reaps nothing later, and it keeps its share
/// of the library so its own code stays mapped. `ArtifactStatus::Unregistered` is
/// what that looks like from outside — and it is why `unload` on such an artifact
/// gives up the name while deliberately closing nothing.
///
/// The detach and the namesake identity check in `adapter_destroyed` are a PAIR,
/// and each masks the other: with the detach in place a former adapter never
/// calls back at all, and with the check in place a call-back that did arrive
/// would not match. Cutting either alone leaves the suite green; cutting both
/// lets a predecessor's adapter reap its namesake's record, which the
/// namesake-load case pins.
///
/// THE ADAPTER'S DESTRUCTOR IS THE REMOVAL NOTIFICATION, and that is the whole
/// synchronization mechanism. It is the one object whose lifetime is exactly "this
/// artifact's instance is live", so it is right no matter WHO initiated the
/// destruction — this Kernel's own unload, a host calling `unregister_weave`, or a
/// transaction aborting and discarding its candidate deep inside a delivery. The
/// Switchboard needs no hook, no observer and no knowledge that a Kernel exists.
class Kernel {
public:
    explicit Kernel(loom::Switchboard& bus);
    ~Kernel();

    /// The honest containment statement for THIS kernel's hosting mode — never
    /// stronger than what is imposed. The in-process kernel isolates NOTHING on
    /// any platform (that is the out-of-process isolation host's job, which
    /// exists only on Linux); the Windows backend additionally exists only as
    /// an explicit development/demo opt-in, and this string is how a host or a
    /// banner says so without room for drift.
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

    /// Load `path`, mount its Weave on the bus under `name`, and return its id.
    /// A non-empty `role` binds the loaded Weave to that role slot — load is the
    /// only moment a role CAN be bound (Switchboard::register_weave is the sole
    /// binder, and roles are singletons), so a role-addressed consumer's reach
    /// across a replacement is decided here. Binding a role already held is a
    /// clean LoadResult failure, not a throw: the incumbent keeps it.
    LoadResult load(const std::string& name, const std::string& path,
                    const std::string& role = "");

    /// As `load`, with the host naming the artifact's grant explicitly (R2E-0).
    ///
    /// The default `load` gives a loaded weave permissive bus SENDS and no
    /// Sense read authority, because Grant's floor is empty and Senses did not
    /// change it: reading a claim is a deliberate host decision, not something a
    /// weave acquires by being loadable. A host that wants a loaded renderer,
    /// inspector or status panel to observe says so here — at the same moment it
    /// decides to load the thing at all, which is the moment it is already
    /// deciding how much to trust it.
    LoadResult load(const std::string& name, const std::string& path, const std::string& role,
                    Grant grant);

    /// Hot-reload `name` from `new_path`: snapshot the live Weave to host-owned
    /// bytes, swap the library behind the same WeaveId, and revive from the
    /// snapshot through the gate.
    ///
    /// RELOAD-IN-PLACE REQUIRES THE WHOLE CONTRACT, not merely the state shape.
    /// Two exact agreements are checked before the incumbent is replaced, and
    /// either one failing is a clean refusal with the incumbent untouched:
    ///   - the STATE schema, identical (name, version, content_id);
    ///   - the ACCEPTED-message schemas, an order-independent exact set match
    ///     against what the bus published for the incumbent.
    /// The accepted half exists because commit does NOT republish: rebind() swaps
    /// the ABI and instance behind the incumbent's adapter, and the Switchboard
    /// keeps routing by the accept-set recorded at the incumbent's registration.
    /// A candidate that changed its doors would therefore be routed to by the old
    /// contract — false composition truth, and it would make the control door's
    /// activation-participation question unanswerable after a reload. Requiring
    /// exact equality is what makes the retained set truthful. Evolving an
    /// accepted contract is REPLACEMENT's business (or a later explicit
    /// manifest-migration design), never reload's.
    ///
    /// WHAT "REFUSED BEFORE COMMIT" SCOPES TO, exactly. A contract mismatch is
    /// refused **before incumbent replacement and before any change to its
    /// published routing contract**: the incumbent's instance, library, WeaveId,
    /// role, state and published accepted-message set are all untouched, the
    /// candidate's behavior is never installed, and no activation is emitted.
    /// It does NOT mean "the Loom is unchanged". Reconstructing the candidate's
    /// manifest is what *produces* the schemas being compared, and reconstruct()
    /// admits them into this Kernel's dependency registry on the way — so a
    /// rejected candidate may already have monotonically admitted schemas there,
    /// observable through the cross-library agreement wall on a later load.
    /// (The Switchboard's own registry is untouched: only register_weave writes
    /// it, and a rejected candidate never registers.) Whether schema admission
    /// should participate in a future replacement transaction, or is
    /// intentionally monotonic, is an R2B design decision this does not make.
    ///
    /// HONEST REMAINING EDGE, not fixed here: this is validate-then-commit, not
    /// transactional. The incumbent's instance is destroyed and the adapter
    /// rebound BEFORE revival is known to have succeeded, so a candidate with an
    /// identical manifest whose revive() fails still leaves the incumbent
    /// unavailable. The prepared-candidate / rollback work is R2B's.
    ///
    /// A SECOND HONEST EDGE, named in R2B-3b-3a: reloading a weave that some
    /// prepared replacement has bound as its CANDIDATE ends that transaction —
    /// new code is a new participant — and ending it discards the candidate,
    /// which is this very artifact. The swap really did happen and is then
    /// undone by the discard, so it reports `reloaded == false` with the reason
    /// rather than claiming a success whose subject no longer exists. Nothing is
    /// left behind: the record is released and the library closed on the way out.
    ReloadResult reload_from(const std::string& name, const std::string& new_path);

    /// Load an artifact as a PREPARED CANDIDATE (R2B-3): opened, constructed,
    /// contract-validated and revivable — everything an ordinary load does — but
    /// SEALED, so it is not a participant in the live world. It receives no
    /// publications, no ordinary sends and no role traffic, and may speak only to
    /// `coordinator`.
    ///
    /// Every artifact-level refusal an ordinary load can produce (open failure,
    /// missing symbol, stale ABI, malformed manifest, schema disagreement) happens
    /// here too and identically, which is the point: a candidate that cannot load
    /// is discovered BEFORE the live world has been touched at all.
    ///
    /// It holds no role by construction. A role can only ever reach it through
    /// `commit_candidate`.
    LoadResult load_candidate(const std::string& name, const std::string& path,
                              loom::WeaveId coordinator);

    /// THE COMMIT (R2B-3): unseal `candidate_name` and move `role` to it from
    /// whoever holds it now, as one indivisible change to what ordinary delivery
    /// can see. Returns false — changing nothing — unless the candidate is a live
    /// sealed weave and the role is held by `incumbent_name`.
    bool commit_candidate(const std::string& incumbent_name, const std::string& candidate_name,
                          const std::string& role);

    /// Release this artifact: take the Weave off the bus, destroy its instance,
    /// then close the library — in that order, leaving no live pointer into a
    /// closed library. False if this Kernel does not hold that name, which
    /// INCLUDES an artifact a transaction already discarded: that is a truthful
    /// "not loaded", not a silent success over wreckage.
    ///
    /// The order is not maintained by this function's statements. Destroying the
    /// adapter destroys the instance and, in the same breath, releases one share
    /// of the library; this Kernel's share goes with the record. The close is
    /// whichever of those happens last, so no sequence of calls can invert it.
    ///
    /// ONE CASE DESTROYS NOTHING, deliberately: if a host took ownership of the
    /// adapter through `unregister_weave` and still holds it, this gives up the
    /// name and this Kernel's share and closes nothing at all — that adapter's
    /// code is still reachable, and it closes the library itself when it goes.
    bool unload(const std::string& name);

    /// Unload whichever loaded library holds `role` RIGHT NOW (false if none
    /// does, or if the holder is a native weave this Kernel did not load). The
    /// holder is resolved from the Switchboard's own role table, so a role that
    /// moved by admission — a prepared replacement committing, or a direct
    /// admission — selects the weave that actually holds it rather than whoever
    /// was loaded under that name. The role is released by the unregister itself,
    /// so the slot is free for a successor.
    bool unload_role(const std::string& role);

    loom::WeaveId weave_id(const std::string& name) const;

    /// Does this Kernel hold an artifact under that name? ONE BIT, and a coarse
    /// one — see `status()` for what kind of thing it is. It is false the moment
    /// the artifact is released, including when a transaction discarded the weave
    /// without this Kernel asking for anything.
    bool is_loaded(const std::string& name) const;
    std::vector<std::string> loaded() const;

    /// What kind of thing this artifact currently is, derived from the
    /// Switchboard every time it is asked. `Dead` outranks `Sealed` outranks
    /// `Live`: aliveness is the coarser fact, and a dead weave receives nothing
    /// whatever its seal says.
    ArtifactStatus status(const std::string& name) const;

    /// The role this artifact's weave holds RIGHT NOW, or empty.
    ///
    /// THE LIVE ROLE, ASKED OF THE AUTHORITY — not the role it was loaded under.
    /// The kernel's own map used to answer this, on the reasoning that load was
    /// the thing that bound the role. That stopped being true when admission
    /// learned to move a role (R2B-3b): a prepared replacement committing, or a
    /// host admitting a candidate directly, changes the holder with no Kernel
    /// call at all, and a cache with two mutators and one updater is a cache that
    /// lies. There is no load-time role kept beside this, deliberately: two
    /// independently mutable answers to one question is the shape being removed.
    std::string role_of(const std::string& name) const;

    /// What a role's holder is, as far as the kernel can honestly say.
    struct RoleQuery {
        loom::WeaveId holder{}; ///< the kernel-loaded holder, or 0 — see below
        bool accepts = false;   ///< holder declares (shape_name, shape_version) in its accept-set
    };

    /// Ask whether the holder of `role` declares a given shape in its accept-set —
    /// the "will you converse?" question, answered from the bus's own role table
    /// plus the bus's own published accept-set. Both halves come from the
    /// Switchboard, so this cannot drift from what delivery would do.
    ///
    /// `holder == 0` means **no kernel-loaded weave holds this role**, which
    /// conflates two states the kernel genuinely cannot tell apart: the role is
    /// unheld, or it is held by a NATIVE (host-mounted) weave whose accept-set the
    /// kernel never saw. Distinguishing them would need a role-holder query the
    /// Switchboard does not expose, and no caller needs the distinction: both are
    /// non-participants, and both take the same path. Native weaves are the host's
    /// own business.
    RoleQuery query_role(const std::string& role, const std::string& shape_name,
                         std::uint32_t shape_version) const;

    /// Does `id` declare (shape_name, shape_version) in the accept-set the bus
    /// published for it? The "will you converse?" question asked of one weave
    /// rather than of a role's holder — the shape the control door needs, since a
    /// freshly loaded weave may hold no role at all.
    ///
    /// The Switchboard's published accept-set is the truth and the ONLY truth
    /// consulted: no second cache lives here, so this can never drift from what
    /// delivery actually matches against. An unknown or unloaded id is a clean
    /// false (the bus answers an empty set), never an error — and this carries no
    /// lifecycle policy of its own; deciding what to do with the answer belongs
    /// entirely to the caller.
    bool accepts(loom::WeaveId id, const std::string& shape_name,
                 std::uint32_t shape_version) const;

private:
    friend class HostAdapter;

    struct Loaded {
        std::string name;
        /// SHARED with the adapter, so the library outlives any code that could
        /// still run from it and closes exactly once when both let go.
        std::shared_ptr<LoadedLibrary> lib;
        const ZenWeaveAbi* abi = nullptr;
        /// NON-OWNING (the Switchboard owns it), and never dangling: no path
        /// destroys the adapter without erasing this record in the same breath.
        /// The reverse does not hold — see the ownership law on the class.
        HostAdapter* adapter = nullptr;
        loom::WeaveId id{};
        /// THIS ARTIFACT'S CLAIM ON THE DEPENDENCY REGISTRY (BL-0). Its manifest's
        /// referenced, accepted, state and claim shapes, claimed as one and held
        /// for exactly as long as the artifact is loaded. Erasing this record is
        /// what releases them, so `unload`, a reaped adapter and a throw on the
        /// way in all clean up by the same mechanism.
        loom::SchemaClaimScope schemas;
    };

    struct Manifest {
        std::vector<std::shared_ptr<const Schema>> accepted;
        std::shared_ptr<const Schema> state;
        /// The declared claim-set (R2E-0/v6) — the Senses this artifact says it
        /// can claim. Empty when it declares none.
        std::vector<std::shared_ptr<const Schema>> claims;
        /// WHAT KEEPS THE THREE ABOVE RESOLVABLE, AND FOR HOW LONG (BL-0).
        /// `reconstruct` is the only thing that publishes a candidate's
        /// vocabulary, and it publishes it into THIS — a claim the caller owns
        /// from the moment it returns. A candidate the compatibility check then
        /// refuses is a Manifest that goes out of scope, and its schemas go with
        /// it. That is how a rejected reload stopped leaving vocabulary behind.
        loom::SchemaClaimScope schemas;
    };

    Manifest reconstruct(const ZenWeaveAbi* abi, void* instance);

    /// THE ADAPTER TELLING US IT IS GONE — called from `~HostAdapter`, after the
    /// library instance has been destroyed and before the adapter releases its
    /// share of the library. Whoever destroyed it, this runs.
    ///
    /// `who` is checked against the record's own adapter rather than trusted:
    /// an artifact name may be reused after its predecessor was discarded, and a
    /// late destructor must never reap a namesake's record.
    void adapter_destroyed(const std::string& name, const HostAdapter* who) noexcept;

    /// Drop the record for `name`, detaching its adapter first so the adapter no
    /// longer names a record that has gone. Idempotent; releases this Kernel's
    /// share of the library, which closes it unless an adapter still holds one.
    void forget(const std::string& name) noexcept;

    /// Is this id one of ours? The kernel-loaded/native distinction `query_role`
    /// and `unload_role` both need.
    const Loaded* record_for(loom::WeaveId id) const;

    loom::Switchboard& bus_;
    loom::Registry registry_; ///< union of loaded Weaves' schemas, for callback resolution
    std::map<std::string, Loaded> libs_;
};

} // namespace loom

#endif // ZEN_KERNEL_KERNEL_HPP
