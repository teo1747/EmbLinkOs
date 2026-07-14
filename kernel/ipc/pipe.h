/* pipe.h */
#ifndef __PIPE_H__
#define __PIPE_H__
#include <stdint.h>
#include <stddef.h>

#define PIPE_BUF_SIZE 1024   /* per your sizing note: plenty for a shell
                               * pipeline; 32 pipes x 1KB = 32KB of .bss */
#define PIPE_MAX 32

struct pipe;

/* Self-locking (take g_sched_lock internally). Call from syscall context. */
int pipe_create(int *out_read_handle, int *out_write_handle);  /* installs BOTH
                                                                 * as obj-handles in
                                                                 * current_process */
int pipe_read(struct pipe *p, void *buf, size_t len, size_t *out_read);
int pipe_write(struct pipe *p, const void *buf, size_t len, size_t *out_written);

/* _locked variants (caller holds g_sched_lock) -- the channel convention,
 * baked in from the start so a locked context (reap, a selftest) can't
 * deadlock. */
void pipe_ref_locked(struct pipe *p, int side);      /* side: 0=read, 1=write */
void pipe_unref_locked(struct pipe *p, int side);    /* the ONLY teardown path:
                                                       * refcount--, wake peer if a
                                                       * side hit zero, free iff both
                                                       * are zero. NO unconditional-
                                                       * free path exists, so release
                                                       * ordering is irrelevant by
                                                       * construction. */

/* For obj_handle_free_dispatch's switch: */
void pipe_release_for_handle(void *obj_end);
void pipe_release_for_handle_locked(void *obj_end);

#endif /* __PIPE_H__ */