#ifndef ZEN_KERNEL_CONTROL_HPP
#define ZEN_KERNEL_CONTROL_HPP

// The kernel's message door: operate the kernel like everything else — by sending
// it messages. A control Weave accepts LoadLibrary / ReloadLibrary / UnloadLibrary
// / UnloadRole / ListLibraries and calls the kernel's existing load / reload_from /
// unload / unload_role / loaded. The right to send those shapes to the control
// Weave is the **load capability** — the canonical dangerous grant. A Weave holding
// it can drive the kernel by message; a Weave without it is denied at delivery
// (CapabilityDenied), protecting the single most dangerous surface in the system
// with the same capability-gating as everything else.
//
// THE DOOR ANSWERS. Every op replies with a standard shape (standard_shapes.hpp) to
// the asker — reply_to if given, else the bus-stamped sender:
//   LoadLibrary    -> zen.Result{weave id}   | zen.Refused{why}
//   ReloadLibrary  -> zen.Ack                | zen.Refused{why}
//   UnloadLibrary  -> zen.Ack                | zen.Refused{why}
//   UnloadRole     -> zen.Ack                | zen.Refused{why}
//   ListLibraries  -> zen.Result{"a,b@role"} | (never refuses)
// Before this, all five outcomes were discarded at the door: the most dangerous
// surface in the system was also the only one that answered nothing, so a
// reload's state-schema mismatch — a real, deliberate, well-shaped refusal — died
// as an unread C++ return value. The kernel now says what happened, and the
// Weave Manager (kernel/manager.hpp) is the first participant to hear it.
//
// The door only *executes* primitives. Composites (swap = unload-the-role-holder
// then load-the-successor) live in the orchestrator, never here — that is what
// makes the orchestrator replaceable: a different Manager can compose the same
// primitives into a different policy.

#include <zen/weave.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/switchboard.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace loom {

/// Load `path` under `name`; if `role` is non-empty, bind the loaded Weave to it.
///
/// v2: `role` joined the shape when the kernel learned to bind one at load (the
/// only moment a role CAN be bound — Switchboard::register_weave is the sole
/// binder). The version bump is the spine's immutable-published-schema rule paid
/// honestly: (LoadLibrary, 1) meant {name, path} and still does, forever.
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

/// Unload whichever loaded library currently holds `role`. Role-addressed, not
/// name-addressed, so "replace the role holder" cannot mistake its victim: the
/// only thing this can unload IS the role's holder.
struct UnloadRole {
    std::string role;
    ZEN_SHAPE(UnloadRole, 1, ZEN_FIELD(role));
};

/// Ask what is loaded. Answered from the kernel's own live map, never a cache.
struct ListLibraries {
    ZEN_SHAPE(ListLibraries, 1);
};

/// The control Weave's state: how many operations it has performed.
struct ControlState {
    std::int64_t ops;
    ZEN_SHAPE(ControlState, 1, ZEN_FIELD(ops));
};

/// A Weave whose handlers drive a Kernel. Its authority to *reach* the kernel is
/// its accept-set being reachable, gated by the *sender's* load capability; its
/// authority to *answer* is the ordinary Emit<...> grant any weave gets.
class ControlWeave
    : public loom::WeaveBase<ControlWeave, ControlState,
                             loom::Accept<LoadLibrary, ReloadLibrary, UnloadLibrary, UnloadRole,
                                          ListLibraries>,
                             loom::Emit<loom::Result, loom::Ack, loom::Refused>> {
public:
    explicit ControlWeave(Kernel& kernel) : kernel_(&kernel) {}

    void on(const LoadLibrary& m, loom::Mail& mail) {
        ++state_.ops;
        const LoadResult r = kernel_->load(m.name, m.path, m.role);
        if (r.ok) {
            answer(mail, loom::Result{std::to_string(r.id.value)});
        } else {
            answer(mail, loom::Refused{r.error});
        }
    }

    void on(const ReloadLibrary& m, loom::Mail& mail) {
        ++state_.ops;
        const ReloadResult r = kernel_->reload_from(m.name, m.path);
        // `reloaded` is the only success; every other outcome — not loaded, open
        // failure, the state-schema mismatch, a refused revive — has already
        // written its own self-contained reason into `error`.
        if (r.reloaded) {
            answer(mail, loom::Ack{});
        } else {
            answer(mail, loom::Refused{r.error});
        }
    }

    void on(const UnloadLibrary& m, loom::Mail& mail) {
        ++state_.ops;
        if (kernel_->unload(m.name)) {
            answer(mail, loom::Ack{});
        } else {
            answer(mail, loom::Refused{"not loaded: " + m.name});
        }
    }

    void on(const UnloadRole& m, loom::Mail& mail) {
        ++state_.ops;
        if (kernel_->unload_role(m.role)) {
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

private:
    /// Answer the asker: reply_to if given, else the bus-stamped sender, echoing
    /// the request's correlation. A request with neither (a root fire-and-forget)
    /// has nowhere to answer — the asker chose not to listen. Mirrors the poke
    /// doors' answer path exactly.
    template <class Answer>
    void answer(loom::Mail& mail, const Answer& a) {
        const loom::WeaveId to = mail.reply_to().valid() ? mail.reply_to() : mail.sender();
        if (!to.valid()) {
            return;
        }
        mail.send(to, a, mail.correlation());
    }

    Kernel* kernel_;
};

/// Register the control Weave on `bus` and return its id. mount() derives its
/// grant from the declared Emit<...> — the three standard reply shapes — so the
/// door can answer without any host-assembled authority.
inline loom::WeaveId mount_control(Kernel& kernel, loom::Switchboard& bus) {
    return loom::mount<ControlWeave>(bus, kernel);
}

/// The grant that lets a Weave drive the kernel: permission to send the five
/// control shapes to the control Weave, and only to it. This is the dangerous
/// grant — it is deliberately target-scoped, never allow_to_any.
inline loom::Grant load_capability(loom::WeaveId control) {
    loom::Grant g;
    g.allow(LoadLibrary::zen_name, LoadLibrary::zen_version, control);
    g.allow(ReloadLibrary::zen_name, ReloadLibrary::zen_version, control);
    g.allow(UnloadLibrary::zen_name, UnloadLibrary::zen_version, control);
    g.allow(UnloadRole::zen_name, UnloadRole::zen_version, control);
    g.allow(ListLibraries::zen_name, ListLibraries::zen_version, control);
    return g;
}

} // namespace loom

#endif // ZEN_KERNEL_CONTROL_HPP
