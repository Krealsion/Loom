// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#include <zen/host/reactor.hpp>

#include <zen/host/line_input.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/weave/shape.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <chrono>
#include <poll.h>
#include <thread>
#endif

namespace loom::host {

namespace {

/// What a source is last told, so it hears each change once.
enum class Told { Nothing, Held, NotHeld };

/// How many places in the wait a set takes: each handle once, as the look names it once. A POSIX
/// descriptor waited on for reading and for room to write is one; a Windows socket says room to
/// write through its event, so `writable` takes nothing there.
std::size_t count_of(const WaitSet& set) {
    std::vector<std::int64_t> all = set.handles;
#ifndef _WIN32
    all.insert(all.end(), set.writable.begin(), set.writable.end());
#endif
    std::sort(all.begin(), all.end());
    return static_cast<std::size_t>(std::unique(all.begin(), all.end()) - all.begin());
}

bool same(const WaitSet& a, const WaitSet& b) {
    return a.handles == b.handles && a.writable == b.writable && a.messages == b.messages;
}

/// `a + b` for `b >= 0`, stopping at the largest reading.
std::int64_t later(std::int64_t a, std::int64_t b) {
    constexpr std::int64_t top = std::numeric_limits<std::int64_t>::max();
    return a > 0 && b > top - a ? top : a + b;
}

/// How long from `now` until the later `until`, stopping at the largest span.
std::int64_t span(std::int64_t now, std::int64_t until) {
    constexpr std::int64_t top = std::numeric_limits<std::int64_t>::max();
    return now < 0 && until > top + now ? top : until - now;
}

/// ONE TRUSTED ROLE: its claim as last read, and what its source was told.
struct Role {
    std::string name;
    bool claimed = false;
    WeaveId author{};
    std::uint64_t life = 0;
    std::uint64_t incarnation = 0;
    std::uint64_t revision = 0;
    std::uint32_t version = 0; ///< of the `WaitSet` it claimed: the one the wait reads, or another
    WaitSet set;
    bool held = false;
    std::string why;
    Told told = Told::Nothing;
    /// A claim this wait will not hold again: a handle the OS refused, or a wake it did not take.
    bool refused = false;
    std::uint64_t refused_revision = 0;
    /// The revision its last readiness answered, so a claim found ready again under it is seen.
    bool woken = false;
    std::uint64_t woken_revision = 0;
    /// The revision it was told its time came for: a claim's time is due once.
    bool due_told = false;
    std::uint64_t due_revision = 0;
    std::uint64_t wakes = 0;

    /// A new holder, a claim of another version, or none: what was told and done before says
    /// nothing about it.
    void forget_holder() {
        claimed = false;
        author = WeaveId{};
        life = 0;
        incarnation = 0;
        revision = 0;
        version = 0;
        set = WaitSet{};
        held = false;
        told = Told::Nothing;
        refused = false;
        woken = false;
        due_told = false;
    }

    std::size_t count() const { return count_of(set); }

