#!/bin/bash
# Build git for EmbLinkOS. Reproducible invocation -- the git analogue of
# configure-py-emblink.sh; see memory/git-port.md for WHY each piece is here.
#
# git's Makefile (unlike CPython's autoconf) takes its platform truth from
# `uname_S`, computed in config.mak.uname on the BUILD host -- which would
# apply the LINUX block (HAVE_CLOCK_MONOTONIC, PROCFS paths, alloca.h...).
# Overriding uname_S on the make COMMAND LINE beats that assignment, so no
# platform block matches and we start from git's most conservative defaults,
# then say exactly what EmbLink has via config.mak.
set -euo pipefail

#   usage: tools/git/build-git-emblink.sh /path/to/git-2.49.1 [make args...]
#
# Needs a CROSS-BUILT zlib (git uses deflate/inflate/crc32 for its object store).
# Point ZLIB_BUILD at it; there is no script for zlib yet -- see the note in
# docs/BUILD_SETUP.md.
SRC="${1:?usage: $0 /path/to/git-2.49.1 [make args...]}"; shift || true
# Paths are DERIVED, never hardcoded -- this script must work on a teammate's
# checkout. MYOS comes from where this script lives; NEWLIB_PREFIX from the
# Makefile (the ONE source of truth: `make print-newlib-prefix`); the source
# tree is an argument. Same convention as tools/tcc/build-tcc-emblink.sh.
HERE=$(cd "$(dirname "$0")" && pwd)
M="${MYOS:-$(cd "$HERE/../.." && pwd)}"
NEWLIB_PREFIX="${NEWLIB_PREFIX:-$(make -C "$M" -s --no-print-directory print-newlib-prefix)}"
NL="$NEWLIB_PREFIX/x86_64-elf"        # the SAME libc the OS's apps link
export PATH="${CROSS_BIN:-/usr/local/cross/bin}:$PATH"   # target binutils (as/ld)
Z="${ZLIB_BUILD:?set ZLIB_BUILD=/path/to/cross-built-zlib (see docs/BUILD_SETUP.md)}"

for o in "$M/build/crt0.o" "$M/build/syscalls.o"; do
    [ -f "$o" ] || { echo "missing $o -- run 'make' in $M first" >&2; exit 1; }
done

# NOTE: this heredoc is deliberately UNQUOTED -- $M/$NL/$Z must expand. That also
# makes backticks COMMAND SUBSTITUTIONS, so every backtick below is escaped (\`).
# Unescaped, `git add` in a comment actually ran git, and `-cpu max` printed
# "command not found" and silently vanished from the generated config.mak.
cat > "$SRC/config.mak" <<EOF
# EmbLinkOS -- written by build-git-emblink.sh; edit THAT, not this.
CC = x86_64-elf-gcc
AR = x86_64-elf-ar

# Same flag set CPython shipped with (see configure-py-emblink.sh for the
# rationale on each: override headers first, newlib's gates opened by hand).
# __LINUX_ERRNO_EXTENSIONS__: newlib GATES its socket-family errnos (ESHUTDOWN,
# ENOTSOCK, ...) behind this; git's compat/poll names them. It only reveals
# macros newlib itself ships -- no numbering changes, and the by-name
# kernel->newlib errno translation in syscalls.c is unaffected.
# SA_RESTART=0: newlib's sigaction struct/decl exist but SA_RESTART doesn't.
# It requests "restart interrupted syscalls instead of EINTR" -- EmbLink never
# interrupts a syscall with a signal AT ALL (cancellation is observed, not
# injected; docs/INTERRUPTION.md), so the request is VACUOUSLY satisfied and 0
# is an honest value, same reasoning as FD_CLOEXEC.
CFLAGS = -O2 -mno-red-zone -fno-stack-protector \
         -I$M/user/lib -isystem $NL/include \
         -DSSIZE_MAX=0x7fffffffffffffffL \
         -D_POSIX_TIMERS=200809L -D_POSIX_MONOTONIC_CLOCK=200809L \
         -D__LINUX_ERRNO_EXTENSIONS__ \
         -DSA_RESTART=0
LDFLAGS = -static -nostartfiles -T $M/user/lib/newlib.ld -L$NL/lib \
          $M/build/crt0.o $M/build/syscalls.o

ZLIB_PATH = $Z

# The CSPRNG. git's csprng_bytes() picks a backend from this; getentropy is the
# one EmbLink genuinely has (user/lib/syscalls.c, RDRAND-backed) and newlib
# declares it in <unistd.h>, which git-compat-util.h already includes -- so no
# shim and no patch. Without it git falls through to opening /dev/urandom, which
# does not exist here, and \`git add\` dies making a temp file.
#
# NOTE this makes git inherit getentropy's REFUSAL to fabricate entropy: on a
# CPU without RDRAND it returns ENOSYS rather than inventing bytes, so git fails
# loudly instead of naming temp files predictably. Run QEMU with \`-cpu max\`
# (same requirement CPython has -- see memory/cpython-port.md).
CSPRNG_METHOD = getentropy

# --- capabilities EmbLink genuinely lacks (each of these is TRUE) -----------
NO_OPENSSL = YesPlease
NO_CURL = YesPlease
NO_EXPAT = YesPlease
NO_ICONV = YesPlease
NO_GETTEXT = YesPlease
NO_PERL = YesPlease
NO_PYTHON = YesPlease
NO_TCLTK = YesPlease
NO_PTHREADS = YesPlease
NO_MMAP = YesPlease
NO_IPV6 = YesPlease
NO_UNIX_SOCKETS = YesPlease
NO_POLL = YesPlease
NO_SETITIMER = YesPlease
NO_REGEX = YesPlease
NO_MKSTEMPS = YesPlease
NO_NSEC = YesPlease
NO_INSTALL_HARDLINKS = YesPlease
# git SELF-SUPPLIES these via compat/ when told the libc lacks them -- better
# than us re-implementing what it already ships:
NO_LIBGEN_H = YesPlease
NO_PREAD = YesPlease
EOF

echo "=== config.mak written; building ==="
cd "$SRC"
make uname_S=EmbLink uname_R=1.0 uname_M=x86_64 git -j4 "$@"
