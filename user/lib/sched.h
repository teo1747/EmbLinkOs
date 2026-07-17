/* EmbLinkOS <sched.h> -- a THIN WRAPPER around newlib's, not a replacement.
 *
 * WHY: newlib hides sched_yield() behind
 *
 *     #if defined(_POSIX_THREADS) || defined(_POSIX_PRIORITY_SCHEDULING)
 *
 * and EmbLink implements neither of those APIs in full -- no pthread library, no
 * priority scheduling. But we DO implement sched_yield itself (it is a thin call
 * over SYS_yield, see user/lib/syscalls.c), so without this header the symbol is
 * LINKABLE BUT NOT CALLABLE: every caller gets "implicit declaration of function
 * 'sched_yield'" and, under -Werror=implicit-function-declaration, fails to
 * build. (Found the moment posixdemo.c became its first caller.)
 *
 * Declaring just the one function we actually provide is deliberate: #defining
 * _POSIX_PRIORITY_SCHEDULING to reveal it would ALSO declare sched_setparam,
 * sched_getscheduler, sched_get_priority_max, sched_rr_get_interval and friends
 * -- none of which exist here, turning a clear compile error into a link error
 * much further from the cause.
 */
#ifndef _EMBK_SCHED_H_
#define _EMBK_SCHED_H_

#include_next <sched.h>

/* Mirror newlib's own condition: when either macro IS set (CPython's
 * pthread_stubs.h defines _POSIX_THREADS, for instance) newlib has already
 * declared sched_yield and we must stay quiet rather than redeclare it. */
#if !defined(_POSIX_THREADS) && !defined(_POSIX_PRIORITY_SCHEDULING)
#ifdef __cplusplus
extern "C" {
#endif
int sched_yield(void);
#ifdef __cplusplus
}
#endif
#endif

#endif /* _EMBK_SCHED_H_ */
