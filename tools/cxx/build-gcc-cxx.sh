#!/bin/bash
# Build x86_64-elf-g++ + libstdc++ against the SAME newlib the apps link
# (newlib-c99: the C99-formats rebuild). Installs into a USER-OWNED prefix --
# the pattern NEWLIB_PREFIX already established -- so no sudo, and the stock
# /usr/local/cross C toolchain stays untouched as a fallback.
set -e
#   usage: tools/cxx/build-gcc-cxx.sh /path/to/gcc-13.2.0 [install-prefix]
GCC_SRC="${1:?usage: $0 /path/to/gcc-13.2.0 [install-prefix]}"
HERE=$(cd "$(dirname "$0")" && pwd)
M="${MYOS:-$(cd "$HERE/../.." && pwd)}"
NEWLIB="${NEWLIB_PREFIX:-$(make -C "$M" -s --no-print-directory print-newlib-prefix)}"
PREFIX="${2:-${CXX_PREFIX:-$GCC_SRC/../gcc-cxx}}"
export PATH="${CROSS_BIN:-/usr/local/cross/bin}:$PATH"   # target binutils (as/ld)

# gcc looks for target headers/libs in $PREFIX/$TARGET/. Seed it with the
# newlib we actually ship, so libstdc++ configures HOSTED against it.
mkdir -p $PREFIX/x86_64-elf
cp -rn $NEWLIB/x86_64-elf/include $PREFIX/x86_64-elf/ 2>/dev/null || true
cp -rn $NEWLIB/x86_64-elf/lib     $PREFIX/x86_64-elf/ 2>/dev/null || true

BLD="$GCC_SRC/../build-gcc-cxx"
rm -rf "$BLD" && mkdir -p "$BLD" && cd "$BLD"
"$GCC_SRC"/configure \
    --target=x86_64-elf --prefix=$PREFIX --disable-nls \
    --enable-languages=c,c++ \
    --with-newlib \
    --with-headers=$NEWLIB/x86_64-elf/include \
    --enable-threads=single \
    --disable-shared --disable-libssp --disable-libquadmath \
    > configure.log 2>&1
echo "=== configure OK ==="
make -j4 all-gcc                    > make-gcc.log 2>&1;    echo "=== all-gcc OK ==="
make -j4 all-target-libgcc          > make-libgcc.log 2>&1; echo "=== libgcc OK ==="
make -j4 all-target-libstdc++-v3    > make-libstdcxx.log 2>&1; echo "=== libstdc++ OK ==="
make install-gcc install-target-libgcc install-target-libstdc++-v3 > install.log 2>&1
echo "=== INSTALL OK ==="
