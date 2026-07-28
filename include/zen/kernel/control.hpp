#ifndef ZEN_KERNEL_CONTROL_HPP
#define ZEN_KERNEL_CONTROL_HPP

// The kernel's message door: operate the kernel like everything else — by sending
// it messages. A control Weave accepts LoadLibrary / ReloadLibrary / UnloadLibrary
// / UnloadRole / ListLibraries / QueryRole and calls the kernel's existing load /
// reload_from / unload / unload_role / loaded / query_role. The right to send
// those shapes to the control
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
//   QueryRole      -> RoleInfo{holder, converses}   (bespoke, by the razor)
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
//
// ---------------------------------------------------------------------------
// R2A-1 — THE ACTIVATION FACT. The door also tells a freshly committed weave, in
// one ordinary message, that it is live: zen.Activated (weave/lifecycle.hpp,
// which states the fact's exact meaning and its long list of non-meanings).
//
// WHY THE DOOR OWNS THIS, and not the Manager, the Kernel, or the Switchboard.
// LoadWeave/SwapWeave/ReloadWeave are Manager composites, but LoadLibrary and
// ReloadLibrary are the primitives that actually call the Kernel — and a
// participant holding load_capability may drive them with no Manager in the
// path at all (the manager suite pins exactly that). A lifecycle fact emitted by
// the Manager would therefore be FALSE ARCHITECTURE: two callers producing
// identical kernel changes, only one of which produced the lifecycle result.
// The door is the ordinary message participant sitting directly on the
// successful kernel operation: it knows the outcome, can name the target, has a
// bus-stamped identity, sends through ordinary Mail, and declares what it emits.
// The Kernel stays a thing that answers questions and never speaks through a
// privileged backchannel; the Switchboard stays routing.
//
// THERE IS NO SWAP-SPECIFIC ACTIVATION CODE ANYWHERE, and that absence is the
// ownership proof. A swap — hard or graceful — eventually succeeds through
// LoadLibrary, so its successor is activated for the same reason any dynamically
// loaded weave is. Neither this file nor the Manager contains a line that knows
// a swap is happening.
//
// WHAT IS NOT COVERED, said out loud: host-native mount<T>() weaves. This door
// only sees dynamic load/reload, so native mounts are simply not activated —
// not "activated silently", not "activated later". Nothing here claims them.
//
// The immediate downstream consumer is Zengine's Timer, which will author its
// beat chain from an activation instead of a one-shot host wind — that is R2A-2
// and no line of it exists yet.

#include <zen/weave.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/kernel/kernel.hpp>
#include <zen/switchboard.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
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

/// Ask about a role's holder: who it is, and whether it declares
/// `zen.PrepareShutdown` — i.e. whether it will converse about its own
/// succession. The steward asks this BEFORE asking anything of the incumbent,
/// so a weave that never opted in is never waited on.
struct QueryRole {
    std::string role;
    ZEN_SHAPE(QueryRole, 1, ZEN_FIELD(role));
};

/// The answer. Bespoke rather than a standard reply, by the least-complete-
/// information razor: BOTH fields are load-bearing and their absence breaks the
/// image. `converses` alone cannot be acted on — a steward that asks an
/// incumbent for its letter must know which weave's stamped sender to require on
/// the way back, or it cannot tell the letter from a forgery. `holder` alone
/// cannot be acted on either. Two facts, one decision.
///
/// `holder == 0` means no KERNEL-LOADED weave holds the role — unheld, or held
/// by a native weave the kernel cannot see into (Kernel::query_role documents
/// why the two are not worth distinguishing). Either way: non-participant.
struct RoleInfo {
    std::int64_t holder;
    bool converses;
    ZEN_SHAPE(RoleInfo, 1, ZEN_FIELD(holder), ZEN_FIELD(converses));
};

