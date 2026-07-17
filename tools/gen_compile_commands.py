#!/usr/bin/env python3
"""Generate compile_commands.json so clangd/clang-tidy can resolve includes.

WHY THIS EXISTS: the tree has FOUR include worlds and the Makefile hands each
its own -I set (kernel: one -Ikernel root with subtree-qualified paths; ui/: a
per-subdir -I sprawl; user/ and shell/: the SDK + newlib). clangd can't infer
any of it, because nothing here compiles per-file into a database: the kernel
is ONE gcc invocation over all of $(KERNEL_SRC), so tools like `bear` would
capture a single command, and there's no bear installed anyway. Without this,
every file opens with a wall of bogus "'include/types.h' file not found" and
the follow-on cascade ("unknown type name 'size_t'", "struct X is incomplete"),
which drowns the REAL diagnostics.

The flag sets below MIRROR the Makefile (CFLAGS / NEWLIB_CFLAGS / UIDEMO_INC /
SHELL_INC / USER_INC). They only need to be close enough for INDEXING -- this
never builds anything; the Makefile remains the single source of build truth.
If you add a new include root to the Makefile, add it here too.

Paths are emitted ABSOLUTE from wherever this runs, so the output is
machine-specific -> it is gitignored and regenerated with `make compile-commands`
rather than checked in.
"""
import json
import os
import glob
import shutil
import subprocess

ROOT = os.path.abspath(os.path.dirname(os.path.dirname(__file__)))


def _make_var(name, default=""):
    """Ask the Makefile for a variable, so this script cannot drift from the
    build and contains nobody's home directory. The Makefile's `?=` defaults
    stay the single source of truth; override them there or on the command
    line and clangd follows automatically."""
    try:
        out = subprocess.run(["make", "-C", ROOT, "-s", "--no-print-directory",
                              "print-" + name],
                             capture_output=True, text=True, timeout=30)
        return out.stdout.strip() or default
    except Exception:
        return default


# CC: on PATH is the normal case (docs/BUILD_SETUP.md puts the cross toolchain
# there); fall back to the conventional location if it is not.
CC = shutil.which("x86_64-elf-gcc") or "/usr/local/cross/bin/x86_64-elf-gcc"

_NEWLIB_PREFIX = _make_var("newlib-prefix")
NEWLIB_INC = (_NEWLIB_PREFIX + "/x86_64-elf/include") if _NEWLIB_PREFIX else ""

_CXX_PREFIX = _make_var("cxx-prefix")
CXX = (_CXX_PREFIX + "/bin/x86_64-elf-g++") if _CXX_PREFIX else ""   # optional; see HAVE_CXX


def toolchain_isystem():
    """The CROSS compiler's own include search path, asked of the compiler
    itself rather than hardcoded (it moves with the gcc version -- today
    .../lib/gcc/x86_64-elf/13.2.0/include).

    Needed because clangd otherwise resolves a bare `#include <string.h>`
    against the HOST's glibc, which then fails on bits/libc-header-start.h --
    glibc's headers are not freestanding. Handing clangd the same headers the
    real build uses makes that disappear. Empty list if the cross gcc isn't
    installed (the db still indexes project headers fine)."""
    try:
        out = subprocess.run([CC, "-ffreestanding", "-E", "-v", "-", "-o", os.devnull],
                             input="", capture_output=True, text=True, timeout=20).stderr
    except (OSError, subprocess.SubprocessError):
        return []
    paths, grabbing = [], False
    for line in out.splitlines():
        if line.startswith("#include <...> search starts here:"):
            grabbing = True
            continue
        if line.startswith("End of search list"):
            break
        if grabbing and line.startswith(" "):
            p = os.path.normpath(line.strip())
            if os.path.isdir(p):
                paths.append(p)
    flags = []
    for p in paths:
        flags += ["-isystem", p]
    return flags


TOOLCHAIN_INC = toolchain_isystem()


def a(*parts):
    """-I<abs path under the repo root>"""
    return "-I" + os.path.join(ROOT, *parts)


# ---- the four include worlds (mirrors the Makefile) ----------------------
KERNEL_FLAGS = [
    "-ffreestanding", "-nostdlib", "-nostartfiles",
    "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-mcmodel=kernel", a("kernel"), "-g", "-O0",
] + TOOLCHAIN_INC

# every ui/ subdir is its own -I (ui/ uses bare basenames: "scene.h")
UI_INC = [a("ui", d) for d in
          ("scene", "backend", "layout", "reactive", "declare", "theme", "kit", "dsl")]

NEWLIB_FLAGS = [
    "-mno-red-zone", "-fno-stack-protector", "-O2", "-Wall",
    a("user", "lib"), "-isystem", NEWLIB_INC,
] + TOOLCHAIN_INC

USER_FLAGS = NEWLIB_FLAGS + UI_INC              # user/bin apps import the toolkit
UI_FLAGS = NEWLIB_FLAGS + UI_INC                # ui/ itself + its host tests
SHELL_FLAGS = NEWLIB_FLAGS + [a("shell")]       # kernel-style: ONE root, qualified


def entries():
    out = []

    def add(pattern, flags, cc=CC):
        for src in glob.glob(os.path.join(ROOT, pattern), recursive=True):
            out.append({
                "directory": ROOT,
                "file": src,
                "arguments": [cc] + flags + ["-c", src, "-o", "/dev/null"],
            })

    add("kernel/**/*.c", KERNEL_FLAGS)
    add("shell/**/*.c", SHELL_FLAGS)
    add("user/bin/*.c", USER_FLAGS)
    add("user/lib/*.c", NEWLIB_FLAGS)
    add("ui/**/*.c", UI_FLAGS)
    # C++ (user/bin/*.cc). Needs the C++ compiler as argv[0] so clangd infers
    # a C++ language mode and finds libstdc++'s headers -- with the C driver
    # it can't even resolve <string>.
    if os.path.isfile(CXX):
        add("user/bin/*.cc", USER_FLAGS, cc=CXX)
    return out


def main():
    db = entries()
    path = os.path.join(ROOT, "compile_commands.json")
    with open(path, "w") as f:
        json.dump(db, f, indent=1)
        f.write("\n")
    print(f"compile_commands.json: {len(db)} entries -> {path}")


if __name__ == "__main__":
    main()
