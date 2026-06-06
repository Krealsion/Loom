#include <zen/value.hpp>

#include <stdexcept>
#include <utility>

namespace zen {

Cell Cell::integer(std::int64_t v) { return Cell(Storage{std::in_place_type<std::int64_t>, v}); }
Cell Cell::real(double v) { return Cell(Storage{std::in_place_type<double>, v}); }
Cell Cell::text(std::string v) { return Cell(Storage{std::in_place_type<std::string>, std::move(v)}); }
Cell Cell::boolean(bool v) { return Cell(Storage{std::in_place_type<bool>, v}); }
Cell Cell::bytes(Bytes v) { return Cell(Storage{std::in_place_type<Bytes>, std::move(v)}); }

Cell Cell::message(Value v) {
    return Cell(Storage{std::in_place_type<std::shared_ptr<Value>>,
                        std::make_shared<Value>(std::move(v))});
}

Cell Cell::message(std::shared_ptr<Value> v) {
    return Cell(Storage{std::in_place_type<std::shared_ptr<Value>>, std::move(v)});
}

Cell Cell::list(Array v) { return Cell(Storage{std::in_place_type<Array>, std::move(v)}); }

Kind Cell::kind() const noexcept {
    switch (data_.index()) {
    case 0:
        return Kind::Int;
    case 1:
        return Kind::Float;
    case 2:
        return Kind::Text;
    case 3:
        return Kind::Bool;
    case 4:
        return Kind::Bytes;
    case 5:
        return Kind::Message;
    default:
        return Kind::List;
    }
}

std::int64_t Cell::as_int() const { return std::get<std::int64_t>(data_); }
double Cell::as_float() const { return std::get<double>(data_); }
const std::string& Cell::as_text() const { return std::get<std::string>(data_); }
bool Cell::as_bool() const { return std::get<bool>(data_); }
const Bytes& Cell::as_bytes() const { return std::get<Bytes>(data_); }
const std::shared_ptr<Value>& Cell::as_message() const {
    return std::get<std::shared_ptr<Value>>(data_);
}
const Cell::Array& Cell::as_list() const { return std::get<Array>(data_); }

Value::Value(std::shared_ptr<const Schema> schema) : schema_(std::move(schema)) {
    if (!schema_) {
        throw std::invalid_argument("Value requires a non-null schema");
    }
    slots_.resize(schema_->fields().size());
}

Value& Value::set(std::string_view field, Cell value) {
    const auto& fields = schema_->fields();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == field) {
            slots_[i] = std::move(value);
            return *this;
        }
    }
    throw std::out_of_range("field '" + std::string(field) + "' is not declared by schema '" +
                            schema_->name() + "'");
}

const Cell* Value::get(std::string_view field) const noexcept {
    const auto& fields = schema_->fields();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == field) {
            return slots_[i] ? &*slots_[i] : nullptr;
        }
    }
    return nullptr;
}

bool Value::has(std::string_view field) const noexcept { return get(field) != nullptr; }

const Cell* Value::at(std::size_t i) const noexcept {
    if (i >= slots_.size()) {
        return nullptr;
    }
    return slots_[i] ? &*slots_[i] : nullptr;
}

Value construct_blind(std::shared_ptr<const Schema> schema, const CellSource& source) {
    Value v(schema);
    for (const Field& f : schema->fields()) {
        std::optional<Cell> cell = source(f);
        if (cell) {
            v.set(f.name, std::move(*cell));
        }
    }
    return v;
}

} // namespace zen
