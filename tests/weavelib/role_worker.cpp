// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

// The role worker, the role-authorship dynamic-parity fixture (MSG-07): a loadable weave
// exercising both halves of role authorship from the FAR side of the C ABI seam, through the
// C++ surface a native weave uses (`mail.as_role(...)`, `mail.authored_from_role(...)`).
// Obedient and unprivileged, it does whatever RoleCommand asks, roles it does not hold
// included, and reports what happened, so the suite measures the host's verdicts. It is loaded
// under role "worker.a", and every act is a REQUEST the host verifies at the authorship moment.

#include "role_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <string>

namespace {

using namespace loom;

struct RoleWorkerState {
    std::int64_t acts = 0;
    ZEN_SHAPE(RoleWorkerState, 1, ZEN_FIELD(acts));
};

class RoleWorker
    : public WeaveBase<RoleWorker, RoleWorkerState,
                       Accept<role::RoleCommand, role::WorkerNews>,
                       Emit<role::WorkerNews, role::RoleReport>> {
public:
    void on(const role::RoleCommand& cmd, Mail& mail) {
        ++state_.acts;
        const WeaveId target{static_cast<std::uint64_t>(cmd.target)};
        role::RoleReport report;
        report.what = cmd.mode;
        if (cmd.mode == "direct") {
            report.authored =
                mail.as_role("worker.a").send(target, role::WorkerNews{"direct"}).valid();
        } else if (cmd.mode == "to-role") {
            report.authored = mail.as_role("worker.a")
                                  .send_to_role("dispatcher", role::WorkerNews{"to-role"})
                                  .valid();
        } else if (cmd.mode == "publish") {
            const RolePublication p =
                mail.as_role("worker.a").publish(role::WorkerNews{"open"});
            report.authored = p.authored;
            report.recipients = static_cast<std::int64_t>(p.recipients);
        } else if (cmd.mode == "personal") {
            (void)mail.publish(role::WorkerNews{"open"});
            report.authored = false;
        } else if (cmd.mode == "forge-direct") {
            // A role this weave does NOT hold. The host must refuse; the
            // report carries the refusal home.
            report.authored =
                mail.as_role("dispatcher").send(target, role::WorkerNews{"forged"}).valid();
        } else if (cmd.mode == "forge-publish") {
            const RolePublication p =
                mail.as_role("dispatcher").publish(role::WorkerNews{"forged"});
            report.authored = p.authored;
            report.recipients = static_cast<std::int64_t>(p.recipients);
        }
        // Every report rides one road: the commander role. The suite binds it,
        // so reports arrive whoever triggered the act.
        mail.send_to_role("commander", report);
    }

    /// Inbound role speech: report the delivery facts this side of the seam
    /// reconstructed — the parity question itself.
    void on(const role::WorkerNews&, Mail& mail) {
        ++state_.acts;
        role::RoleReport report;
        report.what = "heard";
        report.authored = mail.authored_from_role("worker.a");
        report.seen_role = std::string(mail.authored_role());
        mail.send_to_role("commander", report);
    }
};

} // namespace

ZEN_EXPORT_WEAVE(RoleWorker)
