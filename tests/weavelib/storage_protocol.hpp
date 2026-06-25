#ifndef ZEN_TESTS_STORAGE_PROTOCOL_HPP
#define ZEN_TESTS_STORAGE_PROTOCOL_HPP

// The StorageBroker wire protocol + the test-control shapes that drive a storage
// client mod. Shared by the broker .so, the client .so, and the policy test — each
// TU derives the same content-id from the same ZEN_SHAPE, so they agree across the
// .so boundary exactly as the gate requires.

#include <zen/weave/shape.hpp>
#include <zen/value.hpp> // loom::Bytes

#include <cstdint>
#include <string>

namespace storage {

// ---- the broker protocol (what role "storage" accepts / replies) ----------
// Names + version match grant_record.hpp's kStoragePut/kStorageGet/kStorageProtocolVersion,
// which the floor pre-wires every mod to reach.
struct StoragePut {
    std::string key;
    loom::Bytes value;
    ZEN_SHAPE(StoragePut, 1, ZEN_FIELD(key), ZEN_FIELD(value));
};
struct StorageGet {
    std::string key;
    ZEN_SHAPE(StorageGet, 1, ZEN_FIELD(key));
};
struct StorageValue {
    loom::Bytes value; // empty = the key was absent for this sender
    ZEN_SHAPE(StorageValue, 1, ZEN_FIELD(value));
};

// ---- test-control shapes (the test drives the client mod with these) ------
struct DoPut {
    std::string key;
    loom::Bytes value;
    ZEN_SHAPE(DoPut, 1, ZEN_FIELD(key), ZEN_FIELD(value));
};
struct DoGet {
    std::string key;
    ZEN_SHAPE(DoGet, 1, ZEN_FIELD(key));
};
struct Probe { // trigger a direct-disk syscall probe; the result rides back via the broker
    std::int64_t n;
    ZEN_SHAPE(Probe, 1, ZEN_FIELD(n));
};
// Drive the forging mod to emit a role-send StorageGet whose WIRE reply_to is forged to point at
// `victim` (a different Weave's id) — the confused-deputy attack the host must defeat.
struct DoForge {
    std::string key;
    std::int64_t victim; // the WeaveId the attacker forges into the role-send's reply_to
    ZEN_SHAPE(DoForge, 1, ZEN_FIELD(key), ZEN_FIELD(victim));
};

} // namespace storage

#endif // ZEN_TESTS_STORAGE_PROTOCOL_HPP
