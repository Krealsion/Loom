#ifndef ZEN_KERNEL_MANAGER_HPP
#define ZEN_KERNEL_MANAGER_HPP

// The Weave Manager: the lifecycle steward.
//
// Until now, operating the system was a different gesture from using it — using
// it meant sending messages; operating it meant calling C++ on the host's Kernel.
// The Manager closes that gap. It is an ORDINARY woven Weave holding a job: it
// orchestrates load / swap / reload / list by sending gated messages to the
// kernel's control door, and answers its asker with the standard reply shapes.
//
// NO PRIVILEGE. Its authority over the kernel door is exactly `load_capability`
// (control.hpp) — an explicit grant the host assembles at mount, target-scoped to
// the control Weave. Any participant could hold that same grant and drive the door
// directly, with no Manager in the path; the manager suite pins that. The host
// keeps the pen: the Manager cannot widen its own grant, cannot reach the Kernel
// object, and cannot register or kill anything itself. It is REPLACEABLE DEFAULT
// TOOLING — orchestration, never exclusivity.
//
// BUT READ THIS BEFORE GRANTING REACH TO IT. The Manager is a BROKER for the
// kernel door, in the powerbox's exact sense: it holds a dangerous capability and
// performs it on behalf of whoever may ask. So granting a weave
// `allow(zen.LoadWeave, manager)` IS granting it kernel reach, transitively — the
// grant to reach the Manager must be weighed as the dangerous grant it is, not as
// ordinary messaging. And the Manager applies NO policy of its own: it does not
// scope by stamped sender, does not decide who may load what, and asks no
// questions. Anyone permitted to ask is permitted to have. That is deliberate for
// this birth — the decision of who may load what is the triage brain's, and it
// does not exist yet — but it means the ONLY gate today is the host's decision
// about who may reach the Manager at all.
//
// ADDRESSING IS ROLE-FIRST. A consumer that must survive its provider being
// replaced addresses the provider by ROLE, never by WeaveId: the successor is a
// different weave with a different id, and only the role slot carries the
// consumer's reach across the swap.
//
// FAILURES ARE VALUES. Every outcome crosses the Manager's boundary as a standard
// shape — zen.Result / zen.Ack / zen.Refused, each refusal carrying its own
// self-contained why — never as a throw. A composition event is data a steward can
// triage; only a programming bug is an exception.
//
// The ops (path-addressed loading, honestly: naming a weave by the file it lives
// in is what the kernel can do today — content-addressed identity belongs to the
// identity phase):
//   zen.LoadWeave{name, path, role}   -> zen.Result{id}   | zen.Refused{why}
//   zen.SwapWeave{role, name, path}   -> zen.Result{id}   | zen.Refused{why}
//   zen.ReloadWeave{name, path}       -> zen.Ack          | zen.Refused{why}
//   zen.ListLoaded{}                  -> zen.Result{"a,b@role"}
//
// SwapWeave and ReloadWeave are deliberately TWO OPS, not one: they are different
// machines and deserve different names. Reload is reload-IN-PLACE — same weave,
// same WeaveId, same state schema, state transplanted; a differently-shaped
// library is a clean refusal. Swap REPLACES THE ROLE HOLDER — the incumbent is
// unloaded, a successor is loaded into the role, and its state starts fresh; a
// differently-shaped successor is the normal case, not an error. Folding them into
// one op would invite exactly the quiet growth of "reload" into "replace" that the
// two names exist to prevent.
//
// THE SWAP WINDOW, stated honestly. Swap issues two messages: UnloadRole{role},
// then LoadLibrary{name, path, role}. The bus is single-threaded and FIFO, so the
// unload is always delivered first — but between the two deliveries the role is
// UNHELD, and a send that resolves in that window refuses cleanly (NoSuchTarget,
// exactly as an unmounted provider does) rather than blocking or silently
// vanishing. That is the optional-participation floor doing its job, and it is
// pinned. If the successor fails to load, the role STAYS unheld and the asker gets
// the Refused with its reason: the incumbent is gone and its slot is empty. That
// friction is real and is admitted at floor tier. An invisible/atomic rebind is a
// named refinement, to be pulled only if the window is ever actually felt.
//
// The sharpest edge of that window, found by building it and pinned in the suite:
// inbound traffic already queued still reaches the incumbent (the swap's own
// messages go to the queue's TAIL), but the incumbent's in-flight REPLIES do not
// survive it. A gated message is authorized by looking its sender up at DELIVERY
// time, so once the incumbent is unregistered its still-queued answers fail that
// lookup and are refused CapabilityDenied — fail-closed and correct, but it means
// an in-flight request to the incumbent can be answered into the void. This is a
// property of unregistering ANY live weave mid-queue, not something the swap
// invented; the swap is simply the first op that makes it routine. It is the
// concrete thing an atomic rebind would have to solve.
//
// The unload's own answer is deliberately unsolicited (correlation 0, which no
// relay sequence can ever equal — they start at 1), so it is dropped by the
// consumer obligation rather than relayed. That is not a dark fate: the unload's
// outcome is fully subsumed by the load's. If the role was unheld, the unload
// "fails" and the load then binds it — which is precisely what the asker asked
// for. If the role was held, the unload frees it and the load's answer reports
// whether the successor took it. And because the unload is addressed BY ROLE, it
// cannot destroy a weave the asker did not name: the only thing it can unload is
// the role's holder.