    /// The time it waits until, while the wait holds its claim and has not told it that time came.
    std::optional<std::int64_t> pending_until() const {
        if (!held || !set.until_ms || (due_told && due_revision == revision)) {
            return std::nullopt;
        }
        return set.until_ms;
    }
};

/// Who one waited handle answers for: a role's index, the watched console, or what the host
/// serves itself.
constexpr std::size_t kLine = static_cast<std::size_t>(-1);
constexpr std::size_t kOwn = static_cast<std::size_t>(-2);

/// A look's timeout that has no limit: the wait lasts until something is ready.
constexpr std::int64_t kForever = -1;

} // namespace

struct Reactor::Impl {
    Impl(Switchboard& b, Clock* given) : bus(b), clock(given != nullptr ? *given : machine) {
#ifdef _WIN32
        timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
        if (timer == nullptr) {
            timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
#endif
    }
    ~Impl() {
#ifdef _WIN32
        if (timer != nullptr) {
            ::CloseHandle(timer);
        }
#endif
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    Switchboard& bus;
    MachineClock machine;
    Clock& clock;
    std::vector<Role> roles; ///< in the order trusted
    LineInput* line = nullptr;
    std::function<WaitSet()> own;
    WaitSet own_set;          ///< what `own` named for this wait
    bool own_held = false;    ///< whether this wait holds it
    bool own_refused = false; ///< a set naming a handle the OS refused: not waited on while named
    WaitSet own_refused_set;
    std::uint64_t woken = 0;
    std::uint64_t lasted = 0;
#ifdef _WIN32
    HANDLE timer = nullptr;
#endif

    /// What the wait says to a role. A source that does not accept this version of `Waited` is
    /// refused it at its door, which the tap shows.
    void say(Role& o, bool held, bool ready, bool due, const std::string& why) {
        Waited w;
        w.revision = static_cast<std::int64_t>(o.revision);
        w.held = held;
        w.ready = ready;
        w.why = why;
        w.due = due;
        w.now_ms = clock.now_ms();
        (void)bus.send_to_role(o.name, Message(to_value(w)));
    }

    /// Tell the role what the wait does with its claim now, if that changed.
    void tell(Role& o) {
        const Told now = o.held ? Told::Held : Told::NotHeld;
        if (now != o.told) {
            o.told = now;
            say(o, o.held, false, false, o.held ? std::string() : o.why);
        }
    }

    /// Hold a claim no longer, until it changes, and say why.
    void refuse(Role& o, std::string why) {
        o.refused = true;
        o.refused_revision = o.revision;
        o.held = false;
        o.why = std::move(why);
        tell(o);
    }

    /// Read every trusted role's claim and decide, in trust order, which the wait holds.
    std::size_t hold(std::size_t used) {
        const auto current = [](const SenseReading& r) {
            return r && r.value.has_value() && r.by.role_holder_is_current &&
                   r.by.author_life_is_current && r.by.author_incarnation_is_current;
        };
        for (Role& o : roles) {
            std::uint32_t version = WaitSet::zen_version;
            SenseReading r = bus.observe_role(o.name, WaitSet::zen_name, version);
            if (!current(r)) {
                // A claim of another version, which the wait cannot read: its source is told so
                // rather than left to wait on a wake that never comes.
                const WeaveId holder = bus.role_holder(o.name);
                for (const std::shared_ptr<const Schema>& s : bus.claimed_schemas(holder)) {
                    if (s->name() != WaitSet::zen_name || s->version() == WaitSet::zen_version) {
                        continue;
                    }
                    SenseReading other = bus.observe_role(o.name, s->name(), s->version());
                    if (current(other)) {
                        version = s->version();
                        r = std::move(other);
                        break;
                    }
                }
            }
            if (!current(r)) {
                o.forget_holder();
                o.why = "its holder has claimed nothing to wait on";
                continue;
            }
            if (!o.claimed || r.by.author != o.author || r.by.author_life != o.life ||
                r.by.author_incarnation != o.incarnation || version != o.version) {
                o.forget_holder();
                o.author = r.by.author;
                o.life = r.by.author_life;
                o.incarnation = r.by.author_incarnation;
                o.version = version;
            }
            o.claimed = true;
            o.revision = r.by.revision;
            if (version != WaitSet::zen_version) {
                o.set = WaitSet{};
                o.held = false;
                o.why = "it claims " + std::string(WaitSet::zen_name) + " version " +
                        std::to_string(version) + ", and the wait reads version " +
                        std::to_string(WaitSet::zen_version) + " only, as " +
                        std::string(Waited::zen_name) + " version " +
                        std::to_string(Waited::zen_version) + " answers it";
                tell(o);
                continue;
            }
            o.set = from_value<WaitSet>(*r.value);
            std::string why;
            if (o.refused && o.refused_revision == o.revision) {
                why = o.why;
            } else {
                o.refused = false;
                const std::size_t n = o.count();
#ifdef _WIN32
                const bool unholdable_messages = false;
#else
                const bool unholdable_messages = o.set.messages;
#endif
                if (unholdable_messages) {
                    why = "a POSIX wait holds no thread's window messages";
                } else if (used + n > kMaxWaitedHandles) {
                    why = "the wait holds " + std::to_string(kMaxWaitedHandles) +
                          " handles, and " + std::to_string(used) + " are taken";
                }
            }
            o.held = why.empty();
            o.why = std::move(why);
            if (o.held) {
                used += o.count();
            }
            tell(o);
        }
        return used;
    }

    /// The source is ready, due, or both: tell it once, unless the readiness is under the very
    /// claim its last readiness answered, which is a source that did not take it.
    void wake(Role& o, bool ready, bool due, Woke& woke) {
        if (!o.held || (!ready && !due)) {
            return;
        }
        if (ready && o.woken && o.woken_revision == o.revision) {
            refuse(o, "it was ready again before it claimed anew, so it did not take what woke "
                      "it; the wait holds it again once its claim changes");
            return;
        }
        if (ready) {
            o.woken = true;
            o.woken_revision = o.revision;
            ++woke.roles;
        }
        if (due) {
            o.due_told = true;
            o.due_revision = o.revision;
            ++woke.due;
        }
        ++o.wakes;
        say(o, true, ready, due, std::string());
    }

    /// What one look at the handles found.
    struct Found {
        bool line = false;
        bool own = false;
        std::vector<bool> ready; ///< per role
    };

    /// Wait at most `timeout` ms (`kForever`: no limit; 0: only look) on everything held, and say
    /// what is ready. A wait with nothing to wait on and no limit returns at once.
    Found look(std::int64_t timeout);

    Woke after_turn(std::optional<std::int64_t> ms);
};

#ifdef _WIN32

Reactor::Impl::Found Reactor::Impl::look(std::int64_t timeout) {
    Found found;
    found.ready.assign(roles.size(), false);
    // Every handle once (a Windows wait refuses a copy), with whoever waits on it.
    std::vector<HANDLE> hs;
    std::vector<std::vector<std::size_t>> owners;
    std::map<HANDLE, std::size_t> at;
    const auto add = [&](std::int64_t h, std::size_t owner) {
        const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(h));
        const auto [it, fresh] = at.emplace(handle, hs.size());
        if (fresh) {
            hs.push_back(handle);
            owners.emplace_back();
        }
        owners[it->second].push_back(owner);
    };
    bool messages = false;
    if (own_held) {
        for (const std::int64_t h : own_set.handles) {
            add(h, kOwn);
        }
        messages = own_set.messages;
    }
    for (std::size_t i = 0; i < roles.size(); ++i) {
        if (!roles[i].held) {
            continue;
        }
        for (const std::int64_t h : roles[i].set.handles) {
            add(h, i);
        }
        messages = messages || roles[i].set.messages;
    }
    bool at_once = timeout == 0;
    if (line != nullptr && !line->closed()) {
        if (line->ready()) {
            at_once = true;
            found.line = true;
        } else {
            for (const std::int64_t h : line->wait_set().handles) {
                add(h, kLine);
            }
        }
    }
    const DWORD sources = static_cast<DWORD>(hs.size());
    if (!at_once && sources == 0 && !messages && timeout == kForever) {
        return found; // nothing could end this wait
    }
    // The deadline as a high-resolution timer, relative, in 100 ns units.
    constexpr std::int64_t kLongestTimedMs = std::numeric_limits<std::int64_t>::max() / 10000;
    const bool timed = !at_once && timeout > 0 && timer != nullptr && [&] {
        LARGE_INTEGER due{};
        due.QuadPart = -std::min(timeout, kLongestTimedMs) * 10000;
        return ::SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE;
    }();
    if (timed) {
        hs.push_back(timer);
    }
    const DWORD count = static_cast<DWORD>(hs.size());
    const auto bounded = [](std::int64_t ms) {
        return static_cast<DWORD>(std::min<std::int64_t>(ms, INFINITE - 1));
    };
    const DWORD waited = at_once ? 0 : (timed || timeout == kForever ? INFINITE : bounded(timeout));
    DWORD r = WAIT_TIMEOUT;
    if (count == 0 && !messages) {
        if (waited != 0) {
            ::Sleep(waited);
        }
    } else {
        r = ::MsgWaitForMultipleObjectsEx(count, count == 0 ? nullptr : hs.data(), waited,
                                          messages ? QS_ALLINPUT : 0,
                                          messages ? MWMO_INPUTAVAILABLE : 0);
    }
    if (timed) {
        (void)::CancelWaitableTimer(timer);
    }
    if (r == WAIT_FAILED) {
        // A handle the OS will not wait on: its sources are held no longer, and this wait ends
        // without spinning on it. With none to blame, the wait lasts its deadline as a nap.
        bool blamed = false;
        for (DWORD i = 0; i < sources; ++i) {
            if (::WaitForSingleObject(hs[i], 0) != WAIT_FAILED) {
                continue;
            }
            for (const std::size_t owner : owners[i]) {
                if (owner == kOwn) {
                    own_refused = true;
                    own_refused_set = own_set;
                    blamed = true;
                } else if (owner != kLine && roles[owner].held) {
                    refuse(roles[owner], "a handle it names is not one this process can wait "
                                         "on; the wait holds it again once its claim changes");
                    blamed = true;
                }
            }
        }
        if (!blamed && !at_once && timeout > 0) {
            ::Sleep(bounded(timeout));
        }
        return found;
    }
    std::vector<bool> ready(sources, false);
    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + sources) {
        ready[r - WAIT_OBJECT_0] = true; // an auto-reset event this wait took is told here
    }
    for (DWORD i = 0; i < sources; ++i) {
        if (!ready[i] && ::WaitForSingleObject(hs[i], 0) == WAIT_OBJECT_0) {
            ready[i] = true;
        }
    }
    for (DWORD i = 0; i < sources; ++i) {
        if (!ready[i]) {
            continue;
        }
        for (const std::size_t owner : owners[i]) {
            if (owner == kLine) {
                found.line = true;
            } else if (owner == kOwn) {
                found.own = true;
            } else {
                found.ready[owner] = true;
            }
        }
    }
    if (messages && (r == WAIT_OBJECT_0 + count || HIWORD(::GetQueueStatus(QS_ALLINPUT)) != 0)) {
        found.own = found.own || (own_held && own_set.messages);
        for (std::size_t i = 0; i < roles.size(); ++i) {
            if (roles[i].held && roles[i].set.messages) {
                found.ready[i] = true;
            }
        }
    }
    return found;
}

#else // POSIX

Reactor::Impl::Found Reactor::Impl::look(std::int64_t timeout) {
    Found found;
    found.ready.assign(roles.size(), false);
    std::vector<pollfd> fds;
    std::vector<std::vector<std::size_t>> owners;
    std::map<int, std::size_t> at;
    const auto add = [&](std::int64_t h, short events, std::size_t owner) {
        const int fd = static_cast<int>(h);
        const auto [it, fresh] = at.emplace(fd, fds.size());
        if (fresh) {
            pollfd p{};
            p.fd = fd;
            fds.push_back(p);
            owners.emplace_back();
        }
        fds[it->second].events = static_cast<short>(fds[it->second].events | events);
        owners[it->second].push_back(owner);
    };
    if (own_held) {
        for (const std::int64_t h : own_set.handles) {
            add(h, POLLIN, kOwn);
        }
        for (const std::int64_t h : own_set.writable) {
            add(h, POLLOUT, kOwn);
        }
    }
    for (std::size_t i = 0; i < roles.size(); ++i) {
        if (!roles[i].held) {
            continue;
        }
        for (const std::int64_t h : roles[i].set.handles) {
            add(h, POLLIN, i);
        }
        for (const std::int64_t h : roles[i].set.writable) {
            add(h, POLLOUT, i);
        }
    }
    bool at_once = timeout == 0;
    if (line != nullptr && !line->closed()) {
        if (line->ready()) {
            at_once = true;
            found.line = true;
        } else {
            for (const std::int64_t h : line->wait_set().handles) {
                add(h, POLLIN, kLine);
            }
        }
    }
    if (!at_once && fds.empty() && timeout == kForever) {
        return found; // nothing could end this wait
    }
    int waited = 0;
    if (!at_once) {
        waited = timeout == kForever ? -1
                                     : static_cast<int>(std::min<std::int64_t>(timeout, 1 << 30));
    }
    int r = 0;
    if (fds.empty()) {
        if (waited > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(waited));
        }
    } else {
        r = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), waited);
    }
    if (r < 0) {
        return found; // a signal ended it, which is no source's readiness
    }
    for (std::size_t i = 0; r > 0 && i < fds.size(); ++i) {
        const short got = fds[i].revents;
        if (got == 0) {
            continue;
        }
        for (const std::size_t owner : owners[i]) {
            if ((got & POLLNVAL) != 0) {
                // Not an open descriptor: its sources are held no longer, so it never spins.
                if (owner == kOwn) {
                    own_refused = true;
                    own_refused_set = own_set;
                } else if (owner != kLine && roles[owner].held) {
                    refuse(roles[owner], "a descriptor it names is not open; the wait holds it "
                                         "again once its claim changes");
                }
            } else if (owner == kLine) {
                found.line = true;
            } else if (owner == kOwn) {
                found.own = true;
            } else {
                found.ready[owner] = true;
            }
        }
    }
    return found;
}

