// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_HOST_REACTOR_HPP
#define ZEN_HOST_REACTOR_HPP

// THE HOST'S ONE WAIT. After each turn a host that rests waits once, on its dispatch thread, on
// every source it trusts, on the times they are due and on the deadline it gives, instead of each
// source polling or counting time on a beat of its own. A source says what it waits on as its
// role's claim (`WaitSet`, zen/host/wait_set.hpp); the host names the roles it trusts, and the wait
// reads only theirs, so no participant the host did not trust puts a handle or a time in it. The
// wait holds what fits, tells each role whether it is held, and wakes each role whose source is
// ready or due with a `Waited` carrying the time. Nothing is queued on the bus to keep the time:
// between a claim and its wake the bus holds nothing for it. The time is the clock the host gives
// (zen/host/clock.hpp). Nothing here turns the bus: the host turns it (`pump_pending`, MSG-09), and
// the one dispatch thread stays the only thread that touches it. Another thread may end a wait (a
// console's reader does, through `LineInput::wait_set`) and does nothing else.
//
// Windows waits in one `MsgWaitForMultipleObjectsEx`, with a high-resolution waitable timer as the
// deadline; POSIX in one `poll`. On a clock that does not move on its own the wait only looks, and
// moves the clock to its soonest deadline when nothing is ready.
// docs/guides/running-loom.md#a-host-that-rests-until-something-is-ready

#include <zen/host/clock.hpp>
#include <zen/host/wait_set.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loom {
class Switchboard;
}

namespace loom::host {

class LineInput;

/// The most handles one wait holds, its sources' and a watched console's together: a Windows wait
/// takes 64, one of them the thread's message queue and one the deadline's timer. POSIX keeps the
/// same bound. A claim that would pass it is not held, and its source polls.
inline constexpr std::size_t kMaxWaitedHandles = 62;

class Reactor {
public:
    /// The wait for `bus`'s host, on the machine's monotonic clock. The bus must outlive it.
    explicit Reactor(Switchboard& bus);
    /// The wait for `bus`'s host, on the clock the host gives. Both must outlive it.
    Reactor(Switchboard& bus, Clock& clock);
    ~Reactor();
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    /// WAIT ON THIS ROLE'S CLAIM from the next turn on: whoever holds `role` and claims a
    /// `WaitSet` as it. Roles are held in the order they were trusted when not all fit.
    void trust(std::string role);
    /// Stop waiting on `role`; a source the wait held is told it is held no longer.
    void distrust(std::string_view role);
    /// Whether `role` is trusted.
    bool trusts(std::string_view role) const;

    /// Wait on this console's next line too, until it says Closed (`Woke::line`). Watching starts
    /// its reading, as a first `read` does. Null stops watching. It must outlive the watch.
    void watch(LineInput* input);

    /// Wait on something the host serves itself, which is no role on the bus -- a door, a crossing
    /// it services after each turn: `set` is asked before every wait, and what it names ends the
    /// wait when ready (`Woke::own`), the host then serving it as it does after any turn. Its
    /// handles count in the bound before every role's, a set past the bound is not waited on, and
    /// a handle the OS will not wait on is waited on no more while `set` names it. An empty
    /// function stops watching. What it reads must outlive the watch.
    void watch_own(std::function<WaitSet()> set);

    /// What one wait found.
    struct Woke {
        bool line = false;        ///< the watched console has a line, a refusal or its end to take
        std::size_t roles = 0;    ///< how many roles were told their source is ready
        std::size_t due = 0;      ///< how many roles were told their time came
        bool own = false;         ///< what the host serves itself (`watch_own`) is ready
        bool deadline = false;    ///< the host's own deadline came
        std::int64_t now_ms = 0;  ///< the clock when the wait ended
        std::int64_t date_ms = 0; ///< its date then; both are handed to the bus as the next
                                  ///< turn's time (Switchboard::begin_turn)
    };

    /// AFTER A TURN: read every trusted role's claim, tell each whether the wait holds it, wait
    /// at most `ms` (0 or less only looks, at once) on everything held and until the soonest time
    /// a held claim is due, then wake each role whose source is ready or due. Called after every
    /// turn, busy or not, since a source the wait holds polls no more and is read only here.
    ///
    /// A source's wait never spins this thread: a handle the OS will not wait on, and a claim
    /// found ready again before its source claimed anew, are held no longer, their sources told so,
    /// until their claim changes, and a claim's time is due once. Nor does one stall it: the wait
    /// ends at `ms` whatever its sources do.
    ///
    /// On a clock that does not move on its own the wait only looks; when nothing is ready it
    /// moves the clock to its soonest deadline, the host's or a source's, and wakes what is due
    /// then.
    Woke after_turn(std::int64_t ms);
    /// The same with no deadline of the host's own: rest until a source is ready or due, however
    /// long that is. A wait with nothing to wait on and no time to wait until returns at once.
    Woke after_turn();

    /// One trusted role as the wait stands with it, for a host to show and a test to read.
    struct Source {
        std::string role;
        bool claimed = false;       ///< its current holder has claimed a `WaitSet`
        bool held = false;          ///< the wait holds that claim
        std::uint64_t revision = 0; ///< the claim's revision
        std::size_t handles = 0;    ///< how many handles it names, readable and writable
        std::optional<std::int64_t> until_ms; ///< when it is due, until it is told so
        std::uint32_t version = 0;  ///< the version of `WaitSet` it claimed
        std::uint64_t wakes = 0;    ///< how many times it was told it is ready
        std::string why;            ///< why the wait does not hold it, when it does not
    };
    std::vector<Source> sources() const;

    /// How many waits ended before their deadline, and how many lasted it.
    std::uint64_t woken() const noexcept;
    std::uint64_t lasted() const noexcept;

private:
    struct Impl; ///< per platform, in src/host/reactor.cpp
    std::unique_ptr<Impl> impl_;
};

} // namespace loom::host

#endif // ZEN_HOST_REACTOR_HPP
