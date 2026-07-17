/* sys/statvfs.h -- EmbLink override header (newlib ships none).
 *
 * statvfs() is defined in syscalls.c as an honest ENOSYS: the kernel VFS has
 * no filesystem-statistics op yet. When EMBKFS grows one, wire it here for
 * real -- free_blocks is already in the superblock, so this is a genuine
 * "not yet" rather than a "cannot". */
#ifndef _EMBK_SYS_STATVFS_H
#define _EMBK_SYS_STATVFS_H

#include <sys/types.h>

/* fsblkcnt_t / fsfilcnt_t come from newlib's <sys/types.h>. */

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t    f_blocks;
    fsblkcnt_t    f_bfree;
    fsblkcnt_t    f_bavail;
    fsfilcnt_t    f_files;
    fsfilcnt_t    f_ffree;
    fsfilcnt_t    f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

#ifdef __cplusplus
extern "C" {
#endif

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_SYS_STATVFS_H */
