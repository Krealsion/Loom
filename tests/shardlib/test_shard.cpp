// A real Shard, authored as a clean C++ zen::sb::Shard subclass and shipped as a
// .so with a single ZEN_EXPORT_SHARD line. No senses, no std::any — the same
// Shard one would compile in. Compile-time switches produce adversarial variants
// for the kernel's harness and the isolation host's harness:
//   ZEN_SHARD_MALFORMED_SNAPSHOT  — emit a snapshot missing a required field
//   ZEN_SHARD_MALFORMED_MESSAGE   — emit a message missing a required field
//   ZEN_SHARD_STATE_V2            — bump the state schema version (reload mismatch)
//   ZEN_SHARD_CRASH_ON_MAGIC      — abort mid-handle on the magic seq 0xDEAD
//   ZEN_SHARD_CRASH_ON_REVIVE     — abort on revive (drives reload-then-quarantine)
//   ZEN_SHARD_LOW_RELOADS         — max_reloads = 3 (fast crash-budget exhaustion)
//   ZEN_SHARD_SILENT              — handle never replies (liveness: cannot stall host)
//   ZEN_SHARD_NET_PROBE           — on handle, attempt a TCP connect and report the
//                                   errno (B3: proves the sandbox blocks the network)
//   ZEN_SHARD_FS_PROBE            — on handle, probe filesystem reach (read a secret,
//                                   write in/out of scratch, exec from scratch) and
//                                   report each errno (B4: proves the mount-ns view)
//   ZEN_SHARD_MEM_BOMB            — on handle (and revive), allocate a large resident
//                                   block to trip memory.max (B5: OOM-kill containment)
//   ZEN_SHARD_FORK_BOMB           — on handle, fork until it can't and report the count
//                                   (B5: proves pids.max bounds a fork-bomb)

#include <zen/kernel/export.hpp>
#include <zen/switchboard.hpp>
#include <zen/zen.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#ifdef ZEN_SHARD_NET_PROBE
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef ZEN_SHARD_FS_PROBE
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(ZEN_SHARD_MEM_BOMB)
#include <cstring>
#endif

#if defined(ZEN_SHARD_FORK_BOMB)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace zen;
using namespace zen::sb;

