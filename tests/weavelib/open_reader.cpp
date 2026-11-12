// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// A loaded weave that reads an open field and sends one: what it is handed is admitted at its
// own seam against the shapes it declares, and what it sends is admitted at the host's against
// the bus's registry.

#include "open_protocol.hpp"

#include <zen/kernel/export.hpp>

using namespace loom;

namespace {

struct OpenReaderState {
    std::int64_t notes = 0;
    std::string held;
    std::int64_t x = 0;
    ZEN_SHAPE(OpenReaderState, 1, ZEN_FIELD(notes), ZEN_FIELD(held), ZEN_FIELD(x));
};

class OpenReader : public WeaveBase<OpenReader, OpenReaderState,
                                    Accept<open_test::Note, open_test::Pass>,
                                    Emit<open_test::Note, open_test::Moved>> {
public:
    void on(const open_test::Note& n, Mail&) {
        ++state_.notes;
        state_.held = n.item.shape() != nullptr ? n.item.shape()->name() : std::string();
        if (n.item.holds<open_test::Moved>()) {
            state_.x = n.item.as<open_test::Moved>().x;
        }
    }
    void on(const open_test::Pass& p, Mail& mail) {
        open_test::Note note;
        note.item = Any::of(open_test::Moved{p.x});
        (void)mail.send(WeaveId{static_cast<std::uint64_t>(p.target)}, note);
    }
};

} // namespace

ZEN_EXPORT_WEAVE(OpenReader)
