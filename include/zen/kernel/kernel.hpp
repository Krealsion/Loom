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
    /// snapshot through the gate. A state-schema version mismatch is a clean
    /// refusal and the old library keeps running.
    ReloadResult reload_from(const std::string& name, const std::string& new_path);

    /// Stop the Weave, destroy its instance, then close the library — in that
    /// order, leaving no live pointer into the closed library.
    bool unload(const std::string& name);

    /// Unload whichever loaded library holds `role` (false if none does). The
    /// role is released by the unregister itself — the Switchboard clears a
    /// role when its holder is removed — so the slot is free for a successor.
    bool unload_role(const std::string& role);

    loom::WeaveId weave_id(const std::string& name) const;
    bool is_loaded(const std::string& name) const;
    std::vector<std::string> loaded() const;

    /// The role `name` was loaded under, or empty. The kernel's own map is the
    /// truth here: it is the thing that bound the role.
    std::string role_of(const std::string& name) const;

    /// What a role's holder is, as far as the kernel can honestly say.
    struct RoleQuery {
        loom::WeaveId holder{}; ///< the kernel-loaded holder, or 0 — see below
        bool accepts = false;   ///< holder declares (shape_name, shape_version) in its accept-set
    };

    /// Ask whether the holder of `role` declares a given shape in its accept-set —
    /// the "will you converse?" question, answered from data the kernel already
    /// reconstructs at load plus the bus's own published accept-set.
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

private:
    struct Loaded {
        std::string name;
        void* lib = nullptr;
        const ZenWeaveAbi* abi = nullptr;
        HostAdapter* adapter = nullptr; // owned by the Switchboard
        loom::WeaveId id{};
        std::string role{}; ///< the role slot it was bound to (empty if none)
    };

    struct Manifest {
        std::vector<std::shared_ptr<const Schema>> accepted;
        std::shared_ptr<const Schema> state;
    };

    Manifest reconstruct(const ZenWeaveAbi* abi, void* instance);

    loom::Switchboard& bus_;
    loom::Registry registry_; ///< union of loaded Weaves' schemas, for callback resolution
    std::map<std::string, Loaded> libs_;
};

} // namespace loom

#endif // ZEN_KERNEL_KERNEL_HPP
