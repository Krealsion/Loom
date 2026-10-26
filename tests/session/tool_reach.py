# SPDX-License-Identifier: MPL-2.0
# Copyright (c) 2026 Joshua DeMoss
"""WHAT A SHIPPED TOOL REACHES, BY ITS IMPORTS: a package's name says how far its tools reach.

An operator approves a tool package whole (docs/guides/sessions.md, section 2), so a package is
the unit of reach. A package whose name ends in ``-machine`` may reach the machine; every other
package is a MESSAGE package, and each of its tools reaches nothing but the bus and a clock: no
process, no file, no socket of its own. A tool's reach is read from its CLOSURE -- its script and
every module it imports, followed through its own package and then the packages it ``uses`` --
because a module's top level runs when it is imported, so the closure is what a run loads.

Red, naming the tool, the file and the line, when a tool of a message package
  - calls a process (``subprocess``, ``os.system``, ``os.popen``, ``os.exec*``, ``os.spawn*``,
    ``os.fork``, ``os.kill``), a file (``open``, an ``os`` or ``os.path`` call that touches the
    filesystem, a ``pathlib`` read or write, ``shutil``, ``glob``, ``tempfile``) or a socket, or
    runs text as code (``exec``, ``eval``, ``compile``, ``__import__``);
  - imports a module this check cannot read: anything but its own package's modules, those of the
    packages it uses, the session runtime (``loom_session``) and the pure standard modules listed
    below.
And red, for every package, when a tool imports a module that neither its package, nor a package
it uses, nor the standard library holds: the run would not load it.

It reads names. It is a guard that a message tool does not grow a reach, not a sandbox: approving
a package is still trusting its code.

    tool_reach.py --packages <dir holding packages> [--package <package dir>] ...

``--packages`` judges every immediate folder holding ``loom-tool.json``; ``--package`` one. Exit 0
when every tool held, 1 when one did not, 2 when the check's own self-test failed or it was given
nothing to judge.
"""

import argparse
import ast
import importlib.util
import json
import os
import sys
import sysconfig
import tempfile

MACHINE_SUFFIX = "-machine"
RUNTIME = ("loom_session",)
# Standard modules that compute and wait, and touch nothing: what a message tool may import.
# `os` is here for its pure parts (`os.path.join`, `os.sep`); its calls that reach are judged one
# by one below. `pathlib` is not: a path object reads and writes through any name it is bound to.
PURE = frozenset((
    "base64", "binascii", "bisect", "collections", "copy", "dataclasses", "datetime", "decimal",
    "difflib", "enum", "fnmatch", "fractions", "functools", "hashlib", "heapq", "itertools", "json",
    "math", "numbers", "operator", "os", "posixpath", "ntpath", "re", "statistics", "string",
    "struct", "textwrap", "time", "types", "typing", "unicodedata", "zlib",
))
PROCESS_OS = {"system", "popen", "fork", "forkpty", "kill", "killpg", "startfile", "pipe"}
FILE_OS = {"open", "fdopen", "remove", "unlink", "rename", "renames", "replace", "makedirs",
           "mkdir", "listdir", "scandir", "walk", "fwalk", "stat", "lstat", "rmdir", "removedirs",
           "chmod", "chown", "utime", "truncate", "symlink", "link", "readlink", "chdir", "getcwd",
           "access", "mkfifo", "mknod", "statvfs"}
FILE_OS_PATH = {"exists", "lexists", "isfile", "isdir", "islink", "ismount", "getsize",
                "getmtime", "getatime", "getctime", "realpath", "abspath", "samefile", "expanduser",
                "expandvars", "relpath"}
CODE = {"open", "exec", "eval", "compile", "__import__"}


def dotted(node):
    """`a.b.c` for an attribute chain, `?` where a part is not a name."""
    parts = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    parts.append(node.id if isinstance(node, ast.Name) else "?")
    return ".".join(reversed(parts))


def os_reach(name):
    """What calling `os.<name>` reaches, or None."""
    if name in PROCESS_OS or name.startswith(("exec", "spawn")):
        return "a process"
    return "a file" if name in FILE_OS else None


