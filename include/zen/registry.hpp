#ifndef ZEN_REGISTRY_HPP
#define ZEN_REGISTRY_HPP

#include <zen/schema.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace zen {

/// Thrown when a (name, version) already published with one shape is registered
/// again with a different shape. A published schema is immutable: to change a
/// shape, publish a new version.
class SchemaConflict : public std::runtime_error {
public:
    SchemaConflict(std::string name, std::uint32_t version, ContentId existing, ContentId incoming);

    const std::string& schema_name() const noexcept { return name_; }
    std::uint32_t schema_version() const noexcept { return version_; }
    ContentId existing_id() const noexcept { return existing_; }
    ContentId incoming_id() const noexcept { return incoming_; }

private:
    std::string name_;
    std::uint32_t version_;
    ContentId existing_;
    ContentId incoming_;
};

/// The kernel's grammar store: the schemas the system knows. Holds them as the
/// canonical shared owners that values reference, so a value's schema can never
/// dangle. Supports registering schemas discovered at runtime (the DLL case).
///
/// Threading: copy-on-write. The map is an immutable snapshot swapped wholesale
/// on each registration; readers take a shared lock only long enough to copy
/// the snapshot pointer, then traverse it lock-free. Registration takes an
/// exclusive lock and is expected to be rare. (On a toolchain with
/// std::atomic<std::shared_ptr> the snapshot load becomes wait-free with no API
/// change; GCC 11 lacks that specialization, hence the shared_mutex.)
class Registry {
public:
    Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) = delete;
    Registry& operator=(Registry&&) = delete;

    /// The outcome of a registration.
    struct Registration {
        std::shared_ptr<const Schema> schema; ///< the canonical owner (use this)
        bool inserted; ///< true if newly added; false if identical content already present
    };

    /// Publish a schema. Idempotent when the same (name, version) is already
    /// present with identical content (returns the existing canonical schema,
    /// inserted == false). Throws SchemaConflict on a same-key/different-content
    /// collision. Throws std::invalid_argument if `schema` is null.
    Registration register_schema(std::shared_ptr<const Schema> schema);

    /// Resolve a schema by its identity key, or nullptr if not present. Safe to
    /// call concurrently with registration.
    std::shared_ptr<const Schema> lookup(std::string_view name, std::uint32_t version) const;

    bool contains(std::string_view name, std::uint32_t version) const;
    std::size_t size() const;

private:
    using Key = std::pair<std::string, std::uint32_t>;
    using Map = std::map<Key, std::shared_ptr<const Schema>>;

    std::shared_ptr<const Map> snapshot() const;

    mutable std::shared_mutex mtx_;
    std::shared_ptr<const Map> current_;
};

} // namespace zen

#endif // ZEN_REGISTRY_HPP
