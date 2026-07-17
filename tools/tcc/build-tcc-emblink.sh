#!/bin/sh
# Build TCC to RUN ON EmbLinkOS (build/tcc.elf -- the compiler the OS hosts).
#
# Run it from anywhere:
#     tools/tcc/build-tcc-emblink.sh /path/to/tcc-0.9.27
#
# Env (all overridable, same convention as the Makefile's `?=` variables):
#     MYOS           this checkout            (default: the repo this script is in)
#     NEWLIB_PREFIX  the newlib-c99 rebuild   (default: the Makefile's value)
#     CROSS          cross-compiler prefix    (default: x86_64-elf-)
#
# WHY THIS SCRIPT EXISTS, AND WHY IT IS NOT OPTIONAL
# --------------------------------------------------
# A PRISTINE tcc 0.9.27 builds and runs here and produces binaries that CRASH:
# `-static` links get an unbindable PLT (see the patch alongside this script).
# It does not fail loudly at build time -- the compiler works, the object files
# look right, and the program dies at a wild jump. So a teammate who builds tcc
# "the obvious way" gets a broken toolchain with no clue why. Apply the patch.
set -eu

TCC_SRC="${1:-}"
if [ -z "$TCC_SRC" ]; then
    echo "usage: $0 /path/to/tcc-0.9.27" >&2
    exit 2
fi
[ -f "$TCC_SRC/tccelf.c" ] || { echo "$0: $TCC_SRC is not a tcc source tree" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
MYOS="${MYOS:-$(cd "$HERE/../.." && pwd)}"
CROSS="${CROSS:-x86_64-elf-}"

# Default NEWLIB_PREFIX to whatever the Makefile says, so there is ONE source of
# truth for it and this script cannot drift from the build.
if [ -z "${NEWLIB_PREFIX:-}" ]; then
    NEWLIB_PREFIX=$(make -C "$MYOS" -s --no-print-directory print-newlib-prefix 2>/dev/null || true)
fi
[ -n "${NEWLIB_PREFIX:-}" ] || { echo "$0: set NEWLIB_PREFIX (see docs/BUILD_SETUP.md)" >&2; exit 2; }

for f in "$MYOS/build/crt0.o" "$MYOS/build/syscalls.o"; do
    [ -f "$f" ] || { echo "$0: $f missing -- run \`make\` in $MYOS first" >&2; exit 2; }
done

# --- the patch: idempotent, and REQUIRED ------------------------------------
if grep -q 's1->static_link)) {' "$TCC_SRC/tccelf.c"; then
    echo "  patch    already applied"
else
    echo "  PATCH    0001-static-link-plt32-is-pc32"
    patch -p1 -d "$TCC_SRC" < "$HERE/0001-static-link-plt32-is-pc32.patch"
fi

# --- configure ---------------------------------------------------------------
# Each flag is decided by an EmbLink FACT; see docs/BUILD_SETUP.md for the why:
#   --cross-prefix     build tcc WITH our toolchain, so it RUNS on EmbLink
#   --sysincludepaths  /libpaths: where it looks ON the OS (the flat root)
#   -DCONFIG_TCC_STATIC  no dlopen here (that is only for tcc -run)
#   -DCONFIG_TCCBOOT     drops backtrace/bcheck: they need a SIGSEGV handler,
#                        and EmbLink has no async signal delivery, so it could
#                        never fire. Vacuously correct, not a workaround.
cd "$TCC_SRC"
    # NOTE: --cross-prefix ALREADY makes CC "${CROSS}gcc". Passing --cc as well
    # gets them concatenated into `x86_64-elf-x86_64-elf-gcc`. Do not "helpfully"
    # add --cc back.
./configure \
    --cross-prefix="$CROSS" \
    --prefix=/ \
    --sysincludepaths=/include \
    --libpaths=/lib \
    --extra-cflags="-O2 -mno-red-zone -fno-stack-protector -DCONFIG_TCC_STATIC -DCONFIG_TCCBOOT -I$MYOS/user/lib -isystem $NEWLIB_PREFIX/x86_64-elf/include -DSSIZE_MAX=0x7fffffffffffffffL" \
    --extra-ldflags="-static -nostartfiles -T $MYOS/user/lib/newlib.ld -L$NEWLIB_PREFIX/x86_64-elf/lib $MYOS/build/crt0.o $MYOS/build/syscalls.o"

# `make clean` is NOT paranoia: a stale libtcc.a and a fresh tcc.o disagree about
# their config and give IDENTICAL errors with and without a flag -- that cost a
# debugging session once already.
make clean >/dev/null 2>&1 || true
make CONFIG_ldl=no tcc        # CONFIG_ldl=no: tcc's Makefile adds -ldl unconditionally

echo
echo "built: $TCC_SRC/tcc"
echo "now:   make -C $MYOS TCC_SRC=$TCC_SRC     # packs it as build/tcc.elf"
echo "check: boot and run \`test tcc link\`  -> must print exit=42"
