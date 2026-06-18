#ifndef ZEN_KERNEL_CONTROL_HPP
#define ZEN_KERNEL_CONTROL_HPP

// The kernel's message door: operate the kernel like everything else — by sending
// it messages. A control Shard accepts LoadLibrary / ReloadLibrary / UnloadLibrary
// and calls the kernel's existing load / reload_from / unload. The right to send
// those shapes to the control Shard is the **load capability** — the canonical
// dangerous grant. A Shard holding it can drive the kernel by message; a Shard
// without it is denied at delivery (CapabilityDenied), protecting the single most
// dangerous surface in the system with the same capability-gating as everything
// else.

#include <zen/author.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/switchboard.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace zen::kernel {

struct LoadLibrary {
    std::string name;
    std::string path;
    ZEN_SHAPE(LoadLibrary, 1, ZEN_FIELD(name), ZEN_FIELD(path));
};

struct ReloadLibrary {
    std::string name;
    std::string path;
    ZEN_SHAPE(ReloadLibrary, 1, ZEN_FIELD(name), ZEN_FIELD(path));
};

struct UnloadLibrary {
    std::string name;
    ZEN_SHAPE(UnloadLibrary, 1, ZEN_FIELD(name));
};

/// The control Shard's state: how many operations it has performed.
struct ControlState {
    std::int64_t ops;
    ZEN_SHAPE(ControlState, 1, ZEN_FIELD(ops));
};

/// A Shard whose handlers drive a Kernel. It emits nothing (its authority is its
/// accept-set being reachable, gated by the *sender's* load capability).
class ControlShard
    : public zen::author::ShardBase<ControlShard, ControlState,
                                    zen::author::Accept<LoadLibrary, ReloadLibrary, UnloadLibrary>> {
public:
    explicit ControlShard(Kernel& kernel) : kernel_(&kernel) {}

    void on(const LoadLibrary& m, zen::author::Mail&) {
        ++state_.ops;
        (void)kernel_->load(m.name, m.path);
    }
    void on(const ReloadLibrary& m, zen::author::Mail&) {
        ++state_.ops;
        (void)kernel_->reload_from(m.name, m.path);
    }
    void on(const UnloadLibrary& m, zen::author::Mail&) {
        ++state_.ops;
        (void)kernel_->unload(m.name);
    }

private:
    Kernel* kernel_;
};

/// Register the control Shard on `bus` and return its id. It emits nothing, so it
/// is mounted with the empty (emit-default) grant.
inline zen::sb::ShardId mount_control(Kernel& kernel, zen::sb::Switchboard& bus) {
    return zen::author::mount<ControlShard>(bus, kernel);
}

/// The grant that lets a Shard drive the kernel: permission to send the three
/// control shapes to the control Shard, and only to it.
inline zen::sb::Grant load_capability(zen::sb::ShardId control) {
    zen::sb::Grant g;
    g.allow(LoadLibrary::zen_name, LoadLibrary::zen_version, control);
    g.allow(ReloadLibrary::zen_name, ReloadLibrary::zen_version, control);
    g.allow(UnloadLibrary::zen_name, UnloadLibrary::zen_version, control);
    return g;
}

} // namespace zen::kernel

#endif // ZEN_KERNEL_CONTROL_HPP
