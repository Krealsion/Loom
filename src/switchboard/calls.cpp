// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The bus's call door: zen/switchboard/calls.hpp says what a call is.

#include <zen/gate.hpp>
#include <zen/serialize.hpp>
#include <zen/switchboard/calls.hpp>
#include <zen/switchboard/switchboard.hpp>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace loom {

const char* name_of(CallRefusal r) noexcept {
    switch (r) {
    case CallRefusal::None:
        return "None";
    case CallRefusal::NotOffered:
        return "NotOffered";
    case CallRefusal::NotAuthorized:
        return "NotAuthorized";
    case CallRefusal::NoHolder:
        return "NoHolder";
    case CallRefusal::Isolated:
        return "Isolated";
    case CallRefusal::Linked:
        return "Linked";
    case CallRefusal::NotProvided:
        return "NotProvided";
    case CallRefusal::ArgumentRefused:
        return "ArgumentRefused";
    case CallRefusal::Failed:
        return "Failed";
    case CallRefusal::AnswerRefused:
        return "AnswerRefused";
    }
    return "?";
}

std::vector<std::shared_ptr<const Schema>> provided_shapes(const Weave& weave) {
    std::vector<std::shared_ptr<const Schema>> out;
    for (const ProvidedFunction& f : weave.provided_functions()) {
        if (!f.input || !f.output) {
            throw std::invalid_argument("register_weave: a provided function names no shape");
        }
        // The argument's shape names the function, so two from one shape could not be told
        // apart at a call.
        for (std::size_t i = 0; i < out.size(); i += 2) {
            if (out[i]->name() == f.input->name() && out[i]->version() == f.input->version()) {
                throw std::invalid_argument("register_weave: two provided functions take " +
                                            f.input->name() + " v" +
                                            std::to_string(f.input->version()));
            }
        }
        out.push_back(f.input);
        out.push_back(f.output);
    }
    return out;
}

namespace {

/// The function `holder` provides from `input`'s (name, version) to `output`, or why there is none.
/// The input is matched by name and version, as a door is, and the gate then admits the argument
/// against the provider's own definition; the output must be the caller's definition exactly,
/// since the answer is handed to the caller as that shape.
std::optional<ProvidedFunction> find_function(const Weave& holder, const Schema& input,
                                              const Schema& output, std::string& words) {
    for (ProvidedFunction& f : holder.provided_functions()) {
        if (!f.input || !f.output || !f.compute || f.input->name() != input.name() ||
            f.input->version() != input.version()) {
            continue;
        }
        if (!same_identity(*f.output, output)) {
            words = "the function from " + input.name() + " v" + std::to_string(input.version()) +
                    " answers " + f.output->name() + " v" + std::to_string(f.output->version()) +
                    " by its own definition, not " + output.name() + " v" +
                    std::to_string(output.version()) + " as the caller holds it";
            continue;
        }
        return std::move(f);
    }
    if (words.empty()) {
        words = "the holder provides no function from " + input.name() + " v" +
                std::to_string(input.version());
    }
    return std::nullopt;
}

} // namespace

CallResult CallDoor::refuse(Switchboard& sb, WeaveId caller, std::string_view role,
                            const std::string& shape, std::uint32_t version, CallRefusal why,
                            std::string words) {
    BusEvent ev;
    ev.kind = EventKind::Called;
    ev.seq = sb.current_dispatch_seq_;
    ev.sender = caller;
    ev.addressed_role = std::string(role);
    ev.schema_name = shape;
    ev.schema_version = version;
    const Switchboard::WeaveRecord* self = sb.find(caller);
    CallResult r{why, std::nullopt, std::move(words)};
    if (self == nullptr ||
        !effective_permits_call(self->grant.live(), self->delegated, role, shape, version)) {
        r = CallResult{CallRefusal::NotAuthorized, std::nullopt, {}};
    }
    ev.call_refusal = r.refusal;
    ev.call_words = r.words;
    sb.emit(ev);
    return r;
}

CallResult CallDoor::call(Switchboard& sb, WeaveId caller, std::string_view role,
                          const Value& argument, const std::shared_ptr<const Schema>& output) {
    const Schema& input = argument.schema();
    BusEvent ev;
    ev.kind = EventKind::Called;
    ev.seq = sb.current_dispatch_seq_;
    ev.sender = caller;
    ev.addressed_role = std::string(role);
    ev.schema_name = input.name();
    ev.schema_version = input.version();
    // Every way out says how the call ended, on the tap, before the caller's handler goes on.
    auto end = [&sb, &ev](CallResult r) {
        ev.call_refusal = r.refusal;
        ev.call_words = r.words;
        sb.emit(ev);
        return r;
    };

    // Authority first, read live (GATE-05), before the role is resolved: an unauthorized caller
    // learns nothing of who holds it, as an unauthorized reader learns nothing of a claim.
    const Switchboard::WeaveRecord* self = sb.find(caller);
    if (self == nullptr || output == nullptr ||
        !effective_permits_call(self->grant.live(), self->delegated, role, input.name(),
                                input.version())) {
        return end(CallResult{CallRefusal::NotAuthorized, std::nullopt, {}});
    }
    // The holder now, as a send to the role would reach it.
    const WeaveId holder_id = sb.role_holder(role);
    const Switchboard::WeaveRecord* holder = sb.find(holder_id);
    if (holder == nullptr || holder->weave == nullptr) {
        return end(CallResult{CallRefusal::NoHolder, std::nullopt, {}});
    }
    ev.target = holder_id;
    if (const CallRefusal far = holder->weave->elsewhere(); far != CallRefusal::None) {
        return end(CallResult{far, std::nullopt, {}});
    }
    std::string words;
    std::optional<ProvidedFunction> fn = find_function(*holder->weave, input, *output, words);
    if (!fn) {
        return end(CallResult{CallRefusal::NotProvided, std::nullopt, std::move(words)});
    }
    // The argument at the function's input shape, through the one gate, against the bus's
    // registry for what an open field holds.
    Admission arg = admit(argument, *fn->input, sb.registry_);
    if (!arg.ok()) {
        return end(CallResult{CallRefusal::ArgumentRefused, std::nullopt,
                              arg.first_error().message()});
    }
    ProvidedAnswer computed;
    const auto began = std::chrono::steady_clock::now();
    try {
        computed = fn->compute(arg.value());
    } catch (const std::exception& e) {
        computed = ProvidedAnswer{std::nullopt, std::nullopt, e.what()};
    } catch (...) {
        computed = ProvidedAnswer{std::nullopt, std::nullopt, "the function threw"};
    }
    ev.handler_elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             began)
            .count());
    // The answer at the function's output shape: a value made here, or the bytes a function
    // across the seam wrote, each through the gate.
    if (computed.value) {
        Admission a = admit(std::move(*computed.value), *fn->output, sb.registry_);
        if (!a.ok()) {
            return end(
                CallResult{CallRefusal::AnswerRefused, std::nullopt, a.first_error().message()});
        }
        return end(CallResult{CallRefusal::None, std::move(a).value(), {}});
    }
    if (computed.bytes) {
        Admission a = admit(parse(*computed.bytes), fn->output, sb.registry_);
        if (!a.ok()) {
            return end(
                CallResult{CallRefusal::AnswerRefused, std::nullopt, a.first_error().message()});
        }
        return end(CallResult{CallRefusal::None, std::move(a).value(), {}});
    }
    if (computed.failure.empty()) {
        computed.failure = "the function gave no answer";
    }
    return end(CallResult{CallRefusal::Failed, std::nullopt, std::move(computed.failure)});
}

} // namespace loom
