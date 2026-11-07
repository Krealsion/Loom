// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#include <zen/host/reactor.hpp>

#include <zen/host/line_input.hpp>
#include <zen/switchboard/switchboard.hpp>
#include <zen/weave/shape.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
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
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <thread>
#endif

namespace loom::host {

namespace {

/// What a source is last told, so it hears each change once.
enum class Told { Nothing, Held, NotHeld };

/// ONE TRUSTED OFFICE: its claim as last read, and what its source was told.
struct Office {
    std::string name;
    bool claimed = false;
    WeaveId author{};
    std::uint64_t life = 0;
    std::uint64_t incarnation = 0;
    std::uint64_t revision = 0;
    WaitSet set;
    bool held = false;
    std::string why;
    Told told = Told::Nothing;
    /// A claim this wait will not hold again: a handle the OS refused, or a wake it did not take.
    bool refused = false;
    std::uint64_t refused_revision = 0;
    /// The revision its last wake answered, so a claim found ready again under it is seen.
    bool woken = false;
    std::uint64_t woken_revision = 0;
    std::uint64_t wakes = 0;

    /// A new holder, or none: what the last one was told and did says nothing about it.
    void forget_holder() {
        claimed = false;
        author = WeaveId{};
        life = 0;
        incarnation = 0;
        revision = 0;
        set = WaitSet{};
        held = false;
        told = Told::Nothing;
        refused = false;
        woken = false;
    }

