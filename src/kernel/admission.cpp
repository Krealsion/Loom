// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#include <zen/kernel/admission.hpp>

#include <utility>

namespace loom {

const char* name_of(AdmissionStage s) noexcept {
    switch (s) {
    case AdmissionStage::Open:
        return "open";
    case AdmissionStage::Speak:
        return "speak";
    }
    return "?";
}

const char* name_of(AdmissionKind k) noexcept {
    switch (k) {
    case AdmissionKind::Load:
        return "load";
    case AdmissionKind::Candidate:
        return "candidate";
    case AdmissionKind::Reload:
        return "reload";
    }
    return "?";
}

AdmissionPolicy trust_every_artifact(std::string why) {
    // The `why` is captured and handed back in the note of every verdict, so a host that
    // installs this can always say why: this trust has an author and a reason, and both travel
    // with the decision.
    return [reason = std::move(why)](const AdmissionRequest& req) {
        if (req.stage == AdmissionStage::Open) {
            return AdmissionVerdict::admit(Grant{}, reason);
        }
        // Permissive bus sends, and no Sense read authority: Grant's floor has none, and reading
        // another participant's claims stays a deliberate decision even for a host that trusts
        // its own build output.
        return AdmissionVerdict::admit(Grant{}.allow_any(), reason);
    };
}

AdmissionPolicy admit_nothing() {
    return [](const AdmissionRequest& req) {
        return AdmissionVerdict::refuse(
            std::string("no admission policy is installed on this Kernel, so nothing decides "
                        "what a loaded artifact may do; the host must call "
                        "Kernel::admit_with(...) before a ") +
            name_of(req.kind) + " can be admitted");
    };
}

} // namespace loom
