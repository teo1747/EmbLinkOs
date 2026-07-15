/* pipe.c */
#include "pipe.h"
#include "handle.h"
#include "process/process.h"
#include "include/errno.h"
#include "include/kstring.h"

/* struct pipe_end now lives in pipe.h (the INSTALL_OBJ spawn action needs
 * .p/.side); struct pipe itself stays private here. */

struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t head, tail, count;
    int32_t  n_readers, n_writers;    /* COUNTS, not bools -- "the last writer
                                        * left" vs "a writer left" is the whole
                                        * EOF question, and a shell pipeline
                                        * always has two writers (child + the
                                        * shell's own retained handle) */
    struct wait_queue read_wait;
    struct wait_queue write_wait;
    bool used;
    uint32_t id;
};

static struct pipe     g_pipes[PIPE_MAX];        /* static table, proc_table style */
static struct pipe_end g_pipe_ends[PIPE_MAX * 2];
static uint32_t        g_next_pipe_id = 1;

int pipe_create(int *out_read_handle, int *out_write_handle) {
    sched_lock();   /* guards the allocation scan: sys_pipe runs unlocked, two
                     * cores can allocate simultaneously */
    struct pipe *p = NULL;
    for (int i = 0; i < PIPE_MAX; i++)
        if (!g_pipes[i].used) { p = &g_pipes[i]; break; }
    if (!p) { sched_unlock(); return -EMBK_ENFILE; }

    struct pipe_end *re = NULL, *we = NULL;
    for (int i = 0; i < PIPE_MAX * 2 && (!re || !we); i++)
        if (!g_pipe_ends[i].used) { if (!re) re = &g_pipe_ends[i]; else we = &g_pipe_ends[i]; }
    if (!re || !we) { sched_unlock(); return -EMBK_ENFILE; }

    memset(p, 0, sizeof *p);
    p->used = true; p->id = g_next_pipe_id++;
    p->n_readers = 1; p->n_writers = 1;    /* one ref per end, held by the two
                                             * handles about to be installed */
    re->p = p; re->side = 0; re->used = true;
    we->p = p; we->side = 1; we->used = true;
    sched_unlock();

    int rh = obj_handle_alloc(current_process, HANDLE_KIND_PIPE, re);
    if (rh < 0) { /* undo */ sched_lock(); pipe_unref_locked(p, 0); pipe_unref_locked(p, 1);
                  re->used = we->used = false; sched_unlock(); return rh; }
    int wh = obj_handle_alloc(current_process, HANDLE_KIND_PIPE, we);
    if (wh < 0) { obj_handle_free(current_process, rh);   /* drops read ref via dispatch */
                  sched_lock(); pipe_unref_locked(p, 1); we->used = false; sched_unlock();
                  return wh; }

    *out_read_handle = rh; *out_write_handle = wh;
    return EMBK_OK;
}

void pipe_ref_locked(struct pipe *p, int side) {
    if (side == 0) p->n_readers++;
    else           p->n_writers++;
}

void pipe_unref_locked(struct pipe *p, int side) {
    if (side == 0) {
        if (--p->n_readers == 0)
            wait_queue_wake_all(&p->write_wait);   /* writers must wake to see EPIPE,
                                                     * not block forever on a full pipe
                                                     * nobody will drain */
    } else {
        if (--p->n_writers == 0)
            wait_queue_wake_all(&p->read_wait);    /* readers must wake to see EOF --
                                                     * a reader blocked on a writerless
                                                     * pipe is a HANG, not an EOF */
    }
    if (p->n_readers == 0 && p->n_writers == 0)
        p->used = false;    /* free iff BOTH zero -- release order irrelevant */
}

int pipe_read(struct pipe *p, void *buf, size_t len, size_t *out_read) {
    if (!buf || !out_read) return -EMBK_EINVAL;
    if (len == 0) { *out_read = 0; return EMBK_OK; }

    sched_lock();
    while (p->count == 0) {
        if (p->n_writers == 0) {        /* empty AND no writer will ever refill:
                                          * EOF. Checked UNDER the lock, atomically
                                          * with the emptiness test. */
            sched_unlock();
            *out_read = 0;               /* read() = 0 IS the EOF signal */
            return EMBK_OK;
        }
        sched_block_current_locked(&p->read_wait);
        sched_lock();
    }
    size_t n = 0;
    uint8_t *out = buf;
    while (n < len && p->count > 0) {
        out[n++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count--;
    }
    wait_queue_wake_all(&p->write_wait);   /* space freed -> writers retry */
    sched_unlock();
    *out_read = n;
    return EMBK_OK;
}

int pipe_write(struct pipe *p, const void *buf, size_t len, size_t *out_written) {
    if ((!buf && len) || !out_written) return -EMBK_EINVAL;
    if (len == 0) { *out_written = 0; return EMBK_OK; }

    sched_lock();
    while (p->count == PIPE_BUF_SIZE) {
        if (p->n_readers == 0) { sched_unlock(); return -EMBK_EPIPE; }
        sched_block_current_locked(&p->write_wait);
        sched_lock();
    }
    if (p->n_readers == 0) {    /* re-check after any block: readers may have
                                  * vanished while we slept */
        sched_unlock(); return -EMBK_EPIPE;
    }
    size_t n = 0;
    const uint8_t *in = buf;
    while (n < len && p->count < PIPE_BUF_SIZE) {
        p->buf[p->head] = in[n++];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count++;
    }
    wait_queue_wake_all(&p->read_wait);
    sched_unlock();
    *out_written = n;    /* may be < len: partial write, caller loops */
    return EMBK_OK;
}

/* Live-pipe count, for the EOF selftest's leak assertion (mirrors
 * channel_live_count): after every reference is dropped the count must
 * return to its baseline, proving free-at-zero actually fired. */
uint32_t pipe_live_count(void) {
    uint32_t n = 0;
    sched_lock();
    for (int i = 0; i < PIPE_MAX; i++)
        if (g_pipes[i].used) n++;
    sched_unlock();
    return n;
}

void pipe_release_for_handle_locked(void *obj_end) {
    struct pipe_end *e = obj_end;
    if (!e || !e->used) return;
    pipe_unref_locked(e->p, e->side);
    e->used = false;
}
void pipe_release_for_handle(void *obj_end) {
    sched_lock();
    pipe_release_for_handle_locked(obj_end);
    sched_unlock();
}