/// The control Weave's state: how many operations it has performed, and the
/// activation sequence it has reached.
///
/// `last_activation` is the highest sequence this control lineage has emitted;
/// 0 means it has emitted none. Kept in the STATE, not in a live member, which
/// is the whole point: it snapshots and revives like any other weave's
/// bookkeeping, so a revived control weave continues its lineage instead of
/// restarting it (pinned in the manager suite). It is deliberately NOT `ops`:
/// operation count and activation identity are different facts that happen to
/// move near one another today — `ops` counts every command including the ones
/// that refuse or activate nobody.
///
/// IT IS A FINITE SIGNED INTEGER, AND THE CONTRACT IS BOUNDED BY THAT. The
/// lineage's valid range is [0, INT64_MAX]: the last representable sequence is
/// INT64_MAX, emitted exactly once, and every lifecycle operation after it is
/// REFUSED rather than wrapped (activation_block()). A revived state outside
/// that range — the gate admits any Int, so a negative value is constructible
/// through the ordinary revival path — has no valid continuation and refuses the
/// same way. Neither state is normalized, absolute-valued, or wrapped into an
/// apparently-healthy lineage; a forged activation identity would be worse than
/// a refused load.
///
/// v2: `last_activation` joined the shape. `zen.ControlState` v1 meant {ops} and
/// still does, forever — the immutable-published-schema rule, paid as usual.
struct ControlState {
    std::int64_t ops;
    std::int64_t last_activation;
    ZEN_SHAPE(ControlState, 2, ZEN_FIELD(ops), ZEN_FIELD(last_activation));
};

/// The two ways a control lineage can be unable to name another activation.
/// Ordinary self-contained refusal reasons on the existing `zen.Refused` path —
/// no new lifecycle vocabulary, and each says which boundary was hit rather than
/// collapsing both into one half-true word.
inline constexpr const char* kActivationExhausted =
    "activation sequence exhausted; lifecycle operation refused";
inline constexpr const char* kActivationInvalid =
    "activation sequence state is invalid; lifecycle operation refused";

