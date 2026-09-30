# Loom

Loom is a C++20 library, and a host program built on it, for running native code written by
different people inside one application safely. Each piece keeps ownership of its own work, talks
to the others only through messages, and holds only the authority it was given; the runtime
reports exactly what it contained and how. Loom is the substrate of **Zen**, an invitation to a
serious playground for building and sharing software ([zen-vision.md](zen-vision.md)).

## The ideas, in the order they build

- A **weave** is one participant: a piece of native code, compiled into the program or loaded
  from a shared library, that receives and sends messages. It is the unit of authorship.
- A **value** carries its own **shape**: a **schema** is a frozen `(name, version)` with its
  fields, identified across boundaries by a content id rather than by a C++ type. Values can be
  built at runtime from a schema discovered seconds ago.
- **One gate, every boundary.** Whatever crosses a boundary — the live bus, storage, the
  shared-library seam, another process — is checked by one function, `admit`, against its schema.
  Parsed bytes are `Unverified` and have no accessors; the only way to a usable `Value` is
  `admit`. There is no second path.
- **Published schemas are immutable.** Once a `(name, version)` is published, it never changes.
- **Authority is minimal and explicit.** A **grant** says which shapes a weave may send, and to
  whom. A weave with no grant can say nothing. Grants are checked before the gate and are never
  confused with it.
- **Honest enforcement.** Where Loom confines a weave in an operating-system sandbox, it claims
  only what it imposed *and confirmed*, and fails safe when it cannot. The sandbox is built
  against **abuse, not escape**: it stops buggy or greedy code, not a determined attacker.
- **The grammar, never the answers.** Loom hard-codes no application message and no policy; a
  host decides both.

## What is here

| | |
|---|---|
| `loom` (core) | schemas, values, the gate, the schema registry, canonical serialization |
| `zen-switchboard` | the in-process message bus: gated delivery, grants, lifecycle, prepared replacement of a running service |
| `zen-kernel` | weaves loaded from shared libraries across a C ABI (version **<!-- value ZEN_ABI_VERSION -->9<!-- /value -->**), reloaded in place, or prepared as sealed candidates before they take over. A loaded library shares the host's address space and is trusted at that level ([why](docs/guides/dynamic-weaves.md#what-loading-it-in-process-means)) |
| `include/zen/weave/` | the authoring layer: `ZEN_SHAPE`, `WeaveBase`, `Mail`, `mount` |
| `include/zen/host/` | host wiring: lifecycle authority, `loom::PreparedReplacement` |
| isolation · console · bridge | the operating-system sandbox (Linux) · the operator's console · the crossing between two hosts. The bridge **does not authenticate**: a connection acts on nothing until the host's admission policy answers it, and under the operator policy, reaching the socket is enough ([bridge](docs/reference/bridge.md)) |

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
bus.drain_until_idle();
```

`Responder` accepts `Ping`, may emit `Pong`, and keeps a `Count` as its state. Runnable versions
live in [`examples/`](examples/): `quickstart.cpp` (values and the gate), `heartbeat.cpp` and
`heartbeat_woven.cpp` (weaves on the bus, by hand and with the authoring layer), `answering.cpp`
(immediate and deferred answers).

## Run it

Loom ships a host. Install it and you have a program, not a library you have to write a host
around first:

```sh
cmake -B build -DCMAKE_INSTALL_PREFIX=$HOME/loom && cmake --build build && cmake --install build
$HOME/loom/bin/loom-host
```

`loom-host` starts the weaves you chose, in the order you wrote, and gives you a console with
**authority** in it: what may run, what it may say; approve, refuse, inspect, revoke —
remembered across restarts and revocable afterwards. Nothing is loaded because a file named it; a
participant gets authority because your policy permits it.

Start at **[from nothing to a running weave](docs/guides/running-loom.md)**: install, start,
write a weave of your own, run it, change it. It needs [a compiler and CMake](docs/guides/tools.md)
and nothing else.

## Documentation

**[docs/README.md](docs/README.md)** routes everything: start with the
[mental model](docs/guides/mental-model.md) and [writing a weave](docs/guides/writing-a-weave.md);
exact semantics are in the [reference](docs/reference/); the named invariants in the
[laws](docs/laws/README.md); why things are as they are in the
[decisions](docs/decisions/README.md) and [history](docs/history/README.md); what applications
found in the [evidence](docs/evidence/README.md). Every term is defined once, in the
[terminology index](docs/terminology.md). A machine collaborator starts at
[docs/CONTEXT.md](docs/CONTEXT.md) and builds by [AGENTS.md](AGENTS.md).

## Build and test

A C++20 compiler (GCC 11.4 or newer; Linux is the reference platform) and CMake 3.16 or newer to
build and install, 3.22 or newer for the whole test lane.
The build is clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Werror`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cmake -DZEN_BUILD_DIR=build -P tests/verify.cmake   # the official lane

