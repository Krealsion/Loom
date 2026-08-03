// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

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

// ---------------------------------------------------------------------------
// 1b — THE LETTER (cooperative handoff). The protocol vocabulary is Loomstd-tier
// and lives in weave/lifecycle.hpp; the Manager is *a* consumer of it, not its
// owner. Read that header first: it states the two laws (the letter must not know
// the gap; the letter dies with its sender unless waited for) that give the
// design below its shape.
//
// A GRACEFUL SWAP IS THREE CONVERSATIONS, not one. The Manager must know whether
// the incumbent participates before it asks anything (or a non-participant would
// hang the swap forever), must WAIT for the letter before unloading (or the
// letter is refused CapabilityDenied as an in-flight send from a dead sender),
// and must learn the successor's id (or no one can be authorized to claim):
//
//   SwapWeave{graceful}  ->  QueryRole{role}          ->  RoleInfo{holder, converses}
//     converses?  no  ->  the 1a hard swap, automatically, no hang
//                 yes ->  PrepareShutdown -> Bequest  (stamped sender == holder, or dropped)
//                     ->  store the letter, THEN unload + load + bind
//                     ->  the door's load Result names the heir; only it may claim
//
// Each stage is matched by BOTH its correlation and its bus-stamped sender, and
// the sender required is the one the *previous* stage established — never a field
// on the payload. A third party emitting a perfectly-shaped Bequest with a lucky
// correlation is speaking as itself, not as the incumbent, so it is dropped.
//
// Correlations for the chain are drawn from the SAME counter the relay uses, so a
// chain correlation can never collide with a relay's. Two matching tables, one
// number space: `relay.pending` holds answers bound for the ASKER, `swaps` holds
// answers bound for the chain itself.
//
// WHAT THE STEWARD KEEPS, AND FOR HOW LONG. The letter store is bounded, keyed by
// role, latest-only (a newer swap replaces an unclaimed letter), answered exactly
// once, and poke-inspectable — the steward keeps no secret mail. A letter whose
// load FAILED is discarded: no successor exists to authorize a claim, and
// unclaimable mail is a leak wearing the costume of a feature. That does mean a
// failed graceful swap loses the letter along with the incumbent — the honest
// extension of 1a's failed-swap friction, pinned rather than papered over.

#include <zen/kernel/control.hpp>
#include <zen/weave.hpp>
#include <zen/weave/lifecycle.hpp>
#include <zen/weave/relay.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

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
///
/// `graceful` asks for the ceremony: give the incumbent a chance to write its
/// heir a letter first. It is a FIELD rather than a sibling op because this is
/// the same machine — replace-the-role-holder — with one extra stage in front of
/// it, and the three parameters that describe the replacement are identical. A
/// sibling op would duplicate role/name/path and let the two drift. (Contrast
/// SwapWeave vs ReloadWeave, which ARE different machines and so are different
/// ops: that distinction is about mechanism, not ceremony.)
///
/// v2: `graceful` joined the shape. (LoadLibrary v2 set this precedent — a
/// changed shape earns a version, so a frozen (name, version) keeps meaning
/// exactly what it meant.)
struct SwapWeave {
    std::string role;
    std::string name;
    std::string path;
    bool graceful = false;
    using ZenSelf = SwapWeave;
    static constexpr const char* zen_name = "zen.SwapWeave";
    static constexpr std::uint32_t zen_version = 2;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(role), ZEN_FIELD(name), ZEN_FIELD(path),
                               ZEN_FIELD(graceful));
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

// ---- the steward's own state ------------------------------------------------
// All of it is an ordinary ZEN_SHAPE, which is the point: the steward's
// bookkeeping AND its mail are poke-inspectable by construction. That constraint
// is what decided the shape — RelayState is shared substrate and must not grow
// domain fields, so the Manager's state is a sibling struct that CONTAINS a
// relay rather than an extended relay.

/// One graceful swap, mid-conversation.
struct SwapInFlight {
    std::int64_t seq = 0;       ///< the correlation this stage is waiting on
    std::int64_t asker = 0;     ///< captured from routing metadata when the op arrived
    std::int64_t corr = 0;      ///< the asker's own correlation, restored at the end
    std::int64_t incumbent = 0; ///< established by RoleInfo; the sender a Bequest must carry
    std::int64_t stage = 0;     ///< 0 awaiting RoleInfo, 1 awaiting Bequest, 2 awaiting load
    std::string role;
    std::string name;
    std::string path;
    using ZenSelf = SwapInFlight;
    static constexpr const char* zen_name = "zen.SwapInFlight";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(seq), ZEN_FIELD(asker), ZEN_FIELD(corr),
                               ZEN_FIELD(incumbent), ZEN_FIELD(stage), ZEN_FIELD(role),
                               ZEN_FIELD(name), ZEN_FIELD(path));
    }
};

