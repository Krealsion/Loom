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

That is the whole list.

**The shell, the editor and the source**, said out loud rather than assumed:

- **A shell.** On Linux/WSL, whichever one you have. On Windows, **PowerShell** — every
  Windows command on this page is PowerShell, and [the Windows route](#the-windows-route)
  says which of its rules will bite you. `cmd.exe` works too if you translate the variable
  syntax; the MSYS2 shell is for installing packages, not for driving these builds.
- **An editor.** Any text editor. You will write C++ and two small JSON files, and
  nothing on this path needs an IDE, a plugin or a project file.
- **The source.** `git clone https://github.com/Krealsion/Loom.git`, or download a source
  archive from the same place if you would rather not install Git.

### Where to get them

- **Linux / WSL**: `sudo apt install build-essential cmake git` (Debian/Ubuntu),
  `sudo dnf install gcc-c++ cmake make git` (Fedora). WSL Ubuntu 22.04 is the canonical
  development host for this project.
- **Windows**: two routes, both tested — see [the Windows route](#the-windows-route)
  below, which gives the actual commands for each.
- **macOS**: not tested. The portable subset has no reason not to build under Apple
  Clang, and nobody has run it, so this page does not claim it.

## The Linux route, end to end

The shell is any POSIX shell (`bash`, `zsh`); nothing below needs `sudo` after the
packages above. Everything is one shell session in a directory of your choosing.

```sh
git clone https://github.com/Krealsion/Loom.git
cd Loom
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DZEN_BUILD_TESTS=OFF -DZEN_BUILD_EXAMPLES=OFF \
      -DCMAKE_INSTALL_PREFIX="$HOME/loom"
cmake --build build -j"$(nproc)"
cmake --install build
```

What you get, and where:

| | |
|---|---|
| `$HOME/loom/bin/loom-host` | the program you run |
| `$HOME/loom/lib/*.a` | the libraries your weave links |
| `$HOME/loom/lib/cmake/loom/` | what `find_package(loom)` reads — point `CMAKE_PREFIX_PATH` at the **prefix**, not at this directory |
| `$HOME/loom/include/zen/` | the headers |

Your own weave is a shared library (`.so`); Loom does not care where it lives, only that
the boot plan names it. [From nothing to a running weave](running-loom.md) is the rest.

## The Windows route

**Pick one toolchain per build directory and stay with it.** A configured build tree
remembers its compiler and its generator; switching either one means a new directory, not
a new flag.

Two things bite before anything else, and neither is Loom's:

- **PowerShell does not expand `$variables` inside an unquoted argument that starts with
  `-`.** `-DCMAKE_INSTALL_PREFIX=$prefix` reaches CMake as the literal text `$prefix` —
  and CMake cheerfully installs into a directory with that name. **Quote the whole
  argument**: `"-DCMAKE_INSTALL_PREFIX=$prefix"`. Every command below is written that way.
  (`cmd.exe` uses `%prefix%` and has no such rule.)
- **Use forward slashes in paths you hand to CMake and to a boot plan.** Both accept them
  on Windows, and a backslash is an escape character in JSON.

### MSVC (Visual Studio Build Tools)

Install [Visual Studio](https://visualstudio.microsoft.com/) — the free Community edition
or the standalone Build Tools — with the **Desktop development with C++** workload. That
brings `cl.exe`, CMake and Ninja together.

The compiler needs its environment, and the reliable way to get it in PowerShell is to
load `VsDevCmd.bat` and import what it set (`Enter-VsDevShell` needs `vswhere` on `PATH`
and fails while appearing to succeed):

```powershell
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"   # your install path
$env:VSCMD_START_DIR = $PWD                                     # or it moves you
cmd /c "`"$vs\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 >nul && set" |
  ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item ("env:" + $matches[1]) $matches[2] } }
cl    # should print the version banner
```

Then, from the Loom checkout:

```powershell
$prefix = "$PWD\_install"
cmake -S . -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Debug `
      -DLOOM_ENABLE_WINDOWS_KERNEL=ON "-DCMAKE_INSTALL_PREFIX=$prefix"
cmake --build build-msvc --parallel
cmake --install build-msvc
```

### MinGW-w64 (MSYS2)

Install [MSYS2](https://www.msys2.org/) and, in its shell,
`pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja`. Then, in
PowerShell, put that toolchain first on `PATH` **in every session that builds or runs** —
the produced `.exe` loads `libstdc++-6.dll`, `libgcc_s_seh-1.dll` and
`libwinpthread-1.dll` from there:

```powershell
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
$prefix = "$PWD\_install"
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Debug `
      -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe `
      -DLOOM_ENABLE_WINDOWS_KERNEL=ON "-DCMAKE_INSTALL_PREFIX=$prefix"
cmake --build build-mingw --parallel
cmake --install build-mingw
```

### What you get on Windows

| | |
|---|---|
| `<prefix>\bin\loom-host.exe` | the program you run |
| `<prefix>\lib\*.a` (MinGW) / `*.lib` (MSVC) | the libraries your weave links |
| `<prefix>\lib\cmake\loom\` | what `find_package(loom)` reads |
| `<prefix>\include\zen\` | the headers |

Your own weave is a **`.dll`**, and CMake names it `libmine.dll` under MinGW and
`mine.dll` under MSVC — look in your build directory rather than guessing, and write
whichever one is there into the boot plan.

**`-DLOOM_ENABLE_WINDOWS_KERNEL=ON` is not optional if you want to load weaves.** Without
it you still get the libraries, the console and `loom-host`, and `loom-host` says in one
sentence that it can host nothing loadable.

### Building your own weave against it

Exactly the Linux shape, with the prefix quoted:

```powershell
cmake -S . -B build -G Ninja "-DCMAKE_PREFIX_PATH=$prefix" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

An editor is optional here as everywhere: these are shell commands, and
[running Loom](running-loom.md) is driven from a console, not an IDE.

### What has actually been run

Stated because "supported" and "tested" are different words:

| | Configuration | What was run |
|---|---|---|
| WSL Ubuntu 22.04, GCC 11.4.0 | Release, tests off | install, installed-package witness, external `find_package` consumer, the whole operator journey |
| WSL Ubuntu 22.04, GCC 11.4.0 | Debug, tests on | the official lane (`tests/verify.cmake`) |
| Windows, MinGW-w64 GCC 15.2.0 (MSYS2) | Debug, `LOOM_ENABLE_WINDOWS_KERNEL=ON` | install, installed-package witness, external consumer, the operator journey, the official lane |
| Windows, MSVC 19.50 (VS 2026) | Debug, `LOOM_ENABLE_WINDOWS_KERNEL=ON` | install, installed-package witness, the official lane |

macOS and Clang: not run. A default Windows build (kernel **off**) builds and consoles,
and cannot host weaves; that configuration is exercised by CI rather than by this page.

## Optional, and what each one buys you

| | Why you might want it | Without it |
|---|---|---|
| **Git** | To get the source, and to get updates | Download a source archive instead |
| **Ninja** | Noticeably faster rebuilds than Make | Make is fine |
| **`nm`** (binutils) | The installed-package witness and the weave build-contract check ask your artifact what it exports | Those two checks refuse to run rather than passing silently — absence of a tool is a failure, never a skip |
| **A debugger** (`gdb`, `lldb`, Visual Studio) | For a weave that crashes the host — an in-process weave shares the host's address space, so it is an ordinary native debugging problem | Read [diagnostics](diagnostics.md) instead |
| **An editor with `compile_commands.json` support** | Configure with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` and most editors will resolve Loom's headers | Your editor will not know where `<zen/…>` lives |
| **Python 3.8 or newer** (standard library only) | The [session](sessions.md) tooling: the `loom-session` CLI, Python clients, and the editable tools a session's run manager starts. The test lane's `session_journey` entry runs with it | Everything else works: `loom-host`, including `--serve`, needs no Python; the CLI says it found none, the run manager refuses a run in words, and the lane lists `session_journey` as DECLARED ABSENT. `LOOM_SESSION_PYTHON` names an interpreter at configure; `-DLOOM_BUILD_SESSION_TOOLS=OFF` leaves the tooling out |

**You do not need an IDE**, and installing one is not a step on this path. Every command
on the neighbouring pages is a shell command.

## What Loom does NOT need

Said explicitly, because the absence is deliberate and a newcomer reasonably assumes
otherwise:

- **No package manager**, no vcpkg or Conan step. Loom has no third-party dependencies —
  see [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).
- **No crypto library.** The one digest Loom needs is one auditable file in its own tree.
- **No Python, Node or scripting runtime** to build or check it. The build and the checks
  are CMake; Python is needed only by the optional session tooling above, and by the one
  lane entry that exercises it.
- **No Zengine, no Workshop and no already-hosted tool.** That is what
  ["from nothing to a running weave"](running-loom.md) means, and it is why the compiler
  above is the only thing you have to install first.

## Platform differences that will actually bite you

- **The weave kernel is an opt-in on Windows.** Loading weaves in-process applies no
  sandbox anywhere, and on Windows it is not even built unless you ask:
  `-DLOOM_ENABLE_WINDOWS_KERNEL=ON`. A default Windows build gives you the libraries,
  the console and `loom-host`, and `loom-host` will tell you in one sentence that it can
  host nothing loadable. On Linux the kernel is always built.
- **PowerShell's unquoted `-D` arguments do not expand variables** — see
  [the Windows route](#the-windows-route). This one costs an afternoon, because the build
  succeeds and installs somewhere you did not name.
- **A held file cannot be deleted on Windows.** `loom-host` keeps an open handle on
  `<authority-file>.lock` for as long as it runs, which is how one host owns one decision
  file at a time; you will not be able to remove that file while the host is up, and you
  do not need to — the OS releases it when the host exits, however it exits.
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
