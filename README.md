# loom

The substrate of **Zen**: a capability-secure fabric where makers compose each
other's **native** code safely — each keeping ownership of their own work —
with a machine-checkable, honest account of exactly what was contained.

A Zen value **carries its own shape**, typed enough to be challenged at any
boundary and dynamic enough to be built at runtime from a schema discovered
seconds ago. Exactly one gate, `admit`, guards every boundary — the live bus,
persistence, the dynamic-library seam, IPC. Nothing crosses a boundary it
cannot prove it belongs across. The library holds the **grammar, never the
answers**: no application message type, no policy, is hard-coded anywhere.

## The spine

- **One gate, every boundary.** A single validator; there is no second path.
- **Untrusted until proven.** Parsed bytes are `Unverified` — no accessors;
  the only road to a usable `Value` is `admit`.
- **Published schemas are immutable.** A `(name, version)` is frozen; identity
  across boundaries is the content-id, never the C++ type.
- **Authority is minimal and explicit.** A weave says nothing until granted
  reach; grants are checked before the gate and never confused with it.
- **Honest enforcement.** The runtime claims only what it imposed *and
  confirmed*, fails safe when it cannot — and the containment tier is stated
  plainly: **abuse, not escape**.

## What is here

| | |
|---|---|
| `loom` (core) | schema · value · the one gate · registry · canonical serialization |
| `zen-switchboard` | the in-process bus: gated delivery, grants, lifecycle, prepared replacement |
| `zen-kernel` | weaves from dynamic libraries across a true C ABI (**v6**); hot-reload; sealed candidates. In-process: the library shares this address space, and is trusted at that level ([why](docs/guides/dynamic-weaves.md#what-loading-it-in-process-means)) |
| `include/zen/weave/` | authoring sugar: `ZEN_SHAPE`, `WeaveBase`, `Mail`, `mount` |
| `include/zen/host/` | host wiring: lifecycle authority, `loom::PreparedReplacement` |
| isolation · console · bridge | OS sandboxing (Linux) · the operator console/TUI · the remote-operator crossing. The bridge **does not authenticate** — reachability of its socket is operator authority ([bridge](docs/reference/bridge.md)) |

## Sixty seconds of weave

```cpp
struct Ping { std::int64_t seq; ZEN_SHAPE(Ping, 1, ZEN_FIELD(seq)); };
struct Pong { std::int64_t seq; ZEN_SHAPE(Pong, 1, ZEN_FIELD(seq)); };
struct Count { std::int64_t handled; ZEN_SHAPE(Count, 1, ZEN_FIELD(handled)); };

class Responder : public loom::WeaveBase<Responder, Count,
                                         loom::Accept<Ping>, loom::Emit<Pong>> {
public:
    void on(const Ping& p, loom::Mail& mail) { ++state_.handled; mail.reply(Pong{p.seq}); }
};

loom::Switchboard bus;
loom::WeaveId id = loom::mount<Responder>(bus);
bus.send(id, loom::Message(loom::to_value(Ping{7})));
bus.pump();
```

Runnable versions live in [`examples/`](examples/) — `quickstart.cpp` (the
value-and-gate core), `heartbeat_woven.cpp` (the above), `answering.cpp`
(immediate and deferred answers).

## Documentation

**[docs/README.md](docs/README.md)** routes everything: start with the
[mental model](docs/guides/mental-model.md) and
[writing a weave](docs/guides/writing-a-weave.md); exact semantics in
[reference](docs/reference/); the named invariants in
[laws](docs/laws/README.md); why in [decisions](docs/decisions/README.md) and
[history](docs/history/README.md); application evidence in
[evidence](docs/evidence/README.md). Machine collaborators start at
[docs/CONTEXT.md](docs/CONTEXT.md); build rules for agents in
[AGENTS.md](AGENTS.md). The soul of the project is
[zen-vision.md](zen-vision.md).

## Build & test

CMake ≥ 3.16, a C++20 compiler (verified GCC 11.4, WSL/Linux canonical);
builds clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Werror`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cmake -DZEN_BUILD_DIR=build -P tests/verify.cmake   # the official lane

# AddressSanitizer + UBSan lane
cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DZEN_SANITIZE=ON
cmake --build build-san
cmake -DZEN_BUILD_DIR=build-san -P tests/verify.cmake
```

`tests/verify.cmake` is the result worth quoting: a bare `ctest` accepts a
selector that matched nothing as success, and the lane does not. A named suite
that selects zero cases fails, the suite inventory and per-suite case floors in
`tests/suite_population.txt` are checked every run, and the two OS-enforcement
populations are exact and independent (`isolation` 17, `policy` 11 — the
expected numbers live in `tests/test_isolation.cpp` and `tests/test_policy.cpp`
and move only by a deliberate edit). See
[`docs/laws/population-laws.md`](docs/laws/population-laws.md).

The `isolation`/`policy` suites need a delegated cgroup-v2 scope; ctest
launches them through `tests/run-under-scope.sh`, and outside such a scope the
OS-enforcement cases **fail hard by design** rather than pass having verified
nothing. `ZEN_ALLOW_UNENFORCEABLE=1` converts those into marked-degraded skips
for a host that genuinely cannot enforce — such a run prints
`*** NON-ENFORCEMENT MODE ***`, asserts no enforcement population, and is
refused by the official lane; it is never evidence about containment. The
portable suites run everywhere, including Windows — under **both MinGW-w64 and
MSVC** — where the `posix` gate is off and those suites are reported as
*declared absent* rather than passing; the Windows kernel backend is an
explicit development-only opt-in (`LOOM_ENABLE_WINDOWS_KERNEL`), and with it on
the `kernel` gate is taken on either compiler.

MSVC support means the package and the weave ABI, not the security story: the
OS sandbox, isolation and the honesty lattice remain Linux-only on every
Windows compiler. Tested on MSVC 19.50 (Visual Studio 2026) x64; clang-cl and
ARM64 are unverified.

## Consuming loom

```sh
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(loom 0.1 REQUIRED)
target_link_libraries(my_weave PRIVATE loom::core)          # values, schemas, the gate
# target_link_libraries(my_weave PRIVATE loom::switchboard) # ...and the live bus
# gate hosting on:  if(TARGET loom::kernel)                 # "can this install host weaves?"
```

The exported surface is deliberately smaller than the build tree —
`loom::core`, `loom::switchboard`, `loom::kernel` (+ the `sanitize`/`warnings`
interface plumbing, carried so `-Werror` never reaches your sources). Exported
names match the in-tree aliases exactly, so a sibling-source build swaps for
the installed package without touching a link line.

**MSVC consumers get the conforming preprocessor automatically.** `ZEN_SHAPE`'s
access tags (`ZEN_EXPOSE`/`ZEN_HIDE`) dispatch on C++20 `__VA_OPT__`, which
MSVC's default traditional preprocessor does not implement, so `loom::core`
carries `/Zc:preprocessor` as an interface requirement — link the target and it
arrives. Only a consumer compiling these headers **without** the CMake targets
needs to pass it by hand; nothing else about the package is MSVC-specific.
`tests/package/` is the witness that this stays true:

```sh
cmake -DZEN_PREFIX=/path/to/prefix -DZEN_WORK=/tmp/w -P tests/package/run.cmake
```

It builds an external project through `find_package(loom)` alone — no flags, no
sibling include path — compiles every macro form, and then asks the produced
weave what it exports and loads it through the real Kernel.

## Where this lives

```
Zen/
  Loom/        this repo — the substrate, everyone's
  Zengine/     the default weave set (Timer, Input, Surface, snake) — the first consumer
  playground/  your own weaves
```

Zengine consumes the Loom **by the stranger's path** (an installed package),
which keeps the dependency arrow un-invertible and makes an unexported surface
fail at home before it fails for a guest. **Per-repo green:** each repo's
suite proves that repo; every report states *which* repo's green was proven.
Assistant sessions launch from the `Zen/` root (the memory graph is keyed to
it); git runs per-repo (`git -C Loom status`).

## License

Loom is licensed under MPL-2.0.
See [LICENSING.md](LICENSING.md) for the plain-language boundary
and [LICENSE](LICENSE) for the legal terms.