# AddressSanitizer + UBSan lane
cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DZEN_SANITIZE=ON
cmake --build build-san
cmake -DZEN_BUILD_DIR=build-san -P tests/verify.cmake
```

`tests/verify.cmake` is the result worth quoting. A bare `ctest` accepts a selector that matched
nothing as success; the lane does not. What the lane holds:

- a named suite that selects zero cases fails;
- the suite inventory and each suite's case floor in `tests/suite_population.txt` are checked
  every run;
- the CTest entries that are *not* suites are declared in `tests/entry_population.txt` and
  checked by name, by the lane and independently by the `population` entry, so the lane cannot
  quietly get smaller;
- the two OS-enforcement populations are exact: a missing witness and an unannounced extra one
  both fail. The expected counts live only in the `ZEN_ENFORCEMENT_POPULATION(...)` calls that
  close `tests/test_isolation.cpp` and `tests/test_policy.cpp`.

The laws behind this are in [`docs/laws/population-laws.md`](docs/laws/population-laws.md).

The `isolation` and `policy` suites need a delegated cgroup-v2 scope; CTest launches them through
`tests/run-under-scope.sh`, and outside such a scope the OS-enforcement cases **fail by design**
rather than pass having verified nothing. `ZEN_ALLOW_UNENFORCEABLE=1` turns those into marked,
degraded skips for a host that cannot enforce: such a run prints `*** NON-ENFORCEMENT MODE ***`,
asserts no enforcement population, is refused by the official lane, and is never evidence about
containment.

The portable suites run everywhere, including Windows under **both MinGW-w64 and MSVC**, where
the `posix` gate is off and its suites are reported as *declared absent* rather than passing. The
Windows kernel backend is an opt-in for development (`LOOM_ENABLE_WINDOWS_KERNEL`); with it on, the
`kernel` gate runs on either compiler. MSVC support means the package and the weave ABI, not the
security story: the operating-system sandbox and its enforcement reports are Linux-only. Tested on
MSVC 19.50 (Visual Studio 2026) x64; clang-cl and ARM64 are unverified.

## Consuming Loom

```sh
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(loom 0.1 REQUIRED)
target_link_libraries(my_weave PRIVATE loom::core)          # values, schemas, the gate
# target_link_libraries(my_weave PRIVATE loom::switchboard) # ...and the live bus
# gate hosting on:  if(TARGET loom::kernel)                 # "can this install host weaves?"
```

The exported surface is deliberately smaller than the build tree: `loom::core`,
`loom::switchboard`, `loom::terminal`, `loom::history`, `loom::bridge`, `loom::kernel` where it was
built, and the `sanitize` and `warnings` interface targets, carried so that `-Werror` never reaches
your sources. Exported names match the in-tree aliases exactly, so
a build from source and a build against the installed package use the same link lines.

**MSVC consumers get the conforming preprocessor automatically.** `ZEN_SHAPE`'s access tags
(`ZEN_EXPOSE`, `ZEN_HIDE`) dispatch on C++20 `__VA_OPT__`, which MSVC's default traditional
preprocessor does not implement, so `loom::core` carries `/Zc:preprocessor` as an interface
requirement: link the target and it arrives. Only a consumer compiling these headers **without**
the CMake targets needs to pass it by hand. `tests/package/` is the witness that this stays true:

```sh
cmake -DZEN_PREFIX=/path/to/prefix -DZEN_WORK=/tmp/w -P tests/package/run.cmake
```

It builds an external project through `find_package(loom)` alone — no flags, no include path of
its own — compiles every macro form, and then asks the produced weave what it exports and loads
it through the real kernel.

## Who builds on it

Your own weaves live wherever you keep them; nothing here assumes a layout.
[Zengine](https://github.com/Krealsion/Zengine) is the first consumer: the default set of weaves
(Timer, Input, Surface and others), which builds against an installed Loom through
`find_package`, exactly as a stranger would. Its packages and their laws are its own; this
repository documents Loom alone.

## License

Loom is licensed under MPL-2.0.
See [LICENSING.md](LICENSING.md) for the plain-language boundary
and [LICENSE](LICENSE) for the legal terms.
