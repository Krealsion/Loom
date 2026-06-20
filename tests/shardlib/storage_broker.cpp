// The StorageBroker: an ecosystem Shard (not host code), shipped as a .so and mounted
// out-of-process at the TCB tier with FsAccess::WriteScoped(storage_root) and role
// "storage". It holds the real (scoped) disk capability on behalf of untrusted mods
// that have none, and namespaces each mod's data by the STAMPED sender — mail.sender(),
// host-stamped from the connection, which a mod cannot forge — so mod A can never read
// mod B's data, and neither can touch the host home (the bind is contained to
// storage_root). The key is hex-encoded into the filename, so a hostile key (with '/'
// or "..") cannot escape the sender's subdir. The value is opaque bytes.
//
// Storage is SESSION-SCOPED: the sender is the ephemeral runtime ShardId, so a mod's
// subdir changes across host restarts. Persistent-across-restart needs a first-class
// Shard identity — the named successor phase.

#include "storage_protocol.hpp"

#include <zen/author/shard.hpp>
#include <zen/kernel/export.hpp>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>

using namespace zen::author;
using namespace storage;

namespace {

// storage_root is bound at /scratch in the broker's view (the persistent-WriteScoped
// extension). The broker reaches only here — never the host filesystem.
constexpr const char* kRoot = "/scratch";

std::string hex_key(const std::string& key) {
    static const char* const hex = "0123456789abcdef";
    std::string out;
    out.reserve(key.size() * 2);
    for (char ch : key) {
        const unsigned char c = static_cast<unsigned char>(ch);
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0x0F]);
    }
    return out;
}

std::string sender_dir(zen::sb::ShardId sender) {
    return std::string(kRoot) + "/" + std::to_string(sender.value);
}

struct BrokerState {
    std::int64_t puts = 0; // a trivial in-memory index; the data is durable on disk
    ZEN_SHAPE(BrokerState, 1, ZEN_FIELD(puts));
};

class StorageBroker : public ShardBase<StorageBroker, BrokerState, Accept<StoragePut, StorageGet>,
                                       Emit<StorageValue>> {
public:
    void on(const StoragePut& m, Mail& mail) {
        const std::string dir = sender_dir(mail.sender()); // scoped by the STAMPED sender
        (void)::mkdir(dir.c_str(), 0700);                  // idempotent: the sender's keyspace
        const std::string path = dir + "/" + hex_key(m.key);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(m.value.data()),
                  static_cast<std::streamsize>(m.value.size()));
        ++state_.puts;
    }

    void on(const StorageGet& m, Mail& mail) {
        const std::string path = sender_dir(mail.sender()) + "/" + hex_key(m.key);
        zen::Bytes value;
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            const std::string s = ss.str();
            value.assign(s.begin(), s.end());
        }
        mail.reply(StorageValue{std::move(value)}); // empty value = absent for this sender
    }
};

} // namespace

ZEN_EXPORT_SHARD(StorageBroker)
