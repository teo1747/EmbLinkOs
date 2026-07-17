/* EmbLinkOS <sys/utime.h> -- overrides newlib's broken dummy.
 *
 * newlib's own sys/utime.h calls itself "a dummy <sys/utime.h> file, not
 * customized for any particular system. If there is a utime.h in
 * libc/sys/SYSDIR/sys, it will override this one." This is that override, and
 * the dummy needs replacing for two independent reasons:
 *
 *   1. It is BROKEN as written: it uses time_t without including anything that
 *      defines it, so `#include <utime.h>` fails outright ("unknown type name
 *      'time_t'") unless the includer happened to pull in <sys/types.h> first.
 *   2. It defines struct utimbuf but NEVER declares utime(), so callers that
 *      get past (1) then hit an implicit declaration.
 *
 * Both bit CPython: configure sets HAVE_UTIME_H=1 (the header "exists"), and
 * Modules/posixmodule.c then calls utime() with no prototype in scope.
 */
#ifndef _SYS_UTIME_H
#define _SYS_UTIME_H

#include <sys/types.h>   /* time_t -- the dummy's missing include */

#ifdef __cplusplus
extern "C" {
#endif

struct utimbuf {
    time_t actime;    /* access time */
    time_t modtime;   /* modification time */
};

/* Declared so callers compile; user/lib/syscalls.c defines it as an honest
 * ENOSYS failure -- EmbLink tracks mtime but exposes no syscall to SET it. */
int utime(const char *__path, const struct utimbuf *__times);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UTIME_H */
