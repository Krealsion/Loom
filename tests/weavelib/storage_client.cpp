// A storage-client "mod": woven with WeaveBase, mounted on the floor
// (FsAccess::None, no network), holding only the floor's storage role send-rules. It
// persists and retrieves purely by messaging the "storage" broker — zero disk. The
// test drives it with DoPut/DoGet/Probe and reads the broker's StorageValue replies
// off the bus tap. It asks for nothing: the floor already grants what it needs.

#include "storage_protocol.hpp"

#include <zen/weave/weave.hpp>
#include <zen/kernel/export.hpp>

#include <cerrno>
#include <cstdint>
#include <string>

#include <fcntl.h>
#include <unistd.h>

using namespace loom;
using namespace storage;

namespace {

struct ClientState {
    std::int64_t replies = 0; // count of StorageValue replies seen (proofs read the tap)
    ZEN_SHAPE(ClientState, 1, ZEN_FIELD(replies));
};

class StorageClient : public WeaveBase<StorageClient, ClientState,
                                       Accept<DoPut, DoGet, Probe, StorageValue>,
                                       Emit<StoragePut, StorageGet>> {
public:
    void on(const DoPut& m, Mail& mail) {
        mail.send_to_role("storage", StoragePut{m.key, m.value});
    }
    void on(const DoGet& m, Mail& mail) { mail.send_to_role("storage", StorageGet{m.key}); }

    // A direct file open at the syscall level — FsAccess::None must make it fail. The
    // errno is carried back THROUGH the broker (the only reach the floor grants),
    // proving both "no disk of my own" (errno != 0) and "I persist via messages
    // alone". The B4 fs-probe discipline, expressed within the floor's authority.
    void on(const Probe&, Mail& mail) {
        std::int64_t code = 0;
        const int fd = ::open("/zen_mod_direct_write.txt", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            code = errno != 0 ? errno : -1;
        } else {
            ::close(fd);
            code = 0; // it succeeded — the floor leaked disk (the proof would catch this)
        }
        const std::string s = std::to_string(code);
        mail.send_to_role("storage", StoragePut{"__probe__", loom::Bytes(s.begin(), s.end())});
    }

    void on(const StorageValue&, Mail&) { ++state_.replies; }
};

} // namespace

ZEN_EXPORT_WEAVE(StorageClient)
