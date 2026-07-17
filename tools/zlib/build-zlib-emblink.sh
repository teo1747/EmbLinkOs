#!/bin/sh
# Cross-build zlib for EmbLinkOS. git needs it: its object store IS deflate +
# inflate + crc32, so `git init` cannot even write a first object without it.
# Nothing else in the tree uses it today.
#
#   usage: tools/zlib/build-zlib-emblink.sh /path/to/zlib-1.3.1 [outdir]
#
# Produces the layout git's ZLIB_PATH expects:
#     <outdir>/include/{zlib.h,zconf.h}
#     <outdir>/lib/libz.a
# Then: ZLIB_BUILD=<outdir> tools/git/build-git-emblink.sh /path/to/git-x.y.z
#
# Env (all overridable): MYOS, NEWLIB_PREFIX, CROSS_BIN, CROSS
set -eu

ZSRC="${1:?usage: $0 /path/to/zlib-1.3.1 [outdir]}"
[ -f "$ZSRC/zlib.h" ] || { echo "$0: $ZSRC is not a zlib source tree" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
M="${MYOS:-$(cd "$HERE/../.." && pwd)}"
NEWLIB_PREFIX="${NEWLIB_PREFIX:-$(make -C "$M" -s --no-print-directory print-newlib-prefix)}"
NL="$NEWLIB_PREFIX/x86_64-elf"
CROSS="${CROSS:-x86_64-elf-}"
export PATH="${CROSS_BIN:-/usr/local/cross/bin}:$PATH"
OUT="${2:-$ZSRC/../build-zlib}"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# --static only: EmbLink has no dlopen, and every port here links statically.
# CHOST tells zlib's hand-rolled configure it is cross-compiling, so it does not
# try to RUN its probes -- an EmbLink binary cannot execute on the build host
# (same constraint that forces CPython's config.site).
#
# The CFLAGS are the tree's standard set, not a preference:
#   -mno-red-zone         the kernel's interrupt path does not honour the red zone
#   -fno-stack-protector  no __stack_chk_guard/__stack_chk_fail exists here
#   -I$M/user/lib         EmbLink's OVERRIDE headers ahead of newlib's
#   -isystem $NL/include  the C99-formats newlib the apps actually link
cd "$ZSRC"
make distclean >/dev/null 2>&1 || true
CHOST="${CROSS%-}" \
CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
CFLAGS="-O2 -mno-red-zone -fno-stack-protector -I$M/user/lib -isystem $NL/include" \
    ./configure --static --prefix="$OUT"

make -j"$(nproc 2>/dev/null || echo 4)" libz.a
make install

echo
echo "built: $OUT/lib/libz.a"
echo "now:   ZLIB_BUILD=$OUT tools/git/build-git-emblink.sh /path/to/git-2.49.1"
