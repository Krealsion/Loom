# The tools you need

What has to be on your machine before Loom is any use to you, why each one is there, and
where to get it. Nothing here is Loom-hosted: this is the bootstrap, and it is the one
part of Zen that an external toolchain has to do for you.

**Loom does not compile anything.** It loads native code that something else built. So a
compiler and CMake are real prerequisites, not conveniences — and this is the first page
that says so, because it is the first place it matters.

## Required

| | What | Why | Tested here |
|---|---|---|---|
| **C++ compiler** | GCC ≥ 11.4, Clang ≥ 14, or MSVC 19.3x | C++20. Loom's public shape macros use `__VA_OPT__` | GCC 11.4 (WSL Ubuntu 22.04); MinGW-w64 GCC 13.1 and MSVC 19.5x on Windows |
| **CMake** | ≥ 3.16 | Loom's build and its installed package are CMake; `find_package(loom)` and `loom_weave_build_contract()` are how a weave of yours gets built correctly | 3.22 (Linux), 3.31 (Windows) |
| **A build tool** | GNU Make, Ninja, or Visual Studio | CMake generates for one of these; any of them is fine | GNU Make (Linux), Ninja (Windows) |

That is the whole list. A shell, a text editor and a way to get the source are assumed
because you are already reading this file.

### Where to get them

- **Linux / WSL**: `sudo apt install build-essential cmake` (Debian/Ubuntu),
  `sudo dnf install gcc-c++ cmake make` (Fedora). WSL Ubuntu 22.04 is the canonical
  development host for this project.
- **Windows**: either [Visual Studio](https://visualstudio.microsoft.com/) with the
  "Desktop development with C++" workload (MSVC, CMake and Ninja all arrive together),
  or [MSYS2](https://www.msys2.org/) for MinGW-w64 GCC plus
  [CMake](https://cmake.org/download/). Both are tested; pick one and stay with it for a
  given build directory.
- **macOS**: not tested. The portable subset has no reason not to build under Apple
  Clang, and nobody has run it, so this page does not claim it.

## Optional, and what each one buys you

| | Why you might want it | Without it |
|---|---|---|
| **Git** | To get the source, and to get updates | Download a source archive instead |
| **Ninja** | Noticeably faster rebuilds than Make | Make is fine |
| **`nm`** (binutils) | The installed-package witness and the weave build-contract check ask your artifact what it exports | Those two checks refuse to run rather than passing silently — absence of a tool is a failure, never a skip |
| **A debugger** (`gdb`, `lldb`, Visual Studio) | For a weave that crashes the host — an in-process weave shares the host's address space, so it is an ordinary native debugging problem | Read [diagnostics](diagnostics.md) instead |
| **An editor with `compile_commands.json` support** | Configure with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` and most editors will resolve Loom's headers | Your editor will not know where `<zen/…>` lives |

**You do not need an IDE**, and installing one is not a step on this path. Every command
on the neighbouring pages is a shell command.

## What Loom does NOT need

Said explicitly, because the absence is deliberate and a newcomer reasonably assumes
otherwise:

- **No package manager**, no vcpkg or Conan step. Loom has no third-party dependencies —
  see [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).
- **No crypto library.** The one digest Loom needs is one auditable file in its own tree.
- **No Python, Node or scripting runtime.** The build, the tests and the checks are CMake.
- **No Zengine, no Workshop and no already-hosted tool.** That is what
  ["from nothing to a running weave"](running-loom.md) means, and it is why the compiler
  above is the only thing you have to install first.

## Platform differences that will actually bite you

- **The weave kernel is an opt-in on Windows.** Loading weaves in-process applies no
  sandbox anywhere, and on Windows it is not even built unless you ask:
  `-DLOOM_ENABLE_WINDOWS_KERNEL=ON`. A default Windows build gives you the libraries,
  the console and `loom-host`, and `loom-host` will tell you in one sentence that it can
  host nothing loadable. On Linux the kernel is always built.
- **MSVC needs `/Zc:preprocessor`** to compile Loom's public headers at all. The
  installed `loom::core` target carries it for you, so you get it from
  `find_package(loom)` — but a build that never links `loom::core` has to pass it itself.
- **The OS sandbox is Linux-only.** Namespaces and cgroups have no Windows equivalent in
  this tree; the out-of-process isolation host exists only there. See
  [capabilities](../reference/capabilities.md) for what is and is not claimed.
- **Line endings and paths.** Paths in a boot plan are passed to the loader as written;
  on Windows either `C:/loom/weaves/mine.dll` or an escaped backslash works, and forward
  slashes are less trouble.

## Next

- [From nothing to a running weave](running-loom.md) — install Loom, start it, build
  something of your own and run it.
- [Writing a weave](writing-a-weave.md) — the code itself.
- [Contributing](../../CONTRIBUTING.md) — if you want to change Loom rather than use it.
