#!/usr/bin/env python3
"""Build python314.zip -- the Python standard library, as one file, for EmbLinkOS.

WHY A ZIP: CPython's importlib AND zipimport are frozen into our interpreter
binary, so a zip on sys.path needs no filesystem tree, no dlopen and no CPython
patch -- it is CPython's own ZIP_LANDMARK mechanism and the standard recipe for
embedded builds. The alternative (unpacking ~700 loose .py files onto the image)
would need mkfs to build nested directories and would cost far more blocks.

sys.path is pinned by a sibling `python.elf._pth` file (see the Makefile rule):
getpath.py reads `<executable>._pth`, and its lines TOTALLY override sys.path --
which also flips on isolated mode and turns off environment lookup, both of which
suit an OS with no environment at all.

Ships PRECOMPILED .pyc, no source. Compiling the startup modules from .py under
TCG is brutally slow -- the interpreter sat at 97% CPU for 8+ minutes without
finishing `import encodings`. This is only safe because host and target bytecode
magic are IDENTICAL (both PYC_MAGIC_NUMBER 3627 -> token 168627755, checked
against Include/internal/pycore_magic_number.h and the host's
_imp.pyc_magic_number_token). RE-CHECK THAT AFTER ANY PYTHON VERSION BUMP: a
mismatched .pyc is SILENTLY ignored, not reported, and the symptom would be an
inexplicable "No module named 'encodings'" all over again.

usage: mkpystdlib.py <python-src-dir> <out.zip>
"""
import os
import py_compile
import sys
import tempfile
import zipfile

# Trees that exist to test or author Python, not to run it. test/ alone is 37 MB
# of the 55 MB Lib/ (1131 of 1851 .py files); the GUI/tooling trees need modules
# this OS does not have. Excluding them is what makes the zip a few MB rather
# than tens.
EXCLUDE_DIRS = {
    "test", "tests", "idlelib", "tkinter", "turtledemo",
    "lib2to3", "ensurepip", "pydoc_data", "site-packages",
    "__pycache__", "config-3.14-x86_64-linux-gnu",
}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src_root, out = sys.argv[1], sys.argv[2]
    lib = os.path.join(src_root, "Lib")
    if not os.path.isdir(lib):
        print(f"mkpystdlib: {lib} not found", file=sys.stderr)
        return 1

    n = 0
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    # STORED, NOT DEFLATED -- this is not a size preference, it is a hard
    # requirement. zipimport can read a stored entry with nothing but itself,
    # but a deflated one needs the `zlib` MODULE, which this interpreter does
    # not have (it is among the 30 unbuilt modules -- there is no zlib library
    # on this OS). A deflated zip therefore fails at startup with the maximally
    # confusing "ModuleNotFoundError: No module named 'zlib'" while trying to
    # import `encodings`. Costs ~13 MB instead of ~2.5 MB; the 64 MB volume has
    # room. Revisit only if zlib is ever built for the target.
    #
    # Deterministic: sorted walk + a fixed timestamp, so an unchanged stdlib
    # produces a byte-identical zip and doesn't churn the disk image.
    tmp = tempfile.TemporaryDirectory()
    tmpdir = tmp.name
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        for dirpath, dirnames, filenames in os.walk(lib):
            dirnames[:] = sorted(d for d in dirnames if d not in EXCLUDE_DIRS)
            for fn in sorted(filenames):
                if not fn.endswith(".py"):
                    continue
                full = os.path.join(dirpath, fn)
                # Paths inside the zip are relative to Lib/, because that is what
                # sys.path expects: "encodings/__init__.pyc", not "Lib/encodings/...".
                arc = os.path.relpath(full, lib)

                # PRECOMPILED: ship .pyc, not source. Compiling the startup
                # modules from .py under TCG is brutally slow -- the interpreter
                # sat at 97% CPU for 8+ minutes without finishing an `import
                # encodings`. Safe because the host and target bytecode magic are
                # IDENTICAL (both PYC_MAGIC_NUMBER 3627 -> token 168627755);
                # re-check that after any Python version bump, since a mismatched
                # .pyc is SILENTLY ignored rather than reported.
                #
                # UNCHECKED_HASH so the .pyc loads unconditionally: the default
                # timestamp mode makes the loader compare against a source file
                # that isn't in the zip. `dfile` keeps the original name in
                # tracebacks even though no source ships.
                #
                # Name it "<mod>.pyc" ADJACENT, not __pycache__/<mod>.cpython-314.pyc
                # -- zipimport only looks for the legacy adjacent layout.
                arc = arc[:-3] + ".pyc"
                cfile = os.path.join(tmpdir, "m.pyc")
                py_compile.compile(full, cfile=cfile, dfile="/" + arc,
                                   doraise=True,
                                   invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
                full = cfile

                zi = zipfile.ZipInfo(arc, date_time=(1980, 1, 1, 0, 0, 0))
                # PER-ENTRY, and it overrides the ZipFile default above -- so
                # this line is the one that actually decides. Must stay STORED:
                # zipimport needs the zlib MODULE to inflate, and we don't have
                # one (see the constructor comment).
                zi.compress_type = zipfile.ZIP_STORED
                zi.external_attr = 0o644 << 16
                with open(full, "rb") as f:
                    z.writestr(zi, f.read())
                n += 1

    size = os.path.getsize(out)
    print(f"mkpystdlib: {n} modules -> {out} ({size / 1048576:.1f} MB)")

    # encodings is imported during interpreter startup and is NOT frozen, so its
    # absence is the difference between a working interpreter and
    # "Fatal Python error: Failed to import encodings module". Check rather than
    # trust the walk.
    with zipfile.ZipFile(out) as z:
        names = set(z.namelist())
    missing = [m for m in ("encodings/__init__.pyc", "encodings/utf_8.pyc",
                           "encodings/aliases.pyc", "codecs.pyc", "io.pyc",
                           "abc.pyc", "os.pyc") if m not in names]
    if missing:
        print(f"mkpystdlib: MISSING startup-critical modules: {missing}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