/// A letter waiting to be claimed. `heir` is 0 until the successor's load
/// answers; a letter with heir 0 is unclaimable by construction.
struct StoredLetter {
    std::int64_t heir = 0;
    std::string role;
    std::vector<Bytes> items;
    using ZenSelf = StoredLetter;
    static constexpr const char* zen_name = "zen.StoredLetter";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(heir), ZEN_FIELD(role), ZEN_FIELD(items));
    }
};

/// Both stores are bounded, oldest-shed — a steward is not a mailbox of infinite
/// depth, and saying the bound out loud beats discovering it as a leak.
inline constexpr std::size_t kMaxSwapsInFlight = 16;
inline constexpr std::size_t kMaxStoredLetters = 16;

struct ManagerState {
    RelayState relay;
    std::vector<SwapInFlight> swaps;
    std::vector<StoredLetter> letters;
    using ZenSelf = ManagerState;
    static constexpr const char* zen_name = "zen.ManagerState";
    static constexpr std::uint32_t zen_version = 1;
    static auto zen_fields() {
        return std::make_tuple(ZEN_FIELD(relay), ZEN_FIELD(swaps), ZEN_FIELD(letters));
    }
};

/// The lifecycle steward. Mount it with mount_manager() and drive it by message
/// (the console composes the zen.*Weave commands with no console code at all).
class WeaveManager
    : public WeaveBase<
          WeaveManager, ManagerState,
          Accept<LoadWeave, SwapWeave, ReloadWeave, ListLoaded, ClaimBequest, RoleInfo, Bequest,
                 Result, Ack, Refused>,
          Emit<LoadLibrary, ReloadLibrary, UnloadRole, ListLibraries, QueryRole, PrepareShutdown,
               Bequest, Result, Ack, Refused>> {
public:
    explicit WeaveManager(WeaveId control) : control_(control) {}

    void on(const LoadWeave& c, Mail& mail) {
        forward(mail, state_.relay, door(), LoadLibrary{c.name, c.path, c.role});
    }

    void on(const SwapWeave& c, Mail& mail) {
        if (!c.graceful) {
            hard_swap(mail, c.role, c.name, c.path, asker_of(mail), mail.correlation());
            return;
        }
        // The ceremony starts with a question, not a request: ask the door who
        // holds the role and whether they converse, BEFORE asking the incumbent
        // for anything. A weave that never opted in is never waited on — which is
        // what keeps the non-participation floor a floor and not a hang.
        if (state_.swaps.size() >= kMaxSwapsInFlight) {
            state_.swaps.erase(state_.swaps.begin());
        }
        const std::int64_t seq = ++state_.relay.next_seq;
        state_.swaps.push_back(SwapInFlight{seq, static_cast<std::int64_t>(asker_of(mail).value),
                                            static_cast<std::int64_t>(mail.correlation()), 0, 0,
                                            c.role, c.name, c.path});
        mail.send(control_, QueryRole{c.role}, static_cast<std::uint64_t>(seq));
    }

    void on(const ReloadWeave& c, Mail& mail) {
        forward(mail, state_.relay, door(), ReloadLibrary{c.name, c.path});
    }

    void on(const ListLoaded&, Mail& mail) { forward(mail, state_.relay, door(), ListLibraries{}); }

    /// Stage 2 of the ceremony: the door has told us who holds the role.
    void on(const RoleInfo& info, Mail& mail) {
        SwapInFlight* s = match_swap(mail, /*stage=*/0, door());
        if (s == nullptr) {
            return; // unsolicited or forged — the door is the only voice here
        }
        if (!info.converses || info.holder == 0) {
            // Non-participant: fall through to the 1a hard swap, automatically.
            const SwapInFlight done = *s;
            retire(s);
            hard_swap(mail, done.role, done.name, done.path, WeaveId{static_cast<std::uint64_t>(
                                                                 done.asker)},
                      static_cast<std::uint64_t>(done.corr));
            return;
        }
        s->incumbent = info.holder;
        s->stage = 1;
        s->seq = ++state_.relay.next_seq;
        mail.send(WeaveId{static_cast<std::uint64_t>(info.holder)}, PrepareShutdown{},
                  static_cast<std::uint64_t>(s->seq));
    }

    /// Stage 3: the letter arrived. THE WALL: it is accepted only from the weave
    /// the door named as the holder — a third party can emit a perfectly-shaped
    /// Bequest with a lucky correlation, but it speaks as itself, and the bus
    /// stamps the sender.
    void on(const Bequest& letter, Mail& mail) {
        SwapInFlight* s = match_swap(mail, /*stage=*/1, 0);
        if (s == nullptr || mail.sender().value != static_cast<std::uint64_t>(s->incumbent)) {
            return;
        }
        // Filed under the role the STEWARD asked about, never the role the
        // payload claims. The payload's role is the letter's self-description
        // (it travels on to the heir); the key is our own record.
        store_letter(s->role, letter.items);
        const std::int64_t load_seq = begin_replacement(mail, *s);
        s->stage = 2;
        s->seq = load_seq;
    }

    // The answers coming back. loom::relay is the wall for asker-bound answers:
    // it matches correlation AND bus-stamped sender against its own outstanding
    // forwards, so a forged, stale, or unsolicited standard reply is dropped.
    void on(const Result& a, Mail& mail) {
        // A load that completes a graceful swap names the heir — the only weave
        // that may ever claim the letter it is inheriting.
        if (SwapInFlight* s = match_swap(mail, /*stage=*/2, door())) {
            name_heir(s->role, parse_id(a.value));
            retire(s);
        }
        relay(mail, state_.relay, a);
    }

    void on(const Ack& a, Mail& mail) { relay(mail, state_.relay, a); }

    void on(const Refused& a, Mail& mail) {
        // An incumbent that accepts PrepareShutdown may still decline to write:
        // a refusal is an answer. The swap proceeds, letterless.
        if (SwapInFlight* s = match_swap(mail, /*stage=*/1, 0)) {
            if (mail.sender().value == static_cast<std::uint64_t>(s->incumbent)) {
                const std::int64_t load_seq = begin_replacement(mail, *s);
                s->stage = 2;
                s->seq = load_seq;
                return; // this refusal was the incumbent's, not the asker's answer
            }
        }
        // A load that failed has no heir, so its letter can never be claimed.
        // Discard it rather than keep unclaimable mail.
        if (SwapInFlight* s = match_swap(mail, /*stage=*/2, door())) {
            drop_letter(s->role);
            retire(s);
        }
        relay(mail, state_.relay, a);
    }

    /// The heir's question. Honored ONLY from the weave recorded as this role's
    /// successor, and answered exactly once — through the AUTHENTICATED path.
    ///
    /// Why `mail.answer` and not `mail.send` (R2B-1). An heir wakes knowing
    /// nothing and reaches the steward BY ROLE, precisely because it cannot know
    /// the steward's id — which means it cannot pre-bind the answer's sender, and
    /// a correlation plus a shape were the only things it had to go on. Any weave
    /// holding the grant for `zen.Bequest` could speak into that gap, and for a
    /// letter whose contents name identities (Zengine's Timer handoff names the
    /// weaves its firings are addressed to) that is not a cosmetic gap.
    ///
    /// `answer` closes it from this end: the authority to answer belongs to the
    /// DELIVERY, so only the incarnation that actually received this claim can
    /// produce Loom's word for it, once. Loom picks the recipient and the
    /// correlation, so this handler cannot aim its answer elsewhere even by
    /// mistake. Nothing about the steward's reach widens: the answer is still
    /// authorized against the ordinary grant, and the Manager still relays no
    /// domain traffic — it answers a claim, and that is all.
    void on(const ClaimBequest& c, Mail& mail) {
        if (!mail.sender().valid()) {
            return; // a root has no identity to be the heir of, and nowhere to answer
        }
        for (std::size_t i = 0; i < state_.letters.size(); ++i) {
            const StoredLetter& l = state_.letters[i];
            if (l.role != c.role || l.heir == 0 ||
                l.heir != static_cast<std::int64_t>(mail.sender().value)) {
                continue;
            }
            const Bequest answer{l.role, l.items};
            state_.letters.erase(state_.letters.begin() + static_cast<std::ptrdiff_t>(i));
            mail.answer(answer);
            return;
        }
        // No letter, or not yours. Either way the heir gets a real answer and
        // starts fresh — silence would leave it waiting on a letter that will
        // never come. This refusal is authenticated too: "there is nothing for
        // you" is exactly as load-bearing as a letter, because it is what ends
        // the heir's bounded wait.
        mail.answer(Refused{"no bequest is held for role '" + c.role + "' for you"});
    }

private:
    std::int64_t door() const { return static_cast<std::int64_t>(control_.value); }

    static WeaveId asker_of(const Mail& mail) {
        return mail.reply_to().valid() ? mail.reply_to() : mail.sender();
    }

    /// The 1a hard swap, unchanged: unload the role's holder, load the successor
    /// into it, relay the load's answer to the asker.
    void hard_swap(Mail& mail, const std::string& role, const std::string& name,
                   const std::string& path, WeaveId asker, std::uint64_t corr) {
        mail.send(control_, UnloadRole{role}, /*correlation=*/0);
        (void)forward_for(mail, state_.relay, door(), LoadLibrary{name, path, role}, asker, corr);
    }

    /// Same two messages, driven from a recorded swap rather than a live command.
    std::int64_t begin_replacement(Mail& mail, const SwapInFlight& s) {
        mail.send(control_, UnloadRole{s.role}, /*correlation=*/0);
        return forward_for(mail, state_.relay, door(), LoadLibrary{s.name, s.path, s.role},
                           WeaveId{static_cast<std::uint64_t>(s.asker)},
                           static_cast<std::uint64_t>(s.corr));
    }

    /// Find the in-flight swap this message answers: correlation AND stage must
    /// match, and if `required_sender` is non-zero the bus-stamped sender must be
    /// it. (Stage 1 passes 0 because its required sender is the per-swap
    /// incumbent, checked by the caller.)
    SwapInFlight* match_swap(const Mail& mail, std::int64_t stage, std::int64_t required_sender) {
        for (SwapInFlight& s : state_.swaps) {
            if (s.stage != stage || static_cast<std::uint64_t>(s.seq) != mail.correlation()) {
                continue;
            }
            if (required_sender != 0 &&
                mail.sender().value != static_cast<std::uint64_t>(required_sender)) {
                continue;
            }
            return &s;
        }
        return nullptr;
    }

    void retire(SwapInFlight* s) {
        for (auto it = state_.swaps.begin(); it != state_.swaps.end(); ++it) {
            if (&*it == s) {
                state_.swaps.erase(it);
                return;
            }
        }
    }

    /// Latest-letter-per-role: a newer swap replaces an unclaimed letter. The
    /// items are clamped to the published bound — a predecessor cannot make the
    /// steward hold more than it said it would.
    void store_letter(const std::string& role, const std::vector<Bytes>& items) {
        drop_letter(role);
        if (state_.letters.size() >= kMaxStoredLetters) {
            state_.letters.erase(state_.letters.begin());
        }
        StoredLetter l;
        l.heir = 0; // unclaimable until a successor is actually loaded
        l.role = role;
        l.items = items;
        if (l.items.size() > kMaxBequestItems) {
            l.items.resize(kMaxBequestItems);
        }
        state_.letters.push_back(std::move(l));
    }

    void drop_letter(const std::string& role) {
        for (auto it = state_.letters.begin(); it != state_.letters.end(); ++it) {
            if (it->role == role) {
                state_.letters.erase(it);
                return;
            }
        }
    }

    void name_heir(const std::string& role, std::int64_t heir) {
        for (StoredLetter& l : state_.letters) {
            if (l.role == role && l.heir == 0) {
                l.heir = heir;
                return;
            }
        }
    }

    static std::int64_t parse_id(const std::string& text) {
        std::int64_t v = 0;
        const char* first = text.data();
        const char* last = first + text.size();
        const std::from_chars_result r = std::from_chars(first, last, v);
        if (r.ec != std::errc{} || r.ptr != last) {
            return 0;
        }
        return v;
    }

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
    // The letter, both directions. These must be allow_to_any because their
    // targets are discovered at runtime — the incumbent the door names, the heir
    // the load produces — and neither id exists at mount. Note what is NOT here:
    // the steward is given the two LIFECYCLE shapes and nothing else, so it can
    // conduct a succession without being able to say a single domain word. That
    // is the grant wall that makes delivery PULL rather than push.
    g.allow_to_any(PrepareShutdown::zen_name, PrepareShutdown::zen_version);
    g.allow_to_any(Bequest::zen_name, Bequest::zen_version);
    allow_poke_answers(g); // the steward is itself inspectable
    return g;
}

/// Mount a Weave Manager wired to `control`, with the grant above, bound to the
/// well-known steward role.
///
/// The role is what makes the letter claimable at all: an heir wakes knowing
/// nothing — not its predecessor, not the steward's id — so it must reach the
/// steward by an address that outlives every swap. That is exactly what a role
/// is. It also makes the steward a declared singleton: mounting a second Manager
/// throws, which is the honest outcome for a slot that means "the steward".
inline WeaveId mount_manager(WeaveId control, Switchboard& bus) {
    auto weave = std::make_unique<WeaveManager>(control);
    WeaveManager* raw = weave.get();
    WeaveId id = bus.register_weave(std::move(weave), manager_capability(control), kManagerRole);
    raw->zen_set_self(id);
    return id;
}

} // namespace loom

#endif // ZEN_KERNEL_MANAGER_HPP