/// A Weave whose handlers drive a Kernel. Its authority to *reach* the kernel is
/// its accept-set being reachable, gated by the *sender's* load capability; its
/// authority to *answer* is the ordinary Emit<...> grant any weave gets.
class ControlWeave
    : public loom::WeaveBase<ControlWeave, ControlState,
                             loom::Accept<LoadLibrary, ReloadLibrary, UnloadLibrary, UnloadRole,
                                          ListLibraries, QueryRole>,
                             loom::Emit<loom::Result, loom::Ack, loom::Refused, RoleInfo,
                                        loom::Activated>> {
public:
    explicit ControlWeave(Kernel& kernel) : kernel_(&kernel) {}

    void on(const LoadLibrary& m, loom::Mail& mail) {
        ++state_.ops;
        if (const char* blocked = activation_block()) {
            answer(mail, loom::Refused{blocked});
            return; // the Kernel is never called: nothing is opened, nothing bound
        }
        const LoadResult r = kernel_->load(m.name, m.path, m.role);
        if (r.ok) {
            // The weave is registered, its role (if any) bound, its manifest and
            // initial state admitted — the incarnation is committed, so the fact
            // is now true and may be said. Said BEFORE the asker's answer, so the
            // activation is already queued when the operator hears "loaded".
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
        // `reloaded` is the only success; every other outcome — not loaded, open
        // failure, the state-schema mismatch, the accepted-contract mismatch, a
        // refused revive — has already written its own self-contained reason
        // into `error`. No almost-activated: only the success path speaks.
        if (r.reloaded) {
            // A reload PRESERVES the logical WeaveId but is still a new code
            // incarnation, so it earns its own, newer, activation. The id is
            // resolved from the loaded name after success — the door does not
            // have to have been told it.
            announce_activation(mail, kernel_->weave_id(m.name));
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

    void on(const QueryRole& m, loom::Mail& mail) {
        ++state_.ops;
        const Kernel::RoleQuery q =
            kernel_->query_role(m.role, PrepareShutdown::zen_name, PrepareShutdown::zen_version);
        answer(mail, RoleInfo{static_cast<std::int64_t>(q.holder.value), q.accepts});
    }

private:
    /// Why this lineage cannot name another activation, or nullptr if it can.
    ///
    /// THE PREFLIGHT, and it runs BEFORE the Kernel is called. The alternative
    /// ordering is the one that lies: commit the load or reload, *then* discover
    /// that the promised activation cannot be represented, and leave a freshly
    /// installed participating incarnation reported as successful while its own
    /// declared post-commit fact never happened. The door must be able to name an
    /// activation before it starts an operation that may owe one.
    ///
    /// THE ACCEPTED CONSERVATISM, said out loud: at the boundary this refuses
    /// without first learning whether the candidate would even participate. It
    /// cannot know — a candidate's accepted schemas are unknown until the Kernel
    /// inspects or loads it, and calling the Kernel to find out is exactly the
    /// thing the before-Kernel boundary exists to prevent. So a lineage at the
    /// limit refuses one final *non-participating* load it could in principle
    /// have served. That is a deliberate trade: the limit is unreachable by any
    /// natural operation, and correctness at the boundary is worth more than the
    /// last load before it.
    const char* activation_block() const {
        if (state_.last_activation < 0) {
            return kActivationInvalid;
        }
        if (state_.last_activation == std::numeric_limits<std::int64_t>::max()) {
            return kActivationExhausted;
        }
        return nullptr;
    }

    /// The SOLE mutation point for the sequence, and it is total: it refuses
    /// rather than wrapping, so no arithmetic path here can produce a
    /// non-positive or reused activation identity. `++` is safe precisely because
    /// the guard above it has already excluded the only two inputs that make it
    /// unsafe. Nothing is consumed when nothing can be allocated.
    std::optional<std::int64_t> next_activation() {
        if (activation_block() != nullptr) {
            return std::nullopt;
        }
        return ++state_.last_activation;
    }

    /// Tell a freshly committed incarnation that it is live — if, and only if, it
    /// said it would listen.
    ///
    /// PARTICIPATION IS ASKED, NEVER ATTEMPTED. Sending blindly and calling the
    /// resulting refusal "optional participation" would be a lie in two
    /// directions: it manufactures refusal noise on the tap for weaves that did
    /// nothing wrong, and it spends an activation sequence on a conversation that
    /// never happened. So the accept-set is checked first, and a non-participant
    /// costs exactly nothing — no message, no refusal, no sequence, no change to
    /// the operation's own result.
    ///
    /// Correlation 0: an activation is an EVENT, not an answer to an ask. Nothing
    /// is awaited and nothing may be inferred from silence — what a participant
    /// does with the fact is its own business, and it does not owe the door a
    /// reply.
    void announce_activation(loom::Mail& mail, loom::WeaveId target) {
        if (!target.valid() ||
            !kernel_->accepts(target, loom::Activated::zen_name, loom::Activated::zen_version)) {
            return;
        }
        // Allocated only now, for an activation that WILL be emitted. Monotonic
        // within this lineage across snapshot and revival because it lives in
        // the state — and never reused *because next_activation() refuses rather
        // than wrapping* when the lineage has no valid continuation, which is the
        // boundary that makes the word "never" true rather than aspirational.
        const std::optional<std::int64_t> sequence = next_activation();
        if (!sequence) {
            // UNREACHABLE TODAY — every handler that gets here preflighted
            // activation_block() and refused. It is written anyway, and as a
            // silent nothing rather than a wrap, so that a future reordering
            // degrades into a visibly missing activation (a load that succeeded
            // and told nobody) instead of a forged identity on the wire. A gap is
            // recoverable; a lie about which incarnation this is, is not.
            return;
        }
        mail.send(target, loom::Activated{*sequence}, /*correlation=*/0);
    }

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

/// The grant that lets a Weave drive the kernel: permission to send the six
/// control shapes to the control Weave, and only to it. This is the dangerous
/// grant — it is deliberately target-scoped, never allow_to_any.
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
