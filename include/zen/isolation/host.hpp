#ifndef ZEN_ISOLATION_HOST_HPP
#define ZEN_ISOLATION_HOST_HPP

// Out-of-process Shard hosting. IsolationHost spawns a child zen-shard-host per
// Shard, bridges it to the bus through a proxy that *is* a Shard (so the
// Switchboard is unchanged), and supervises it: crash detection → bounded reload
// from a host-owned snapshot → quarantine. Everything is single-threaded — the
// only async is the child's reply *timing* — so the bus's FIFO and reentrancy
// guarantees hold. The Switchboard must outlive the host.
//
// Honest containment: an out-of-process Shard here is *isolated* (process
// boundary: crash-contained, cannot touch host memory) but *not sandboxed* (no OS
// enforcement of the grant's OS-capability flags yet — that is B3). The status
// reports this and never claims sandboxing.

#include <zen/isolation/channel.hpp>
#include <zen/registry.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/value.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h> // pid_t

namespace zen::isolation {

class OutOfProcessShard;

struct OutOfProcessResult {
    bool ok = false;
    zen::sb::ShardId id{};
    std::string error;
};

class IsolationHost {
public:
    /// `shard_host_exe` is the path to the zen-shard-host child executable.
    IsolationHost(zen::sb::Switchboard& bus, std::string shard_host_exe);
    ~IsolationHost();

    IsolationHost(const IsolationHost&) = delete;
    IsolationHost& operator=(const IsolationHost&) = delete;

    /// Mount a Shard out-of-process from `so_path` under `name`, with `grant`.
    /// Spawns a child, handshakes (reconstructs its schemas, caches its initial
    /// snapshot and policy), and registers the proxy on the bus.
    OutOfProcessResult mount(const std::string& name, const std::string& so_path,
                             zen::sb::Grant grant);

    /// One host-loop iteration, single-threaded: flush + drain child I/O
    /// (re-enqueue child output gated, refresh cached snapshots, note deaths) →
    /// pump the bus (proxies fire-and-continue) → supervise (reap dead children,
    /// drive bounded reload-then-quarantine). Pure in-process systems can keep
    /// calling Switchboard::pump() alone; this composes with it.
    void step();

    /// Step until `predicate()` holds or `max_steps` is reached. Sleeps briefly
    /// between steps so child processes make progress. Returns predicate's result.
    template <class Pred>
    bool run_until(Pred predicate, int max_steps);

    void unmount(const std::string& name);

    bool is_mounted(const std::string& name) const;
    bool quarantined(const std::string& name) const;
    /// The honest containment level of a hosted Shard.
    std::string containment(const std::string& name) const;

private:
    struct Link {
        std::string name;
        std::string so_path;
        zen::sb::ShardId id{};
        std::unique_ptr<Channel> channel; // null when no live child
        pid_t pid = -1;
        std::vector<std::shared_ptr<const Schema>> accept;
        std::shared_ptr<const Schema> state_schema;
        std::optional<Value> snapshot_value; // last good admitted snapshot (host-owned)
        std::string snapshot_bytes;          // its canonical bytes, for revival
        std::optional<Value> policy_value;
        OutOfProcessShard* proxy = nullptr;
        bool dead = false;
        bool death_signaled = false;
        bool quarantined = false;
    };

    friend class OutOfProcessShard;

    // Proxy-facing operations.
    void ship_deliver(Link& link, const zen::sb::Message& in);
    void respawn_and_revive(Link& link, const Value& state);

    // Spawn a child for `link`, wait for its Hello; optionally return its bytes.
    bool spawn_and_handshake(Link& link, std::string* manifest, std::string* policy,
                             std::string* snapshot, std::string& error);
    void reconstruct_and_cache(Link& link, const std::string& manifest, const std::string& policy,
                               const std::string& snapshot);
    void handle_child_frame(Link& link, const Incoming& frame);
    void on_child_death(Link& link);
    void recover(Link& link);
    void teardown_child(Link& link);

    zen::sb::Switchboard& bus_;
    std::string exe_;
    zen::Registry registry_; // reconstructed child schemas (decode deps + resolution)
    std::map<std::string, std::unique_ptr<Link>> links_;
};

template <class Pred>
bool IsolationHost::run_until(Pred predicate, int max_steps) {
    for (int i = 0; i < max_steps; ++i) {
        if (predicate()) {
            return true;
        }
        step();
        // Yield briefly so child processes make progress between iterations
        // (their replies/deaths are asynchronous to the host loop).
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

} // namespace zen::isolation

#endif // ZEN_ISOLATION_HOST_HPP
