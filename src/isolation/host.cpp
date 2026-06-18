#include <zen/isolation/host.hpp>

#include <zen/kernel/schema_codec.hpp> // manifest_schema, decode_schema (shared encode/decode)
#include <zen/serialize.hpp>           // parse, admit, serialize
#include <zen/value.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace zen::isolation {

namespace {

constexpr int kChildFd = 3;            // the child reads its socket from this fd
constexpr int kHandshakeTimeoutMs = 5000;

// Bounded, blocking wait for the child's first Hello frame. The child has nothing
// to do but handshake, so a short wait is enough; a child that never speaks is a
// failed spawn, not a host hang.
bool wait_for_hello(Channel& ch, Incoming& out, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::vector<Incoming> frames;
    for (;;) {
        ch.poll(frames);
        for (auto& f : frames) {
            if (f.op == Op::Hello) {
                out = std::move(f);
                return true;
            }
        }
        frames.clear();
        if (ch.done()) {
            return false; // EOF/error before any Hello: the child died at startup
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{};
        pfd.fd = ch.fd();
        pfd.events = POLLIN;
        const int r = ::poll(&pfd, 1, static_cast<int>(left));
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false; // timeout or poll error
        }
        // readable: loop back to drain frames
    }
}

} // namespace

// ---- the proxy: a Shard, on the bus, backed by a child process ---------------
//
// To the Switchboard this is an ordinary Shard. handle() serializes and ships the
// message to the child and returns at once (fire-and-continue — a slow or hung
// child never blocks the bus). snapshot()/policy() return the host-owned cached
// values refreshed from the child's proactive Snapshot frames. revive() (re)spawns
// the child if dead and ships the state.
class OutOfProcessShard final : public zen::sb::Shard {
public:
    OutOfProcessShard(IsolationHost* host, IsolationHost::Link* link) : host_(host), link_(link) {}

    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return link_->accept;
    }
    void handle(const zen::sb::Message& in, zen::sb::Bus& /*bus*/) override {
        host_->ship_deliver(*link_, in);
    }
    zen::Value snapshot() const override { return *link_->snapshot_value; }
    zen::Value policy() const override { return *link_->policy_value; }
    void revive(const zen::Value& state) override { host_->respawn_and_revive(*link_, state); }

private:
    IsolationHost* host_;
    IsolationHost::Link* link_;
};

// ---- IsolationHost -----------------------------------------------------------

IsolationHost::IsolationHost(zen::sb::Switchboard& bus, std::string shard_host_exe)
    : bus_(bus), exe_(std::move(shard_host_exe)) {}

IsolationHost::~IsolationHost() {
    std::vector<std::string> names;
    names.reserve(links_.size());
    for (const auto& entry : links_) {
        names.push_back(entry.first);
    }
    for (const std::string& n : names) {
        unmount(n);
    }
}

bool IsolationHost::spawn_and_handshake(Link& link, std::string* manifest, std::string* policy,
                                        std::string* snapshot, std::string& error) {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        error = "socketpair failed";
        return false;
    }
    // The parent end gets CLOEXEC so it is not inherited by *other* children we
    // later spawn; the child end is placed at a known fd via dup2 (which clears
    // CLOEXEC, so it survives this child's exec).
    (void)::fcntl(sv[0], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], kChildFd);
    // Close the original ends in the child — but never the fd we just dup'd onto
    // (socketpair may hand back kChildFd itself, and closing it would leave the
    // child with no socket).
    if (sv[0] != kChildFd) {
        posix_spawn_file_actions_addclose(&fa, sv[0]);
    }
    if (sv[1] != kChildFd) {
        posix_spawn_file_actions_addclose(&fa, sv[1]);
    }

    const std::string fd_arg = std::to_string(kChildFd);
    char* argv[] = {const_cast<char*>(exe_.c_str()), const_cast<char*>(fd_arg.c_str()),
                    const_cast<char*>(link.so_path.c_str()), nullptr};

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, exe_.c_str(), &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(sv[1]); // the parent never uses the child end

    if (rc != 0) {
        ::close(sv[0]);
        error = "posix_spawn failed";
        return false;
    }
    link.pid = pid;
    link.channel = std::make_unique<Channel>(sv[0]);

    Incoming hello;
    if (!wait_for_hello(*link.channel, hello, kHandshakeTimeoutMs)) {
        error = "child did not complete the handshake";
        return false; // caller tears down
    }

    Cursor cursor(hello.payload);
    std::string_view m;
    std::string_view p;
    std::string_view s;
    if (!cursor.bytes(m) || !cursor.bytes(p) || !cursor.bytes(s)) {
        error = "malformed Hello frame";
        return false;
    }
    if (manifest != nullptr) {
        *manifest = std::string(m);
    }
    if (policy != nullptr) {
        *policy = std::string(p);
    }
    if (snapshot != nullptr) {
        *snapshot = std::string(s);
    }
    return true;
}

