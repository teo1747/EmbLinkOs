/* sys/utsname.h -- EmbLink override header (newlib ships none).
 *
 * Unlike the syslog.h shim next door, uname() is DEFINED (syscalls.c): "what
 * OS is this" is a question EmbLink can answer truthfully, and git links it
 * (git-compat-util.h includes this unconditionally; `git version
 * --build-options` and bugreport call it). */
#ifndef _EMBK_SYS_UTSNAME_H
#define _EMBK_SYS_UTSNAME_H

#define _UTSNAME_LEN 65

struct utsname {
    char sysname[_UTSNAME_LEN];    /* "EmbLink" */
    char nodename[_UTSNAME_LEN];   /* single machine, no network naming */
    char release[_UTSNAME_LEN];
    char version[_UTSNAME_LEN];
    char machine[_UTSNAME_LEN];    /* "x86_64" */
};

#ifdef __cplusplus
extern "C" {
#endif

int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_SYS_UTSNAME_H */