#endif

Reactor::Woke Reactor::Impl::after_turn(std::optional<std::int64_t> ms) {
    const std::int64_t now = clock.now_ms();
    std::size_t used = 0;
    if (line != nullptr && !line->closed()) {
        used += line->wait_set().handles.size();
    }
    own_held = false;
    if (own) {
        own_set = own();
        if (own_refused && !same(own_set, own_refused_set)) {
            own_refused = false;
        }
        own_held = !own_refused && used + count_of(own_set) <= kMaxWaitedHandles;
        if (own_held) {
            used += count_of(own_set);
        }
    }
    (void)hold(used);

    // The soonest deadline: the host's, and each held claim's time it has not yet been told of.
    const bool host_deadline = ms.has_value() && *ms > 0;
    std::optional<std::int64_t> soonest;
    if (ms.has_value()) {
        soonest = host_deadline ? later(now, *ms) : now;
    }
    const std::optional<std::int64_t> host_until = soonest;
    for (const Role& o : roles) {
        const std::optional<std::int64_t> until = o.pending_until();
        if (until && (!soonest || *until < *soonest)) {
            soonest = until;
        }
    }
    std::int64_t timeout = kForever;
    if ((soonest && *soonest <= now) || !clock.moves_on_its_own()) {
        timeout = 0;
    } else if (soonest) {
        timeout = span(now, *soonest);
    }
    const Found found = look(timeout);
    const bool any_ready = found.line || found.own ||
                           std::find(found.ready.begin(), found.ready.end(), true) !=
                               found.ready.end();
    if (!clock.moves_on_its_own() && !any_ready && soonest && *soonest > now) {
        clock.arrive(*soonest); // nothing came first, so what comes next is the deadline
    }

    Woke woke;
    woke.now_ms = clock.now_ms();
    woke.date_ms = clock.date_ms(); // read with it, and handed to the bus with it
    bus.begin_turn(TurnFacts{woke.now_ms, woke.date_ms}); // the turn this wait begins
    woke.line = found.line;
    woke.own = found.own;
    woke.deadline = host_deadline && woke.now_ms >= *host_until;
    for (std::size_t i = 0; i < roles.size(); ++i) {
        const std::optional<std::int64_t> until = roles[i].pending_until();
        wake(roles[i], found.ready[i], until.has_value() && woke.now_ms >= *until, woke);
    }
    if (woke.line || woke.own || woke.roles > 0 || woke.due > 0) {
        ++woken;
    } else if (woke.deadline) {
        ++lasted;
    }
    return woke;
}

