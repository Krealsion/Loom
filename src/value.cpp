// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#include <zen/value.hpp>

#include <stdexcept>
#include <utility>

namespace loom {

Cell Cell::integer(std::int64_t v) { return Cell(Storage{std::in_place_type<std::int64_t>, v}); }
Cell Cell::real(double v) { return Cell(Storage{std::in_place_type<double>, v}); }
Cell Cell::text(std::string v) { return Cell(Storage{std::in_place_type<std::string>, std::move(v)}); }
Cell Cell::boolean(bool v) { return Cell(Storage{std::in_place_type<bool>, v}); }
Cell Cell::bytes(Bytes v) { return Cell(Storage{std::in_place_type<Bytes>, std::move(v)}); }

Cell Cell::message(Value v) {
    return Cell(Storage{std::in_place_type<std::shared_ptr<const Value>>,
                        std::make_shared<const Value>(std::move(v))});
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
    case 5: {
        const auto& nested = std::get<std::shared_ptr<const Value>>(data_);
        return nested && nested->schema().is_union() ? Kind::Union : Kind::Message;
    }
    default:
        return Kind::List;
    }
}

std::int64_t Cell::as_int() const { return std::get<std::int64_t>(data_); }
double Cell::as_float() const { return std::get<double>(data_); }
const std::string& Cell::as_text() const { return std::get<std::string>(data_); }
bool Cell::as_bool() const { return std::get<bool>(data_); }
const Bytes& Cell::as_bytes() const { return std::get<Bytes>(data_); }
const std::shared_ptr<const Value>& Cell::as_message() const {
    return std::get<std::shared_ptr<const Value>>(data_);
}
const Cell::Array& Cell::as_list() const { return std::get<Array>(data_); }

Value::Value(std::shared_ptr<const Schema> schema) : schema_(std::move(schema)) {
    if (!schema_) {
        throw std::invalid_argument("Value requires a non-null schema");
    }
    slots_.resize(schema_->is_union() ? 1 : schema_->fields().size());
}

std::size_t Value::index_of(std::string_view field) const {
    const auto& fields = schema_->fields();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == field) {
            return i;
        }
    }
    throw std::out_of_range(std::string(schema_->is_union() ? "case '" : "field '") +
                            std::string(field) + "' is not declared by schema '" +
                            schema_->name() + "'");
}

Value& Value::set(std::string_view field, Cell value) {
    const std::size_t i = index_of(field);
    if (!schema_->is_union()) {
        slots_[i] = std::move(value);
        return *this;
    }
    if (!schema_->fields()[i].carries) {
        throw std::invalid_argument("case '" + std::string(field) + "' of union '" +
                                    schema_->name() + "' carries nothing; choose it");
    }
    case_ = i;
    slots_[0] = std::move(value);
    return *this;
}

Value& Value::choose(std::string_view case_name) {
    if (!schema_->is_union()) {
        throw std::out_of_range("'" + schema_->name() + "' is a record, which has no case to choose");
    }
    const std::size_t i = index_of(case_name);
    if (schema_->fields()[i].carries) {
        throw std::invalid_argument("case '" + std::string(case_name) + "' of union '" +
                                    schema_->name() + "' carries a value; set it");
    }
    case_ = i;
    slots_[0].reset();
    return *this;
}

const Cell* Value::get(std::string_view field) const noexcept {
    const auto& fields = schema_->fields();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == field) {
            return at(i);
        }
    }
    return nullptr;
}

bool Value::has(std::string_view field) const noexcept {
    if (!schema_->is_union()) {
        return get(field) != nullptr;
    }
    const auto& fields = schema_->fields();
    return case_ != kNoCase && fields[case_].name == field;
}

std::optional<std::size_t> Value::chosen() const noexcept {
    if (!schema_->is_union() || case_ == kNoCase) {
        return std::nullopt;
    }
    return case_;
}

const Cell* Value::at(std::size_t i) const noexcept {
    if (schema_->is_union()) {
        return i == case_ && slots_[0] ? &*slots_[0] : nullptr;
    }
    if (i >= slots_.size()) {
        return nullptr;
    }
    return slots_[i] ? &*slots_[i] : nullptr;
}

Value construct_blind(std::shared_ptr<const Schema> schema, const CellSource& source) {
    Value v(schema);
    for (const Field& f : schema->fields()) {
        std::optional<Cell> cell = source(f);
        if (!cell) {
            continue;
        }
        if (!schema->is_union()) {
            v.set(f.name, std::move(*cell));
            continue;
        }
        if (f.carries) {
            v.set(f.name, std::move(*cell));
        } else {
            v.choose(f.name);
        }
        break; // a union holds the first case answered
    }
    return v;
}

} // namespace loom
