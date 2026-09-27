// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_CONTROL_HPP
#define ZEN_KERNEL_CONTROL_HPP

// The kernel's message door: a control Weave accepts LoadLibrary, ReloadLibrary, UnloadLibrary,
// UnloadRole, ListLibraries and QueryRole and calls the Kernel's load, reload_from, unload,
// unload_role, loaded and query_role. The right to send those shapes to it is the load
// capability, the most dangerous grant; a Weave without it is `CapabilityDenied` at delivery.
//
// Every operation replies to the asker (reply_to if given, else the stamped sender):
//   LoadLibrary    -> zen.Result{weave id}   | zen.Refused{why}
//   ReloadLibrary  -> zen.Ack                | zen.Refused{why}
//   UnloadLibrary  -> zen.Ack                | zen.Refused{why}
//   UnloadRole     -> zen.Ack                | zen.Refused{why}
//   ListLibraries  -> zen.Result{"a,b@role"} | (never refuses)
//   QueryRole      -> RoleInfo{holder, converses}
// The door executes primitives only; composites such as a swap belong to an orchestrator (the
// Weave Manager, kernel/manager.hpp), which another can replace.
//
// The door also tells a freshly committed weave it is live, with zen.Activated
// (weave/lifecycle.hpp says what that means and does not). It owns this because a participant
// holding the load capability can drive LoadLibrary and ReloadLibrary with no Manager in the
// path, and the door is what sits on every successful kernel operation. A swap's successor is
// activated because it arrived through LoadLibrary; nothing here knows a swap is happening.
// Weaves mounted natively with mount<T>() never pass this door and are not activated.

#include <zen/host/lifecycle_wiring.hpp> // host wiring: the one mint, not weave-facing
#include <zen/weave.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/switchboard.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace loom {

/// Load `path` under `name`; a non-empty `role` binds the loaded Weave to it, and load is the
/// only moment a role can be bound.
struct LoadLibrary {
    std::string name;
    std::string path;
    std::string role; ///< empty = bind no role
    ZEN_SHAPE(LoadLibrary, 2, ZEN_FIELD(name), ZEN_FIELD(path), ZEN_FIELD(role));
};

struct ReloadLibrary {
    std::string name;
    std::string path;
    ZEN_SHAPE(ReloadLibrary, 1, ZEN_FIELD(name), ZEN_FIELD(path));
};

struct UnloadLibrary {
    std::string name;
    ZEN_SHAPE(UnloadLibrary, 1, ZEN_FIELD(name));
};

/// Unload whichever loaded library holds `role` now: the only thing it can unload is the role's
/// holder.
struct UnloadRole {
    std::string role;
    ZEN_SHAPE(UnloadRole, 1, ZEN_FIELD(role));
};

/// Ask what is loaded, answered from the kernel's live map.
struct ListLibraries {
    ZEN_SHAPE(ListLibraries, 1);
};

/// Ask about a role's holder: who it is, and whether it declares `zen.PrepareShutdown`, that
/// is, whether it will converse about its own succession. A steward asks this first, so it
/// never waits on a weave that did not opt in.
struct QueryRole {
    std::string role;
    ZEN_SHAPE(QueryRole, 1, ZEN_FIELD(role));
};

/// The answer, bespoke because both fields are needed to act: a steward asking an incumbent for
/// its letter must know whose stamped sender to require on the reply. `holder == 0` means no
/// weave this Kernel loaded holds the role: unheld, or held natively. Either way, not a
/// participant.
struct RoleInfo {
    std::int64_t holder;
    bool converses;
    ZEN_SHAPE(RoleInfo, 1, ZEN_FIELD(holder), ZEN_FIELD(converses));
};

/// The control Weave's state: how many operations it has performed, and the activation sequence
/// it has reached. `last_activation` is the highest sequence this lineage has sent (0 for
/// none); in the state, so a revived control weave continues its lineage. It is not `ops`,
/// which counts every command, refused ones included.
///
/// Its valid range is [0, INT64_MAX]: INT64_MAX is sent once, and every lifecycle operation
/// after it is refused, never wrapped (activation_block()). A revived negative value, which the
/// gate admits, is refused the same way; neither is normalized into a healthy-looking lineage.
struct ControlState {
    std::int64_t ops;
    std::int64_t last_activation;
    ZEN_SHAPE(ControlState, 2, ZEN_FIELD(ops), ZEN_FIELD(last_activation));
};

/// The two ways a control lineage cannot name another activation, sent as ordinary
/// `zen.Refused` reasons.
inline constexpr const char* kActivationExhausted =
    "activation sequence exhausted; lifecycle operation refused";
inline constexpr const char* kActivationInvalid =
    "activation sequence state is invalid; lifecycle operation refused";