void IsolationHost::reconstruct_and_cache(Link& link, const std::string& manifest,
                                          const std::string& policy, const std::string& snapshot) {
    // Manifest -> the child's accept-set + state schema, re-admitted through the
    // gate against the kernel's meta-schema and reconstructed exactly as a loaded
    // library's manifest is.
    zen::Unverified um = zen::parse(manifest);
    zen::Admission am = zen::admit(um, zen::kernel::manifest_schema());
    if (!am.ok()) {
        throw std::runtime_error("manifest refused: " + am.first_error().message());
    }
    const zen::Value& mv = am.value();

    std::vector<std::shared_ptr<const Schema>> accept;
    for (const zen::Cell& c : mv.get("accepted")->as_list()) {
        auto s = zen::kernel::decode_schema(*c.as_message(), registry_);
        registry_.register_schema(s); // cross-mount agreement on (name, version)
        accept.push_back(std::move(s));
    }
    auto state = zen::kernel::decode_schema(*mv.get("state")->as_message(), registry_);
    registry_.register_schema(state);
    link.accept = std::move(accept);
    link.state_schema = state;

    // Policy -> cached, validated against the fixed lifecycle grammar.
    zen::Unverified up = zen::parse(policy);
    zen::Admission ap = zen::admit(up, zen::sb::lifecycle_policy_schema());
    if (!ap.ok()) {
        throw std::runtime_error("policy refused: " + ap.first_error().message());
    }
    link.policy_value = std::move(ap).value();

    // Initial snapshot -> cached as host-owned last-known-good (value + bytes).
    zen::Unverified us = zen::parse(snapshot);
    zen::Admission as = zen::admit(us, link.state_schema);
    if (!as.ok()) {
        throw std::runtime_error("snapshot refused: " + as.first_error().message());
    }
    link.snapshot_value = std::move(as).value();
    link.snapshot_bytes = snapshot;
}

OutOfProcessResult IsolationHost::mount(const std::string& name, const std::string& so_path,
                                        zen::sb::Grant grant) {
    if (links_.count(name) != 0) {
        return {false, {}, "already mounted: " + name};
    }
    auto link = std::make_unique<Link>();
    link->name = name;
    link->so_path = so_path;

    std::string manifest;
    std::string policy;
    std::string snapshot;
    std::string error;
    if (!spawn_and_handshake(*link, &manifest, &policy, &snapshot, error)) {
        teardown_child(*link);
        return {false, {}, "spawn/handshake failed: " + error};
    }
    try {
        reconstruct_and_cache(*link, manifest, policy, snapshot);
    } catch (const std::exception& e) {
        teardown_child(*link);
        return {false, {}, std::string("handshake refused by the gate: ") + e.what()};
    }

    auto proxy = std::make_unique<OutOfProcessShard>(this, link.get());
    OutOfProcessShard* raw = proxy.get();
    zen::sb::ShardId id;
    try {
        id = bus_.register_shard(std::move(proxy), std::move(grant));
    } catch (const std::exception& e) {
        teardown_child(*link);
        return {false, {}, std::string("register refused: ") + e.what()};
    }
    link->id = id;
    link->proxy = raw;
    links_.emplace(name, std::move(link));
    return {true, id, ""};
}

void IsolationHost::ship_deliver(Link& link, const zen::sb::Message& in) {
    if (!link.channel) {
        return; // no live child (dead/quarantined); the delivery is dropped
    }
    const std::string bytes = zen::serialize(in.payload);
    std::string frame;
    put_u64(frame, in.sender.value);
    put_u64(frame, in.reply_to.value);
    put_u64(frame, in.correlation);
    frame.append(bytes);
    link.channel->queue(Op::Deliver, frame); // flushed on the next step()
}

void IsolationHost::respawn_and_revive(Link& link, const zen::Value& state) {
    if (link.channel) {
        teardown_child(link); // a stale child should not exist here; be safe
    }
    std::string error;
    if (!spawn_and_handshake(link, nullptr, nullptr, nullptr, error)) {
        teardown_child(link); // spawn failed; recover() detects via a null channel
        return;
    }
    const std::string bytes = zen::serialize(state);
    link.channel->queue(Op::Revive, bytes); // child applies it, then ships a Snapshot
}