class Module(object):
    """One Python file: the modules it imports (anywhere in it, a function's own imports too) and
    each place it reaches past the bus, as (line, kind, what). `os` and `os.path` are followed
    under any name they are bound to here."""

    def __init__(self, path):
        self.path = path
        with open(path, encoding="utf-8") as f:
            tree = ast.parse(f.read(), filename=path)
        self.imports = []          # (line, top-level module name, or None for a relative import)
        self.reaches = []
        oses, paths = set(), set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                for a in node.names:
                    self.imports.append((node.lineno, a.name.split(".")[0]))
                    if a.name == "os":
                        oses.add(a.asname or "os")
                    elif a.name == "os.path":
                        (paths if a.asname else oses).add(a.asname or "os")
            elif isinstance(node, ast.ImportFrom):
                if node.level:
                    self.imports.append((node.lineno, None))
                    continue
                self.imports.append((node.lineno, node.module.split(".")[0]))
                for a in node.names:
                    said = "from %s import %s" % (node.module, a.name)
                    if node.module == "os" and a.name == "path":
                        paths.add(a.asname or "path")
                    elif node.module == "os" and os_reach(a.name):
                        self.reaches.append((node.lineno, os_reach(a.name), said))
                    elif node.module in ("os.path", "posixpath", "ntpath") and a.name in FILE_OS_PATH:
                        self.reaches.append((node.lineno, "a file", said))
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            d = dotted(node.func)
            parts = d.split(".")
            kind = None
            if len(parts) == 1 and d in CODE:
                kind = "a file" if d == "open" else "code from text"
            elif len(parts) == 2 and parts[0] in oses:
                kind = os_reach(parts[1])
            elif len(parts) == 3 and parts[0] in oses and parts[1] == "path" and parts[2] in FILE_OS_PATH:
                kind = "a file"
            elif len(parts) == 2 and parts[0] in paths and parts[1] in FILE_OS_PATH:
                kind = "a file"
            if kind is not None:
                self.reaches.append((node.lineno, kind, d))


def standard(name):
    """Whether `name` is a module of this Python's standard library."""
    names = getattr(sys, "stdlib_module_names", None)
    if names is not None:
        return name in names
    if name in sys.builtin_module_names:
        return True
    spec = importlib.util.find_spec(name)
    if spec is None or not spec.origin:
        return False
    if spec.origin in ("built-in", "frozen"):
        return True
    lib = os.path.realpath(sysconfig.get_paths()["stdlib"])
    return os.path.realpath(spec.origin).startswith(lib + os.sep)


class Package(object):
    def __init__(self, directory):
        self.dir = os.path.abspath(directory)
        with open(os.path.join(self.dir, "loom-tool.json"), encoding="utf-8") as f:
            self.manifest = json.load(f)
        self.name = self.manifest["package"]
        self.uses = list(self.manifest.get("uses", []))
        self.tools = [(t["name"], t["script"]) for t in self.manifest.get("tools", [])]
        self.machine = self.name.endswith(MACHINE_SUFFIX)

    def module(self, name):
        path = os.path.join(self.dir, name + ".py")
        return path if os.path.isfile(path) else None


def judge(packages):
    """Every tool's verdict lines: (tool id, None) when it held, (tool id, why) for each fault."""
    by_name = dict((p.name, p) for p in packages)
    root = os.path.commonpath([os.path.dirname(p.dir) for p in packages])
    modules = {}
    out = []

    def load(path):
        if path not in modules:
            modules[path] = Module(path)
        return modules[path]

    for p in packages:
        order = [p]
        for u in p.uses:
            if u not in by_name:
                out.append(("%s/*" % p.name, "uses '%s', which this check was not given" % u))
            else:
                order.append(by_name[u])
        for tool, script in p.tools:
            tid = "%s/%s" % (p.name, tool)
            faults = []
            start = os.path.join(p.dir, script)
            if not os.path.isfile(start):
                out.append((tid, "its script %s is not in the package" % script))
                continue
            seen, todo = [], [start]
            while todo:
                path = todo.pop()
                if path in seen:
                    continue
                seen.append(path)
                m = load(path)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                for line, name in m.imports:
                    if name is None:
                        faults.append("%s:%d imports relatively; a tool imports by name" % (rel, line))
                        continue
                    found = next((q.module(name) for q in order if q.module(name)), None)
                    if found:
                        todo.append(found)
                    elif name in RUNTIME:
                        continue
                    elif not standard(name):
                        faults.append("%s:%d imports %s, which neither its package, nor a package "
                                      "it uses, nor the standard library holds" % (rel, line, name))
                    elif not p.machine and name not in PURE:
                        faults.append("%s:%d imports %s, which this check cannot read; a message "
                                      "tool imports only modules that compute" % (rel, line, name))
                if not p.machine:
                    for line, kind, what in m.reaches:
                        faults.append("%s:%d reaches %s: %s" % (rel, line, kind, what))
            out.extend((tid, f) for f in faults)
            if not faults:
                out.append((tid, None))
    return out


