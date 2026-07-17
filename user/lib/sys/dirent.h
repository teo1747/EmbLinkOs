/* EmbLinkOS <sys/dirent.h> -- the types behind <dirent.h>.
 *
 * WHY THIS FILE EXISTS: newlib ships only a placeholder sys/dirent.h for bare
 * x86_64-elf whose entire body is `#error "<dirent.h> not supported"`. Its own
 * comment names the intended fix: "On a system which supports <dirent.h>, this
 * file is overridden by dirent.h in the libc/sys/.../sys directory." This IS
 * that override; it wins because USER_INC's -Iuser/lib is searched before
 * newlib's -isystem directory.
 *
 * newlib's <dirent.h> already declares the whole API (opendir/readdir/closedir/
 * rewinddir/scandir/telldir/...) and needs only DIR and struct dirent from
 * here. user/lib/syscalls.c implements the honest subset; the rest stay
 * DECLARED BUT UNDEFINED on purpose, so reaching for one is a link error at
 * build time rather than a stub that lies at run time.
 */
#ifndef _SYS_DIRENT_H_
#define _SYS_DIRENT_H_

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* d_type values. These are the POSIX/BSD numbers, and they deliberately do NOT
 * match EmbLink's native VFS_DT_ tags (REG=1, DIR=2, LNK=3). The kernel keeps
 * its own enum; syscalls.c translates at this boundary. Don't "simplify" by
 * aliasing them -- the two numbering schemes disagree on every value. */
#define DT_UNKNOWN   0
#define DT_FIFO      1
#define DT_CHR       2
#define DT_DIR       4
#define DT_BLK       6
#define DT_REG       8
#define DT_LNK      10
#define DT_SOCK     12

/* The kernel's sys_dirent carries a 59-byte NUL-terminated name and silently
 * truncates anything longer, so names are in practice bounded well below this;
 * 255 is the conventional POSIX figure and costs only address space here. */
#define _EMBK_NAME_MAX 255

struct dirent {
    ino_t          d_ino;                      /* fs-private object id */
    unsigned char  d_type;                     /* DT_* (translated, see above) */
    unsigned short d_reclen;                   /* fixed: records aren't packed */
    char           d_name[_EMBK_NAME_MAX + 1]; /* NUL-terminated */
};

/* Opaque: the snapshot layout is syscalls.c's business, and callers only ever
 * hold a DIR* handed back by opendir(). */
typedef struct __dirstream DIR;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_DIRENT_H_ */
