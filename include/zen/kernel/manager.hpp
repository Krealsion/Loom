// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_KERNEL_MANAGER_HPP
#define ZEN_KERNEL_MANAGER_HPP

// The Weave Manager, the lifecycle steward: an ordinary woven Weave that loads, swaps, reloads
// and lists by sending gated messages to the kernel's control door, and answers its asker
// with the standard reply shapes.
// docs/reference/lifecycle.md#graceful-swap--the-other-ceremony-and-when-to-prefer-it
//
// It has no privilege: its reach is `load_capability` (control.hpp), which the host grants at
// mount and which any participant could hold to drive the door directly. It cannot widen its
// own grant, reach the Kernel object, or register or kill anything. But it is a broker: it
// performs a dangerous capability for whoever may ask, and applies no policy of its own. So
// granting a weave `allow(zen.LoadWeave, manager)` grants it kernel reach, and the host's
// choice of who may reach the Manager is the only gate.
//
// A consumer that must survive its provider's replacement addresses it by role: the successor
// is a different weave with a different id. Every outcome crosses as a standard shape, never a
// throw:
//   zen.LoadWeave{name, path, role}   -> zen.Result{id}   | zen.Refused{why}
//   zen.SwapWeave{role, name, path}   -> zen.Result{id}   | zen.Refused{why}
//   zen.ReloadWeave{name, path}       -> zen.Ack          | zen.Refused{why}
//   zen.ListLoaded{}                  -> zen.Result{"a,b@role"}
// A weave is named by the file it is loaded from.
//
// Reload and swap are different machines. Reload is in place: same WeaveId, same state schema,
// state transplanted, and a differently shaped library refused. Swap replaces the role holder:
// the incumbent is unloaded and a successor loaded into the role with fresh state.
//
// A swap is two messages, UnloadRole then LoadLibrary, delivered in that order. Between them
// the role is unheld, and a send resolved then is refused `NoSuchTarget`. If the successor
// fails to load the role stays unheld and the asker gets the refusal. Traffic already queued
// to the incumbent still reaches it, but its own queued replies are refused `SenderLifeEnded`
// once it is removed (MSG-03). The unload's answer is unsolicited (correlation 0) and dropped:
// the load's answer says everything, and a role-addressed unload can remove only the holder.

// ---- the letter: a cooperative handoff ------------------------------------
// The protocol is weave/lifecycle.hpp's; the Manager is one consumer of it. A graceful swap is
// three conversations: learn whether the incumbent participates (or a non-participant would
// hang the swap), wait for its letter before unloading (a sender removed mid-queue loses its
// queued sends), and learn the successor's id (so only it may claim):
//
//   SwapWeave{graceful}  ->  QueryRole{role}  ->  RoleInfo{holder, converses}
//     converses?  no  ->  the hard swap, automatically
//                 yes ->  PrepareShutdown -> Bequest  (stamped sender == holder, or dropped)
//                     ->  store the letter, then unload + load + bind
//                     ->  the door's load Result names the heir; only it may claim
//
// Each stage matches both correlation and stamped sender, the sender being the one the
// previous stage established. Correlations come from the relay's counter, so they never
// collide with its own. The letter store is bounded, keyed by role, one letter each, answered
// once and inspectable; a letter whose load failed is discarded with the incumbent.

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
// Registered by hand so the wire names carry the "zen." prefix.

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

/// Replace whoever holds `role` with a fresh weave loaded from `path` under `name`; a
/// different state shape is expected. `graceful` first gives the incumbent the chance to
/// write its heir a letter: the same replacement with one stage in front, so a field rather
/// than another shape.
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
// All ordinary ZEN_SHAPEs, so the steward's bookkeeping and its mail can be poked. The state
// contains a relay rather than extending RelayState, which is shared.

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

/// A letter waiting to be claimed; `heir` is 0, and nobody can claim, until the successor's
/// load answers.
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

/// Both stores are bounded and shed their oldest.
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

