// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// The box's bounds, each met by a real module (LOOM_ENABLE_WASM): what a module writes to its
// standard output and error, the names it hands a door, its ending itself, the clock it is
// refused, and the compiled modules the backend keeps. Suite `box`, gate `box`. Each case reads
// the bound from the constant that owns it.
// docs/reference/kernel.md#a-weave-built-as-a-webassembly-module

#include "switchboard_fixtures.hpp"
#include "weavelib/box_probe_shapes.hpp"

#include "kernel/wasm_box.hpp" // the bounds' owners

#include <zen/kernel/kernel.hpp>

#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace loom;
using namespace loom::detail;
using namespace sbfx;
using namespace box_probe;

namespace {

/// The host's standard error, taken into a file of its own while it is held: the box's only voice
/// of its own is one line there (`[loom box] ...`), so this is where its log bound is seen.
class HeldStderr {
public:
    HeldStderr() {
        path_ = std::filesystem::temp_directory_path() /
                ("zen-box-stderr-" + std::to_string(pid()) + ".txt");
        std::fflush(stderr);
#if defined(_WIN32)
        const int fd = _open(path_.string().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                             _S_IREAD | _S_IWRITE);
        saved_ = _dup(2);
        _dup2(fd, 2);
        _close(fd);
#else
        const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        saved_ = ::dup(2);
        ::dup2(fd, 2);
        ::close(fd);
#endif
    }
    HeldStderr(const HeldStderr&) = delete;
    HeldStderr& operator=(const HeldStderr&) = delete;
    ~HeldStderr() { release(); }

    /// Give standard error back, and what was written to it while it was held.
    std::string take() {
        release();
        std::ifstream in(path_, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        return text;
    }

private:
    static long pid() {
#if defined(_WIN32)
        return _getpid();
#else
        return static_cast<long>(::getpid());
#endif
    }
    void release() {
        if (saved_ < 0) {
            return;
        }
        std::fflush(stderr);
#if defined(_WIN32)
        _dup2(saved_, 2);
        _close(saved_);
#else
        ::dup2(saved_, 2);
        ::close(saved_);
#endif
        saved_ = -1;
    }

    std::filesystem::path path_;
    int saved_ = -1;
};

/// A boxed probe and a native asker, as test_box.cpp's: what it answered, and how many of the
/// box's deliveries failed.
struct Probe {
    Switchboard bus;
    Kernel kernel{bus, fixture_admission()};
    LoadResult box;
    Registered asker{};
    std::vector<BoxPong> answers;
    std::size_t failed = 0;