namespace {

std::shared_ptr<const Schema> ping_schema() {
    static const auto s = SchemaBuilder("Ping", 1).field("seq", Kind::Int).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> pong_schema() { // unused by the silent variant
    static const auto s = SchemaBuilder("Pong", 1).field("seq", Kind::Int).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> netresult_schema() { // only the net-probe variant
    static const auto s = SchemaBuilder("NetResult", 1).field("code", Kind::Int).build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> fsresult_schema() { // only the fs-probe variant
    static const auto s = SchemaBuilder("FsResult", 1)
                              .field("secret_read", Kind::Int)
                              .field("scratch_write", Kind::Int)
                              .field("outside_write", Kind::Int)
                              .field("noexec_exec", Kind::Int)
                              .build();
    return s;
}
[[maybe_unused]] std::shared_ptr<const Schema> forkresult_schema() { // only the fork-bomb variant
    static const auto s = SchemaBuilder("ForkResult", 1).field("forked", Kind::Int).build();
    return s;
}
std::shared_ptr<const Schema> counter_schema() {
#ifdef ZEN_SHARD_STATE_V2
    static const auto s = SchemaBuilder("Counter", 2)
                              .field("count", Kind::Int)
                              .field("note", Kind::Text, /*required=*/false)
                              .build();
#else
    static const auto s = SchemaBuilder("Counter", 1).field("count", Kind::Int).build();
#endif
    return s;
}

// Accepts Ping, replies Pong, and counts what it has handled as its state.
class TestShard : public Shard {
public:
    std::vector<std::shared_ptr<const Schema>> accepted_schemas() const override {
        return {ping_schema()};
    }

    void handle(const Message& in, Bus& bus) override {
        const std::int64_t seq = in.payload.get("seq")->as_int();
#ifdef ZEN_SHARD_CRASH_ON_MAGIC
        if (seq == 0xDEAD) {
            std::abort(); // crash mid-handle; the isolation host must contain this
        }
#endif
        ++count_;
#if defined(ZEN_SHARD_SILENT)
        (void)seq;
        (void)bus; // a deliberately silent Shard: it never replies
#elif defined(ZEN_SHARD_MALFORMED_MESSAGE)
        (void)seq;
        bus.send(in.reply_to, Message(Value(pong_schema()))); // 'seq' deliberately absent
#elif defined(ZEN_SHARD_NET_PROBE)
        (void)seq;
        // Instruction-level reach: open a TCP socket directly — the exact move a bus
        // grant cannot stop and only an OS sandbox can. Report the errno class so the
        // test distinguishes ENETUNREACH (no interface) from ECONNREFUSED (reachable).
        std::int64_t code = 0;
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            code = errno != 0 ? errno : -1;
        } else {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(1); // a port nothing listens on → ECONNREFUSED when reachable
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            code = rc == 0 ? 0 : errno;
            ::close(fd);
        }
        Value result(netresult_schema());
        result.set("code", Cell::integer(code));
        bus.send(in.reply_to, Message(std::move(result)));
#elif defined(ZEN_SHARD_FS_PROBE)
        (void)seq;
        // Instruction-level filesystem reach: read a secret outside the view, write
        // inside scratch, write outside it, and execute from scratch. Each reports its
        // errno (0 = succeeded) so the test reads the OS verdict off the bus — the
        // failures are the sandbox, not the grant.
        const auto try_read = [](const char* p) -> std::int64_t {
            const int fd = ::open(p, O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                return errno;
            }
            ::close(fd);
            return 0;
        };
        const auto try_write = [](const char* p) -> std::int64_t {
            const int fd = ::open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
            if (fd < 0) {
                return errno;
            }
            ::close(fd);
            return 0;
        };
        const std::int64_t secret = try_read("/tmp/zen_b4_secret.txt");
        const std::int64_t scratch = try_write("/scratch/probe.txt");
        const std::int64_t outside = try_write("/zen_outside.txt");
        std::int64_t noexec = 0;
        {
            const int fd = ::open("/scratch/prog", O_WRONLY | O_CREAT | O_CLOEXEC, 0755);
            if (fd < 0) {
                noexec = errno; // no scratch at all (None/ReadOnly) → report why
            } else {
                const char* script = "#!/bin/sh\nexit 0\n";
                (void)!::write(fd, script, std::strlen(script));
                ::close(fd);
                (void)::chmod("/scratch/prog", 0755);
                const pid_t gp = ::fork();
                if (gp == 0) {
                    ::execl("/scratch/prog", "prog", static_cast<char*>(nullptr));
                    ::_exit(errno); // exec refused (e.g. noexec → EACCES) → carry it out
                }
                int st = 0;
                (void)::waitpid(gp, &st, 0);
                noexec = WIFEXITED(st) ? WEXITSTATUS(st) : -1; // 0 = ran; EACCES = noexec
            }
        }
        Value result(fsresult_schema());
        result.set("secret_read", Cell::integer(secret));
        result.set("scratch_write", Cell::integer(scratch));
        result.set("outside_write", Cell::integer(outside));
        result.set("noexec_exec", Cell::integer(noexec));
        bus.send(in.reply_to, Message(std::move(result)));
#elif defined(ZEN_SHARD_MEM_BOMB)
        // Allocate a large resident block to trip memory.max. Held (not freed) so RSS
        // stays high; below the cgroup cap the kernel OOM-kills us mid-handle (the Pong
        // below is only reached if we survived — proving the kill is the cap).
        {
            const std::size_t bomb = 200UL * 1024 * 1024;
            char* p = static_cast<char*>(std::malloc(bomb));
            if (p != nullptr) {
                std::memset(p, 1, bomb);
            }
        }
        {
            Value pong(pong_schema());
            pong.set("seq", Cell::integer(seq));
            bus.send(in.reply_to, Message(std::move(pong)));
        }
#elif defined(ZEN_SHARD_FORK_BOMB)
        (void)seq;
        // Fork until the kernel refuses (pids.max), counting successes, then clean up.
        // Bounded ⇒ pids.max held; unbounded would fork the whole loop.
        {
            std::int64_t forked = 0;
            std::vector<pid_t> kids;
            for (int i = 0; i < 4000; ++i) {
                const pid_t k = ::fork();
                if (k == 0) {
                    ::pause(); // child: stay alive (counts against pids.max) until killed
                    ::_exit(0);
                }
                if (k < 0) {
                    break; // EAGAIN: hit pids.max
                }
                kids.push_back(k);
                ++forked;
            }
            for (pid_t k : kids) {
                ::kill(k, SIGKILL);
            }
            for (pid_t k : kids) {
                (void)::waitpid(k, nullptr, 0);
            }
            Value res(forkresult_schema());
            res.set("forked", Cell::integer(forked));
            bus.send(in.reply_to, Message(std::move(res)));
        }
#else
        Value pong(pong_schema());
        pong.set("seq", Cell::integer(seq));
        bus.send(in.reply_to, Message(std::move(pong)));
#endif
    }

    Value snapshot() const override {
        Value v(counter_schema());
#ifndef ZEN_SHARD_MALFORMED_SNAPSHOT
        v.set("count", Cell::integer(count_));
#endif
        return v;
    }

    Value policy() const override {
        Value v(lifecycle_policy_schema());
#ifdef ZEN_SHARD_LOW_RELOADS
        v.set("max_reloads", Cell::integer(3));
#else
        v.set("max_reloads", Cell::integer(8));
#endif
        v.set("revive_from_last_good", Cell::boolean(true));
        return v;
    }

    void revive(const Value& state) override {
#ifdef ZEN_SHARD_CRASH_ON_REVIVE
        (void)state;
        std::abort(); // crash on revive; drives bounded reload-then-quarantine
#elif defined(ZEN_SHARD_MEM_BOMB)
        (void)state;
        // Re-OOM on revive so a memory bomb exhausts its reload budget and quarantines.
        const std::size_t bomb = 200UL * 1024 * 1024;
        char* p = static_cast<char*>(std::malloc(bomb));
        if (p != nullptr) {
            std::memset(p, 1, bomb);
        }
        count_ = 0;
#else
        count_ = state.get("count")->as_int();
#endif
    }

private:
    std::int64_t count_ = 0;
};

} // namespace

ZEN_EXPORT_SHARD(TestShard)