/// What the host does about a lifecycle change this door just made: host wiring, handed over at
/// mount like the `LifecycleAuthority`. Delegated live authority can be installed only from
/// inside a live delivery, and the one delivery between "committed" and "told it is live" is
/// this handler, so installing what a person approved here puts it in force for the
/// incarnation's first message; on the operation's answer it would arrive too late.
///
/// Not a policy hook: the operation is already committed and the callback cannot refuse it;
/// admission is `AdmissionPolicy`'s, earlier. The `Mail&` is this delivery's, and
/// `delegate_authority` is authorized by the capability, never by who calls (GATE-05). Both
/// halves are optional.
struct LifecycleAdoption {
    /// A freshly committed incarnation, named and identified by the Kernel, before
    /// `zen.Activated` is queued for it. Called for a load and for a reload in place: a reload
    /// keeps its WeaveId, and new code behind it is still new code.
    std::function<void(loom::Mail&, const std::string& name, loom::WeaveId id)> admitted;

    /// `name` has left: unregistered, its library released. Called after `UnloadLibrary` and
    /// `UnloadRole` succeed; for the second the name is resolved from the role first.
    std::function<void(loom::Mail&, const std::string& name)> retired;
};

/// A Weave whose handlers drive a Kernel. Reaching it takes the sender's load capability;
/// answering takes only the ordinary Emit<...> grant.
class ControlWeave
    : public loom::WeaveBase<ControlWeave, ControlState,
                             loom::Accept<LoadLibrary, ReloadLibrary, UnloadLibrary, UnloadRole,
                                          ListLibraries, QueryRole>,
                             loom::Emit<loom::Result, loom::Ack, loom::Refused, RoleInfo,
                                        loom::Activated>> {
public:
    /// The lifecycle authority is host-supplied wiring, like the Kernel reference: the host
    /// decides which weave attests activations and hands it the capability at mount. No weave
    /// acquires it by learning a shape.
    ControlWeave(Kernel& kernel, loom::LifecycleAuthority authority,
                 LifecycleAdoption adoption = {})
        : kernel_(&kernel), authority_(authority), adoption_(std::move(adoption)) {}

    /// A load that arrives as a message is one the host did not write, so it goes through the
    /// Kernel's policy-mediated `load` (`zen/kernel/admission.hpp`). A refusal carries the
    /// policy's own sentence back to the asker as `zen.Refused`, under its correlation.
    void on(const LoadLibrary& m, loom::Mail& mail) {
        ++state_.ops;
        if (const char* blocked = activation_block()) {
            answer(mail, loom::Refused{blocked});
            return; // the Kernel is never called: nothing is opened, nothing bound
        }
        const LoadResult r = kernel_->load(m.name, m.path, m.role);
        if (r.ok) {
            // The incarnation is committed. The host adopts it first (LifecycleAdoption), then
            // the activation is queued, then the asker hears "loaded".
            adopt(mail, m.name, r.id);
            announce_activation(mail, r.id);
            answer(mail, loom::Result{std::to_string(r.id.value)});
        } else {
            answer(mail, loom::Refused{r.error});
        }
    }

    void on(const ReloadLibrary& m, loom::Mail& mail) {
        ++state_.ops;
        if (const char* blocked = activation_block()) {
            answer(mail, loom::Refused{blocked});
            return; // the incumbent is never touched: no snapshot, no rebind, no revive
        }
        const ReloadResult r = kernel_->reload_from(m.name, m.path);
        // `reloaded` is the only success; every other outcome has written its own reason into
        // `error`, and only success is announced.
        if (r.reloaded) {
            // A reload keeps the WeaveId and is still a new incarnation, so it earns its own
            // activation.
            const loom::WeaveId id = kernel_->weave_id(m.name);
            adopt(mail, m.name, id); // same window, same reason (see LifecycleAdoption)
            announce_activation(mail, id);
            answer(mail, loom::Ack{});
        } else {
            answer(mail, loom::Refused{r.error});
        }
    }

    void on(const UnloadLibrary& m, loom::Mail& mail) {
        ++state_.ops;
        if (kernel_->unload(m.name)) {
            retire(mail, m.name); // the host's records are kept under this same name
            answer(mail, loom::Ack{});
        } else {
            answer(mail, loom::Refused{"not loaded: " + m.name});
        }
    }

    void on(const UnloadRole& m, loom::Mail& mail) {
        ++state_.ops;
        // Resolved before the unload, while the holder is loaded: a host keeps its records
        // under the artifact name.
        const std::string name = artifact_holding(m.role);
        if (kernel_->unload_role(m.role)) {
            if (!name.empty()) {
                retire(mail, name);
            }
            answer(mail, loom::Ack{});
        } else {
            answer(mail, loom::Refused{"no loaded library holds role '" + m.role + "'"});
        }
    }

    void on(const ListLibraries&, loom::Mail& mail) {
        ++state_.ops;
        std::string out;
        for (const std::string& n : kernel_->loaded()) {
            if (!out.empty()) {
                out += ',';
            }
            out += n;
            const std::string role = kernel_->role_of(n);
            if (!role.empty()) {
                out += '@';
                out += role;
            }
        }
        answer(mail, loom::Result{out});
    }

    void on(const QueryRole& m, loom::Mail& mail) {
        ++state_.ops;
        const Kernel::RoleQuery q =
            kernel_->query_role(m.role, PrepareShutdown::zen_name, PrepareShutdown::zen_version);
        answer(mail, RoleInfo{static_cast<std::int64_t>(q.holder.value), q.accepts});
    }

private:
    /// Why this lineage cannot name another activation, or nullptr. Checked before the Kernel is
    /// called, so an operation never commits and then owes an activation it cannot name. At the
    /// limit this also refuses a load that would not have participated, which cannot be known
    /// without calling the Kernel.
    const char* activation_block() const {
        if (state_.last_activation < 0) {
            return kActivationInvalid;
        }
        if (state_.last_activation == std::numeric_limits<std::int64_t>::max()) {
            return kActivationExhausted;
        }
        return nullptr;
    }

    /// The one place the sequence advances. It refuses rather than wraps, so no activation
    /// identity is ever non-positive or reused; nothing is consumed when nothing is allocated.
    std::optional<std::int64_t> next_activation() {
        if (activation_block() != nullptr) {
            return std::nullopt;
        }
        return ++state_.last_activation;
    }

    /// Tell a freshly committed incarnation it is live, if its accept-set says it listens; a
    /// non-participant costs nothing: no message, no refusal, no sequence. Correlation 0: an
    /// activation is an event, not an answer, and nothing is owed back.
    void announce_activation(loom::Mail& mail, loom::WeaveId target) {
        if (!target.valid() ||
            !kernel_->accepts(target, loom::Activated::zen_name, loom::Activated::zen_version)) {
            return;
        }
        // Allocated only for an activation that will be sent; monotonic across snapshot and
        // revival because it lives in the state.
        const std::optional<std::int64_t> sequence = next_activation();
        if (!sequence) {
            // Unreachable while every handler preflights activation_block(); a gap here is
            // recoverable, a reused identity would not be.
            return;
        }
        // Attested, not merely sent (LIFE-04): the send is still gated by the door's grant, and
        // Loom's word says this is a real commit for this incarnation and this sequence. The
        // sequence travels in the payload and to Loom, so the consumer can check they agree.
        mail.announce_lifecycle(authority_, target, loom::Activated{*sequence}, *sequence);
    }

    /// Hand the host its moment (LifecycleAdoption), if it wired one.
    void adopt(loom::Mail& mail, const std::string& name, loom::WeaveId id) {
        if (adoption_.admitted && id.valid()) {
            adoption_.admitted(mail, name, id);
        }
    }

    void retire(loom::Mail& mail, const std::string& name) {
        if (adoption_.retired) {
            adoption_.retired(mail, name);
        }
    }

    /// Which loaded artifact holds `role` now, or empty, from the kernel's live answers.
    std::string artifact_holding(const std::string& role) const {
        if (role.empty()) {
            return {};
        }
        for (const std::string& n : kernel_->loaded()) {
            if (kernel_->role_of(n) == role) {
                return n;
            }
        }
        return {};
    }

    /// Reply to the asker, reply_to if given or else the stamped sender, echoing the request's
    /// correlation. An ordinary send, not an authenticated answer. A request with neither address
    /// gets no reply.
    template <class Answer>
    void answer(loom::Mail& mail, const Answer& a) {
        const loom::WeaveId to = mail.reply_to().valid() ? mail.reply_to() : mail.sender();
        if (!to.valid()) {
            return;
        }
        mail.send(to, a, mail.correlation());
    }

    Kernel* kernel_;
    loom::LifecycleAuthority authority_; ///< host-supplied; the right to attest, not to send
    LifecycleAdoption adoption_;         ///< host-supplied; may be empty (see the type)
};

