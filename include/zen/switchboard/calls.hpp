// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss
#ifndef ZEN_SWITCHBOARD_CALLS_HPP
#define ZEN_SWITCHBOARD_CALLS_HPP

// Calls: a weave calls a function another weave provides, inside its own turn, and has the answer
// or a refusal before its handler goes on. The function is handed its argument alone: never its
// provider's instance, never a bus, never a claim. No handler runs and nothing is delivered, so a
// call nests no delivery (MSG-01). A unit the bus consults, so the Switchboard's own file and class
// do not take the subject on.
// MSG-14; docs/reference/messaging.md#calls-a-function-of-its-argument-alone

#include <zen/schema.hpp>
#include <zen/switchboard/message.hpp>
#include <zen/value.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

class Switchboard;

/// Why a call has no answer. Each is its own word because each points at a different fix, and
/// none is ever folded into another. The order is the order they are decided in.
enum class CallRefusal : std::uint8_t {
    None = 0,
    /// This bus has no call door: an isolated child's, a library's on a host table built before
    /// the door, or a weave's function, which has none. Nothing was called.
    NotOffered,
    /// The caller's grant has no call rule for this role and input shape. Asked before the role
    /// is resolved, so an unauthorized caller learns nothing of who holds it.
    NotAuthorized,
    /// Nobody holds the role now.
    NoHolder,
    /// The holder runs in an isolated child: a call does not cross the isolation pipe.
    Isolated,
    /// The holder is a link to another host: a call does not cross a link. A far function is asked
    /// by message.
    Linked,
    /// The holder provides no function from this input shape to this output shape.
    NotProvided,
    /// The gate refused the argument at the function's input shape; the function never ran.
    ArgumentRefused,
    /// The provider's code threw, trapped, ran out of its steps or wrote no answer.
    Failed,
    /// The gate refused the answer at the function's output shape: the caller is handed nothing
    /// its shape does not admit.
    AnswerRefused,
};

const char* name_of(CallRefusal r) noexcept;

class Weave;

/// What a provided function came to, before the bus admits it: the answer as a value (a function
/// in this process) or as the bytes a function across the seam wrote, or neither, with the words
/// of its failure.
struct ProvidedAnswer {
    std::optional<Value> value;
    std::optional<std::string> bytes;
    std::string failure;
};

/// One function a weave provides: from its input shape to its output shape. `compute` is handed
/// the argument the gate admitted at `input`, and nothing else; it runs no handler, has no bus and
/// is never handed the provider's instance. A native function may throw, and its words are the
/// call's `Failed`.
struct ProvidedFunction {
    std::shared_ptr<const Schema> input;
    std::shared_ptr<const Schema> output;
    std::function<ProvidedAnswer(const Value& argument)> compute;
};

/// The shapes `weave`'s functions take and answer, each input before its output: vocabulary the
/// bus claims with the rest at registration and at a code swap. A function naming no shape is
/// refused there.
std::vector<std::shared_ptr<const Schema>> provided_shapes(const Weave& weave);

/// A call's end, as the bus hands it to the caller: the answer, admitted at the function's output
/// shape, or a refusal. `words` says the gate's or the provider's own words where there are any.
struct CallResult {
    CallRefusal refusal = CallRefusal::NotOffered;
    std::optional<Value> answer;
    std::string words;

    explicit operator bool() const noexcept { return refusal == CallRefusal::None; }
};

/// The bus's call door: resolves the role to its holder now, finds the function, admits the
/// argument and the answer, and tells the tap how the call ended. Only `Switchboard::WeaveBus`
/// opens it, for the weave whose delivery is being handled.
struct CallDoor {
    static CallResult call(Switchboard& sb, WeaveId caller, std::string_view role,
                           const Value& argument, const std::shared_ptr<const Schema>& output);

    /// A call a seam refused before it reached the door, because the argument's shape resolves
    /// to nothing or the seam's gate refused it: told to the tap as the door tells its own, and
    /// authority first, so an unauthorized caller is told `NotAuthorized` whatever else held.
    static CallResult refuse(Switchboard& sb, WeaveId caller, std::string_view role,
                             const std::string& shape, std::uint32_t version, CallRefusal why,
                             std::string words);
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_CALLS_HPP
