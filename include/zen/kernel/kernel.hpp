#ifndef ZEN_KERNEL_KERNEL_HPP
#define ZEN_KERNEL_KERNEL_HPP

#include <zen/kernel/abi.h>
#include <zen/registry.hpp>
#include <zen/switchboard/switchboard.hpp>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace loom {

class HostAdapter; // host-side Weave wrapping a loaded library instance

/// Thrown host-side when a library hands back bytes that fail the gate, or a
/// thunk reports an error. The Kernel turns these into clean results.
class DllBoundaryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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
    ReloadResult reload_from(const std::string& name, const std::string& new_path);

    /// Stop the Weave, destroy its instance, then close the library — in that
    /// order, leaving no live pointer into the closed library.
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
    bool is_loaded(const std::string& name) const;
    std::vector<std::string> loaded() const;

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
    struct Loaded {
        std::string name;
        void* lib = nullptr;
        const ZenWeaveAbi* abi = nullptr;
        HostAdapter* adapter = nullptr; // owned by the Switchboard
        loom::WeaveId id{};
        // NO CACHED ROLE (R2B-3b-3a). There was one, written at load and patched
        // by `commit_candidate`; admission moves a role with no Kernel call at
        // all, so it could only ever have been right by luck. The queries derive.
    };

    struct Manifest {
        std::vector<std::shared_ptr<const Schema>> accepted;
        std::shared_ptr<const Schema> state;
    };

    Manifest reconstruct(const ZenWeaveAbi* abi, void* instance);

    /// Is this id one of ours? The kernel-loaded/native distinction `query_role`
    /// and `unload_role` both need once the holder comes from the bus.
    const Loaded* record_for(loom::WeaveId id) const;

    loom::Switchboard& bus_;
    loom::Registry registry_; ///< union of loaded Weaves' schemas, for callback resolution
    std::map<std::string, Loaded> libs_;
};

} // namespace loom

#endif // ZEN_KERNEL_KERNEL_HPP
