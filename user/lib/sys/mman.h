/* sys/mman.h -- EmbLink override header (newlib ships none).
 *
 * EmbLink has NO mmap. Memory comes from sbrk (a real, growable heap) and from
 * the typed object primitives (shared surfaces, zero-copy windows) -- mapping a
 * FILE into an address space is a capability this OS has never had, not one it
 * mislaid. The kernel VMM could grow it; nothing has needed it.
 *
 * These exist so portable code COMPILES (TCC's tccrun.c -- its `-run` mode --
 * includes this unconditionally even when you only ever compile to a file).
 * The functions are defined in syscalls.c as honest ENOSYS refusals, so
 * `tcc -run` fails loudly and `tcc -o` never comes near them. Nothing here
 * fakes an anonymous mapping out of malloc: a caller that asked for MAP_SHARED
 * or a file mapping would get something that silently is not one. */
#ifndef _EMBK_SYS_MMAN_H
#define _EMBK_SYS_MMAN_H

#include <sys/types.h>
#include <stddef.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANON      0x20
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void *)-1)

#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int   munmap(void *addr, size_t len);
int   mprotect(void *addr, size_t len, int prot);
int   msync(void *addr, size_t len, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_SYS_MMAN_H */