    Probe() {
        box = kernel.load("box", ZEN_BOX_PROBE);
        asker = register_probe(bus, {schema_of<BoxPong>()});
        asker.weave->on_handle = [this](const Message& in, Bus&, ProbeWeave&) {
            answers.push_back(BoxPong{in.payload.get("seq")->as_int(),
                                      in.payload.get("count")->as_int(),
                                      in.payload.get("memory_refused")->as_bool(),
                                      in.payload.get("clock_refused")->as_bool()});
        };
        bus.add_observer([this](const BusEvent& ev) {
            if (ev.kind == EventKind::HandlerFailed && ev.target == box.id) {
                ++failed;
            }
        });
    }
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    /// One ask, sent as the asker, so the answer comes back to it.
    void ask(std::int64_t seq, std::int64_t size = 0) {
        bus.send_as(asker.id, box.id, Message(to_value(BoxPing{seq, size}), WeaveId{}, WeaveId{}, 7));
        bus.drain_until_idle();
    }
};

std::size_t count_of(const std::string& text, const std::string& words) {
    std::size_t n = 0;
    for (std::size_t at = text.find(words); at != std::string::npos;
         at = text.find(words, at + words.size())) {
        ++n;
    }
    return n;
}

} // namespace

TEST_SUITE("box") {

TEST_CASE("what a module writes to its standard error is said as one line, kept to "
          "kBoxLogBytes an entry, and the rest is counted") {
    Probe p;
    REQUIRE_MESSAGE(p.box.ok, p.box.error);
    const std::string kept(kBoxLogBytes, 'x');
    const auto bound = static_cast<std::int64_t>(kBoxLogBytes);
    {
        HeldStderr held;
        p.ask(kLog, bound); // exactly the bound: all of it, nothing dropped
        const std::string said = held.take();
        CAPTURE(said.size());
        CHECK(count_of(said, "[loom box] the module wrote: ") == 1);
        CHECK(said.find("the module wrote: " + kept + "\n") != std::string::npos);
        CHECK(said.find("dropped") == std::string::npos);
    }
    {
        HeldStderr held;
        p.ask(kLog, bound + 5); // past it: the bound kept, the five beyond it counted
        const std::string said = held.take();
        CAPTURE(said.size());
        CHECK(count_of(said, "[loom box] the module wrote: ") == 1);
        CHECK(said.find("the module wrote: " + kept + " [5 bytes dropped]\n") != std::string::npos);
    }
    {
        HeldStderr held;
        p.ask(1); // the next entry starts its own log: nothing carried over
        CHECK(held.take().find("[loom box]") == std::string::npos);
    }
    // Writing is not a fault: every ask answered, none failed.
    REQUIRE(p.answers.size() == 3);
    CHECK(p.answers.back().count == 3);
    CHECK(p.failed == 0);
}

TEST_CASE("a name kBoxMaxNameBytes long reaches the door, and one byte longer traps the instance, "
          "which is revived") {
    Probe p;
    REQUIRE_MESSAGE(p.box.ok, p.box.error);
    const auto bound = static_cast<std::int64_t>(kBoxMaxNameBytes);
    p.ask(kName, bound); // the door takes it, and refuses the empty send: the module answers
    REQUIRE(p.answers.size() == 1);
    CHECK(p.failed == 0);
    const KernelLifetimeCounts before = kernel_lifetime_counts();
    p.ask(kName, bound + 1);
    const KernelLifetimeCounts after = kernel_lifetime_counts();
    CHECK(p.failed == 1);
    CHECK(p.answers.size() == 1);
    CHECK(after.stores_opened - before.stores_opened == 1); // the revived instance's
    p.ask(2);
    REQUIRE(p.answers.size() == 2);
    CHECK(p.answers.back().count == 2); // revived from the state before the trap
}

TEST_CASE("a module ending itself is a trap, said in words, and the weave is revived") {
    Probe p;
    REQUIRE_MESSAGE(p.box.ok, p.box.error);
    p.ask(1);
    std::string said;
    {
        HeldStderr held;
        p.ask(kExit);
        said = held.take();
    }
    CAPTURE(said);
    CHECK(p.failed == 1);
    CHECK(said.find("the module called proc_exit(" + std::to_string(kExitStatus) + ")") !=
          std::string::npos);
    p.ask(2);
    REQUIRE(p.answers.size() == 2);
    CHECK(p.answers.back().count == 2);
}

TEST_CASE("a module asking the clock is refused it, and answers") {
    Probe p;
    REQUIRE_MESSAGE(p.box.ok, p.box.error);
    p.ask(1);
    p.ask(kClock);
    REQUIRE(p.answers.size() == 2);
    CHECK_FALSE(p.answers.front().clock_refused); // an ordinary ask asks no clock
    CHECK(p.answers.back().clock_refused);
    CHECK(p.failed == 0);
}

TEST_CASE("the backend keeps kBoxModulesKept compiled modules after their images close, and "
          "compiles again the one it let go") {
    // Every distinct module the suite can open, each loaded and unloaded in a Kernel of its own:
    // one more than the backend keeps, so opening the first again finds it let go.
    const std::vector<const char*> modules = {
        ZEN_BOX_PROBE,        ZEN_BOX_COUNTER_WASM, ZEN_BOX_DEFERS,
        ZEN_BOX_SENSES,       ZEN_BOX_OFFICE_WORKER, ZEN_BOX_DISPATCH,
        ZEN_BOX_JOINT,        ZEN_BOX_VERSIONED_V1, ZEN_BOX_VERSIONED_V2};
    REQUIRE(modules.size() == kBoxModulesKept + 1);
    const auto open_and_close = [](const char* path) {
        Switchboard bus;
        Kernel kernel(bus, fixture_admission());
        const LoadResult lr = kernel.load("m", path);
        REQUIRE_MESSAGE(lr.ok, lr.error);
        REQUIRE(kernel.unload("m"));
    };
    for (const char* m : modules) {
        CAPTURE(m);
        open_and_close(m);
    }
    const auto compiled = [] { return kernel_lifetime_counts().modules_compiled; };
    std::uint64_t before = compiled();
    open_and_close(modules.back()); // the most recently opened: kept
    CHECK(compiled() == before);
    before = compiled();
    open_and_close(modules.front()); // the least recently opened: let go, so compiled again
    CHECK(compiled() == before + 1);
    before = compiled();
    open_and_close(modules[1]); // the next oldest, let go to keep the first
    CHECK(compiled() == before + 1);
    before = compiled();
    open_and_close(modules.front()); // and the first is kept now
    CHECK(compiled() == before);
}

} // TEST_SUITE("box")