#include <zen/kernel/control.hpp>
#include <zen/weave.hpp>
#include <zen/weave/relay.hpp>

#include <cstdint>
#include <string>
#include <tuple>

namespace loom {

// ---- the lifecycle-operator command shapes ---------------------------------
// Hand-written registration blocks so the wire names carry the substrate's
// "zen." prefix, which #ShapeName cannot produce.

/// Load a weave, optionally binding it to a role.
struct LoadWeave {
    std::string name;
    std::string path;
    std::string role; ///< empty = bind no role
    using ZenSelf = LoadWeave;
    static constexpr const char* zen_name = "zen.LoadWeave";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(path), ZEN_FIELD(role));
    }
};

/// Replace whoever holds `role` with a fresh weave loaded from `path` under
/// `name`. The successor starts with fresh state; a different state shape is
/// expected, not refused.
struct SwapWeave {
    std::string role;
    std::string name;
    std::string path;
    using ZenSelf = SwapWeave;
    static constexpr const char* zen_name = "zen.SwapWeave";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(role), ZEN_FIELD(name), ZEN_FIELD(path));
    }
};

/// Reload `name` in place from `path`: same WeaveId, state transplanted through
/// the gate. Never replaces — a differently-shaped library is refused, with why.
struct ReloadWeave {
    std::string name;
    std::string path;
    using ZenSelf = ReloadWeave;
    static constexpr const char* zen_name = "zen.ReloadWeave";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(ZEN_FIELD(name), ZEN_FIELD(path)); }
};

/// Ask what is loaded, and under which roles.
struct ListLoaded {
    using ZenSelf = ListLoaded;
    static constexpr const char* zen_name = "zen.ListLoaded";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() { return std::make_tuple(); }
};

/// The lifecycle steward. Mount it with mount_manager() and drive it by message
/// (the console composes the zen.*Weave commands with no console code at all).
/// Its whole state is the relay bookkeeping — honest, bounded, poke-inspectable.
class WeaveManager
    : public WeaveBase<WeaveManager, RelayState,
                       Accept<LoadWeave, SwapWeave, ReloadWeave, ListLoaded, Result, Ack, Refused>,
                       Emit<LoadLibrary, ReloadLibrary, UnloadRole, ListLibraries, Result, Ack,
                            Refused>> {
public:
    explicit WeaveManager(WeaveId control) : control_(control) {}

    void on(const LoadWeave& c, Mail& mail) {
        forward(mail, state_, door(), LoadLibrary{c.name, c.path, c.role});
    }

    void on(const SwapWeave& c, Mail& mail) {
        // Two messages, one answer. The bus is FIFO and non-reentrant, so the
        // unload lands before the load without either of them nesting inside the
        // other's handler — the ordering the swap depends on is the substrate's
        // documented dispatch contract, not a hope.
        mail.send(WeaveId{static_cast<std::uint64_t>(door())}, UnloadRole{c.role},
                  /*correlation=*/0);
        forward(mail, state_, door(), LoadLibrary{c.name, c.path, c.role});
    }

    void on(const ReloadWeave& c, Mail& mail) {
        forward(mail, state_, door(), ReloadLibrary{c.name, c.path});
    }

    void on(const ListLoaded&, Mail& mail) { forward(mail, state_, door(), ListLibraries{}); }

    // The answers coming back from the door. loom::relay is the wall: it matches
    // correlation AND bus-stamped sender against its own outstanding forwards, so
    // a forged, stale, or unsolicited standard reply is dropped, never relayed.
    void on(const Result& a, Mail& mail) { relay(mail, state_, a); }
    void on(const Ack& a, Mail& mail) { relay(mail, state_, a); }
    void on(const Refused& a, Mail& mail) { relay(mail, state_, a); }

private:
    std::int64_t door() const { return static_cast<std::int64_t>(control_.value); }

    WeaveId control_; ///< the kernel's control door — host-supplied wiring, not state
};

/// The grant a Weave Manager needs. Assembled by the HOST at mount and handed in
/// whole: the dangerous half (the kernel door, target-scoped) plus the ordinary
/// half (its answers to askers).
///
/// The three standard reply shapes are granted EXPLICITLY even though
/// allow_poke_answers happens to cover the same three. They are two different
/// authorities that coincide: one is the construction layer answering pokes, the
/// other is the Manager's own maker code answering its asker. Spelling both out
/// keeps them from silently depending on each other.
///
/// This is deliberately NOT mount()'s emit-default grant: that would derive
/// `allow_to_any(LoadLibrary)` from the Emit<...> declaration, letting the
/// Manager send the kernel's control shapes to ANY accepter. The dangerous grant
/// must name its target.
inline Grant manager_capability(WeaveId control) {
    Grant g = load_capability(control);
    g.allow_to_any(Result::zen_name, Result::zen_version);
    g.allow_to_any(Ack::zen_name, Ack::zen_version);
    g.allow_to_any(Refused::zen_name, Refused::zen_version);
    allow_poke_answers(g); // the steward is itself inspectable
    return g;
}

/// Mount a Weave Manager wired to `control`, with the grant above.
inline WeaveId mount_manager(WeaveId control, Switchboard& bus) {
    return mount_granted<WeaveManager>(bus, manager_capability(control), control);
}

} // namespace loom

#endif // ZEN_KERNEL_MANAGER_HPP
