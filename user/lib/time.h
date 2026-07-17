/* EmbLinkOS <time.h> -- a THIN WRAPPER around newlib's, not a replacement.
 *
 * WHY: newlib hides clock_gettime()/clock_getres() behind
 * `#if defined(_POSIX_TIMERS)` and CLOCK_MONOTONIC behind a SECOND, independent
 * `#if defined(_POSIX_MONOTONIC_CLOCK)` -- and its <sys/features.h> only defines
 * those for targets it knows (RTEMS/Cygwin), NEVER for bare x86_64-elf. No
 * feature-test macro reveals them (verified: neither the default nor
 * -D_POSIX_C_SOURCE=200809L works). EmbLink's libc DOES implement both clocks
 * (user/lib/syscalls.c, over the HPET and the RTC), so without this header they
 * are LINKABLE BUT NOT CALLABLE -- every caller gets an implicit declaration and
 * "'CLOCK_MONOTONIC' undeclared". (Found the moment posixdemo.c called them.)
 *
 * We assert the macros and then let newlib's own header supply the prototypes
 * AND the CLOCK_* values. Hand-writing those constants here instead would
 * duplicate ABI numbers that must match newlib exactly (CLOCK_REALTIME=1,
 * CLOCK_MONOTONIC=4) -- a silent-drift hazard that is worse than the tradeoff
 * below.
 *
 * THE TRADEOFF, accepted knowingly: _POSIX_TIMERS also reveals timer_create(),
 * clock_settime(), clock_nanosleep() and friends, which we do NOT implement.
 * They stay DECLARED BUT UNDEFINED, so calling one is a link error at build time
 * -- the same deliberate policy as user/lib/sys/dirent.h. A link error naming
 * the symbol is a far better failure than a stub that returns 0 and lies.
 *
 * (user/lib/syscalls.c defines these macros itself as well, since it must set
 * them before ANY libc header; the values are identical, so the redefinition is
 * legal and harmless.)
 */
#ifndef _EMBK_TIME_H_
#define _EMBK_TIME_H_

#ifndef _POSIX_TIMERS
#define _POSIX_TIMERS 200809L
#endif
#ifndef _POSIX_MONOTONIC_CLOCK
#define _POSIX_MONOTONIC_CLOCK 200809L
#endif

#include_next <time.h>

#endif /* _EMBK_TIME_H_ */
