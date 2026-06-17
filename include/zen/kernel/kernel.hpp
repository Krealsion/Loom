#ifndef ZEN_KERNEL_KERNEL_HPP
#define ZEN_KERNEL_KERNEL_HPP

#include <zen/kernel/abi.h>
#include <zen/registry.hpp>
#include <zen/switchboard/switchboard.hpp>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace zen::kernel {

class HostAdapter; // host-side Shard wrapping a loaded library instance

/// Thrown host-side when a library hands back bytes that fail the gate, or a
/// thunk reports an error. The Kernel turns these into clean results.
class DllBoundaryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct LoadResult {
    bool ok = false;
    zen::sb::ShardId id{};
    std::string error;
};

struct ReloadResult {
    bool ok = false;               ///< the operation completed without a hard error
    bool reloaded = false;         ///< the Shard is now running the new library, state restored
    bool version_mismatch = false; ///< the new library's state schema version differs (clean refusal)
    std::string error;
};

/// Loads Shards from dynamic libraries and hosts them on a Switchboard. An owned
/// object, not a singleton. It reuses zen-core (gate, serialize, schema) and
/// zen-switchboard (routing, lifecycle) and adds only the library boundary:
/// everything a library hands back crosses as bytes and is re-admitted through
/// the same gate. The Switchboard must outlive the Kernel.
class Kernel {
public:
    explicit Kernel(zen::sb::Switchboard& bus);
    ~Kernel();

    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    /// Load `path`, mount its Shard on the bus under `name`, and return its id.
    LoadResult load(const std::string& name, const std::string& path);

    /// Hot-reload `name` from `new_path`: snapshot the live Shard to host-owned
    /// bytes, swap the library behind the same ShardId, and revive from the
    /// snapshot through the gate. A state-schema version mismatch is a clean
    /// refusal and the old library keeps running.
    ReloadResult reload_from(const std::string& name, const std::string& new_path);

    /// Stop the Shard, destroy its instance, then close the library — in that
    /// order, leaving no live pointer into the closed library.
    bool unload(const std::string& name);

    zen::sb::ShardId shard_id(const std::string& name) const;
    bool is_loaded(const std::string& name) const;
    std::vector<std::string> loaded() const;

private:
    struct Loaded {
        std::string name;
        void* lib = nullptr;
        const ZenShardAbi* abi = nullptr;
        HostAdapter* adapter = nullptr; // owned by the Switchboard
        zen::sb::ShardId id{};
    };

    struct Manifest {
        std::vector<std::shared_ptr<const Schema>> accepted;
        std::shared_ptr<const Schema> state;
    };

    Manifest reconstruct(const ZenShardAbi* abi, void* instance);

    zen::sb::Switchboard& bus_;
    zen::Registry registry_; ///< union of loaded Shards' schemas, for callback resolution
    std::map<std::string, Loaded> libs_;
};

} // namespace zen::kernel

#endif // ZEN_KERNEL_KERNEL_HPP