Reactor::Reactor(Switchboard& bus) : impl_(std::make_unique<Impl>(bus, nullptr)) {}

Reactor::Reactor(Switchboard& bus, Clock& clock) : impl_(std::make_unique<Impl>(bus, &clock)) {}

Reactor::~Reactor() = default;

void Reactor::trust(std::string role) {
    if (trusts(role)) {
        return;
    }
    Role o;
    o.name = std::move(role);
    impl_->roles.push_back(std::move(o));
}

void Reactor::distrust(std::string_view role) {
    auto& roles = impl_->roles;
    const auto it = std::find_if(roles.begin(), roles.end(),
                                 [role](const Role& o) { return o.name == role; });
    if (it == roles.end()) {
        return;
    }
    if (it->told == Told::Held) {
        impl_->say(*it, false, false, false, "the host no longer waits on this role");
    }
    roles.erase(it);
}

bool Reactor::trusts(std::string_view role) const {
    return std::any_of(impl_->roles.begin(), impl_->roles.end(),
                       [role](const Role& o) { return o.name == role; });
}

void Reactor::watch(LineInput* input) { impl_->line = input; }

void Reactor::watch_own(std::function<WaitSet()> set) {
    impl_->own = std::move(set);
    impl_->own_held = false;
    impl_->own_refused = false;
}

Reactor::Woke Reactor::after_turn(std::int64_t ms) { return impl_->after_turn(ms); }

Reactor::Woke Reactor::after_turn() { return impl_->after_turn(std::nullopt); }

std::vector<Reactor::Source> Reactor::sources() const {
    std::vector<Source> out;
    for (const Role& o : impl_->roles) {
        Source s;
        s.role = o.name;
        s.claimed = o.claimed;
        s.held = o.held;
        s.revision = o.revision;
        s.handles = o.count();
        s.until_ms = o.pending_until();
        s.version = o.version;
        s.wakes = o.wakes;
        s.why = o.held ? std::string() : o.why;
        out.push_back(std::move(s));
    }
    return out;
}

std::uint64_t Reactor::woken() const noexcept { return impl_->woken; }

std::uint64_t Reactor::lasted() const noexcept { return impl_->lasted; }

} // namespace loom::host