    std::size_t count() const {
#ifdef _WIN32
        return set.handles.size(); // a Windows socket says room to write through its event
#else
        return set.handles.size() + set.writable.size();
#endif
    }
};

/// Who one waited handle answers for: an office, or the watched console.
constexpr std::size_t kLine = static_cast<std::size_t>(-1);

} // namespace

struct Reactor::Impl {
    explicit Impl(Switchboard& b) : bus(b) {
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
    std::vector<Office> offices; ///< in the order trusted
    LineInput* line = nullptr;
    std::uint64_t woken = 0;
    std::uint64_t lasted = 0;
#ifdef _WIN32
    HANDLE timer = nullptr;
#endif

    void say(Office& o, bool held, bool ready, const std::string& why) {
        Waited w;
        w.revision = static_cast<std::int64_t>(o.revision);
        w.held = held;
        w.ready = ready;
        w.why = why;
        (void)bus.send_to_role(o.name, Message(to_value(w)));
    }

    /// Tell the office what the wait does with its claim now, if that changed.
    void tell(Office& o) {
        const Told now = o.held ? Told::Held : Told::NotHeld;
        if (now != o.told) {
            o.told = now;
            say(o, o.held, false, o.held ? std::string() : o.why);
        }
    }

    /// Hold a claim no longer, until it changes, and say why.
    void refuse(Office& o, std::string why) {
        o.refused = true;
        o.refused_revision = o.revision;
        o.held = false;
        o.why = std::move(why);
        tell(o);
    }

    /// Read every trusted office's claim and decide, in trust order, which the wait holds.
    std::size_t hold(std::size_t used) {
        for (Office& o : offices) {
            const SenseReading r = bus.observe_office(o.name, WaitSet::zen_name,
                                                      WaitSet::zen_version);
            const bool current = r && r.value.has_value() && r.by.office_holder_is_current &&
                                 r.by.author_life_is_current && r.by.author_incarnation_is_current;
            if (!current) {
                o.forget_holder();
                o.why = "its holder has claimed nothing to wait on";
                continue;
            }
            if (!o.claimed || r.by.author != o.author || r.by.author_life != o.life ||
                r.by.author_incarnation != o.incarnation) {
                o.forget_holder();
                o.author = r.by.author;
                o.life = r.by.author_life;
                o.incarnation = r.by.author_incarnation;
            }
            o.claimed = true;
            o.revision = r.by.revision;
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

    /// The source woke: tell it, unless its claim is the very one its last wake answered.
    void wake(Office& o, Woke& woke) {
        if (!o.held) {
            return;
        }
        if (o.woken && o.woken_revision == o.revision) {
            refuse(o, "it was ready again before it claimed anew, so it did not take what woke "
                      "it; the wait holds it again once its claim changes");
            return;
        }
        o.woken = true;
        o.woken_revision = o.revision;
        ++o.wakes;
        ++woke.offices;
        say(o, true, true, std::string());
    }

    Woke wait(std::int64_t ms);
};

#ifdef _WIN32

Reactor::Woke Reactor::Impl::wait(std::int64_t ms) {
    Woke woke;
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
    for (std::size_t i = 0; i < offices.size(); ++i) {
        if (!offices[i].held) {
            continue;
        }
        for (const std::int64_t h : offices[i].set.handles) {
            add(h, i);
        }
        messages = messages || offices[i].set.messages;
    }
    bool now = false;
    if (line != nullptr && !line->closed()) {
        if (line->ready()) {
            now = true;
            woke.line = true;
        } else {
            for (const std::int64_t h : line->wait_set().handles) {
                add(h, kLine);
            }
        }
    }
    const DWORD sources = static_cast<DWORD>(hs.size());
    const bool timed = !now && ms > 0 && timer != nullptr && [&] {
        LARGE_INTEGER due{};
        due.QuadPart = -ms * 10000; // relative, in 100 ns
        return ::SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE;
    }();
    if (timed) {
        hs.push_back(timer);
    }
    const DWORD count = static_cast<DWORD>(hs.size());
    const DWORD timeout = now || ms <= 0 ? 0 : (timed ? INFINITE : static_cast<DWORD>(ms));
    DWORD r = WAIT_TIMEOUT;
    if (count == 0 && !messages) {
        if (timeout != 0) {
            ::Sleep(timeout);
        }
    } else {
        r = ::MsgWaitForMultipleObjectsEx(count, count == 0 ? nullptr : hs.data(), timeout,
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
                if (owner != kLine && offices[owner].held) {
                    refuse(offices[owner], "a handle it names is not one this process can wait "
                                           "on; the wait holds it again once its claim changes");
                    blamed = true;
                }
            }
        }
        if (!blamed && !now && ms > 0) {
            ::Sleep(static_cast<DWORD>(ms));
        }
        return woke;
    }
    woke.deadline =
        !now && ms > 0 && (r == WAIT_TIMEOUT || (timed && r == WAIT_OBJECT_0 + count - 1));
    std::vector<bool> ready(sources, false);
    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + sources) {
        ready[r - WAIT_OBJECT_0] = true; // an auto-reset event this wait took is told here
    }
    for (DWORD i = 0; i < sources; ++i) {
        if (!ready[i] && ::WaitForSingleObject(hs[i], 0) == WAIT_OBJECT_0) {
            ready[i] = true;
        }
    }
    std::vector<bool> office_ready(offices.size(), false);
    for (DWORD i = 0; i < sources; ++i) {
        if (!ready[i]) {
            continue;
        }
        for (const std::size_t owner : owners[i]) {
            if (owner == kLine) {
                woke.line = true;
            } else {
                office_ready[owner] = true;
            }
        }
    }
    if (messages && (r == WAIT_OBJECT_0 + count || HIWORD(::GetQueueStatus(QS_ALLINPUT)) != 0)) {
        for (std::size_t i = 0; i < offices.size(); ++i) {
            if (offices[i].held && offices[i].set.messages) {
                office_ready[i] = true;
            }
        }
    }
    for (std::size_t i = 0; i < offices.size(); ++i) {
        if (office_ready[i]) {
            wake(offices[i], woke);
        }
    }
    return woke;
}

#else // POSIX

Reactor::Woke Reactor::Impl::wait(std::int64_t ms) {
    Woke woke;
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
    for (std::size_t i = 0; i < offices.size(); ++i) {
        if (!offices[i].held) {
            continue;
        }
        for (const std::int64_t h : offices[i].set.handles) {
            add(h, POLLIN, i);
        }
        for (const std::int64_t h : offices[i].set.writable) {
            add(h, POLLOUT, i);
        }
    }
    bool now = false;
    if (line != nullptr && !line->closed()) {
        if (line->ready()) {
            now = true;
            woke.line = true;
        } else {
            for (const std::int64_t h : line->wait_set().handles) {
                add(h, POLLIN, kLine);
            }
        }
    }
    const int timeout = now || ms <= 0 ? 0 : static_cast<int>(std::min<std::int64_t>(ms, 1 << 30));
    int r = 0;
    if (fds.empty()) {
        if (timeout > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
        }
    } else {
        r = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout);
    }
    if (r < 0) {
        return woke; // a signal ended it, which is no source's readiness and no deadline
    }
    woke.deadline = r == 0 && !now && timeout > 0;
    std::vector<bool> office_ready(offices.size(), false);
    for (std::size_t i = 0; r > 0 && i < fds.size(); ++i) {
        const short got = fds[i].revents;
        if (got == 0) {
            continue;
        }
        for (const std::size_t owner : owners[i]) {
            if ((got & POLLNVAL) != 0) {
                // Not an open descriptor: its sources are held no longer, so it never spins.
                if (owner != kLine && offices[owner].held) {
                    refuse(offices[owner], "a descriptor it names is not open; the wait holds it "
                                           "again once its claim changes");
                }
            } else if (owner == kLine) {
                woke.line = true;
            } else {
                office_ready[owner] = true;
            }
        }
    }
    for (std::size_t i = 0; i < offices.size(); ++i) {
        if (office_ready[i]) {
            wake(offices[i], woke);
        }
    }
    return woke;
}

#endif

Reactor::Reactor(Switchboard& bus) : impl_(std::make_unique<Impl>(bus)) {}

Reactor::~Reactor() = default;

void Reactor::trust(std::string office) {
    if (trusts(office)) {
        return;
    }
    Office o;
    o.name = std::move(office);
    impl_->offices.push_back(std::move(o));
}

void Reactor::distrust(std::string_view office) {
    auto& offices = impl_->offices;
    const auto it = std::find_if(offices.begin(), offices.end(),
                                 [office](const Office& o) { return o.name == office; });
    if (it == offices.end()) {
        return;
    }
    if (it->told == Told::Held) {
        impl_->say(*it, false, false, "the host no longer waits on this office");
    }
    offices.erase(it);
}

bool Reactor::trusts(std::string_view office) const {
    return std::any_of(impl_->offices.begin(), impl_->offices.end(),
                       [office](const Office& o) { return o.name == office; });
}

void Reactor::watch(LineInput* input) { impl_->line = input; }

Reactor::Woke Reactor::after_turn(std::int64_t ms) {
    std::size_t used = 0;
    if (impl_->line != nullptr && !impl_->line->closed()) {
        used += impl_->line->wait_set().handles.size();
    }
    (void)impl_->hold(used);
    const Woke woke = impl_->wait(ms);
    if (woke.deadline) {
        ++impl_->lasted;
    } else if (woke.line || woke.offices > 0) {
        ++impl_->woken;
    }
    return woke;
}

std::vector<Reactor::Source> Reactor::sources() const {
    std::vector<Source> out;
    for (const Office& o : impl_->offices) {
        Source s;
        s.office = o.name;
        s.claimed = o.claimed;
        s.held = o.held;
        s.revision = o.revision;
        s.handles = o.count();
        s.wakes = o.wakes;
        s.why = o.held ? std::string() : o.why;
        out.push_back(std::move(s));
    }
    return out;
}

std::uint64_t Reactor::woken() const noexcept { return impl_->woken; }

std::uint64_t Reactor::lasted() const noexcept { return impl_->lasted; }

} // namespace loom::host
