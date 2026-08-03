// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The office worker — the R2D-0 dynamic-parity fixture.
//
// A loadable weave that exercises every half of role authorship from the FAR
// side of the C ABI seam, through the exact same C++ surface a native weave
// uses: `mail.as_role(...)` outbound, `mail.authored_from_role(...)` inbound.
// It is deliberately obedient and deliberately unprivileged: it does whatever
// OfficeCommand asks — including asking to speak for an office it does not
// hold — and reports what actually happened, so the suite measures the HOST's
// verdicts, not the fixture's manners.
//
// The host loads it under the role "worker.a". Nothing in this source claims
// that role as an identity: every act is a REQUEST the host verifies at the
// authorship moment, which is the entire dynamic outgoing law.

#include "office_protocol.hpp"

#include <zen/kernel/export.hpp>
#include <zen/weave.hpp>

#include <cstdint>
#include <string>

namespace {

using namespace loom;

struct OfficeWorkerState {
    std::int64_t acts = 0;
    ZEN_SHAPE(OfficeWorkerState, 1, ZEN_FIELD(acts));
};

class OfficeWorker
    : public WeaveBase<OfficeWorker, OfficeWorkerState,
                       Accept<office::OfficeCommand, office::WorkerNews>,
                       Emit<office::WorkerNews, office::OfficeReport>> {
public:
    void on(const office::OfficeCommand& cmd, Mail& mail) {
        ++state_.acts;
        const WeaveId target{static_cast<std::uint64_t>(cmd.target)};
        office::OfficeReport report;
        report.what = cmd.mode;
        if (cmd.mode == "direct") {
            report.authored =
                mail.as_role("worker.a").send(target, office::WorkerNews{"direct"}).valid();
        } else if (cmd.mode == "to-role") {
            report.authored = mail.as_role("worker.a")
                                  .send_to_role("dispatcher", office::WorkerNews{"to-role"})
                                  .valid();
        } else if (cmd.mode == "publish") {
            const OfficePublication p =
                mail.as_role("worker.a").publish(office::WorkerNews{"open"});
            report.authored = p.authored;
            report.recipients = static_cast<std::int64_t>(p.recipients);
        } else if (cmd.mode == "personal") {
            (void)mail.publish(office::WorkerNews{"open"});
            report.authored = false;
        } else if (cmd.mode == "forge-direct") {
            // An office this weave does NOT hold. The host must refuse; the
            // report carries the refusal home.
            report.authored =
                mail.as_role("dispatcher").send(target, office::WorkerNews{"forged"}).valid();
        } else if (cmd.mode == "forge-publish") {
            const OfficePublication p =
                mail.as_role("dispatcher").publish(office::WorkerNews{"forged"});
            report.authored = p.authored;
            report.recipients = static_cast<std::int64_t>(p.recipients);
        }
        // Every report rides one road: the commander role. The suite binds it,
        // so reports arrive whoever triggered the act.
        mail.send_to_role("commander", report);
    }

    /// Inbound office speech: report the delivery facts this side of the seam
    /// reconstructed — the parity question itself.
    void on(const office::WorkerNews&, Mail& mail) {
        ++state_.acts;
        office::OfficeReport report;
        report.what = "heard";
        report.authored = mail.authored_from_role("worker.a");
        report.seen_role = std::string(mail.authored_role());
        mail.send_to_role("commander", report);
    }
};

} // namespace

ZEN_EXPORT_WEAVE(OfficeWorker)