void IsolationHost::handle_child_frame(Link& link, const Incoming& frame) {
    if (frame.op == Op::Emit) {
        Cursor cursor(frame.payload);
        std::uint8_t kind = 0;
        std::uint64_t target = 0;
        std::uint64_t reply_to = 0;
        std::uint64_t correlation = 0;
        if (!cursor.u8(kind) || !cursor.u64(target) || !cursor.u64(reply_to) ||
            !cursor.u64(correlation)) {
            return; // malformed Emit header -> drop
        }
        const std::string_view payload = cursor.rest();

        // Re-admit the child's output through the one gate, host-side, exactly as
        // the kernel does for a loaded library's emitted message.
        zen::Unverified u = zen::parse(payload);
        std::shared_ptr<const Schema> door = bus_.resolve_schema(u.claimed_name(), u.claimed_version());
        if (!door) {
            return; // a schema the system does not know -> drop (cannot be gated)
        }
        zen::Admission a = zen::admit(u, door);
        if (!a.ok()) {
            return; // gate-refused (malformed/hostile child output) -> drop
        }
        // The sender is stamped from the connection (link.id), never from the
        // payload — the child has no way to express a sender. send_as/publish_as
        // then authorize against this Shard's grant at delivery (CapabilityDenied
        // on a violation), identical to the in-process ShardBus path.
        zen::sb::Message msg(std::move(a).value(), zen::sb::ShardId{}, zen::sb::ShardId{reply_to},
                             correlation);
        if (kind == kEmitPublish) {
            (void)bus_.publish_as(link.id, std::move(msg));
        } else {
            (void)bus_.send_as(link.id, zen::sb::ShardId{target}, std::move(msg));
        }
        return;
    }

    if (frame.op == Op::Snapshot) {
        // A fresh post-handle/post-revive snapshot. Admit it host-side; on success
        // it becomes the host-owned last-known-good. A malformed snapshot is
        // ignored — the previous good one stands.
        zen::Unverified u = zen::parse(frame.payload);
        zen::Admission a = zen::admit(u, link.state_schema);
        if (!a.ok()) {
            return;
        }
        link.snapshot_value = std::move(a).value();
        link.snapshot_bytes = frame.payload;
        return;
    }
    // Hello mid-stream or unknown ops: ignored.
}

void IsolationHost::recover(Link& link) {
    // The child is dead. Reap it, then drive bounded reload from the host-owned
    // snapshot. reload() checks/decrements the policy's max_reloads and, when the
    // budget allows, calls proxy->revive() — which respawns a fresh child and ships
    // the state. When the budget is exhausted, revive() is never called and the
    // Shard stays dead: quarantine.
    teardown_child(link);
    const zen::sb::ReviveOutcome ro = bus_.reload(link.id, link.snapshot_bytes);
    if (ro.revived && link.channel) {
        link.dead = false;
        link.death_signaled = false;
    } else {
        link.quarantined = true;
        link.dead = true;
        teardown_child(link); // ensure no child process lingers
    }
}

void IsolationHost::teardown_child(Link& link) {
    if (link.channel && !link.channel->done()) {
        link.channel->queue(Op::Shutdown, "");
        link.channel->flush(); // best-effort clean stop
    }
    link.channel.reset(); // closes the fd

    if (link.pid > 0) {
        // Give a clean exit a brief chance (so the child's own sanitizer checks
        // run), then force it. A crashed child is already a zombie and reaps at
        // once.
        bool reaped = false;
        for (int i = 0; i < 200; ++i) {
            int status = 0;
            const pid_t r = ::waitpid(link.pid, &status, WNOHANG);
            if (r == link.pid || r < 0) {
                reaped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!reaped) {
            ::kill(link.pid, SIGKILL);
            int status = 0;
            (void)::waitpid(link.pid, &status, 0);
        }
        link.pid = -1;
    }
}

void IsolationHost::step() {
    // (1) flush queued output to each child, then drain its input (re-enqueue
    //     emitted messages gated, refresh cached snapshots).
    for (auto& entry : links_) {
        Link& link = *entry.second;
        if (!link.channel) {
            continue;
        }
        link.channel->flush();
        std::vector<Incoming> frames;
        link.channel->poll(frames);
        for (const Incoming& f : frames) {
            handle_child_frame(link, f);
        }
    }

    // (2) pump the bus — proxies fire-and-continue, so this never blocks on a child.
    bus_.pump();

    // (3) supervise: detect deaths, drive bounded reload-then-quarantine.
    for (auto& entry : links_) {
        Link& link = *entry.second;
        if (link.quarantined) {
            continue;
        }
        const bool dead_now = !link.channel || link.channel->done();
        if (dead_now && !link.death_signaled) {
            link.dead = true;
            link.death_signaled = true;
            bus_.kill(link.id); // mark dead on the bus + emit Died (once per death)
        }
        if (link.dead) {
            recover(link);
        }
    }
}

void IsolationHost::unmount(const std::string& name) {
    auto it = links_.find(name);
    if (it == links_.end()) {
        return;
    }
    Link& link = *it->second;
    // Drop the proxy from the bus first (so no further delivery lands on it), then
    // stop the child. The proxy holds a Link* so it must die before the Link.
    std::unique_ptr<zen::sb::Shard> proxy = bus_.unregister_shard(link.id);
    teardown_child(link);
    proxy.reset();
    links_.erase(it);
}

bool IsolationHost::is_mounted(const std::string& name) const {
    return links_.count(name) != 0;
}

bool IsolationHost::quarantined(const std::string& name) const {
    auto it = links_.find(name);
    return it != links_.end() && it->second->quarantined;
}

std::string IsolationHost::containment(const std::string& name) const {
    auto it = links_.find(name);
    if (it == links_.end()) {
        return "not mounted";
    }
    const Link& link = *it->second;
    if (link.quarantined) {
        return "isolated (process boundary); quarantined: dead after exhausting reloads. "
               "Not sandboxed.";
    }
    return "isolated (process boundary): crash-contained, cannot corrupt host memory. "
           "Not sandboxed: the grant's OS-capability flags are not OS-enforced yet (B3).";
}

} // namespace zen::isolation
