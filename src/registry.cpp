#include <zen/registry.hpp>

#include <mutex>
#include <utility>

namespace zen {

namespace {
std::string conflict_what(const std::string& name, std::uint32_t version) {
    return "schema '" + name + "' v" + std::to_string(version) +
           " is already published with a different shape (published schemas are immutable)";
}
} // namespace

SchemaConflict::SchemaConflict(std::string name, std::uint32_t version, ContentId existing,
                               ContentId incoming)
    : std::runtime_error(conflict_what(name, version)), name_(std::move(name)), version_(version),
      existing_(existing), incoming_(incoming) {}

Registry::Registry() : current_(std::make_shared<const Map>()) {}

std::shared_ptr<const Registry::Map> Registry::snapshot() const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    return current_;
}

Registry::Registration Registry::register_schema(std::shared_ptr<const Schema> schema) {
    if (!schema) {
        throw std::invalid_argument("Registry::register_schema requires a non-null schema");
    }
    Key key{schema->name(), schema->version()};

    std::unique_lock<std::shared_mutex> lk(mtx_);
    if (auto it = current_->find(key); it != current_->end()) {
        const std::shared_ptr<const Schema>& existing = it->second;
        if (existing->content_id() == schema->content_id()) {
            return {existing, false}; // identical re-registration is a no-op
        }
        throw SchemaConflict(schema->name(), schema->version(), existing->content_id(),
                             schema->content_id());
    }

    // Copy-on-write: publish a new immutable snapshot with the addition, then
    // swap it in. Existing reader snapshots keep the old map alive untouched.
    auto next = std::make_shared<Map>(*current_);
    (*next)[std::move(key)] = schema;
    current_ = std::move(next);
    return {std::move(schema), true};
}

std::shared_ptr<const Schema> Registry::lookup(std::string_view name, std::uint32_t version) const {
    std::shared_ptr<const Map> snap = snapshot();
    auto it = snap->find(Key{std::string(name), version});
    if (it == snap->end()) {
        return nullptr;
    }
    return it->second;
}

bool Registry::contains(std::string_view name, std::uint32_t version) const {
    return lookup(name, version) != nullptr;
}

std::size_t Registry::size() const { return snapshot()->size(); }

} // namespace zen
