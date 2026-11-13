// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_CREATION_HPP
#define ZEN_KERNEL_CREATION_HPP

// The value a weave is made with, and the one judgement of it: the value a load names, admitted
// by the gate against the shape the image declares, before any of the weave's code sees it.
// Header-only, so the kernel and the isolation host judge a making alike.
// docs/reference/dynamic-abi.md#the-creation-value

#include <zen/kernel/schema_codec.hpp>
#include <zen/registry.hpp>
#include <zen/serialize.hpp>
#include <zen/value.hpp>

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace loom {

/// The value a weave is made with, as a load names it: one value, in its canonical bytes or in its
/// JSON form. Data, never a door. An empty one names none.
class Creation {
public:
    Creation() = default;
    /// A value the host holds, by its canonical bytes.
    static Creation of(const Value& value) {
        Creation c;
        c.text_ = serialize(value);
        return c;
    }
    /// A value in its JSON form (`{"zen":1,"schema":...,"version":...,"fields":{...}}`), as a plan
    /// row or a load message carries it; empty text names none.
    static Creation json(std::string text) {
        Creation c;
        c.text_ = std::move(text);
        c.json_ = true;
        return c;
    }

    bool empty() const noexcept { return text_.empty(); }
    /// The value as the gate reads it: parsed, not yet admitted.
    Unverified unverified() const { return json_ ? compat::parse(text_) : parse(text_); }

private:
    std::string text_;
    bool json_ = false;
};

/// Judge a making: `declared` is what the image's `describe_creation` wrote (zen.Creation, or
/// nothing for a weave made with none) and `given` the value the load names. True when the weave
/// may be made: with `admitted` empty when both are none, or holding the value the gate admitted
/// against the declared shape. False, with `error`, when one is named and the other is not, or
/// when the declaration or the value does not pass the gate.
inline bool judge_creation(std::string_view declared, const Creation& given,
                           std::optional<Value>& admitted, std::string& error) {
    admitted.reset();
    if (declared.empty()) {
        if (given.empty()) {
            return true;
        }
        const Unverified u = given.unverified();
        error = "the weave is made with no value, and the load names one (" + u.claimed_name() +
                " v" + std::to_string(u.claimed_version()) + ")";
        return false;
    }
    Admission desc = admit(parse(declared), creation_schema());
    if (!desc.ok()) {
        error = "the image's creation shape is refused by the gate: " + desc.first_error().message();
        return false;
    }
    Registry deps;
    std::shared_ptr<const Schema> shape;
    try {
        decode_referenced(desc.value(), deps);
        shape = decode_schema(*desc.value().get("shape")->as_message(), deps);
    } catch (const std::exception& e) {
        error = std::string("the image's creation shape is refused: ") + e.what();
        return false;
    }
    if (given.empty()) {
        error = "the weave is made with " + shape->name() + " v" +
                std::to_string(shape->version()) + ", and the load names no value";
        return false;
    }
    Admission a = admit(given.unverified(), shape, deps);
    if (!a.ok()) {
        error = "the creation value is refused by the gate: " + a.first_error().message();
        return false;
    }
    admitted = a.value();
    return true;
}

} // namespace loom

#endif // ZEN_KERNEL_CREATION_HPP