/// The lifecycle steward. Mount it with mount_manager() and drive it by message.
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
        // Ask the door who holds the role and whether it converses before asking the
        // incumbent anything, so a weave that never opted in is never waited on.
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
            // Not a participant: the hard swap, automatically.
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

    /// Stage 3: the letter, accepted only from the weave the door named as holder; the bus
    /// stamps the sender.
    void on(const Bequest& letter, Mail& mail) {
        SwapInFlight* s = match_swap(mail, /*stage=*/1, 0);
        if (s == nullptr || mail.sender().value != static_cast<std::uint64_t>(s->incumbent)) {
            return;
        }
        // Filed under the role the steward asked about, never the role the payload claims.
        store_letter(s->role, letter.items);
        const std::int64_t load_seq = begin_replacement(mail, *s);
        s->stage = 2;
        s->seq = load_seq;
    }

    // Answers coming back: loom::relay matches correlation and stamped sender against its own
    // forwards, so a forged, stale or unsolicited reply is dropped.
    void on(const Result& a, Mail& mail) {
        // A load that completes a graceful swap names the heir, the only weave that may claim.
        if (SwapInFlight* s = match_swap(mail, /*stage=*/2, door())) {
            name_heir(s->role, parse_id(a.value));
            retire(s);
        }
        relay(mail, state_.relay, a);
    }

    void on(const Ack& a, Mail& mail) { relay(mail, state_.relay, a); }

    void on(const Refused& a, Mail& mail) {
        // An incumbent may accept PrepareShutdown and still decline to write: the swap goes on
        // without a letter.
        if (SwapInFlight* s = match_swap(mail, /*stage=*/1, 0)) {
            if (mail.sender().value == static_cast<std::uint64_t>(s->incumbent)) {
                const std::int64_t load_seq = begin_replacement(mail, *s);
                s->stage = 2;
                s->seq = load_seq;
                return; // this refusal was the incumbent's, not the asker's answer
            }
        }
        // A load that failed has no heir; its letter is discarded.
        if (SwapInFlight* s = match_swap(mail, /*stage=*/2, door())) {
            drop_letter(s->role);
            retire(s);
        }
        relay(mail, state_.relay, a);
    }

    /// The heir's claim, honored only from the weave recorded as the role's successor, once,
    /// through `mail.answer` (ANS-01). The heir reaches the steward by role and cannot know its
    /// id in advance, so only Loom's attested answer tells it the letter came from the
    /// incarnation it asked, not from another weave holding the grant for `zen.Bequest`. The
    /// answer is still authorized against the ordinary grant.
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
        // No letter, or not yours: an authenticated refusal, which ends the heir's wait.
        mail.answer(Refused{"no bequest is held for role '" + c.role + "' for you"});
    }

private:
    std::int64_t door() const { return static_cast<std::int64_t>(control_.value); }

    static WeaveId asker_of(const Mail& mail) {
        return mail.reply_to().valid() ? mail.reply_to() : mail.sender();
    }

    /// The hard swap: unload the role's holder, load the successor into it, relay the load's
    /// answer to the asker.
    void hard_swap(Mail& mail, const std::string& role, const std::string& name,
                   const std::string& path, WeaveId asker, std::uint64_t corr) {
        mail.send(control_, UnloadRole{role}, /*correlation=*/0);
        (void)forward_for(mail, state_.relay, door(), LoadLibrary{name, path, role}, asker, corr);
    }

    /// The same two messages, from a recorded swap.
    std::int64_t begin_replacement(Mail& mail, const SwapInFlight& s) {
        mail.send(control_, UnloadRole{s.role}, /*correlation=*/0);
        return forward_for(mail, state_.relay, door(), LoadLibrary{s.name, s.path, s.role},
                           WeaveId{static_cast<std::uint64_t>(s.asker)},
                           static_cast<std::uint64_t>(s.corr));
    }

    /// The in-flight swap this message answers: correlation and stage must match, and a non-zero
    /// `required_sender` must be the stamped sender. Stage 1's sender, the incumbent, is checked
    /// by the caller.
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

    /// One letter per role: a newer swap replaces an unclaimed letter. Items are clamped to
    /// `kMaxBequestItems`.
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

/// The grant a Weave Manager needs, assembled by the host: the kernel door, target-scoped, and
/// its answers to askers. The three reply shapes are granted explicitly although
/// allow_poke_answers also covers them: they are two authorities that happen to coincide. Not
/// mount()'s emit-derived grant, which would let it send control shapes to any accepter.
inline Grant manager_capability(WeaveId control) {
    Grant g = load_capability(control);
    g.allow_to_any(Result::zen_name, Result::zen_version);
    g.allow_to_any(Ack::zen_name, Ack::zen_version);
    g.allow_to_any(Refused::zen_name, Refused::zen_version);
    // The letter, both ways, to any target: the incumbent and the heir are learned at run
    // time. Only these two lifecycle shapes: the steward can conduct a succession and cannot
    // say one domain word.
    g.allow_to_any(PrepareShutdown::zen_name, PrepareShutdown::zen_version);
    g.allow_to_any(Bequest::zen_name, Bequest::zen_version);
    allow_poke_answers(g); // the steward is itself inspectable
    return g;
}

/// Mount a Weave Manager wired to `control`, with the grant above, bound to the steward role.
/// An heir knows neither its predecessor nor the steward's id, so it reaches the steward by a
/// role that outlives every swap; a second Manager's mount therefore throws.
inline WeaveId mount_manager(WeaveId control, Switchboard& bus) {
    auto weave = std::make_unique<WeaveManager>(control);
    WeaveManager* raw = weave.get();
    WeaveId id = bus.register_weave(std::move(weave), manager_capability(control), kManagerRole);
    raw->zen_set_self(id);
    return id;
}

} // namespace loom

#endif // ZEN_KERNEL_MANAGER_HPP