/// Register the control Weave on `bus` and return its id. Its grant comes from its declared
/// Emit<...>, the replies and zen.Activated. The lifecycle authority is handed over here, by the
/// host choosing which weave sits on the kernel's operations; accepting the control shapes
/// confers none.
inline loom::WeaveId mount_control(Kernel& kernel, loom::Switchboard& bus,
                                   LifecycleAdoption adoption = {}) {
    return loom::mount<ControlWeave>(bus, kernel, loom::host_lifecycle_authority(bus),
                                     std::move(adoption));
}

/// The grant that lets a Weave drive the kernel: the six control shapes, to the control Weave
/// and nowhere else. The dangerous grant, so it is target-scoped, never allow_to_any.
inline loom::Grant load_capability(loom::WeaveId control) {
    loom::Grant g;
    g.allow(LoadLibrary::zen_name, LoadLibrary::zen_version, control);
    g.allow(ReloadLibrary::zen_name, ReloadLibrary::zen_version, control);
    g.allow(UnloadLibrary::zen_name, UnloadLibrary::zen_version, control);
    g.allow(UnloadRole::zen_name, UnloadRole::zen_version, control);
    g.allow(ListLibraries::zen_name, ListLibraries::zen_version, control);
    g.allow(QueryRole::zen_name, QueryRole::zen_version, control);
    return g;
}

} // namespace loom

#endif // ZEN_KERNEL_CONTROL_HPP
