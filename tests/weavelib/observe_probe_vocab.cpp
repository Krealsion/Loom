// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// THE OBSERVATION JOURNEY'S VOCABULARY, for the session host (tests/session/observe_journey.py
// boots it). A tool that asks the far producer through a link writes its command as JSON, and the
// link encodes it against THIS host's shape (docs/reference/bridge.md): some participant here must
// declare it. This one declares it and does nothing else -- it accepts nothing and says nothing.
// What the far producer publishes needs no declaration here: the relay sends its descriptors.

#include "../observe_far/probe_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

#include <cstdint>

namespace {

struct ProbeVocabularyState {
    std::int64_t declared = 1;
    ZEN_SHAPE(ProbeVocabularyState, 1, ZEN_FIELD(declared));
};

class ProbeVocabulary final
    : public loom::WeaveBase<ProbeVocabulary, ProbeVocabularyState, loom::Accept<>,
                             loom::Emit<zen_tests::observe_probe::ObserveProbeCommand>> {};

} // namespace

ZEN_EXPORT_WEAVE(ProbeVocabulary)
