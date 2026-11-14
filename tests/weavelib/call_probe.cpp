// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The call probe: a weave that calls the role a knock names and keeps how the call ended, and that
// provides one function, CallAdd to CallSum, of its argument alone. Built native and boxed alike;
// CALL_PROBE_V2 is its successor (its answers say `by` 2), CALL_PROBE_CALLER_ONLY provides nothing,
// so it can be built against a header from before the provided functions.

#include "call_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

#include <stdexcept>

using namespace loom;
using namespace calling;

namespace {

#if defined(CALL_PROBE_CALLER_ONLY)
#elif defined(CALL_PROBE_V2)
constexpr std::int64_t kBy = 2;
#else
constexpr std::int64_t kBy = 1;
#endif

/// How many times this image's function ran, in whatever memory it ran in: the weave's own in a
/// library, a store of its own in a box.
std::int64_t g_provided = 0;

class CallProbe final : public WeaveBase<CallProbe, CallHeard, Accept<CallKnock, CallPeek>, Emit<>,
                                         Claims<>
#if !defined(CALL_PROBE_CALLER_ONLY)
                                         ,
                                         Provides<CallAdd>
#endif
                                         > {
public:
    void on(const CallKnock& k, Mail& mail) {
        ++state_.knocks;
        const Called<CallSum> r = mail.call<CallSum>(k.role, CallAdd{k.a, k.b});
        state_.refusal = static_cast<std::int64_t>(r.refusal());
        state_.words = r.words();
        state_.value = r ? r->value : 0;
        state_.by = r ? r->by : 0;
    }
    void on(const CallPeek&, Mail&) { state_.provided_here = g_provided; }

#if !defined(CALL_PROBE_CALLER_ONLY)
    static CallSum provide(const CallAdd& add) {
        ++g_provided;
        if (add.a == kCallFails) {
            throw std::runtime_error("the probe was asked to fail");
        }
        return CallSum{add.a + add.b, kBy};
    }
#endif
};

} // namespace

ZEN_EXPORT_WEAVE(CallProbe)
