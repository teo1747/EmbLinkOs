/* EmbLinkOS <sys/stat.h> -- a THIN WRAPPER around newlib's, not a replacement.
 *
 * #include_next pulls in newlib's real header first (found later on the search
 * path, since -Iuser/lib precedes newlib's -isystem); we only add back the
 * POSIX declarations newlib withholds from this target.
 *
 * WHY: newlib declares lstat() only inside
 *
 *     #if defined (__SPU__) || defined(__rtems__) || defined(__CYGWIN__)
 *
 * -- a TARGET ALLOWLIST, not a feature gate. __POSIX_VISIBLE is already 202405
 * (maximum) on x86_64-elf and lstat STILL isn't declared, so no -D_POSIX_*
 * or _XOPEN_SOURCE can reveal it: the guard never consults POSIX visibility.
 * user/lib/syscalls.c defines lstat, so the symbol is real and only the
 * prototype was missing -- callers hit "implicit declaration of lstat".
 */
#ifndef _EMBK_SYS_STAT_H_
#define _EMBK_SYS_STAT_H_

#include_next <sys/stat.h>

/* Mirror newlib's own allowlist so that if it ever DOES declare lstat for this
 * target (a newlib upgrade, or a differently-configured build), we go quiet
 * instead of colliding. Signature copied from newlib's line verbatim. */
#if !defined(__SPU__) && !defined(__rtems__) && !defined(__CYGWIN__)
#ifdef __cplusplus
extern "C" {
#endif
int lstat (const char *__restrict __path, struct stat *__restrict __buf);
#ifdef __cplusplus
}
#endif
#endif

#endif /* _EMBK_SYS_STAT_H_ */