def self_test():
    """The predicate says no where nothing reaches and yes where something does, and a package
    named for the machine is let reach: a canary both ways, before the tree is believed."""
    root = tempfile.mkdtemp(prefix="tool-reach-")

    def package(name, files, uses=()):
        d = os.path.join(root, name)
        os.makedirs(d)
        tools = [{"name": n[:-3], "script": n} for n in files if n.startswith("t_")]
        with open(os.path.join(d, "loom-tool.json"), "w", encoding="utf-8") as f:
            json.dump({"package": name, "uses": list(uses), "tools": tools}, f)
        for n, text in files.items():
            with open(os.path.join(d, n), "w", encoding="utf-8") as f:
                f.write(text)
        return Package(d)

    quiet = package("quiet", {"t_ask.py": "import json, time\nimport helper\n"
                                          "def run(ctx):\n    return json.dumps(ctx.ask('o', 'S', {}))\n",
                              "helper.py": "import re\nWORD = re.compile('x')\n"})
    loud = package("loud", {"t_write.py": "import helper\ndef run(ctx):\n    return 1\n",
                            "helper.py": "def keep(p):\n    with open(p, 'w') as f:\n        f.write('x')\n",
                            "t_spawn.py": "import subprocess\ndef run(ctx):\n    subprocess.run(['x'])\n",
                            "t_path.py": "from pathlib import Path\ndef run(ctx):\n    Path('x').write_bytes(b'')\n",
                            "t_sock.py": "import socket\ndef run(ctx):\n    socket.create_connection(('h', 1))\n"})
    machine = package("loud-machine", {"t_spawn.py": "import subprocess\nimport helper\n"
                                                     "def run(ctx):\n    subprocess.run(['x'])\n"},
                      uses=("quiet",))
    lost = package("lost-machine", {"t_lost.py": "import helper\ndef run(ctx):\n    return 1\n"})
    verdicts = judge([quiet, loud, machine, lost])
    red = set(t for t, why in verdicts if why)
    green = set(t for t, why in verdicts if why is None)
    want_red = {"loud/t_write", "loud/t_spawn", "loud/t_path", "loud/t_sock", "lost-machine/t_lost"}
    want_green = {"quiet/t_ask", "loud-machine/t_spawn"}
    if red != want_red or green != want_green:
        print("tool_reach: SELF-TEST FAILED: red %s (wanted %s), green %s (wanted %s)"
              % (sorted(red), sorted(want_red), sorted(green), sorted(want_green)))
        return False
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--packages", action="append", default=[],
                    help="a folder whose immediate folders holding loom-tool.json are judged")
    ap.add_argument("--package", action="append", default=[], help="one package folder")
    args = ap.parse_args(argv)
    if not self_test():
        return 2
    dirs = list(args.package)
    for root in args.packages:
        for child in sorted(os.listdir(root)):
            if os.path.isfile(os.path.join(root, child, "loom-tool.json")):
                dirs.append(os.path.join(root, child))
    packages = [Package(d) for d in dirs]
    tools = sum(len(p.tools) for p in packages)
    if not packages or not tools:
        print("tool_reach: nothing to judge (%d package(s), %d tool(s)): an empty population is a red"
              % (len(packages), tools))
        return 2
    verdicts = judge(packages)
    faults = [(t, why) for t, why in verdicts if why]
    for p in packages:
        print("%-24s %-8s %d tool(s)%s" % (p.name, "machine" if p.machine else "message",
                                           len(p.tools), "; uses " + ", ".join(p.uses) if p.uses else ""))
    for t, why in faults:
        print("FAIL %s: %s" % (t, why))
    held = sum(1 for t, why in verdicts if why is None)
    message = sum(len(p.tools) for p in packages if not p.machine)
    print("tool_reach: %d package(s), %d tool(s), %d of them in message packages; %d held, %d "
          "fault(s)" % (len(packages), tools, message, held, len(faults)))
    return 1 if faults else 0


if __name__ == "__main__":
    sys.exit(main())
