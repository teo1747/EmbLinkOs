#ifndef __FD_H__
#define __FD_H__

#include "../include/types.h"  
#include "vfs.h"
#include <stdint.h>



/* open() flags. bit values are our own; only O_CREAT/O_EXCL act in v1.
 * O_TRUNC and O_APPEND are reserved - declared so callers can compile against
 * the final ABI, but the open/write path don't use them yet*/

#define O_RDONLY     0x0000
#define O_WRONLY     0x0001
#define O_RDWR       0x0002
#define O_ACCMODE    0x0003  /* mask for the above three */
#define O_CREAT      0x0040
#define O_EXCL       0x0080
#define O_TRUNC      0x0200  /* reserved (needs a truncate op)*/
#define O_APPEND     0x0400  /* reserved  (write is cursor-driven for now)*/


void vfs_fd_init(void);

/* The Unix-shared fd surfaces, by path. returns an fd >= 3 on success, or a
 * negative error code on failure. */

int vfs_open(const char *path, int flags, uint32_t mode);
int vfs_close(int fd);
int vfs_fd_read(int fd, void *buf, size_t len, size_t *out_read);
int vfs_fd_write(int fd, const void *buf, size_t len, size_t *out_written);
int vfs_fd_seek(int fd, int64_t delta, int whence, uint64_t *out_offset);
int vfs_fd_fstat(int fd, struct vfs_stat *out);
int vfs_fd_run_selftests(void);


#endif /* __FD_H__ */