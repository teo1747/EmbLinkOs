#include "fs/embkfs/embkfs.h"
#include "fs/embkfs/crc32c.h"
#include "block/block.h"
#include "include/kmalloc.h"   /* kmalloc / kfree — the Phase 8 heap */
#include "include/kprintf.h"
#include "include/errno.h"
#include "include/kstring.h"   /* memcmp, strlen */
#include "drivers/timer/rtc.h"       /* rtc_now_ns — real inode timestamps (v2.2) */
#include "drivers/timer/hpet.h"       /* fs-lock contention timing */
#include "drivers/timer/pit.h"       /* pit_delay_ms — selftest RTC-resolution wait */
#include "fs/embkfs/embkfs_compress.h"         /* per-extent compression (v2.2 Phase 3) */
#include "crypto/xts.h"        /* AES-256-XTS (v2.2 Phase 4) */
#include "crypto/pbkdf2.h"     /* passphrase KDF (v2.2 Phase 4) */
#include "crypto/hmac.h"       /* HMAC-SHA256 (v2.2 Phase 5d verified-root) */
#include "drivers/input/keyboard.h"  /* keyboard_getchar — mount-time passphrase prompt */
#include "process/process.h"   /* current_process->pid — writer provenance (v2.2 Phase 5c) */

/* --------------------------------------------------------------------------
 * THE EMBKFS BIG LOCK.
 *
 * This file is full of shared `static uint8_t buf[4096]` node/probe scratch
 * buffers ("the kernel stack is tiny"), and the per-volume read cache
 * (vol->rcache_*) is likewise shared. Fine while every caller was the one boot
 * CPU; a genuine data race once two cores descend the tree at once.
 *
 * It stayed LATENT while the boot image was a single-leaf tree -- racing
 * descents read the SAME block into the shared buffer, identical bytes,
 * invisible. The first 2-leaf image made concurrent descents read DIFFERENT
 * blocks, and the Merkle check caught core B validating core A's just-loaded
 * leaf as the root (observed live: `run /term.elf` racing home's clockw spawn,
 * "block 17 checksum 0xF7E73490 != parent's 0x6D1CCFF1", where F7E73490 was
 * leaf 19's csum). Worth restating: the bug was invisible until the DATA got
 * big enough, not until the code changed.
 *
 * A SLEEPING lock, not a spinlock: these ops block on disk I/O, so waiters are
 * ordinary schedulable threads on a wait queue. Coarse by design -- one lock
 * for the whole filesystem. Correctness first; per-volume or per-path locking
 * is a later optimization nothing has yet asked for.
 *
 * WHY IT LIVES HERE and not at the VFS bridge, where it started: the bridge
 * covered everything userspace and the ELF loader do, but the kernel console's
 * DIRECT calls -- `snap`, `test embkfs ...` -- went around it, which was
 * documented at the time and left unfixed. A lock belongs with the data it
 * protects, so it now sits in this file and the bridge delegates to it. The
 * console paths lock because the entry points they call do.
 *
 * RECURSIVE BY OWNER THREAD, deliberately: vfs_ls's readdir CALLBACK stats
 * each entry (the boot dump's size column), so stat legitimately nests inside
 * a locked readdir -- a non-recursive lock self-deadlocked the boot at `ls /:`
 * on the very first locked build. That same recursion is what lets a selftest
 * take the lock and still call the public API underneath it.
 *
 * Owner identity is current_thread; the only NULL-current context that runs fs
 * code is the single pre-adoption boot CPU, so NULL==NULL cannot alias two
 * threads.
 *
 * NEVER hold this across something that waits on ANOTHER thread doing fs I/O
 * (spawning a process and waiting for it, say): the child's ops take this same
 * lock from a different thread and would block on a holder that is itself
 * blocked on the child. That is why the selftest dispatcher does NOT wrap
 * commands wholesale -- the lock is taken by the leaf entry points instead.
 * -------------------------------------------------------------------------- */
static int g_fs_busy = 0;
static struct thread *g_fs_owner = 0;   /* valid only while busy */
static int g_fs_depth = 0;
static struct wait_queue g_fs_wq;       /* zero-init = empty */

/* Contention instrumentation. The obvious next move on this lock is to make it
 * finer-grained (per-volume, or per-object), and that is a large change: 34
 * shared `static` scratch buffers in this file would have to become per-caller
 * first, because THEY are what the lock is really protecting. Before paying
 * that, measure whether anything is actually waiting -- the same rule the I/O
 * path rebuild was held to. `waits` counts acquisitions that had to block;
 * `wait_us` is the wall time they spent blocked. */
static struct embkfs_lockstat g_lockstat;

/* Same shape as block.c's blk_now_us: the HPET is the only wall clock here,
 * and a machine without one just reports 0 rather than a fabricated number. */
static uint64_t fs_now_us(void)
{
    if (!hpet_available()) return 0;
    uint64_t pf = hpet_period_fs();
    if (!pf) return 0;
    uint64_t tpus = 1000000000ULL / pf;
    if (tpus == 0) tpus = 1;
    return hpet_read_counter() / tpus;
}

void embkfs_lockstat_get(struct embkfs_lockstat *out) { if (out) *out = g_lockstat; }
void embkfs_lockstat_reset(void)
{
    g_lockstat.acquires = 0; g_lockstat.recursive = 0;
    g_lockstat.waits = 0; g_lockstat.wait_us = 0;
}

void embkfs_lock(void)
{
    sched_lock();
    if (g_fs_busy && g_fs_owner == current_thread) {
        g_fs_depth++;
        g_lockstat.recursive++;
        sched_unlock();
        return;
    }
    g_lockstat.acquires++;
    bool waited = g_fs_busy;
    uint64_t t0 = waited ? fs_now_us() : 0;
    while (g_fs_busy) {
        sched_block_current_locked(&g_fs_wq);   /* returns UNLOCKED */
        sched_lock();
    }
    if (waited) {
        g_lockstat.waits++;
        g_lockstat.wait_us += fs_now_us() - t0;
    }
    g_fs_busy = 1;
    g_fs_owner = current_thread;
    g_fs_depth = 1;
    sched_unlock();
}

void embkfs_unlock(void)
{
    sched_lock();
    if (--g_fs_depth == 0) {
        g_fs_busy = 0;
        g_fs_owner = 0;
        wait_queue_wake_one(&g_fs_wq);
    }
    sched_unlock();
}

_Static_assert(sizeof(struct aes_xts_ctx) <= sizeof(((struct embkfs_volume *)0)->xts_opaque),
               "embkfs_volume.xts_opaque too small for struct aes_xts_ctx");

/* Encrypts/decrypts every AES block within ONE on-disk filesystem block:
 * the tweak is derived fresh from that block's own disk address (v2.2
 * Phase 4's design decision -- see embkfs.h's crypto header comment), so
 * every physical block is its own independent XTS "sector" regardless of
 * which extent or file it belongs to. */
static inline struct aes_xts_ctx *embkfs_xts(struct embkfs_volume *vol) {
    return (struct aes_xts_ctx *)vol->xts_opaque;
}

static struct embkfs_volume *g_embkfs_live = NULL;

/* v2.2 (Phase 1): every mounted volume, in the order embkfs_init() found
 * them. g_embkfs_live always aliases &g_embkfs_volumes[0] once at least
 * one volume is mounted -- kept as its own pointer (rather than always
 * writing &g_embkfs_volumes[0] at every call site) purely so every
 * existing single-volume caller in this file needs zero changes. */
static struct embkfs_volume g_embkfs_volumes[EMBKFS_MAX_VOLUMES];
static uint32_t g_embkfs_volume_count = 0;

struct embkfs_volume *embkfs_live_volume(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted)
        return NULL;
    return g_embkfs_live;
}

uint32_t embkfs_volume_count(void)
{
    return g_embkfs_volume_count;
}

struct embkfs_volume *embkfs_volume_at(uint32_t index)
{
    if (index >= g_embkfs_volume_count) return NULL;
    return &g_embkfs_volumes[index];
}


static inline void embk_bm_set (uint8_t *bm, uint64_t b) { bm[b >> 3] |= (uint8_t)(1u << (b & 7)); }
static inline bool embk_bm_test(uint8_t *bm, uint64_t b) { return (bm[b >> 3] >> (b & 7)) & 1u; }

/* Item header `i` in a leaf: the header array grows forward from just after the
 * node header, 32 bytes each. (Packed struct, so the unaligned read is fine.) */

static inline const struct embk_item_header *embk_leaf_item(const uint8_t *node, uint32_t i)
{
    return (const struct embk_item_header *)(node + sizeof(struct embk_node_header)
                                             + (size_t)i * sizeof(struct embk_item_header));
}

/* Pointer to an item's data, but only if the item actually holds at least
 * `min_size` bytes and they sit within the block. NULL otherwise. Every item
 * body is decoded through this so a malformed offset/size can't walk us off
 * the block. */
static const void *embk_item_data(const uint8_t *buf, uint64_t block_size,
                                  const struct embk_item_header *it, uint32_t min_size)
{
    if (it->size < min_size) return NULL;
    if ((uint64_t)it->offset + it->size > block_size) return NULL;
    return buf + it->offset;
}

/* Validate core extent invariants before any caller trusts the run. */
static int embkfs_extent_validate(const struct embkfs_volume *vol,
                                  const struct embk_extent_item *ext,
                                  uint64_t key_offset,
                                  const char *where)
{
    bool is_hole = (ext->flags & EMBKFS_EXTENT_F_HOLE) != 0;
    if (is_hole) {
        if (ext->length != 0 || ext->disk_block != 0) {
            kprintf("EMBKFS: %s: %s: hole extent@%lu must have zero disk run\n",
                    vol->dev->name, where, key_offset);
            return -EMBK_EINVAL;
        }
        if (ext->logical_size == 0) {
            kprintf("EMBKFS: %s: %s: hole extent@%lu has zero logical size\n",
                    vol->dev->name, where, key_offset);
            return -EMBK_EINVAL;
        }
        return EMBK_OK;
    }

    if (ext->length == 0) {
        kprintf("EMBKFS: %s: %s: extent@%lu has zero length\n", vol->dev->name, where, key_offset);
        return -EMBK_EINVAL;
    }
    if (ext->disk_block >= vol->total_blocks || ext->length > vol->total_blocks - ext->disk_block) {
        kprintf("EMBKFS: %s: %s: extent@%lu run out of range (start %lu len %lu)\n",
                vol->dev->name, where, key_offset, ext->disk_block, ext->length);
        return -EMBK_EINVAL;
    }
    uint64_t cap = ext->length * vol->block_size;
    if (ext->flags & EMBKFS_EXTENT_F_COMPRESSED) {
        /* Compression is exactly the case where logical_size (decompressed)
         * legitimately exceeds what `length` blocks could hold uncompressed
         * -- that's the point. The physical invariant to check instead is
         * that the COMPRESSED payload actually fits in the blocks reserved
         * for it, and that the volume-wide total (used as a read-side
         * allocation size) is bounded so a corrupt extent can't make the
         * read path kmalloc() something absurd. */
        uint64_t comp_size = embk_extent_compressed_size(ext);
        if (comp_size == 0 || comp_size > cap) {
            kprintf("EMBKFS: %s: %s: extent@%lu compressed_size %lu invalid for %lu blocks\n",
                    vol->dev->name, where, key_offset, comp_size, ext->length);
            return -EMBK_EINVAL;
        }
        if (ext->logical_size == 0 || ext->logical_size > vol->total_blocks * vol->block_size) {
            kprintf("EMBKFS: %s: %s: extent@%lu logical_size %lu implausible (volume is %lu blocks)\n",
                    vol->dev->name, where, key_offset, ext->logical_size, vol->total_blocks);
            return -EMBK_EINVAL;
        }
        return EMBK_OK;
    }
    if (ext->logical_size == 0 || ext->logical_size > cap) {
        kprintf("EMBKFS: %s: %s: extent@%lu logical_size %lu invalid for %lu blocks\n",
                vol->dev->name, where, key_offset, ext->logical_size, ext->length);
        return -EMBK_EINVAL;
    }
    return EMBK_OK;
}


/* Walk the live tree rooted at *ptr, marking every referenced block USED in
 * vol->block_bitmap. Each node is verified against its parent pointer on the
 * way down (embkfs_read_node) — walking an unverified tree would prove nothing.
 * Recursion depth is tree height (a handful of levels), not block count. */
static int embkfs_mark_tree(struct embkfs_volume *vol, const struct embk_block_ptr *ptr)
{
    const char *dev = vol->dev->name;
    static uint8_t nodebuf[4096];          /* ONE shared buffer (kernel stack is tiny) */

    /* 1. read + verify this node */
    int rc = embkfs_read_node(vol, ptr, nodebuf, sizeof nodebuf);
    if (rc != EMBK_OK)
        return rc;

    /* 2. mark the node's own block used */
    embk_bm_set(vol->block_bitmap, ptr->block);

    const struct embk_node_header *h = (const struct embk_node_header *)nodebuf;

    if (h->level > 0) {
        /* 3. INTERNAL node. Copy every child pointer into a local array BEFORE
         *    recursing: the recursive call reuses nodebuf, so once we descend
         *    into child 0, this node's slots are overwritten. Copy first, then
         *    the shared buffer is free for the recursion to clobber. */
        if (h->nritems > EMBKFS_MAX_SLOTS) {       /* never trust on-disk counts */
            kprintf("EMBKFS: %s: internal node nritems %u too large\n", dev, (unsigned)h->nritems);
            return -EMBK_EINVAL;
        }
        struct embk_block_ptr children[EMBKFS_MAX_SLOTS];
        uint32_t n = h->nritems;

        const struct embk_internal_slot *slots =
            (const struct embk_internal_slot *)(nodebuf + sizeof(struct embk_node_header));
        for (uint32_t i = 0; i < n; i++)
            memcpy(&children[i], &slots[i].ptr, sizeof children[i]);   /* aligned copy out */

        for (uint32_t i = 0; i < n; i++) {
            rc = embkfs_mark_tree(vol, &children[i]);                  /* nodebuf reused here */
            if (rc != EMBK_OK)
                return rc;
        }
    } else {
        /* 4. LEAF node. Mark the data blocks of every extent item. An extent
         *    references a RUN: disk_block .. disk_block + length_blocks - 1. */
        for (uint32_t i = 0; i < h->nritems; i++) {
            const struct embk_item_header *it = embk_leaf_item(nodebuf, i);
            uint64_t koid  = it->key.object_id;     /* read fields before any buffer reuse */
            uint64_t ktype = it->key.type;

            /* Object-id high-water mark: every object has an inode in some leaf,
             * so the largest object_id across all leaf items is the highest id in
             * use. The allocator hands out next_oid = max + 1 (finalised in
             * embkfs_alloc_init). Tracked here to reuse the one full tree walk. */
            if (koid > vol->next_oid)
                vol->next_oid = koid;

            if (ktype != EMBK_TYPE_EXTENT)
                continue;

            const struct embk_extent_item *ext =
                embk_item_data(nodebuf, vol->block_size, it, sizeof *ext);
            if (!ext) {
                kprintf("EMBKFS: %s: extent item truncated during walk\n", dev);
                return -EMBK_EINVAL;
            }
            rc = embkfs_extent_validate(vol, ext, it->key.offset, "tree walk");
            if (rc != EMBK_OK)
                return rc;
            uint64_t start = ext->disk_block;
            uint64_t len   = ext->length;           /* length_blocks */
            if (ext->flags & EMBKFS_EXTENT_F_HOLE)
                continue;
            for (uint64_t b = 0; b < len; b++) {
                embk_bm_set(vol->block_bitmap, start + b);
            }
        }
    }
    return EMBK_OK;
}


/* ---- Transactional allocator -------------------------------------------
 * A COW commit allocates new blocks (the rewritten tree path, fresh data) and
 * supersedes old ones (the previous versions of those nodes, replaced data). The
 * old blocks cannot be reused until the commit is durable — until then the old
 * tree must stay whole so a crash falls back to it cleanly. So while a write op
 * runs, every allocation and every supersession is recorded in a transaction;
 * embkfs_txn_end then reconciles the in-memory bitmap:
 *   - commit succeeded -> release the SUPERSEDED blocks (reclaim them this
 *     session, instead of leaking them until the next mount's tree walk);
 *   - commit failed     -> release the ALLOCATED blocks (roll the attempt back).
 * The lists are bounded (one commit touches a tree-height-sized handful of
 * nodes plus a few data blocks); on the rare overflow we fall back to rebuilding
 * the bitmap from the live tree, which is always exact. Snapshots will later
 * gate this (a block an older root still needs must not be freed); v1 has none,
 * so a superseded block is always reclaimable. */


static int embkfs_bitmap_build(struct embkfs_volume *vol);   /* defined below; overflow backstop */

static inline void embk_bm_clear(uint8_t *bm, uint64_t b) { bm[b >> 3] &= (uint8_t)~(1u << (b & 7)); }

static void embkfs_free_index_clear(struct embkfs_volume *vol)
{
    if (!vol) return;
    kfree(vol->free_ext);
    vol->free_ext = NULL;
    vol->free_ext_n = 0;
    vol->free_ext_cap = 0;
}

static int embkfs_free_index_reserve(struct embkfs_volume *vol, uint32_t need)
{
    if (vol->free_ext_cap >= need) return EMBK_OK;
    uint32_t cap = vol->free_ext_cap ? vol->free_ext_cap : 16;
    while (cap < need) {
        if (cap > UINT32_MAX / 2) return -EMBK_ENOMEM;
        cap *= 2;
    }
    struct embk_run *grown = krealloc(vol->free_ext, (uint64_t)cap * sizeof(struct embk_run));
    if (!grown) return -EMBK_ENOMEM;
    vol->free_ext = grown;
    vol->free_ext_cap = cap;
    return EMBK_OK;
}

static int embkfs_free_index_insert_merge(struct embkfs_volume *vol, uint64_t start, uint64_t len)
{
    if (len == 0) return EMBK_OK;
    if (start >= vol->total_blocks || len > vol->total_blocks - start) return -EMBK_EINVAL;

    uint64_t ns = start;
    uint64_t ne = start + len;
    uint32_t i = 0;

    while (i < vol->free_ext_n) {
        uint64_t s = vol->free_ext[i].start;
        uint64_t e = s + vol->free_ext[i].len;
        if (e < ns) { i++; continue; }
        if (ne < s) break;

        /* overlap or adjacency: coalesce into one larger free run */
        if (s < ns) ns = s;
        if (e > ne) ne = e;
        for (uint32_t k = i; k + 1 < vol->free_ext_n; k++)
            vol->free_ext[k] = vol->free_ext[k + 1];
        vol->free_ext_n--;
    }

    int rc = embkfs_free_index_reserve(vol, vol->free_ext_n + 1);
    if (rc != EMBK_OK) return rc;
    for (uint32_t k = vol->free_ext_n; k > i; k--)
        vol->free_ext[k] = vol->free_ext[k - 1];
    vol->free_ext[i].start = ns;
    vol->free_ext[i].len = ne - ns;
    vol->free_ext_n++;
    return EMBK_OK;
}

static int embkfs_free_index_rebuild(struct embkfs_volume *vol)
{
    embkfs_free_index_clear(vol);
    uint64_t b = 0;
    while (b < vol->total_blocks) {
        while (b < vol->total_blocks && embk_bm_test(vol->block_bitmap, b)) b++;
        if (b == vol->total_blocks) break;
        uint64_t start = b;
        while (b < vol->total_blocks && !embk_bm_test(vol->block_bitmap, b)) b++;
        uint64_t len = b - start;
        int rc = embkfs_free_index_insert_merge(vol, start, len);
        if (rc != EMBK_OK) {
            embkfs_free_index_clear(vol);
            return rc;
        }
    }
    return EMBK_OK;
}

static int embkfs_free_run(struct embkfs_volume *vol, uint64_t start, uint64_t len)
{
    if (len == 0) return EMBK_OK;
    if (start >= vol->total_blocks || len > vol->total_blocks - start) return -EMBK_EINVAL;

    for (uint64_t b = 0; b < len; b++) {
        if (!embk_bm_test(vol->block_bitmap, start + b)) {
            kprintf("EMBKFS: %s: double-free detected at block %lu\n", vol->dev->name, start + b);
            return -EMBK_EEXIST;
        }
    }
    for (uint64_t b = 0; b < len; b++)
        embk_bm_clear(vol->block_bitmap, start + b);
    return embkfs_free_index_insert_merge(vol, start, len);
}

static void embk_txn_push(uint64_t *arr, uint32_t *n, bool *overflow, uint64_t blk)
{
    if (*n < EMBK_TXN_MAX) arr[(*n)++] = blk;
    else                   *overflow = true;
}

/* Record a contiguous run rather than each of its blocks — this is what keeps a
 * many-block file from overflowing the per-block lists. */
static void embk_txn_push_run(struct embk_run **arr, uint32_t *n, uint32_t *cap,
                              bool *overflow, uint64_t start, uint64_t len)
{
    if (len == 0) return;
    if (*overflow) return;
    if (*n >= *cap) {
        uint32_t new_cap = (*cap == 0) ? EMBK_TXN_RUNS : (*cap * 2);
        struct embk_run *grown = krealloc(*arr, (uint64_t)new_cap * sizeof(struct embk_run));
        if (!grown) { *overflow = true; return; }
        *arr = grown;
        *cap = new_cap;
    }
    (*arr)[*n].start = start;
    (*arr)[*n].len   = len;
    (*n)++;
}

/* A superseded node block (old metadata): released once the commit is durable. */
static void embkfs_note_freed(struct embkfs_volume *vol, uint64_t blk)
{
    if (vol->txn) embk_txn_push(vol->txn->freed, &vol->txn->freed_n, &vol->txn->overflow, blk);
}

/* A superseded data RUN (an old extent's blocks): released once durable. */
static void embkfs_note_freed_run(struct embkfs_volume *vol, uint64_t start, uint64_t len)
{
    if (vol->txn) embk_txn_push_run(&vol->txn->frun, &vol->txn->frun_n, &vol->txn->frun_cap,
                                    &vol->txn->overflow, start, len);
}

static int embkfs_txn_begin(struct embkfs_volume *vol, struct embk_txn *t)
{
    if (!vol || !t) return -EMBK_EINVAL;
    if (vol->txn) return -EMBK_EBUSY;

    t->alloc_n = 0; t->freed_n = 0;
    t->arun_n  = 0; t->frun_n  = 0;
    t->arun_cap = EMBK_TXN_RUNS;
    t->frun_cap = EMBK_TXN_RUNS;
    t->arun = NULL;
    t->frun = NULL;
    t->arun = kmalloc((uint64_t)t->arun_cap * sizeof *t->arun);
    t->frun = kmalloc((uint64_t)t->frun_cap * sizeof *t->frun);
    t->overflow = (!t->arun || !t->frun);
    if (t->overflow) {
        kfree(t->arun);
        kfree(t->frun);
        t->arun = NULL;
        t->frun = NULL;
        t->arun_cap = t->frun_cap = 0;
        return -EMBK_ENOMEM;
    }
    vol->txn = t;
    return EMBK_OK;
}

/* Close a txn and reconcile the bitmap. Commit -> release superseded (nodes and
 * data runs); failure -> release the just-allocated (roll back). Overflow ->
 * rebuild exactly from the live tree. */
static void embkfs_txn_end(struct embkfs_volume *vol, struct embk_txn *t, bool committed)
{
    vol->txn = NULL;                                  /* stop recording first */

    if (t->overflow) {                                /* rare: giant txn outgrew the lists */
        embkfs_bitmap_build(vol);                     /* exact recompute from vol->root */
        embkfs_free_index_rebuild(vol);

        /* Even in overflow fallback, release per-txn heap state. */
        kfree(t->arun);
        kfree(t->frun);
        t->arun = NULL; t->frun = NULL;
        t->arun_n = t->frun_n = 0;
        t->arun_cap = t->frun_cap = 0;
        return;
    }

    bool free_err = false;

    /* SNAPSHOT-AWARE HOLD-BACK (v2.2 Phase 5b): t->freed/t->frun (the
     * committed=true case) are blocks the OLD tree referenced that the
     * NEW tree doesn't -- exactly the blocks a snapshot's frozen root
     * might still need, since a snapshot IS a pointer into that old
     * structure. While any snapshot is retained, leave them marked used
     * instead of reclaiming them (see the snapshot section's comment for
     * why this is a conservative policy, not exact refcounting).
     * t->alloc/t->arun (the committed=false rollback case) are always
     * safe to reclaim immediately regardless -- they were never part of
     * any committed tree a snapshot could possibly reference. */
    bool should_reclaim = !committed || vol->snapshot_count == 0;

    /* per-block node lists */
    const uint64_t *blk = committed ? t->freed   : t->alloc;
    uint32_t        bn  = committed ? t->freed_n : t->alloc_n;
    if (should_reclaim) {
        for (uint32_t i = 0; i < bn; i++) {
            int rc = embkfs_free_run(vol, blk[i], 1);
            if (rc != EMBK_OK) free_err = true;
        }
    }

    /* data-run lists */
    const struct embk_run *run = committed ? t->frun   : t->arun;
    uint32_t               rn  = committed ? t->frun_n : t->arun_n;
    if (should_reclaim) {
        for (uint32_t i = 0; i < rn; i++) {
            int rc = embkfs_free_run(vol, run[i].start, run[i].len);
            if (rc != EMBK_OK) free_err = true;
        }
    }

    if (free_err) {
        kprintf("EMBKFS: %s: allocator reconciliation inconsistency, rebuilding free index\n",
                vol->dev->name);
        embkfs_bitmap_build(vol);
        embkfs_free_index_rebuild(vol);
    }

    kfree(t->arun);
    kfree(t->frun);
    t->arun = NULL; t->frun = NULL;
    t->arun_n = t->frun_n = 0;
    t->arun_cap = t->frun_cap = 0;
}

/* Free-block count the new tree will present at next mount, from the txn's net
 * block delta. Now sums data-run blocks alongside the per-block node counts, so
 * the formula stays general across data writes, metadata rewrites, and splits. */
static int embkfs_txn_new_free(struct embkfs_volume *vol, uint64_t *new_free)
{
    struct embk_txn *t = vol->txn;
    if (t->overflow) { kprintf("EMBKFS: %s: transaction too large to track\n", vol->dev->name); return -EMBK_ENOSPC; }

    uint64_t allocated = t->alloc_n, freed = t->freed_n;
    for (uint32_t i = 0; i < t->arun_n; i++) allocated += t->arun[i].len;
    for (uint32_t i = 0; i < t->frun_n; i++) freed     += t->frun[i].len;

    /* Snapshot hold-back (v2.2 Phase 5b): embkfs_txn_end() won't actually
     * reclaim freed blocks while any snapshot is retained, so they must
     * not be counted as newly-free here either -- otherwise the
     * superblock's free_blocks would overstate what the bitmap actually
     * has free (this is the same conservative policy, applied to the
     * count instead of the bitmap). */
    if (vol->snapshot_count > 0) freed = 0;

    int64_t net = (int64_t)allocated - (int64_t)freed;     /* >0 grew, <0 shrank */
    *new_free = (uint64_t)((int64_t)vol->free_blocks - net);
    return EMBK_OK;
}

/* embkfs_alloc_block stays exactly as it is. Add the run allocator right after it. */
static int embkfs_alloc_run(struct embkfs_volume *vol, uint64_t want,
                            uint64_t *out_start, uint64_t *out_got);

/* Find a free block, mark it used, return it. Errs toward B by construction:
 * only ever returns a block the bitmap proves free, and marks it used at once
 * so it cannot be handed out twice. (Linear scan — same O(n) as the PMM for
 * now; a free list is a later optimization.) During a transaction the block is
 * recorded so it can be rolled back if the commit fails. */
static int embkfs_alloc_block(struct embkfs_volume *vol, uint64_t *out_block)
{
    uint64_t got = 0;
    int rc = embkfs_alloc_run(vol, 1, out_block, &got); // Allocate a single block
    if (rc != EMBK_OK) return rc;
    if (got != 1) return -EMBK_EINVAL;
    return EMBK_OK;
}

/* Find a contiguous run of free blocks, up to `want`, mark it used, return its
 * start and the length actually found (1..want). Call repeatedly to cover a
 * file across a fragmented bitmap. Same O(n) linear scan as embkfs_alloc_block;
 * a free-list / best-fit search is a later optimization. During a txn the run is
 * recorded for rollback. */
static int embkfs_alloc_run(struct embkfs_volume *vol, uint64_t want,
                            uint64_t *out_start, uint64_t *out_got)
{
    if (want == 0) return -EMBK_EINVAL;
    if (!out_start || !out_got) return -EMBK_EINVAL;

    for (uint32_t i = 0; i < vol->free_ext_n; i++) {
        if (vol->free_ext[i].len == 0) continue;
        uint64_t start = vol->free_ext[i].start;
        uint64_t got = vol->free_ext[i].len;
        if (got > want) got = want;

        for (uint64_t b = 0; b < got; b++) {
            if (embk_bm_test(vol->block_bitmap, start + b)) {
                kprintf("EMBKFS: %s: free index mismatch at block %lu, rebuilding\n",
                        vol->dev->name, start + b);
                int rrc = embkfs_free_index_rebuild(vol);
                if (rrc != EMBK_OK) return rrc;
                return embkfs_alloc_run(vol, want, out_start, out_got);
            }
            embk_bm_set(vol->block_bitmap, start + b);
        }

        vol->free_ext[i].start += got;
        vol->free_ext[i].len -= got;
        if (vol->free_ext[i].len == 0) {
            for (uint32_t k = i; k + 1 < vol->free_ext_n; k++)
                vol->free_ext[k] = vol->free_ext[k + 1];
            vol->free_ext_n--;
        }

        if (vol->txn)
            embk_txn_push_run(&vol->txn->arun, &vol->txn->arun_n, &vol->txn->arun_cap,
                              &vol->txn->overflow, start, got);
        *out_start = start;
        *out_got = got;
        return EMBK_OK;
    }

    return -EMBK_ENOSPC;
}

/* Field-wise key comparison (spec §7): object_id, then type, then offset.
 * Compares the u64 *values*, never the raw bytes — see why under "key
 * ordering" below. Returns <0, 0, >0 like memcmp's contract (but correct). */
static int embk_key_cmp(const struct embk_key *a, const struct embk_key *b)
{
    if (a->object_id != b->object_id)
        return (a->object_id < b->object_id) ? -1 : 1;
    if (a->type != b->type)
        return (a->type < b->type) ? -1 : 1;
    if (a->offset != b->offset)
        return (a->offset < b->offset) ? -1 : 1;
    return 0;
}

static const char *embk_type_name(uint64_t type)
{
    switch (type) {
        case EMBK_TYPE_INODE:     return "INODE";
        case EMBK_TYPE_DIR_ENTRY: return "DIR_ENTRY";
        case EMBK_TYPE_EXTENT:    return "EXTENT";
        case EMBK_TYPE_XATTR:     return "XATTR";
        default:                  return "?";
    }
}


/*
 * Walk a verified leaf: parse its nritems item headers, enforce the
 * strictly-increasing key invariant, and print each item's key and data
 * location. Does NOT decode item bodies (inode/dir-entry/extent) — that's
 * steps 6-7. Mirrors verify_embkfs.py §3.
 */
static int embkfs_leaf_dump(struct embkfs_volume *vol, const uint8_t *buf)
{
    const struct embk_node_header *h = (const struct embk_node_header *)buf;
    const char *name = vol->dev->name;
    /* The header array must fit in the block. (The node checksum already makes
     * nritems trustworthy; this is cheap defense-in-depth against a logic bug.) */
    if (sizeof(struct embk_node_header)
        + (uint64_t)h->nritems * sizeof(struct embk_item_header) > vol->block_size) {
        kprintf("EMBKFS: %s: nritems %u too large for block\n",
                name, (unsigned int)h->nritems);
        return -EMBK_EINVAL;
    }

    struct embk_key prev;
    bool have_prev = false;

    for (uint32_t i = 0; i < h->nritems; i++) {
        const struct embk_item_header *it = embk_leaf_item(buf, i);

        /* Leaf array is kept sorted: each key strictly greater than the last.
         * FIELD-WISE compare — a memcmp of the LE key bytes would be wrong. */
        if (have_prev && embk_key_cmp(&it->key, &prev) <= 0) {
            kprintf("EMBKFS: %s: leaf items out of order at index %u\n", name, i);
            return -EMBK_EINVAL;
        }
        prev = it->key;
        have_prev = true;

        /* Data lives at [offset .. offset+size) from the START of the block;
         * bounds-check before anyone trusts those bytes. */
        if ((uint64_t)it->offset + it->size > vol->block_size) {
            kprintf("EMBKFS: %s: item %u data out of bounds (off %u size %u)\n",
                    name, i, (unsigned int)it->offset, (unsigned int)it->size);
            return -EMBK_EINVAL;
        }

        kprintf("EMBKFS: %s:   [%u] {obj=%lu, type=%s, off=0x%08X}  data@%u size=%u\n",
                name, (unsigned int)i,
                it->key.object_id, embk_type_name(it->key.type),
                (unsigned int)it->key.offset,
                (unsigned int)it->offset, (unsigned int)it->size);
    }

    kprintf("EMBKFS: %s: leaf walk OK (%u items, key order verified)\n",
            name, (unsigned int)h->nritems);
    return EMBK_OK;
}


/* Find an item in a leaf by exact key {object_id, type, offset}. The leaf is
 * key-sorted, so a real implementation binary-searches; linear is fine for a
 * handful of items. Reads key fields by value — no pointer to a packed member. */
static const struct embk_item_header *
embk_leaf_find(const uint8_t *buf, uint64_t object_id, uint64_t type, uint64_t offset)
{
    const struct embk_node_header *h = (const struct embk_node_header *)buf;
    for (uint32_t i = 0; i < h->nritems; i++) {
        const struct embk_item_header *it = embk_leaf_item(buf, i);
        if (it->key.object_id == object_id &&
            it->key.type      == type &&
            it->key.offset    == offset) {
            return it;
        }
    }
    return NULL;
}


/* Descend from the root to the leaf that should hold `target`, leaving it in
 * `nodebuf`. embkfs_read_node verifies every node against its parent pointer on
 * the way down, so the Merkle chain extends to the full tree depth for free. */
static int embkfs_descend_to_leaf(struct embkfs_volume *vol,
                                  const struct embk_key *target,
                                  uint8_t *nodebuf, size_t buf_size)
{
    struct embk_block_ptr ptr = vol->root;          /* start at the root */
    for (;;) {
        int rc = embkfs_read_node(vol, &ptr, nodebuf, buf_size);
        if (rc != EMBK_OK)
            return rc;

        const struct embk_node_header *h = (const struct embk_node_header *)nodebuf;
        if (h->level == 0)
            return EMBK_OK;                          /* reached the leaf */

        /* Internal node: a contiguous array of {key, ptr} slots after the
         * header. Choose the RIGHTMOST slot whose key is <= target — an
         * upper-bound search, so an exact boundary key descends RIGHT. */
        const struct embk_internal_slot *slots =
            (const struct embk_internal_slot *)(nodebuf + sizeof(struct embk_node_header));
        struct embk_block_ptr child;
        bool have_child = false;
        for (uint32_t i = 0; i < h->nritems; i++) {
            /* memcpy into aligned locals — the clean way to read a packed
             * member without an unaligned-pointer (the step-1 heads-up). */
            struct embk_key slot_key;
            memcpy(&slot_key, &slots[i].key, sizeof slot_key);
            if (embk_key_cmp(&slot_key, target) <= 0) {
                memcpy(&child, &slots[i].ptr, sizeof child);
                have_child = true;
            } else {
                break;                               /* sorted: no later slot qualifies */
            }
        }
        if (!have_child) {                            /* target < slot[0].key */
            kprintf("EMBKFS: %s: key below first slot during descent\n", vol->dev->name);
            return -EMBK_ENOENT;
        }
        ptr = child;                                  /* descend one level */
    }
}

/* Descend for `{oid,type,offset}`, then scan the reached leaf. Returns the item
 * header (pointing into nodebuf), or NULL. Drop-in replacement for the old
 * embk_leaf_find(single_leaf, ...) — it just finds the leaf first. */
static const struct embk_item_header *
embkfs_find_item(struct embkfs_volume *vol, uint64_t oid, uint64_t type,
                 uint64_t offset, uint8_t *nodebuf, size_t buf_size)
{
    struct embk_key target = { .object_id = oid, .type = type, .offset = offset };
    if (embkfs_descend_to_leaf(vol, &target, nodebuf, buf_size) != EMBK_OK)
        return NULL;
    return embk_leaf_find(nodebuf, oid, type, offset);
}

/* ---- Snapshots (v2.2 Phase 5b) -------------------------------------
 * Slots are a small fixed linear range (0..EMBKFS_MAX_SNAPSHOTS-1), not a
 * name hash like directory entries -- with only 16 possible slots, a
 * direct per-slot embkfs_find_item() probe is simpler and just as fast
 * as maintaining a separate free-list, and needs no collision-chain
 * handling at all. */

static void embk_snapshot_name_pad(const char *name, uint8_t out[32]) {
    memset(out, 0, 32);
    size_t n = strlen(name);
    if (n > EMBKFS_SNAPSHOT_NAME_MAX) n = EMBKFS_SNAPSHOT_NAME_MAX;
    memcpy(out, name, n);
}

/* ---- The registry, v2.3: one fixed block outside the CoW tree -------------
 *
 * Why a block and not superblock-adjacent bytes: the crypto (offset 200) and
 * verify (260) headers share the superblock's 512-byte sector, but 16 slots x
 * 80 bytes is 1280 -- it does not fit, and shrinking the entry to make it fit
 * would have cost either the name length or the snapshot count. A fixed block
 * costs one block out of the 15 the formatter already leaves unused before the
 * superblock, and nothing else.
 *
 * These two helpers are the whole seam between layouts. Everything above them
 * works on an in-memory array of slots and does not know or care where it came
 * from, which is what keeps the legacy in-tree path alive without duplicating
 * create/delete/list/rollback. */
static bool embk_snapreg_enabled(const struct embkfs_volume *vol)
{
    return (vol->feature_incompat & EMBKFS_INCOMPAT_SNAPREG) != 0;
}

/* Read the registry block into `out` (EMBKFS_MAX_SNAPSHOTS slots), returning
 * the live count. A registry that fails its magic or checksum is reported as
 * EMPTY rather than guessed at: a bogus root pointer would send
 * embkfs_bitmap_build() marking arbitrary blocks in use, which is a worse
 * outcome than losing the list of snapshots. Loud, because silently having no
 * snapshots is exactly the kind of thing that should not pass unremarked. */
static int embkfs_snapreg_load(struct embkfs_volume *vol,
                               struct embk_snapshot_item *out, uint32_t *out_n)
{
    static uint8_t blk[4096];
    uint64_t spb = vol->block_size / vol->dev->block_size;
    if (vol->block_size > sizeof blk) return -EMBK_EINVAL;

    int rc = embk_block_read(vol->dev, EMBKFS_SNAPREG_BLOCK * spb, spb, blk);
    if (rc != EMBK_OK) return rc;

    const struct embk_snapshot_registry *hdr = (const struct embk_snapshot_registry *)blk;
    uint64_t body = sizeof *hdr + (uint64_t)EMBKFS_MAX_SNAPSHOTS * sizeof(struct embk_snapshot_item);
    if (body > vol->block_size) return -EMBK_EINVAL;

    if (hdr->magic != EMBKFS_SNAPREG_MAGIC) {
        kprintf("EMBKFS: %s: snapshot registry block %lu has bad magic -- "
                "treating as empty\n", vol->dev->name, EMBKFS_SNAPREG_BLOCK);
        *out_n = 0;
        return EMBK_OK;
    }
    uint32_t want = embk_crc32c(blk + 8, (uint32_t)(body - 8), 0);
    if ((uint32_t)hdr->checksum != want) {
        kprintf("EMBKFS: %s: snapshot registry checksum 0x%x != 0x%x -- "
                "treating as empty (snapshots lost, live tree unaffected)\n",
                vol->dev->name, (unsigned)hdr->checksum, (unsigned)want);
        *out_n = 0;
        return EMBK_OK;
    }
    uint32_t n = hdr->count;
    if (n > EMBKFS_MAX_SNAPSHOTS) n = EMBKFS_MAX_SNAPSHOTS;   /* never trust a count */
    const struct embk_snapshot_item *slots =
        (const struct embk_snapshot_item *)(blk + sizeof *hdr);
    for (uint32_t i = 0; i < n; i++) out[i] = slots[i];
    *out_n = n;
    return EMBK_OK;
}

/* Write `n` slots back. One block write, no transaction: the registry is not
 * part of the tree, so it has no CoW machinery to ride and needs none -- it is
 * a single sector-aligned block whose checksum makes a torn write detectable.
 * The failure mode that matters (power loss mid-write) yields a checksum
 * mismatch, which load() reports as empty rather than as garbage. */
static int embkfs_snapreg_store(struct embkfs_volume *vol,
                                const struct embk_snapshot_item *slots, uint32_t n)
{
    static uint8_t blk[4096];
    uint64_t spb = vol->block_size / vol->dev->block_size;
    if (vol->block_size > sizeof blk) return -EMBK_EINVAL;
    if (n > EMBKFS_MAX_SNAPSHOTS) return -EMBK_EINVAL;

    memset(blk, 0, vol->block_size);
    struct embk_snapshot_registry *hdr = (struct embk_snapshot_registry *)blk;
    hdr->magic = EMBKFS_SNAPREG_MAGIC;
    hdr->count = n;
    hdr->reserved = 0;
    struct embk_snapshot_item *dst = (struct embk_snapshot_item *)(blk + sizeof *hdr);
    for (uint32_t i = 0; i < n; i++) dst[i] = slots[i];

    uint64_t body = sizeof *hdr + (uint64_t)EMBKFS_MAX_SNAPSHOTS * sizeof(struct embk_snapshot_item);
    hdr->checksum = embk_crc32c(blk + 8, (uint32_t)(body - 8), 0);

    return embk_block_write(vol->dev, EMBKFS_SNAPREG_BLOCK * spb, spb, blk);
}

/* Collects every currently-registered snapshot. out_items/max may be
 * NULL/0 to just get a count. Used both by the public embkfs_snapshot_list()
 * and internally by embkfs_bitmap_build() to protect snapshot-only blocks. */
static int embkfs_snapshot_list_internal(struct embkfs_volume *vol,
                                         struct embk_snapshot_item *out_items,
                                         uint32_t max, uint32_t *out_n)
{
    if (embk_snapreg_enabled(vol)) {
        struct embk_snapshot_item slots[EMBKFS_MAX_SNAPSHOTS];
        uint32_t n = 0;
        int rc = embkfs_snapreg_load(vol, slots, &n);
        if (rc != EMBK_OK) return rc;
        for (uint32_t i = 0; out_items && i < n && i < max; i++) out_items[i] = slots[i];
        if (out_n) *out_n = n;
        return EMBK_OK;
    }

    /* legacy: items inside the versioned tree (pre-v2.3 volumes) */
    static uint8_t buf[4096];
    uint32_t n = 0;
    for (uint32_t slot = 0; slot < EMBKFS_MAX_SNAPSHOTS; slot++) {
        const struct embk_item_header *it =
            embkfs_find_item(vol, EMBKFS_ROOT_OBJECT_ID, EMBK_TYPE_SNAPSHOT, slot, buf, sizeof buf);
        if (!it) continue;
        const struct embk_snapshot_item *si = embk_item_data(buf, vol->block_size, it, sizeof *si);
        if (!si) continue;
        if (out_items && n < max) out_items[n] = *si;
        n++;
    }
    if (out_n) *out_n = n;
    return EMBK_OK;
}

static int embkfs_snapshot_find_slot(struct embkfs_volume *vol, const char *name,
                                     uint32_t *out_slot, struct embk_snapshot_item *out_item)
{
    uint8_t padded[32];
    embk_snapshot_name_pad(name, padded);

    if (embk_snapreg_enabled(vol)) {
        struct embk_snapshot_item slots[EMBKFS_MAX_SNAPSHOTS];
        uint32_t n = 0;
        int rc = embkfs_snapreg_load(vol, slots, &n);
        if (rc != EMBK_OK) return rc;
        for (uint32_t i = 0; i < n; i++) {
            if (memcmp(slots[i].name, padded, sizeof padded) == 0) {
                if (out_slot) *out_slot = i;
                if (out_item) *out_item = slots[i];
                return EMBK_OK;
            }
        }
        return -EMBK_ENOENT;
    }

    static uint8_t buf[4096];
    for (uint32_t slot = 0; slot < EMBKFS_MAX_SNAPSHOTS; slot++) {
        const struct embk_item_header *it =
            embkfs_find_item(vol, EMBKFS_ROOT_OBJECT_ID, EMBK_TYPE_SNAPSHOT, slot, buf, sizeof buf);
        if (!it) continue;
        const struct embk_snapshot_item *si = embk_item_data(buf, vol->block_size, it, sizeof *si);
        if (!si) continue;
        if (memcmp(si->name, padded, sizeof padded) == 0) {
            if (out_slot) *out_slot = slot;
            if (out_item) *out_item = *si;
            return EMBK_OK;
        }
    }
    return -EMBK_ENOENT;
}

static int embkfs_snapshot_find_free_slot(struct embkfs_volume *vol, uint32_t *out_slot)
{
    static uint8_t buf[4096];
    for (uint32_t slot = 0; slot < EMBKFS_MAX_SNAPSHOTS; slot++) {
        if (!embkfs_find_item(vol, EMBKFS_ROOT_OBJECT_ID, EMBK_TYPE_SNAPSHOT, slot, buf, sizeof buf)) {
            *out_slot = slot;
            return EMBK_OK;
        }
    }
    return -EMBK_ENOSPC;
}

/*
 * Resolve one name inside directory `dir_oid` to its target object id, against
 * a verified leaf. Mirrors verify_embkfs.py §4a-4b:
 *   (a) the directory's own inode exists and is actually a directory
 *   (b) find the entry whose key offset == CRC32C(name)
 *   (c) confirm the STORED name — a 32-bit hash match is only a candidate
 */
static int embkfs_lookup(struct embkfs_volume *vol, uint64_t dir_oid,
                         const char *name, uint64_t *out_oid)
{
    const char *dev = vol->dev->name;
    size_t name_len = strlen(name);
    static uint8_t buf[4096];                  /* node buffer, reused across descents */

    /* (a) directory inode — descend for {dir_oid, INODE, 0} */
    const struct embk_item_header *di =
        embkfs_find_item(vol, dir_oid, EMBK_TYPE_INODE, 0, buf, sizeof buf);
    if (!di) { kprintf("EMBKFS: %s: dir object %lu has no inode\n", dev, dir_oid); return -EMBK_ENOENT; }
    const struct embk_inode_item *dino = embk_item_data(buf, vol->block_size, di, sizeof *dino);
    if (!dino) { kprintf("EMBKFS: %s: object %lu inode truncated\n", dev, dir_oid); return -EMBK_EINVAL; }
    if ((dino->mode & EMBKFS_S_IFMT) != EMBKFS_S_IFDIR) {
        kprintf("EMBKFS: %s: object %lu is not a directory\n", dev, dir_oid); return -EMBK_ENOTDIR;
    }
    /* dino consumed (mode checked) before the next descent reuses buf */

    /* (b) dir entry — descend for {dir_oid, DIR_ENTRY, hash(name)} */
    uint32_t hash = embk_crc32c(name, name_len, 0);
    const struct embk_item_header *de =
        embkfs_find_item(vol, dir_oid, EMBK_TYPE_DIR_ENTRY, hash, buf, sizeof buf);
    if (!de) { kprintf("EMBKFS: %s: \"%s\" (hash 0x%08X) not found\n", dev, name, hash); return -EMBK_ENOENT; }
    const struct embk_dir_entry_item *dent = embk_item_data(buf, vol->block_size, de, sizeof *dent);
    if (!dent) { kprintf("EMBKFS: %s: dir entry truncated\n", dev); return -EMBK_EINVAL; }

    /* (c) walk the collision chain, authoritative name compare — UNCHANGED */
    const uint8_t *p = (const uint8_t *)dent;
    uint32_t remaining = de->size;
    while (remaining >= sizeof(struct embk_dir_entry_item)) {
        const struct embk_dir_entry_item *rec = (const struct embk_dir_entry_item *)p;
        uint32_t rec_len = sizeof *rec + rec->name_len;
        if (rec_len > remaining) { kprintf("EMBKFS: %s: record runs past item\n", dev); return -EMBK_EINVAL; }
        if (rec->name_len == name_len &&
            memcmp((const char *)rec + sizeof *rec, name, name_len) == 0) {
            *out_oid = rec->target_object_id;
            return EMBK_OK;
        }
        p += rec_len; remaining -= rec_len;
    }
    return -EMBK_ENOENT;
}

/* ===========================================================================
 * Multi-extent file support: enumerate every extent of an object
 * =========================================================================== */

/* One extent, decoded out of the tree (the fields a reader/rewriter needs). */
struct embk_extref {
    uint64_t offset;          /* key offset = file byte position of this run */
    uint64_t disk_block;      /* first disk block of the run                */
    uint64_t length;          /* run length in blocks                       */
    uint64_t logical_size;    /* valid bytes in this extent                 */
    uint32_t checksum;        /* CRC32C over this extent's on-disk bytes    */
    uint32_t flags;           /* EMBKFS_EXTENT_F_*                          */
    uint64_t compressed_size; /* valid only when F_COMPRESSED is set (v2.2) */
};

static int embkfs_collect_extents_rec(struct embkfs_volume *vol, const struct embk_block_ptr *ptr,
                                      uint64_t oid, struct embk_extref *out, uint32_t max,
                                      uint32_t *n, bool *overflow)
{
    uint8_t *buf = kmalloc(vol->block_size);
    if (!buf) return -EMBK_ENOMEM;
    int rc = embkfs_read_node(vol, ptr, buf, vol->block_size);     /* verifies */
    if (rc != EMBK_OK) { kfree(buf); return rc; }

    const struct embk_node_header *h = (const struct embk_node_header *)buf;
    const struct embk_key lo = { .object_id = oid, .type = EMBK_TYPE_EXTENT, .offset = 0 };
    const struct embk_key hi = { .object_id = oid, .type = EMBK_TYPE_EXTENT, .offset = UINT64_MAX };

    if (h->level == 0) {
        for (uint32_t i = 0; i < h->nritems; i++) {
            const struct embk_item_header *it = embk_leaf_item(buf, i);
            if (it->key.object_id != oid || it->key.type != EMBK_TYPE_EXTENT) continue;
            const struct embk_extent_item *ext = embk_item_data(buf, vol->block_size, it, sizeof *ext);
            if (!ext) { kfree(buf); return -EMBK_EINVAL; }
            rc = embkfs_extent_validate(vol, ext, it->key.offset, "extent collect");
            if (rc != EMBK_OK) { kfree(buf); return rc; }
            if (*n >= max) { *overflow = true; kfree(buf); return EMBK_OK; }
            out[*n].offset          = it->key.offset;
            out[*n].disk_block      = ext->disk_block;
            out[*n].length          = ext->length;
            out[*n].logical_size    = ext->logical_size;
            out[*n].checksum        = (uint32_t)ext->checksum;
            out[*n].flags           = ext->flags;
            out[*n].compressed_size = embk_extent_compressed_size(ext);
            (*n)++;
        }
        kfree(buf);
        return EMBK_OK;
    }

    if (h->nritems == 0 || h->nritems > EMBKFS_MAX_SLOTS) { kfree(buf); return -EMBK_EINVAL; }
    const struct embk_internal_slot *slots =
        (const struct embk_internal_slot *)(buf + sizeof(struct embk_node_header));
    for (uint32_t i = 0; i < h->nritems; i++) {
        struct embk_key clo;                          /* child i covers [clo, chi) */
        memcpy(&clo, &slots[i].key, sizeof clo);
        if (embk_key_cmp(&clo, &hi) > 0) break;        /* this child (and all later) start past prefix */
        if (i + 1 < h->nritems) {                      /* skip a child ending at/before prefix */
            struct embk_key chi;
            memcpy(&chi, &slots[i + 1].key, sizeof chi);
            if (embk_key_cmp(&chi, &lo) <= 0) continue;
        }
        struct embk_block_ptr cp;
        memcpy(&cp, &slots[i].ptr, sizeof cp);
        rc = embkfs_collect_extents_rec(vol, &cp, oid, out, max, n, overflow);
        if (rc != EMBK_OK) { kfree(buf); return rc; }
    }
    kfree(buf);
    return EMBK_OK;
}

/* Count extents of `oid` with the same pruned walk and validation as collect. */
static int embkfs_count_extents_rec(struct embkfs_volume *vol, const struct embk_block_ptr *ptr,
                                    uint64_t oid, uint32_t *n)
{
    uint8_t *buf = kmalloc(vol->block_size);
    if (!buf) return -EMBK_ENOMEM;
    int rc = embkfs_read_node(vol, ptr, buf, vol->block_size);
    if (rc != EMBK_OK) { kfree(buf); return rc; }

    const struct embk_node_header *h = (const struct embk_node_header *)buf;
    const struct embk_key lo = { .object_id = oid, .type = EMBK_TYPE_EXTENT, .offset = 0 };
    const struct embk_key hi = { .object_id = oid, .type = EMBK_TYPE_EXTENT, .offset = UINT64_MAX };

    if (h->level == 0) {
        for (uint32_t i = 0; i < h->nritems; i++) {
            const struct embk_item_header *it = embk_leaf_item(buf, i);
            if (it->key.object_id != oid || it->key.type != EMBK_TYPE_EXTENT) continue;
            const struct embk_extent_item *ext = embk_item_data(buf, vol->block_size, it, sizeof *ext);
            if (!ext) { kfree(buf); return -EMBK_EINVAL; }
            rc = embkfs_extent_validate(vol, ext, it->key.offset, "extent count");
            if (rc != EMBK_OK) { kfree(buf); return rc; }
            if (*n == UINT32_MAX) { kfree(buf); return -EMBK_EINVAL; }
            (*n)++;
        }
        kfree(buf);
        return EMBK_OK;
    }

    if (h->nritems == 0 || h->nritems > EMBKFS_MAX_SLOTS) { kfree(buf); return -EMBK_EINVAL; }
    const struct embk_internal_slot *slots =
        (const struct embk_internal_slot *)(buf + sizeof(struct embk_node_header));
    for (uint32_t i = 0; i < h->nritems; i++) {
        struct embk_key clo;
        memcpy(&clo, &slots[i].key, sizeof clo);
        if (embk_key_cmp(&clo, &hi) > 0) break;
        if (i + 1 < h->nritems) {
            struct embk_key chi;
            memcpy(&chi, &slots[i + 1].key, sizeof chi);
            if (embk_key_cmp(&chi, &lo) <= 0) continue;
        }
        struct embk_block_ptr cp;
        memcpy(&cp, &slots[i].ptr, sizeof cp);
        rc = embkfs_count_extents_rec(vol, &cp, oid, n);
        if (rc != EMBK_OK) { kfree(buf); return rc; }
    }
    kfree(buf);
    return EMBK_OK;
}

static int embkfs_count_extents(struct embkfs_volume *vol, uint64_t oid, uint32_t *n)
{
    *n = 0;
    return embkfs_count_extents_rec(vol, &vol->root, oid, n);
}

/* Collect up to `max` extents of `oid` into `out`, in ascending file offset.
 * Sets *overflow if the object has more than `max` extents. */
static int embkfs_collect_extents(struct embkfs_volume *vol, uint64_t oid,
                                  struct embk_extref *out, uint32_t max,
                                  uint32_t *n, bool *overflow)
{
    *n = 0; *overflow = false;
    return embkfs_collect_extents_rec(vol, &vol->root, oid, out, max, n, overflow);
}

/* Validate extent ordering/coverage invariants over an extent map. */
static int embkfs_validate_extent_map(struct embkfs_volume *vol, const struct embk_extref *ext,
                                      uint32_t en, uint64_t inode_size, const char *where)
{
    uint64_t expect = 0;
    for (uint32_t i = 0; i < en; i++) {
        if (ext[i].offset != expect) {
            kprintf("EMBKFS: %s: %s: extent map discontinuity at %u (off %lu expect %lu)\n",
                    vol->dev->name, where, i, ext[i].offset, expect);
            return -EMBK_EINVAL;
        }
        if (ext[i].offset > UINT64_MAX - ext[i].logical_size) {
            kprintf("EMBKFS: %s: %s: extent %u offset/logical overflow\n",
                    vol->dev->name, where, i);
            return -EMBK_EINVAL;
        }
        expect = ext[i].offset + ext[i].logical_size;
    }
    if (expect != inode_size) {
        kprintf("EMBKFS: %s: %s: extent bytes %lu != inode size %lu\n",
                vol->dev->name, where, expect, inode_size);
        return -EMBK_EINVAL;
    }
    return EMBK_OK;
}

static int embkfs_dump_file(struct embkfs_volume *vol, uint64_t oid, const char *label)
{
    const char *dev = vol->dev->name;
    static uint8_t probe[4096];
    static uint8_t datablk[4096];

    const struct embk_item_header *fi = embkfs_find_item(vol, oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!fi) { kprintf("EMBKFS: %s: object %lu has no inode\n", dev, oid); return -EMBK_ENOENT; }
    const struct embk_inode_item *ino = embk_item_data(probe, vol->block_size, fi, sizeof *ino);
    if (!ino) { kprintf("EMBKFS: %s: object %lu inode truncated\n", dev, oid); return -EMBK_EINVAL; }
    if ((ino->mode & EMBKFS_S_IFMT) != EMBKFS_S_IFREG) {
        kprintf("EMBKFS: %s: object %lu is not a regular file\n", dev, oid); return -EMBK_EINVAL;
    }
    uint64_t fsize = ino->size;                /* SAVE before probe reuse */
    kprintf("EMBKFS: %s: object %lu inode: size %lu (regular file)\n", dev, oid, fsize);

    if (fsize == 0) {
        kprintf("EMBKFS: %s: ----- /%s ----- (empty)\n", dev, label);
        kprintf("EMBKFS: %s: ----------------------\n", dev);
        return EMBK_OK;
    }

    uint32_t en = 0;
    int rc = embkfs_count_extents(vol, oid, &en);
    if (rc != EMBK_OK) return rc;
    if (en == 0) { kprintf("EMBKFS: %s: object %lu size %lu but no extents\n", dev, oid, fsize); return -EMBK_EINVAL; }

    struct embk_extref *ext = kmalloc((uint64_t)en * sizeof *ext);
    if (!ext) return -EMBK_ENOMEM;
    uint32_t got = 0; bool eover = false;
    rc = embkfs_collect_extents(vol, oid, ext, en, &got, &eover);
    if (rc != EMBK_OK) { kfree(ext); return rc; }
    if (eover || got != en) { kfree(ext); return -EMBK_EINVAL; }
    rc = embkfs_validate_extent_map(vol, ext, en, fsize, "dump_file");
    if (rc != EMBK_OK) { kfree(ext); return rc; }

    uint64_t spb = vol->block_size / vol->dev->block_size;
    kprintf("EMBKFS: %s: ----- /%s ----- (%lu bytes, %u extent%s)\n",
            dev, label, fsize, en, en == 1 ? "" : "s");

    uint64_t expect_off = 0;
    for (uint32_t i = 0; i < en; i++) {
        if (ext[i].offset != expect_off) {             /* contiguity: extents must tile the file */
            kprintf("\nEMBKFS: %s: extent gap (have offset %lu, expected %lu)\n", dev, ext[i].offset, expect_off);
            kfree(ext);
            return -EMBK_EINVAL;
        }
        if (ext[i].flags & EMBKFS_EXTENT_F_HOLE) {
            for (uint64_t k = 0; k < ext[i].logical_size; k++) kprintf("%c", '\0');
            expect_off += ext[i].logical_size;
            continue;
        }
        if (ext[i].flags & (EMBKFS_EXTENT_F_COMPRESSED | EMBKFS_EXTENT_F_ENCRYPTED)) {
            /* This diagnostic dump doesn't decompress or decrypt (v2.2
             * Phase 3/4) -- skip rather than print raw compressed/
             * encrypted bytes as if they were text, or misread the block
             * count against logical_size. */
            kprintf("EMBKFS: %s: [%s%s%s extent @%lu, %lu logical bytes -- dump skipped]\n",
                    dev,
                    (ext[i].flags & EMBKFS_EXTENT_F_COMPRESSED) ? "compressed" : "",
                    (ext[i].flags & (EMBKFS_EXTENT_F_COMPRESSED | EMBKFS_EXTENT_F_ENCRYPTED))
                        == (EMBKFS_EXTENT_F_COMPRESSED | EMBKFS_EXTENT_F_ENCRYPTED) ? "+" : "",
                    (ext[i].flags & EMBKFS_EXTENT_F_ENCRYPTED) ? "encrypted" : "",
                    ext[i].offset, ext[i].logical_size);
            expect_off += ext[i].logical_size;
            continue;
        }
        uint32_t csum = 0; uint64_t written = 0;
        for (uint64_t blk = 0; blk < ext[i].length; blk++) {
            uint64_t chunk = ext[i].logical_size - written;
            if (chunk > vol->block_size) chunk = vol->block_size;
            rc = embk_block_read(vol->dev, (ext[i].disk_block + blk) * spb, spb, datablk);
            if (rc != EMBK_OK) { kprintf("\nEMBKFS: %s: data read failed: %s\n", dev, embk_strerror(rc)); kfree(ext); return rc; }
            for (uint64_t k = 0; k < chunk; k++) kprintf("%c", datablk[k]);
            csum = embk_crc32c(datablk, chunk, csum);  /* thread CRC over logical bytes only */
            written += chunk;
        }
        if (csum != ext[i].checksum) {
            kprintf("\nEMBKFS: %s: extent @%lu DATA checksum bad (stored 0x%08X, calc 0x%08X)\n",
                    dev, ext[i].offset, ext[i].checksum, csum);
            kfree(ext);
            return -EMBK_EINVAL;
        }
        expect_off += ext[i].logical_size;
    }
    if (expect_off != fsize) {                          /* sum of extents must equal inode size */
        kprintf("\nEMBKFS: %s: extents cover %lu bytes but inode says %lu\n", dev, expect_off, fsize);
        kfree(ext);
        return -EMBK_EINVAL;
    }
    kfree(ext);
    kprintf("\nEMBKFS: %s: ---------------------- (csum OK, end-to-end verified)\n", dev);
    return EMBK_OK;
}




/* The leaf/internal rebuild + split machinery lives just above embkfs_cow_apply_rec
 * (it needs embkfs_write_node / embkfs_alloc_block, defined below). */


/* Stamp a node block for its NEW home and write it: self-block number, the new
 * commit's generation, and the self-checksum (over [8..end]). Returns the
 * block_ptr a parent must store. Every COW-relocated node goes through here. */
static int embkfs_write_node(struct embkfs_volume *vol, uint64_t block,
                             uint64_t generation, uint8_t *buf,
                             struct embk_block_ptr *out_ptr)
{
    struct embk_node_header *h = (struct embk_node_header *)buf;
    h->block      = block;                                  /* node now lives here */
    h->generation = generation;                            /* belongs to this commit */
    uint32_t csum = embk_crc32c(buf + 8, vol->block_size - 8, 0);
    h->checksum   = csum;                                   /* first field, low 32 = CRC */

    uint64_t spb = vol->block_size / vol->dev->block_size;
    int rc = embk_block_write(vol->dev, block * spb, spb, buf);
    if (rc != EMBK_OK) return rc;

    out_ptr->block = block;  out_ptr->checksum = csum;
    out_ptr->generation = generation;  out_ptr->flags = 0;
    return EMBK_OK;
}


/* HMAC key for the verified-root boot check (v2.2 Phase 5d) -- embedded in
 * the kernel BINARY, not a secret an attacker with a copy of this source/
 * build couldn't derive too. See embkfs.h's embk_verify_header comment
 * for the honestly-scoped threat model this does and doesn't cover. */
static const uint8_t EMBKFS_VERIFY_ROOT_KEY[32] = {
    0x45, 0x6d, 0x62, 0x4c, 0x69, 0x6e, 0x6b, 0x4f, 0x53, 0x2d, 0x45, 0x4d, 0x42, 0x4b, 0x46, 0x53,
    0x2d, 0x76, 0x32, 0x2e, 0x32, 0x2d, 0x72, 0x6f, 0x6f, 0x74, 0x2d, 0x6b, 0x65, 0x79, 0x21, 0x21
};

/* The exact 40 authenticated bytes: the new root block_ptr (32B) and the
 * new generation (8B) -- everything else in the superblock is either
 * fixed at format time (block_size, total_blocks, uuid) or, for
 * free_blocks, already implied by generation+root together, so covering
 * it would be redundant weight on every recompute (this runs on EVERY
 * commit, not just at mount). */
static void embkfs_verify_root_hmac(const struct embk_block_ptr *root, uint64_t generation,
                                    uint8_t out[SHA256_DIGEST_SIZE]) {
    uint8_t msg[32 + 8];
    memcpy(msg, root, 32);
    for (int i = 0; i < 8; i++) msg[32 + i] = (uint8_t)(generation >> (8 * i));
    hmac_sha256(EMBKFS_VERIFY_ROOT_KEY, sizeof EMBKFS_VERIFY_ROOT_KEY, msg, sizeof msg, out);
}

/* Write a new superblock: patch root pointer, generation, and free count onto
 * the existing one, re-checksum the body, write primary then backup. THE COMMIT. */
static int embkfs_write_superblock(struct embkfs_volume *vol,
                                   const struct embk_block_ptr *new_root,
                                   uint64_t new_generation, uint64_t free_blocks)
{
    static uint8_t sbbuf[4096];
    uint64_t spb      = vol->block_size / vol->dev->block_size;
    uint64_t sb_block = EMBKFS_SB_OFFSET / vol->block_size;     /* 16 */

    int rc = embk_block_read(vol->dev, sb_block * spb, spb, sbbuf);
    if (rc != EMBK_OK) return rc;

    struct embk_superblock *sb = (struct embk_superblock *)sbbuf;
    sb->free_blocks = free_blocks;                 /* body @56  */
    sb->generation  = new_generation;              /* body @80  */
    memcpy(&sb->root, new_root, sizeof sb->root);  /* body @88  (32B block_ptr) */
    sb->feature_incompat = vol->feature_incompat;  /* body @32  (only ever gains bits, v2.2) */

    /* Re-sign the verified-root HMAC on EVERY commit (v2.2 Phase 5d) --
     * this kernel has the embedded key, so it can (and must) keep the
     * stored HMAC in sync with its own legitimate writes; only tampering
     * by something WITHOUT the key ever makes the next mount's check fail. */
    if (vol->feature_incompat & EMBKFS_INCOMPAT_VERIFIED_ROOT) {
        struct embk_verify_header *vh =
            (struct embk_verify_header *)(sbbuf + EMBKFS_VERIFY_HEADER_OFFSET);
        vh->magic = EMBKFS_VERIFY_HEADER_MAGIC;
        embkfs_verify_root_hmac(new_root, new_generation, vh->hmac);
    }

    sb->checksum = embk_crc32c(sbbuf, EMBKFS_SB_BODY_SIZE, 0);   /* body [0..152) */

    rc = embk_block_write(vol->dev, sb_block * spb, spb, sbbuf);
    if (rc != EMBK_OK) return rc;
    return embk_block_write(vol->dev, (vol->total_blocks - 1) * spb, spb, sbbuf);
}


#define EMBKFS_PASSPHRASE_MAX      128
#define EMBKFS_UNLOCK_MAX_ATTEMPTS 3

/* Blocking, masked-echo line read from the keyboard -- used only at mount
 * time, before the shell's own polling input loop (main.c) exists, so a
 * simple direct keyboard_getchar() loop is fine here (nothing else needs
 * to run concurrently during this prompt). */
static uint32_t embkfs_read_passphrase(char *out, uint32_t out_cap) {
    uint32_t len = 0;
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\r' || c == '\n') {
            kprintf("\n");
            break;
        } else if ((c == '\b' || c == 127) && len > 0) {
            len--;
            kprintf("\b \b");
        } else if (c >= 32 && c <= 126) {
            if (len + 1 < out_cap) {
                out[len++] = c;
                kprintf("*");
            }
        }
    }
    out[len] = '\0';
    return len;
}

/* Any fixed 16-byte plaintext works for the key-check -- it's never a
 * secret, just a known value the correctly-derived key must reproduce.
 * Block number 0 for this XTS encryption is likewise arbitrary and fixed;
 * the key-check ciphertext is stored independently of any real extent, so
 * it can't collide with (or leak anything about) actual file data. */
static const uint8_t EMBKFS_KEY_CHECK_PLAINTEXT[16] = {
    'E','M','B','K','F','S','-','K','E','Y','-','C','H','E','C','K'
};

/* Derives XTS keys from `passphrase` per `hdr` and checks them against
 * hdr->key_check_ciphertext, initializing *xts on a match. Pure function
 * of its arguments -- doesn't touch vol, so the caller decides what a
 * verified key means. */
static bool embkfs_try_unlock(const struct embk_crypto_header *hdr,
                              const char *passphrase, uint32_t passphrase_len,
                              struct aes_xts_ctx *xts) {
    uint8_t keymat[64];   /* [0..32) data key, [32..64) tweak key */
    pbkdf2_hmac_sha256((const uint8_t *)passphrase, passphrase_len,
                        hdr->kdf_salt, sizeof hdr->kdf_salt,
                        hdr->kdf_iterations, keymat, sizeof keymat);
    aes_xts_init(xts, keymat, keymat + 32);

    uint8_t check[16];
    aes_xts_encrypt(xts, 0, EMBKFS_KEY_CHECK_PLAINTEXT, check, sizeof check);
    bool ok = memcmp(check, hdr->key_check_ciphertext, sizeof check) == 0;
    memset(keymat, 0, sizeof keymat);
    return ok;
}

/* Mount-time passphrase flow: prompt (masked echo), derive, verify, retry
 * a bounded number of times. A failed unlock here only fails THIS mount
 * attempt -- embkfs_init()'s per-device loop moves on to the next block
 * device, so one encrypted (or mistyped) volume never blocks any other,
 * unencrypted volume from mounting. */
static bool embkfs_unlock_volume(struct embk_block_device *dev, struct embkfs_volume *vol,
                                 const struct embk_crypto_header *hdr) {
    char passphrase[EMBKFS_PASSPHRASE_MAX];
    for (int attempt = 1; attempt <= EMBKFS_UNLOCK_MAX_ATTEMPTS; attempt++) {
        kprintf("EMBKFS: %s: this volume is encrypted -- passphrase (attempt %d/%d): ",
                dev->name, attempt, EMBKFS_UNLOCK_MAX_ATTEMPTS);
        uint32_t len = embkfs_read_passphrase(passphrase, sizeof passphrase);
        struct aes_xts_ctx xts;
        bool ok = embkfs_try_unlock(hdr, passphrase, len, &xts);
        memset(passphrase, 0, sizeof passphrase);   /* scrub before reuse/return */
        if (ok) {
            memcpy(vol->xts_opaque, &xts, sizeof xts);
            memset(&xts, 0, sizeof xts);
            vol->encrypted = true;
            kprintf("EMBKFS: %s: passphrase accepted\n", dev->name);
            return true;
        }
        kprintf("EMBKFS: %s: wrong passphrase\n", dev->name);
    }
    return false;
}

/*
 * Read-only EMBKFS mount. So far: bring up CRC32C, then read and verify the
 * superblock — the format's root of trust. Later steps follow the root
 * pointer into the metadata tree.
 */

int embkfs_mount(struct embk_block_device *dev, struct embkfs_volume *vol)
{
    if (!dev || !vol) {
        return -EMBK_EINVAL;
    }

    /* The superblock sits at a FIXED BYTE offset (65536), independent of the
     * filesystem block size — block_size lives inside the superblock, so we
     * can't address by block until we've read it. The block device reads in
     * sectors of dev->block_size bytes (512 here), so translate the byte
     * offset to an LBA: 65536 / 512 = sector 128. */
    if (EMBKFS_SB_OFFSET % dev->block_size != 0) {
        return -EMBK_EINVAL;                 /* geometry our mkfs never makes */
    }
    uint64_t sb_lba = EMBKFS_SB_OFFSET / dev->block_size;

    /* The 160-byte superblock fits in one 512-byte sector. Read both copies
     * (primary + backup) and use the newest VALID one. */
    static uint8_t sb_primary[512] __attribute__((aligned(8)));
    static uint8_t sb_backup[512] __attribute__((aligned(8)));

    bool primary_valid = false;
    bool backup_valid  = false;
    int rc = embk_block_read(dev, sb_lba, 1, sb_primary);
    if (rc == EMBK_OK) {
        const struct embk_superblock *psb = (const struct embk_superblock *)sb_primary;
        if (psb->magic == EMBKFS_MAGIC) {
            uint32_t pcalc = embk_crc32c(sb_primary, EMBKFS_SB_BODY_SIZE, 0);
            primary_valid = ((uint32_t)psb->checksum == pcalc);
        }
    }

    /* Backup superblock is at the start of the last EMBKFS block. Since block
     * size is stored in the superblock itself, probe the permitted block sizes
     * and accept the first checksum-valid backup we find. */
    static const uint64_t spb_candidates[] = { 8, 16, 32, 64, 128 }; /* 4K..64K over 512B sectors */
    for (uint32_t i = 0; i < (uint32_t)(sizeof spb_candidates / sizeof spb_candidates[0]); i++) {
        uint64_t spb = spb_candidates[i];
        if (dev->block_count < spb) continue;
        uint64_t backup_lba = dev->block_count - spb;
        if (embk_block_read(dev, backup_lba, 1, sb_backup) != EMBK_OK)
            continue;

        const struct embk_superblock *bsb = (const struct embk_superblock *)sb_backup;
        if (bsb->magic != EMBKFS_MAGIC)
            continue;

        uint32_t bcalc = embk_crc32c(sb_backup, EMBKFS_SB_BODY_SIZE, 0);
        if ((uint32_t)bsb->checksum != bcalc)
            continue;

        /* Ensure this candidate actually claims the probed block size. */
        if (bsb->block_size != spb * dev->block_size)
            continue;

        backup_valid = true;
        break;
    }

    if (!primary_valid && !backup_valid) {
        return -EMBK_EINVAL;
    }

    const struct embk_superblock *sb = NULL;
    if (primary_valid && backup_valid) {
        const struct embk_superblock *psb = (const struct embk_superblock *)sb_primary;
        const struct embk_superblock *bsb = (const struct embk_superblock *)sb_backup;
        sb = (bsb->generation > psb->generation) ? bsb : psb;
        if (sb == bsb)
            kprintf("EMBKFS: %s: using newer backup superblock (gen %lu > %lu)\n",
                    dev->name, bsb->generation, psb->generation);
    } else if (primary_valid) {
        sb = (const struct embk_superblock *)sb_primary;
    } else {
        sb = (const struct embk_superblock *)sb_backup;
        kprintf("EMBKFS: %s: primary superblock invalid, mounted from backup\n", dev->name);
    }

    /* (2b) SELF-HEALING (v2.2 Phase 5a): the two copies can disagree in
     * exactly two ways -- one is invalid (bad checksum/magic, e.g. a torn
     * write or bit rot), or both are valid but at different generations
     * (a crash between writing the primary and the backup mid-commit,
     * spec 5.2's commit protocol writes them sequentially, not
     * atomically-as-a-pair). Either way, once the authoritative copy is
     * chosen above, write ITS full block over the stale/bad one so a
     * future mount doesn't need to fall back again. A repair failure is
     * only logged: the volume still mounts fine from the good copy
     * regardless, this can never block the mount itself. */
    {
        bool sb_is_primary = ((const uint8_t *)sb == sb_primary);
        uint64_t good_spb = sb->block_size / dev->block_size;
        uint64_t backup_lba = (dev->block_count >= good_spb) ? dev->block_count - good_spb : 0;
        uint64_t good_lba = sb_is_primary ? sb_lba : backup_lba;
        uint64_t bad_lba  = sb_is_primary ? backup_lba : sb_lba;

        bool primary_matches = primary_valid &&
            ((const struct embk_superblock *)sb_primary)->generation == sb->generation;
        bool backup_matches = backup_valid &&
            ((const struct embk_superblock *)sb_backup)->generation == sb->generation;
        bool other_ok = sb_is_primary ? backup_matches : primary_matches;

        if (!other_ok && good_spb > 0 && good_spb <= 128) {
            uint8_t *goodblk = kmalloc(good_spb * dev->block_size);
            if (goodblk && embk_block_read(dev, good_lba, good_spb, goodblk) == EMBK_OK) {
                int wrc = embk_block_write(dev, bad_lba, good_spb, goodblk);
                if (wrc == EMBK_OK) {
                    kprintf("EMBKFS: %s: self-heal: repaired %s superblock copy (now matches gen %lu)\n",
                            dev->name, sb_is_primary ? "backup" : "primary", sb->generation);
                } else {
                    kprintf("EMBKFS: %s: self-heal: repair write failed: %s\n",
                            dev->name, embk_strerror(wrc));
                }
            }
            kfree(goodblk);
        }
    }

    /* (3) FEATURE NEGOTIATION (spec §4.2 / §5.1). We understand no optional
     *     features yet: unknown incompat -> refuse, unknown ro_compat ->
     *     read-only, compat bits are always safe to ignore. */
    if (sb->feature_incompat & ~EMBKFS_KNOWN_INCOMPAT) {
        kprintf("EMBKFS: %s: unknown incompat features 0x%lX — refusing\n",
                dev->name, sb->feature_incompat);
        return -EMBK_EINVAL;
    }
    bool read_only = (sb->feature_ro_compat & ~EMBKFS_KNOWN_RO_COMPAT) != 0;

    /* Version backstop: a newer major could mean anything. */
    if (sb->version_major > EMBKFS_MAX_KNOWN_MAJOR) {
        kprintf("EMBKFS: %s: version %u.%u newer than we understand\n",
                dev->name, (unsigned int)sb->version_major,
                (unsigned int)sb->version_minor);
        return -EMBK_EINVAL;
    }

    /* (3b) ENCRYPTION (v2.2 Phase 4). Must unlock BEFORE anything below
     * trusts the volume's contents -- the crypto header lives in the same
     * 512-byte sector as `sb` (whichever buffer that pointer is currently
     * aliasing), just past the checksummed region. A wrong/missing
     * passphrase fails just THIS mount, never other volumes. */
    vol->encrypted = false;
    if (sb->feature_incompat & EMBKFS_INCOMPAT_ENCRYPTED) {
        const struct embk_crypto_header *hdr =
            (const struct embk_crypto_header *)((const uint8_t *)sb + EMBKFS_CRYPTO_HEADER_OFFSET);
        if (hdr->magic != EMBKFS_CRYPTO_HEADER_MAGIC) {
            kprintf("EMBKFS: %s: ENCRYPTED feature set but crypto header magic is wrong — refusing\n",
                    dev->name);
            return -EMBK_EINVAL;
        }
        if (!embkfs_unlock_volume(dev, vol, hdr)) {
            kprintf("EMBKFS: %s: too many wrong passphrases — refusing to mount\n", dev->name);
            return -EMBK_EACCES;
        }
    }

    /* (3c) VERIFIED ROOT (v2.2 Phase 5d). The OS refuses to trust a
     * tampered volume before executing anything from it: recompute the
     * HMAC over THIS superblock's root+generation and compare against
     * what's stored. See embkfs.h's embk_verify_header comment for the
     * honestly-scoped threat model (kernel-embedded key, not asymmetric
     * signing). A mismatch fails just THIS mount, never other volumes. */
    if (sb->feature_incompat & EMBKFS_INCOMPAT_VERIFIED_ROOT) {
        const struct embk_verify_header *vh =
            (const struct embk_verify_header *)((const uint8_t *)sb + EMBKFS_VERIFY_HEADER_OFFSET);
        if (vh->magic != EMBKFS_VERIFY_HEADER_MAGIC) {
            kprintf("EMBKFS: %s: VERIFIED_ROOT feature set but verify header magic is wrong — refusing\n",
                    dev->name);
            return -EMBK_EINVAL;
        }
        uint8_t expected[SHA256_DIGEST_SIZE];
        embkfs_verify_root_hmac(&sb->root, sb->generation, expected);
        if (memcmp(expected, vh->hmac, SHA256_DIGEST_SIZE) != 0) {
            kprintf("EMBKFS: %s: verified-root HMAC mismatch — refusing to trust this volume "
                    "(tampered, or generation %lu wasn't committed by this kernel)\n",
                    dev->name, sb->generation);
            return -EMBK_EACCES;
        }
        kprintf("EMBKFS: %s: verified-root HMAC OK (gen %lu)\n", dev->name, sb->generation);
    }

    /* Validated. Record the mount state. root is a 32-byte struct copied by
     * value — our entry point into the metadata tree next step. */
    vol->dev          = dev;
    vol->block_size   = sb->block_size;
    vol->total_blocks = sb->total_blocks;
    vol->free_blocks  = sb->free_blocks;
    vol->generation   = sb->generation;
    /* read cache starts empty (rcache_gen=0 never matches a real generation) */
    vol->rcache_buf = NULL; vol->rcache_oid = 0; vol->rcache_gen = 0; vol->rcache_len = 0;
    /* extent-map cache: same deal, same keying (see embkfs.h). Must be seeded --
     * read_object_range tests ecache_ext before ecache_gen, so a garbage pointer
     * from an uninitialised volume would be dereferenced. */
    vol->ecache_ext = NULL; vol->ecache_oid = 0; vol->ecache_gen = 0; vol->ecache_n = 0;
    vol->ecache_verified = NULL;
    /* Inode cache: icache_valid is the gate, so it MUST be seeded -- a garbage
     * true here would serve a stale/garbage inode on the very first read. */
    vol->icache_valid = false; vol->icache_oid = 0; vol->icache_gen = 0;
    /* Read-ahead window: wcache_buf is the gate (NULL = unallocated), same shape
     * as rcache/ecache, so a garbage pointer here would be read as a live cache. */
    vol->wcache_buf = NULL; vol->wcache_oid = 0; vol->wcache_gen = 0;
    vol->wcache_off = 0;    vol->wcache_len = 0;
    vol->root         = sb->root;
    vol->read_only    = read_only;
    vol->feature_incompat = sb->feature_incompat;
    vol->mounted      = true;
    vol->txn          = NULL;          /* no transaction in flight */
    vol->block_bitmap = NULL;
    vol->free_ext     = NULL;
    vol->free_ext_n   = 0;
    vol->snapshot_count = 0;           /* real count set by embkfs_bitmap_build()
                                        * during embkfs_finish_mount(); zeroed here
                                        * as a safe default for the gap in between,
                                        * and for any caller that only calls
                                        * embkfs_mount() without finishing (v2.2
                                        * Phase 5b) */
    vol->free_ext_cap = 0;

    kprintf("EMBKFS: %s: mounted  v%u.%u  block_size %lu  blocks %lu  "
            "free %lu  gen %lu  (%s)\n",
            dev->name,
            (unsigned int)sb->version_major, (unsigned int)sb->version_minor,
            sb->block_size, sb->total_blocks, sb->free_blocks, sb->generation,
            read_only ? "read-only" : "read-write");
    kprintf("EMBKFS: %s: root -> block %lu  (ptr-csum 0x%08X, gen %lu)\n",
            dev->name, sb->root.block,
            (unsigned int)sb->root.checksum, sb->root.generation);

    return EMBK_OK;
}


/*
 * Read the block a pointer targets and verify it as a tree node — against that
 * very pointer. Per spec §8.1, in order:
 *   (1) magic           == EMBKFS_NODE_MAGIC
 *   (2) self-checksum   : CRC32C over [8 .. block_size-1] == header->checksum
 *   (3) Merkle link     : header->checksum == ptr->checksum   (parent vouches)
 *   (4) generation      : header->generation == ptr->generation
 *   (5) self-block       : header->block == ptr->block
 * On success, `buf` is a valid, parent-vouched node block.
 */
/* See struct embkfs_stat (embkfs.h). Attributes EMBKFS's block reads by cause. */
static struct embkfs_stat g_efs_stat;
void embkfs_stat_reset(void) { memset(&g_efs_stat, 0, sizeof g_efs_stat); }
void embkfs_stat_get(struct embkfs_stat *out) { if (out) *out = g_efs_stat; }

int embkfs_read_node(struct embkfs_volume *vol,
                     const struct embk_block_ptr *ptr,
                     uint8_t *buf, size_t buf_size)
{
    g_efs_stat.node_reads++;
    const char *name = vol->dev->name;

    if (vol->block_size > buf_size) {
        kprintf("EMBKFS: %s: node buffer too small (%lu < block_size %lu)\n",
                name, (unsigned long)buf_size, vol->block_size);
        return -EMBK_EINVAL;
    }

    /* A node IS the whole block. Translate block number -> device LBA: block N
     * starts at sector N*(block_size/512) and spans block_size/512 sectors
     * (spec §3). For block_size 4096 that's 8 sectors; block 17 -> LBA 136. */
    uint64_t spb = vol->block_size / vol->dev->block_size;   /* sectors/block */
    uint64_t lba = ptr->block * spb;

    int rc = embk_block_read(vol->dev, lba, spb, buf);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: %s: node block %lu read (LBA %lu) failed: %s\n",
                name, ptr->block, lba, embk_strerror(rc));
        return rc;
    }

    const struct embk_node_header *h = (const struct embk_node_header *)buf;

    /* (1) Magic. */
    if (h->magic != EMBKFS_NODE_MAGIC) {
        kprintf("EMBKFS: %s: block %lu bad node magic 0x%lX\n",
                name, ptr->block, h->magic);
        return -EMBK_EINVAL;
    }

    /* (2) Self-checksum. The checksum field is at offset 0, so it covers
     *     everything after it: bytes [8 .. block_size-1]. Does the block match
     *     its own tag? */
    uint32_t calc = embk_crc32c(buf + 8, vol->block_size - 8, 0);
    if ((uint32_t)h->checksum != calc) {
        kprintf("EMBKFS: %s: block %lu checksum bad (stored 0x%08X, calc 0x%08X)\n",
                name, ptr->block, (unsigned int)h->checksum, (unsigned int)calc);
        return -EMBK_EINVAL;
    }

    /* (3) Merkle link. The parent independently recorded this node's checksum;
     *     matching it proves we reached the exact block the parent committed to,
     *     not a stale or substituted but self-consistent one. */
    if ((uint32_t)h->checksum != (uint32_t)ptr->checksum) {
        kprintf("EMBKFS: %s: block %lu checksum 0x%08X != parent's 0x%08X "
                "(stale/substituted block)\n",
                name, ptr->block, (unsigned int)h->checksum,
                (unsigned int)ptr->checksum);
        return -EMBK_EINVAL;
    }

    /* (4) Generation: COW leaves old versions behind; confirm this block carries
     *     the generation the pointer expects. */
    if (h->generation != ptr->generation) {
        kprintf("EMBKFS: %s: block %lu generation %lu != pointer's %lu\n",
                name, ptr->block, h->generation, ptr->generation);
        return -EMBK_EINVAL;
    }

    /* (5) Self-block: the node records its own number; catches a misdirected
     *     read or a block relocated without a pointer fix-up. */
    if (h->block != ptr->block) {
        kprintf("EMBKFS: %s: block %lu self-id says %lu (misplaced block)\n",
                name, ptr->block, h->block);
        return -EMBK_EINVAL;
    }

    return EMBK_OK;
}


/* ---- COW rebuild + split/merge machinery -------------------------------
 * A node rebuild no longer yields exactly one node. Applying ops can make a leaf
 * (or internal node) OVERFLOW one block — then it splits into two — or become
 * EMPTY — then it is dropped. So a rebuilt subtree hands its parent a list of
 * 0, 1, or 2 child entries (struct embk_child: the subtree minimum key + its
 * pointer). The parent substitutes that list in place of the child's old slot,
 * which may in turn overflow the parent (it splits) or, if all its children
 * vanished, empty it (it is dropped). At the top, two root entries grow the tree
 * a level; see embkfs_cow_apply. The split bound is 2 because a single
 * transaction adds only a few items, so any one node overflows by less than a
 * block; bulk inserts needing 3+ way splits are a future concern. */



/* Decode a leaf's items and apply the ops (replace / insert / delete by key),
 * yielding the sorted working list. Pure list manipulation — packing into one or
 * two blocks happens afterwards. Allocates *out_items (caller frees). */
static int embk_leaf_build_items(struct embkfs_volume *vol, const uint8_t *src,
                                 const struct embk_put *ops, uint32_t nops,
                                 struct embk_litem **out_items, uint32_t *out_n)
{
    const struct embk_node_header *sh = (const struct embk_node_header *)src;
    uint32_t src_n = sh->nritems;

    struct embk_litem *items = kmalloc(((uint64_t)src_n + nops + 1) * sizeof *items);
    if (!items) return -EMBK_ENOMEM;

    uint32_t n = 0;
    for (uint32_t i = 0; i < src_n; i++) {
        const struct embk_item_header *it = embk_leaf_item(src, i);
        if ((uint64_t)it->offset + it->size > vol->block_size) { kfree(items); return -EMBK_EINVAL; }
        items[n].key = it->key; items[n].data = src + it->offset; items[n].size = it->size; n++;
    }
    for (uint32_t p = 0; p < nops; p++) {
        uint32_t pos = 0; int cmp = 1;
        while (pos < n && (cmp = embk_key_cmp(&items[pos].key, &ops[p].key)) < 0) pos++;
        bool found = (pos < n && cmp == 0);
        if (ops[p].del) {
            if (found) { for (uint32_t k = pos; k + 1 < n; k++) items[k] = items[k + 1]; n--; }
        } else if (found) {
            items[pos].data = ops[p].data; items[pos].size = ops[p].size;
        } else {
            for (uint32_t k = n; k > pos; k--) items[k] = items[k - 1];
            items[pos].key = ops[p].key; items[pos].data = ops[p].data; items[pos].size = ops[p].size; n++;
        }
    }
    *out_items = items; *out_n = n;
    return EMBK_OK;
}

/* Serialize items[start .. start+count) as a fresh leaf (spec §8.3 slotted page),
 * allocate a block, and write it — returning the {min key, ptr} child. */
static int embk_emit_leaf(struct embkfs_volume *vol, const struct embk_litem *items,
                          uint32_t start, uint32_t count, uint64_t new_gen,
                          uint8_t *dst, struct embk_child *out)
{
    uint64_t need = sizeof(struct embk_node_header)
                  + (uint64_t)count * sizeof(struct embk_item_header);
    for (uint32_t i = 0; i < count; i++) need += items[start + i].size;
    if (need > vol->block_size) return -EMBK_ENOSPC;

    memset(dst, 0, vol->block_size);
    struct embk_node_header *dh = (struct embk_node_header *)dst;
    dh->magic = EMBKFS_NODE_MAGIC; dh->level = 0; dh->nritems = count;

    uint64_t cursor = vol->block_size;
    for (uint32_t i = 0; i < count; i++) {
        const struct embk_litem *it = &items[start + i];
        cursor -= it->size;
        memcpy(dst + cursor, it->data, it->size);
        struct embk_item_header *ih = (struct embk_item_header *)
            (dst + sizeof(struct embk_node_header) + (size_t)i * sizeof *ih);
        ih->key = it->key; ih->offset = (uint32_t)cursor; ih->size = it->size;
    }

    if (count > 0) out->key = items[start].key;
    else           memset(&out->key, 0, sizeof out->key);

    uint64_t nb;
    int rc = embkfs_alloc_block(vol, &nb);
    if (rc != EMBK_OK) return rc;
    return embkfs_write_node(vol, nb, new_gen, dst, &out->ptr);
}

/* Serialize children[start .. start+count) as a fresh internal node at `level`
 * (spec §8.2 contiguous {key, ptr} slots), allocate a block, and write it. */
static int embk_emit_internal(struct embkfs_volume *vol, const struct embk_child *kids,
                              uint32_t start, uint32_t count, uint8_t level,
                              uint64_t new_gen, uint8_t *dst, struct embk_child *out)
{
    if (sizeof(struct embk_node_header)
        + (uint64_t)count * sizeof(struct embk_internal_slot) > vol->block_size)
        return -EMBK_ENOSPC;

    memset(dst, 0, vol->block_size);
    struct embk_node_header *dh = (struct embk_node_header *)dst;
    dh->magic = EMBKFS_NODE_MAGIC; dh->level = level; dh->nritems = count;

    struct embk_internal_slot *slots =
        (struct embk_internal_slot *)(dst + sizeof(struct embk_node_header));
    for (uint32_t i = 0; i < count; i++) {
        memcpy(&slots[i].key, &kids[start + i].key, sizeof slots[i].key);
        memcpy(&slots[i].ptr, &kids[start + i].ptr, sizeof slots[i].ptr);
    }
    out->key = kids[start].key;

    uint64_t nb;
    int rc = embkfs_alloc_block(vol, &nb);
    if (rc != EMBK_OK) return rc;
    return embkfs_write_node(vol, nb, new_gen, dst, &out->ptr);
}

/* Emit a leaf item list as 0 children (empty), 1 (fits), or 2 (overflow ->
 * split, balanced by bytes). `dst` is reusable scratch. */
static int embk_emit_leaf_list(struct embkfs_volume *vol, const struct embk_litem *items,
                               uint32_t n, uint64_t new_gen, uint8_t *dst,
                               struct embk_child *out, uint32_t *out_n)
{
    if (n == 0) { *out_n = 0; return EMBK_OK; }

    uint64_t total = sizeof(struct embk_node_header);
    for (uint32_t i = 0; i < n; i++) total += sizeof(struct embk_item_header) + items[i].size;

    if (total <= vol->block_size) {                    /* fits in one leaf */
        *out_n = 1;
        return embk_emit_leaf(vol, items, 0, n, new_gen, dst, &out[0]);
    }

    /* two-way split, balanced by bytes: cut where the running cost reaches half */
    uint64_t half = total / 2, acc = sizeof(struct embk_node_header);
    uint32_t sp = 0;
    for (sp = 0; sp < n; sp++) {
        acc += sizeof(struct embk_item_header) + items[sp].size;
        if (acc >= half) { sp++; break; }
    }
    if (sp < 1)     sp = 1;
    if (sp > n - 1) sp = n - 1;

    int rc = embk_emit_leaf(vol, items, 0, sp, new_gen, dst, &out[0]);
    if (rc == EMBK_OK) rc = embk_emit_leaf(vol, items, sp, n - sp, new_gen, dst, &out[1]);
    *out_n = 2;
    return rc;
}

/* Emit an internal child list as 0, 1, or 2 nodes (split on slot count). */
static int embk_emit_internal_list(struct embkfs_volume *vol, const struct embk_child *kids,
                                   uint32_t n, uint8_t level, uint64_t new_gen, uint8_t *dst,
                                   struct embk_child *out, uint32_t *out_n)
{
    if (n == 0) { *out_n = 0; return EMBK_OK; }
    if (n <= EMBKFS_MAX_SLOTS) {
        *out_n = 1;
        return embk_emit_internal(vol, kids, 0, n, level, new_gen, dst, &out[0]);
    }
    uint32_t sp = n / 2;                                /* balanced by count */
    int rc = embk_emit_internal(vol, kids, 0, sp, level, new_gen, dst, &out[0]);
    if (rc == EMBK_OK) rc = embk_emit_internal(vol, kids, sp, n - sp, level, new_gen, dst, &out[1]);
    *out_n = 2;
    return rc;
}

static int embk_child_shape(struct embkfs_volume *vol, const struct embk_block_ptr *ptr,
                            uint8_t *out_level, uint32_t *out_nritems)
{
    uint8_t *buf = kmalloc(vol->block_size);
    if (!buf) return -EMBK_ENOMEM;
    int rc = embkfs_read_node(vol, ptr, buf, vol->block_size);
    if (rc != EMBK_OK) { kfree(buf); return rc; }
    const struct embk_node_header *h = (const struct embk_node_header *)buf;
    *out_level = h->level;
    *out_nritems = h->nritems;
    kfree(buf);
    return EMBK_OK;
}

static int embk_rebalance_leaf_pair(struct embkfs_volume *vol,
                                    const struct embk_block_ptr *left,
                                    const struct embk_block_ptr *right,
                                    uint64_t new_gen,
                                    struct embk_child *out, uint32_t *out_n)
{
    uint8_t *lbuf = kmalloc(vol->block_size);
    uint8_t *rbuf = kmalloc(vol->block_size);
    uint8_t *dst  = kmalloc(vol->block_size);
    if (!lbuf || !rbuf || !dst) { kfree(lbuf); kfree(rbuf); kfree(dst); return -EMBK_ENOMEM; }

    int rc = embkfs_read_node(vol, left, lbuf, vol->block_size);
    if (rc != EMBK_OK) { kfree(lbuf); kfree(rbuf); kfree(dst); return rc; }
    rc = embkfs_read_node(vol, right, rbuf, vol->block_size);
    if (rc != EMBK_OK) { kfree(lbuf); kfree(rbuf); kfree(dst); return rc; }

    const struct embk_node_header *lh = (const struct embk_node_header *)lbuf;
    const struct embk_node_header *rh = (const struct embk_node_header *)rbuf;
    if (lh->level != 0 || rh->level != 0) { kfree(lbuf); kfree(rbuf); kfree(dst); return -EMBK_EINVAL; }

    uint32_t n = lh->nritems + rh->nritems;
    struct embk_litem *items = kmalloc((uint64_t)n * sizeof *items);
    if (!items) { kfree(lbuf); kfree(rbuf); kfree(dst); return -EMBK_ENOMEM; }

    uint32_t at = 0;
    for (uint32_t i = 0; i < lh->nritems; i++) {
        const struct embk_item_header *it = embk_leaf_item(lbuf, i);
        if ((uint64_t)it->offset + it->size > vol->block_size) { rc = -EMBK_EINVAL; goto out; }
        items[at].key = it->key;
        items[at].data = lbuf + it->offset;
        items[at].size = it->size;
        at++;
    }
    for (uint32_t i = 0; i < rh->nritems; i++) {
        const struct embk_item_header *it = embk_leaf_item(rbuf, i);
        if ((uint64_t)it->offset + it->size > vol->block_size) { rc = -EMBK_EINVAL; goto out; }
        items[at].key = it->key;
        items[at].data = rbuf + it->offset;
        items[at].size = it->size;
        at++;
    }

    rc = embk_emit_leaf_list(vol, items, n, new_gen, dst, out, out_n);

out:
    kfree(items);
    kfree(lbuf);
    kfree(rbuf);
    kfree(dst);
    return rc;
}

static int embk_rebalance_internal_pair(struct embkfs_volume *vol,
                                        const struct embk_block_ptr *left,
                                        const struct embk_block_ptr *right,
                                        uint8_t child_level, uint64_t new_gen,
                                        struct embk_child *out, uint32_t *out_n)
{
    uint8_t *lbuf = kmalloc(vol->block_size);
    uint8_t *rbuf = kmalloc(vol->block_size);
    uint8_t *dst  = kmalloc(vol->block_size);
    if (!lbuf || !rbuf || !dst) { kfree(lbuf); kfree(rbuf); kfree(dst); return -EMBK_ENOMEM; }

    int rc = embkfs_read_node(vol, left, lbuf, vol->block_size);
    if (rc != EMBK_OK) { kfree(lbuf); kfree(rbuf); kfree(dst); return rc; }
    rc = embkfs_read_node(vol, right, rbuf, vol->block_size);
    if (rc != EMBK_OK) { kfree(lbuf); kfree(rbuf); kfree(dst); return rc; }

    const struct embk_node_header *lh = (const struct embk_node_header *)lbuf;
    const struct embk_node_header *rh = (const struct embk_node_header *)rbuf;
    if (lh->level != child_level || rh->level != child_level) { kfree(lbuf); kfree(rbuf); kfree(dst); return -EMBK_EINVAL; }
    if (lh->nritems > EMBKFS_MAX_SLOTS || rh->nritems > EMBKFS_MAX_SLOTS) { kfree(lbuf); kfree(rbuf); kfree(dst); return -EMBK_EINVAL; }

    uint32_t n = lh->nritems + rh->nritems;
    struct embk_child *kids = kmalloc((uint64_t)n * sizeof *kids);
    if (!kids) { kfree(lbuf); kfree(rbuf); kfree(dst); return -EMBK_ENOMEM; }

    const struct embk_internal_slot *ls =
        (const struct embk_internal_slot *)(lbuf + sizeof(struct embk_node_header));
    const struct embk_internal_slot *rs =
        (const struct embk_internal_slot *)(rbuf + sizeof(struct embk_node_header));
    uint32_t at = 0;
    for (uint32_t i = 0; i < lh->nritems; i++) {
        memcpy(&kids[at].key, &ls[i].key, sizeof kids[at].key);
        memcpy(&kids[at].ptr, &ls[i].ptr, sizeof kids[at].ptr);
        at++;
    }
    for (uint32_t i = 0; i < rh->nritems; i++) {
        memcpy(&kids[at].key, &rs[i].key, sizeof kids[at].key);
        memcpy(&kids[at].ptr, &rs[i].ptr, sizeof kids[at].ptr);
        at++;
    }

    rc = embk_emit_internal_list(vol, kids, n, child_level, new_gen, dst, out, out_n);

    kfree(kids);
    kfree(lbuf);
    kfree(rbuf);
    kfree(dst);
    return rc;
}

static int embk_rebalance_children(struct embkfs_volume *vol,
                                   struct embk_child *kids, uint32_t *inout_kn,
                                   uint8_t parent_level, uint64_t new_gen)
{
    if (!kids || !inout_kn || *inout_kn < 2 || parent_level == 0) return EMBK_OK;

    uint32_t kn = *inout_kn;
    uint32_t i = 0;
    while (i + 1 < kn) {
        uint8_t ll = 0, rl = 0;
        uint32_t ln = 0, rn = 0;
        int rc = embk_child_shape(vol, &kids[i].ptr, &ll, &ln);
        if (rc != EMBK_OK) return rc;
        rc = embk_child_shape(vol, &kids[i + 1].ptr, &rl, &rn);
        if (rc != EMBK_OK) return rc;
        if (ll != rl) return -EMBK_EINVAL;

        uint32_t min_slots = (ll == 0) ? 2u : ((EMBKFS_MAX_SLOTS + 1u) / 2u);
        if (ln >= min_slots && rn >= min_slots) { i++; continue; }

        struct embk_child repl[2];
        uint32_t rn_out = 0;
        if (ll == 0)
            rc = embk_rebalance_leaf_pair(vol, &kids[i].ptr, &kids[i + 1].ptr,
                                          new_gen, repl, &rn_out);
        else
            rc = embk_rebalance_internal_pair(vol, &kids[i].ptr, &kids[i + 1].ptr,
                                              ll, new_gen, repl, &rn_out);
        if (rc != EMBK_OK) return rc;
        if (rn_out == 0 || rn_out > 2) return -EMBK_EINVAL;

        embkfs_note_freed(vol, kids[i].ptr.block);
        embkfs_note_freed(vol, kids[i + 1].ptr.block);

        kids[i] = repl[0];
        if (rn_out == 2) {
            kids[i + 1] = repl[1];
            i++;
            continue;
        }

        for (uint32_t k = i + 1; k + 1 < kn; k++)
            kids[k] = kids[k + 1];
        kn--;
        if (i > 0) i--;
    }

    *inout_kn = kn;
    return EMBK_OK;
}

/* The COW write engine (recursive core). Apply ops — puts and deletes — to the
 * subtree at *ptr, write every new block into free space, and return the 0/1/2
 * child entries this subtree now contributes to its parent (see the split/merge
 * note above). `*out_level` is the level of those nodes (so the root wrapper can
 * build a parent one level up). Old blocks are never modified — the live tree
 * stays whole until the superblock swap — and each superseded node is recorded
 * for reclamation. Every child entry's key is its subtree's true minimum, so the
 * spec §8.2 routing invariant holds by construction.
 *
 * Each recursion level owns its node buffer, held across the recursive call;
 * depth = tree height, not the block count. */
static int embkfs_cow_apply_rec(struct embkfs_volume *vol, const struct embk_block_ptr *ptr,
                                const struct embk_put *ops, uint32_t nops, uint64_t new_gen,
                                struct embk_child *out, uint32_t *out_n, uint8_t *out_level)
{
    uint8_t *nodebuf = kmalloc(vol->block_size);
    if (!nodebuf) return -EMBK_ENOMEM;
    int rc = embkfs_read_node(vol, ptr, nodebuf, vol->block_size);   /* verifies */
    if (rc != EMBK_OK) { kfree(nodebuf); return rc; }
    struct embk_node_header *h = (struct embk_node_header *)nodebuf;

    if (h->level == 0) {
        *out_level = 0;
        struct embk_litem *items; uint32_t n;
        rc = embk_leaf_build_items(vol, nodebuf, ops, nops, &items, &n);
        if (rc != EMBK_OK) { kfree(nodebuf); return rc; }
        embkfs_note_freed(vol, ptr->block);            /* old leaf superseded */

        uint8_t *dst = kmalloc(vol->block_size);
        if (!dst) { kfree(items); kfree(nodebuf); return -EMBK_ENOMEM; }
        rc = embk_emit_leaf_list(vol, items, n, new_gen, dst, out, out_n);
        kfree(dst); kfree(items); kfree(nodebuf);
        return rc;
    }

    /* INTERNAL: route ops to children, recurse, and reassemble the slot list. */
    if (h->nritems == 0 || h->nritems > EMBKFS_MAX_SLOTS) { kfree(nodebuf); return -EMBK_EINVAL; }
    *out_level = h->level;
    struct embk_internal_slot *slots =
        (struct embk_internal_slot *)(nodebuf + sizeof(struct embk_node_header));

    int               *slot_of = kmalloc((uint64_t)nops * sizeof *slot_of);
    struct embk_put   *sub     = kmalloc((uint64_t)nops * sizeof *sub);
    struct embk_child *kids    = kmalloc((uint64_t)(2 * h->nritems) * sizeof *kids);  /* <=2 per child */
    if (!slot_of || !sub || !kids) { kfree(slot_of); kfree(sub); kfree(kids); kfree(nodebuf); return -EMBK_ENOMEM; }

    for (uint32_t p = 0; p < nops; p++) {
        int chosen = -1;
        for (uint32_t i = 0; i < h->nritems; i++) {
            struct embk_key sk;
            memcpy(&sk, &slots[i].key, sizeof sk);
            if (embk_key_cmp(&sk, &ops[p].key) <= 0) chosen = (int)i;   /* rightmost <= */
            else break;
        }
        if (chosen < 0) { kfree(slot_of); kfree(sub); kfree(kids); kfree(nodebuf); return -EMBK_ENOENT; }
        slot_of[p] = chosen;
    }

    uint32_t kn = 0;
    for (uint32_t i = 0; i < h->nritems; i++) {
        uint32_t m = 0;
        for (uint32_t p = 0; p < nops; p++)
            if (slot_of[p] == (int)i) sub[m++] = ops[p];

        if (m == 0) {                                  /* untouched: keep the slot */
            memcpy(&kids[kn].key, &slots[i].key, sizeof kids[kn].key);
            memcpy(&kids[kn].ptr, &slots[i].ptr, sizeof kids[kn].ptr);
            kn++;
            continue;
        }
        struct embk_block_ptr child_ptr;
        memcpy(&child_ptr, &slots[i].ptr, sizeof child_ptr);
        struct embk_child cout[2];
        uint32_t          cn;
        uint8_t           clevel;
        rc = embkfs_cow_apply_rec(vol, &child_ptr, sub, m, new_gen, cout, &cn, &clevel);
        if (rc != EMBK_OK) { kfree(slot_of); kfree(sub); kfree(kids); kfree(nodebuf); return rc; }
        for (uint32_t c = 0; c < cn; c++) kids[kn++] = cout[c];   /* 0, 1, or 2 entries */
    }
    kfree(slot_of); kfree(sub);

    embkfs_note_freed(vol, ptr->block);                /* old internal node superseded */

    rc = embk_rebalance_children(vol, kids, &kn, h->level, new_gen);
    if (rc != EMBK_OK) { kfree(kids); kfree(nodebuf); return rc; }

    uint8_t *dst = kmalloc(vol->block_size);
    if (!dst) { kfree(kids); kfree(nodebuf); return -EMBK_ENOMEM; }
    rc = embk_emit_internal_list(vol, kids, kn, h->level, new_gen, dst, out, out_n);
    kfree(dst); kfree(kids); kfree(nodebuf);
    return rc;
}

/* Public entry: apply ops to the whole tree, returning the new root pointer.
 * Reconciles what the root's rebuild produced:
 *   1 child  -> that is the new root (common case);
 *   2 children -> the root split, so build a new internal root one level up
 *                (the tree GREW a level);
 *   0 children -> the tree emptied (not expected — the root dir inode always
 *                exists), so install an empty root leaf to stay well-formed. */
static int embkfs_cow_apply(struct embkfs_volume *vol, const struct embk_block_ptr *ptr,
                            const struct embk_put *ops, uint32_t nops,
                            uint64_t new_gen, struct embk_block_ptr *out_root)
{
    struct embk_child top[2];
    uint32_t          tn;
    uint8_t           tlevel;
    int rc = embkfs_cow_apply_rec(vol, ptr, ops, nops, new_gen, top, &tn, &tlevel);
    if (rc != EMBK_OK) return rc;

    if (tn == 1) {
        struct embk_block_ptr root = top[0].ptr;
        uint8_t *buf = kmalloc(vol->block_size);
        if (!buf) return -EMBK_ENOMEM;

        for (;;) {
            rc = embkfs_read_node(vol, &root, buf, vol->block_size);
            if (rc != EMBK_OK) { kfree(buf); return rc; }

            const struct embk_node_header *h = (const struct embk_node_header *)buf;
            if (h->level == 0 || h->nritems != 1) break;

            const struct embk_internal_slot *slots =
                (const struct embk_internal_slot *)(buf + sizeof(struct embk_node_header));
            struct embk_block_ptr child;
            memcpy(&child, &slots[0].ptr, sizeof child);
            embkfs_note_freed(vol, root.block);      /* collapsed level, reclaim old root node */
            root = child;
        }
        kfree(buf);
        *out_root = root;
        return EMBK_OK;
    }

    uint8_t *dst = kmalloc(vol->block_size);
    if (!dst) return -EMBK_ENOMEM;
    struct embk_child newroot;
    if (tn == 2)                                   /* root split: grow a level */
        rc = embk_emit_internal(vol, top, 0, 2, tlevel + 1, new_gen, dst, &newroot);
    else                                           /* tn == 0: empty tree */
        rc = embk_emit_leaf(vol, NULL, 0, 0, new_gen, dst, &newroot);
    kfree(dst);
    if (rc == EMBK_OK) *out_root = newroot.ptr;
    return rc;
}


/* Finish a COW transaction whose new tree is already staged at *new_root: the
 * two-flush barrier protocol that makes the swap crash-safe, then advance the
 * in-memory root + generation to match committed disk (only on full success).
 *
 * `new_free` is the free-block count the new tree will present at next mount —
 * the caller derives it from the transaction's net block delta (see
 * embkfs_txn_new_free), which captures data writes, metadata rewrites, and node
 * splits/merges alike. Dead blocks are simply not reached by the next mount's
 * tree walk. */
static int embkfs_commit(struct embkfs_volume *vol,
                         const struct embk_block_ptr *new_root,
                         uint64_t new_gen, uint64_t new_free)
{
    const char *dev = vol->dev->name;

    /* BARRIER: force the whole new tree durable BEFORE the superblock that names
     * it. A drive's write-back cache can make writes durable out of issue order,
     * so issuing the superblock last is not enough; without this a power loss
     * could leave a valid superblock pointing at a spine that never landed. */
    int rc = embk_block_flush(vol->dev);
    if (rc != EMBK_OK) { kprintf("EMBKFS: %s: pre-commit flush failed: %s\n", dev, embk_strerror(rc)); return rc; }

    /* THE COMMIT: install a new superblock naming the new root, generation bumped.
     * Until this write lands, the old superblock still names the old tree. */
    rc = embkfs_write_superblock(vol, new_root, new_gen, new_free);
    if (rc != EMBK_OK) { kprintf("EMBKFS: %s: commit failed: %s\n", dev, embk_strerror(rc)); return rc; }

    /* SEAL: flush again so the superblock itself is durable before we report
     * success. The new tree is already durable, so a crash here is still safe —
     * remount sees the old superblock and the intact old tree. */
    rc = embk_block_flush(vol->dev);
    if (rc != EMBK_OK) { kprintf("EMBKFS: %s: commit-seal flush failed: %s\n", dev, embk_strerror(rc)); return rc; }

    vol->root        = *new_root;
    vol->generation  = new_gen;
    vol->free_blocks = new_free;
    return EMBK_OK;
}

/* Metadata transaction manager.
 * Required ordering for every metadata update:
 *   1) Begin transaction (start allocation/supersede tracking)
 *   2) Modify metadata ops (caller builds ops[])
 *   3) Allocate/COW rewrite blocks while applying ops to tree
 *   4) Update tree root candidate
 *   5) Write checksums (node CRCs + superblock CRC done in COW/commit path)
 *   6) Commit (publish new superblock/root)
 * Never partially update metadata: success publishes all, failure publishes none. */
static int embkfs_txn_apply_ops(struct embkfs_volume *vol,
                                const struct embk_put *ops, uint32_t nops,
                                uint64_t new_gen)
{
    struct embk_txn txn;
    int rc = embkfs_txn_begin(vol, &txn);
    if (rc != EMBK_OK) return rc;

    struct embk_block_ptr new_root;
    rc = embkfs_cow_apply(vol, &vol->root, ops, nops, new_gen, &new_root);
    if (rc == EMBK_OK) {
        uint64_t new_free;
        rc = embkfs_txn_new_free(vol, &new_free);
        if (rc == EMBK_OK)
            rc = embkfs_commit(vol, &new_root, new_gen, new_free);
    }
    embkfs_txn_end(vol, &txn, rc == EMBK_OK);
    return rc;
}

static bool embk_bytes_all_zero(const uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        if (p[i] != 0)
            return false;
    return true;
}


/* Write `len` bytes as the entire contents of regular file `oid`, in ONE atomic
 * COW transaction. Works for any length the volume can hold in <= EMBK_TXN_RUNS
 * extents: the contents are laid out as contiguous runs (one extent each), so a
 * file gets a single extent on unfragmented space and several when fragmented.
 *
 * len == 0 is truncate-to-empty: no extents are written, all old extents are
 * deleted, the inode goes to size 0.
 *
 * Note the signature: len is now uint64_t (was uint32_t). Existing call sites
 * that pass a small length still compile.
 *
 * THE "EXTENT-SUPERSEDE" BUG: no longer reproducible, and NOT knowingly fixed.
 * Read that second clause carefully before trusting this code.
 *
 * The original report (v2 Phase 0) said a shrinking write through
 * embkfs_truncate_object() failed with -EMBK_EINVAL when the object's prior
 * data happened to land as ONE multi-block extent instead of several
 * single-block ones, and called itself "100% reproducible" via
 * `test embkfs timestamps` immediately followed by `test embkfs obj` on a
 * freshly formatted volume.
 *
 * Re-checked 2026-07-23: that sequence now passes, AND it passes with the
 * triggering shape genuinely present -- the log shows the 4103-byte write
 * landing as `1 extent, 2 blk`, exactly the geometry the report blames, and
 * the following truncate succeeding. So the failure is gone from the case
 * that produced it.
 *
 * What was NOT established: why. No fix was identified. The two things the
 * original note suspected are unchanged in this function since the report --
 * puts_cap is still `1 + old_n + max_new`, and the allocate/write/supersede
 * loop is structurally what it was (checked against 00fa091). Something else
 * in the intervening work moved, or the failure needed a bitmap state finer
 * than "one extent vs several". Either way, "cannot reproduce" is not "fixed",
 * and this comment will not pretend otherwise.
 *
 * What DID change is that the invariant is now tested instead of hoped for:
 * `test embkfs shrink` (embkfs_run_shrink_selftests) runs twelve shrinking
 * writes across whatever extent shapes the volume produces -- including
 * truncate-to-empty, block-boundary and off-by-one cuts, and a hole-bearing
 * file -- and checks the surviving BYTES, not just the return code. It reports
 * which shapes it actually saw, so a run that never hit the single
 * multi-block extent cannot be mistaken for one that did.
 *
 * The original repro's real defect is worth remembering: it depended on the
 * free-block bitmap state two unrelated tests happened to leave behind. That
 * is not a repro, it is a coincidence with a procedure attached, and it decayed
 * into a passing sequence that could not tell "fixed" from "hidden". Phase 3
 * (compression) and Phase 4 (encryption) still add extent-shape variation on
 * this exact path, so if the failure returns, `test embkfs shrink` is where it
 * should surface -- and it will name the shape it was holding at the time. */
static int embkfs_write_file(struct embkfs_volume *vol, uint64_t oid,
                             const uint8_t *newdata, uint64_t len)
{
    const char *dev = vol->dev->name;
    static uint8_t datablk[4096];
    static uint8_t probe[4096];

    struct embk_extent_item *exts = NULL;
    struct embk_put *puts = NULL;
    struct embk_extref *old_ext = NULL;
    uint64_t *new_off = NULL;

    if (vol->read_only) return -EMBK_EROFS;
    if (vol->block_size > sizeof datablk) {
        kprintf("EMBKFS: %s: block_size %lu exceeds write buffer\n", dev, vol->block_size); return -EMBK_EINVAL;
    }
    if (len > 0 && !newdata) {
        kprintf("EMBKFS: %s: write object %lu got NULL data with non-zero len\n", dev, oid);
        return -EMBK_EINVAL;
    }

    /* 1. inode: must exist and be a regular file. Copy out before probe reuse. */
    const struct embk_item_header *ii =
        embkfs_find_item(vol, oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ii) { kprintf("EMBKFS: %s: object %lu has no inode\n", dev, oid); return -EMBK_ENOENT; }
    const struct embk_inode_item *old_ino = embk_item_data(probe, vol->block_size, ii, sizeof *old_ino);
    if (!old_ino) return -EMBK_EINVAL;
    uint32_t omode = old_ino->mode & EMBKFS_S_IFMT;
    if (omode != EMBKFS_S_IFREG && omode != EMBKFS_S_IFLNK) {
        kprintf("EMBKFS: %s: object %lu is not a writable file/symlink\n", dev, oid); return -EMBK_EINVAL;
    }
    struct embk_inode_item ino = *old_ino;

    /* 2. enumerate current extents: their runs are the data this write
     *    supersedes, and their keys are the ones we may have to delete. */
    uint32_t old_n = 0;
    int rc = embkfs_count_extents(vol, oid, &old_n);
    if (rc != EMBK_OK) return rc;
    if (old_n) {
        old_ext = kmalloc((uint64_t)old_n * sizeof *old_ext);
        if (!old_ext) return -EMBK_ENOMEM;
        uint32_t got = 0; bool over = false;
        rc = embkfs_collect_extents(vol, oid, old_ext, old_n, &got, &over);
        if (rc != EMBK_OK || over || got != old_n) { kfree(old_ext); return -EMBK_EINVAL; }
        rc = embkfs_validate_extent_map(vol, old_ext, old_n, ino.size, "write_file old map");
        if (rc != EMBK_OK) { kfree(old_ext); return rc; }
    }

    uint32_t max_new = (len == 0) ? 0 : (uint32_t)((len + vol->block_size - 1) / vol->block_size);
    if (max_new) {
        exts = kmalloc((uint64_t)max_new * sizeof *exts);
        new_off = kmalloc((uint64_t)max_new * sizeof *new_off);
        if (!exts || !new_off) { kfree(old_ext); kfree(exts); kfree(new_off); return -EMBK_ENOMEM; }
    }
    uint32_t puts_cap = 1 + old_n + max_new;
    puts = kmalloc((uint64_t)puts_cap * sizeof *puts);
    if (!puts) { kfree(old_ext); kfree(exts); kfree(new_off); return -EMBK_ENOMEM; }

    uint64_t new_gen = vol->generation + 1;
    uint64_t spb     = vol->block_size / vol->dev->block_size;
    uint32_t nputs   = 0;
    uint32_t new_n   = 0;

    struct embk_txn txn;
    rc = embkfs_txn_begin(vol, &txn);
    if (rc != EMBK_OK) { kfree(old_ext); kfree(exts); kfree(new_off); kfree(puts); return rc; }

    /* supersede every old data run (reclaimed once the commit is durable) */
    for (uint32_t i = 0; i < old_n; i++)
        embkfs_note_freed_run(vol, old_ext[i].disk_block, old_ext[i].length);

    /* 3. lay the new contents out as one or more extents, writing each data run.
     *    Old data stays marked used (only NOTED freed), so allocation never
     *    reuses it before the commit — the live tree remains intact on a crash. */
    uint64_t remaining = len, foff = 0, total_blocks = 0;
    bool any_compressed = false;
    while (remaining > 0 && rc == EMBK_OK) {
        if (new_n >= max_new) { rc = -EMBK_EINVAL; break; }
        uint64_t need = (remaining + vol->block_size - 1) / vol->block_size;   /* blocks still to place */

        /* Sparse hole synthesis: contiguous full zero blocks become logical-only extents. */
        if (remaining >= vol->block_size && embk_bytes_all_zero(newdata + foff, vol->block_size)) {
            uint64_t hole_blocks = 1;
            while (hole_blocks < need) {
                uint64_t off = foff + hole_blocks * vol->block_size;
                if (!embk_bytes_all_zero(newdata + off, vol->block_size)) break;
                hole_blocks++;
            }
            uint64_t hole_bytes = hole_blocks * vol->block_size;

            struct embk_extent_item *e = &exts[new_n];
            memset(e, 0, sizeof *e);
            e->disk_block   = 0;
            e->length       = 0;
            e->logical_size = hole_bytes;
            e->checksum     = 0;
            e->generation   = new_gen;
            e->flags        = EMBKFS_EXTENT_F_HOLE;

            new_off[new_n] = foff;
            if (nputs >= puts_cap) { rc = -EMBK_EINVAL; break; }
            puts[nputs++] = (struct embk_put){
                .key  = { .object_id = oid, .type = EMBK_TYPE_EXTENT, .offset = foff },
                .data = (const uint8_t *)e, .size = sizeof *e };
            new_n++;
            foff      += hole_bytes;
            remaining -= hole_bytes;
            continue;
        }

        uint64_t start, got;
        rc = embkfs_alloc_run(vol, need, &start, &got);
        if (rc != EMBK_OK) break;
        if (got == 0) { rc = -EMBK_ENOSPC; break; }

        uint64_t ext_bytes = got * vol->block_size;
        if (ext_bytes > remaining) ext_bytes = remaining;     /* only the tail extent is partial */

        /* Try compression before committing to the full `got` blocks: if
         * the payload shrinks by at least one whole block, give the unused
         * tail of this run back to the allocator instead of writing (and
         * keeping) it. Below two blocks' worth there's no way to save even
         * one whole block, so don't bother -- compressing a few bytes is
         * pure overhead (v2.2 Phase 3). */
        bool compressed = false;
        uint8_t *comp_buf = NULL;
        uint32_t comp_len = 0;
        uint64_t comp_blocks = got;
        if (ext_bytes >= 2 * vol->block_size) {
            uint32_t bound = embk_compress_bound((uint32_t)ext_bytes);
            comp_buf = kmalloc(bound);
            if (comp_buf && embk_compress(newdata + foff, (uint32_t)ext_bytes, comp_buf, bound, &comp_len)) {
                uint64_t need_blocks = (comp_len + vol->block_size - 1) / vol->block_size;
                if (need_blocks < got) {
                    compressed = true;
                    comp_blocks = need_blocks;
                }
            }
        }

        /* write this run block-by-block, threading the CRC over whatever
         * bytes actually land on disk (compressed and/or encrypted
         * payload, or plain logical bytes otherwise -- spec 9.3's
         * checksum covers the DATA that's really there, verified before
         * any decryption/decompression on read). Encryption is always the
         * LAST transform: it runs on the exact bytes about to hit the
         * platter, after compression has already decided the payload
         * (v2.2 Phase 4's "compress, then encrypt" ordering) -- and
         * because it turns the zero-padding of a partial last block into
         * ciphertext too, an encrypted extent's checksum covers the WHOLE
         * block_size per block, not just the `chunk` of real bytes (an
         * unencrypted extent has no such padding-vs-real distinction to
         * make on read, so it keeps checksumming only the real bytes). */
        uint32_t csum = 0; uint64_t written = 0;
        uint64_t write_blocks = compressed ? comp_blocks : got;
        const uint8_t *src_bytes = compressed ? comp_buf : (newdata + foff);
        uint64_t src_len = compressed ? comp_len : ext_bytes;
        for (uint64_t blk = 0; blk < write_blocks && rc == EMBK_OK; blk++) {
            uint64_t chunk = src_len - written;
            if (chunk > vol->block_size) chunk = vol->block_size;
            memset(datablk, 0, vol->block_size);              /* zero-pad a partial last block */
            if (chunk) memcpy(datablk, src_bytes + written, chunk);
            uint64_t csum_len = chunk;
            if (vol->encrypted) {
                aes_xts_encrypt(embkfs_xts(vol), start + blk, datablk, datablk, vol->block_size);
                csum_len = vol->block_size;
            }
            rc = embk_block_write(vol->dev, (start + blk) * spb, spb, datablk);
            if (rc == EMBK_OK) { csum = embk_crc32c(datablk, csum_len, csum); written += chunk; }
        }
        kfree(comp_buf);
        if (rc != EMBK_OK) break;

        if (compressed && comp_blocks < got) {
            embkfs_note_freed_run(vol, start + comp_blocks, got - comp_blocks);
        }

        struct embk_extent_item *e = &exts[new_n];
        memset(e, 0, sizeof *e);
        e->disk_block   = start;
        e->length       = write_blocks;
        e->logical_size = ext_bytes;
        e->checksum     = csum;
        e->generation   = new_gen;
        if (compressed) {
            e->flags |= EMBKFS_EXTENT_F_COMPRESSED;
            embk_extent_set_compressed_size(e, comp_len);
            any_compressed = true;
        }
        if (vol->encrypted) {
            e->flags |= EMBKFS_EXTENT_F_ENCRYPTED;
        }

        new_off[new_n] = foff;
        if (nputs >= puts_cap) { rc = -EMBK_EINVAL; break; }
        puts[nputs++] = (struct embk_put){
            .key  = { .object_id = oid, .type = EMBK_TYPE_EXTENT, .offset = foff },
            .data = (const uint8_t *)e, .size = sizeof *e };
        new_n++;

        total_blocks += write_blocks;
        foff         += ext_bytes;
        remaining    -= ext_bytes;
    }

    if (rc == EMBK_OK) {
        /* 4. updated inode: new size, block count, generation, timestamps.
         * This is the one place every content-mutating public API funnels
         * through (embkfs_write_object/_at/_append_object/_truncate_object/
         * _resize_object all call embkfs_write_file) -- mtime AND ctime
         * both move: the data changed (mtime) and so did the inode itself
         * (ctime), the standard POSIX pairing for a real write. */
        uint64_t now_ns = rtc_now_ns();
        ino.size       = len;
        ino.blocks     = total_blocks;
        ino.mtime      = now_ns;
        ino.ctime      = now_ns;
        ino.generation = new_gen;
        embk_inode_set_writer_pid(&ino, current_thread ? current_process->pid : 0);
        if (nputs >= puts_cap) { rc = -EMBK_EINVAL; }
        if (rc == EMBK_OK) puts[nputs++] = (struct embk_put){
            .key  = { .object_id = oid, .type = EMBK_TYPE_INODE, .offset = 0 },
            .data = (const uint8_t *)&ino, .size = sizeof ino };

        /* 5. delete every OLD extent key the new layout does not reuse. (A new
         *    extent at the same file offset is a PUT that replaces it instead.) */
        for (uint32_t i = 0; i < old_n; i++) {
            bool reused = false;
            for (uint32_t j = 0; j < new_n; j++)
                if (new_off[j] == old_ext[i].offset) { reused = true; break; }
            if (!reused) {
                if (nputs >= puts_cap) { rc = -EMBK_EINVAL; break; }
                puts[nputs++] = (struct embk_put){
                    .key = { .object_id = oid, .type = EMBK_TYPE_EXTENT, .offset = old_ext[i].offset },
                    .del = true };
            }
        }

        /* 6. one atomic commit: inode + all extent puts/deletes route together.
         * If this write produced a compressed extent, the INCOMPAT bit must
         * land in the SAME superblock write as the extent itself -- setting
         * it here (before the commit that persists both) keeps that atomic;
         * if the commit fails, neither the bit nor the extent persist. */
        if (rc == EMBK_OK) {
            if (any_compressed) vol->feature_incompat |= EMBKFS_INCOMPAT_COMPRESSION;
            struct embk_block_ptr new_root;
            rc = embkfs_cow_apply(vol, &vol->root, puts, nputs, new_gen, &new_root);
            if (rc == EMBK_OK) {
                uint64_t new_free;
                rc = embkfs_txn_new_free(vol, &new_free);   /* new data - freed old data +/- node splits */
                if (rc == EMBK_OK)
                    rc = embkfs_commit(vol, &new_root, new_gen, new_free);
            }
        }
    }

    /* 7. reconcile: reclaim old data + orphaned nodes on success, else roll back. */
    embkfs_txn_end(vol, &txn, rc == EMBK_OK);
    kfree(old_ext);
    kfree(exts);
    kfree(new_off);
    kfree(puts);
    if (rc != EMBK_OK) { kprintf("EMBKFS: %s: write object %lu failed: %s\n", dev, oid, embk_strerror(rc)); return rc; }

    kprintf("EMBKFS: %s: wrote object %lu (%lu bytes, %u extent%s, %lu blk), gen now %lu, free %lu\n",
            dev, oid, len, new_n, new_n == 1 ? "" : "s", total_blocks,
            vol->generation, vol->free_blocks);
    return EMBK_OK;
}


/* Create a new object `name` in directory `dir_oid`, as ONE atomic COW
 * transaction — the shared core of embkfs_create (files) and embkfs_mkdir
 * (directories). `mode` is the full POSIX st_mode (type bits decide the rest):
 *
 *   - the new object's INODE is written: a directory starts with link count 2
 *     (the parent's entry for it, plus its own conceptual "."), a file with 1;
 *   - a DIR_ENTRY naming it is added to the parent (a fresh item, or a record
 *     appended to an existing name-hash collision chain), tagged with the right
 *     target_type so a listing needn't open each child;
 *   - for a directory ONLY, the parent's inode link count is bumped by one — the
 *     new child's conceptual ".." raises the parent's nlink (standard POSIX
 *     directory bookkeeping, the same reason an empty dir is nlink 2).
 *
 * All of these items commit together under a single superblock swap, so a crash
 * never leaves a half-made object. "." and ".." are not stored as on-disk
 * entries (matching how the formatter builds the root); the link counts carry
 * that relationship numerically. No data block is allocated (an empty object has
 * none), so the free count is unchanged. The new object id is returned in
 * *out_oid. */
static int embkfs_make_object(struct embkfs_volume *vol, uint64_t dir_oid,
                              const char *name, uint32_t mode, uint64_t *out_oid)
{
    const char *dev = vol->dev->name;
    static uint8_t probe[4096];          /* descent buffer, reused between lookups */
    bool is_dir = (mode & EMBKFS_S_IFMT) == EMBKFS_S_IFDIR;

    if (vol->read_only) return -EMBK_EROFS;
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -EMBK_EINVAL;

    /* (a) reject a duplicate name. embkfs_lookup also proves dir_oid is a real
     *     directory (it validates the dir inode), so this guards both at once. */
    uint64_t existing;
    int look = embkfs_lookup(vol, dir_oid, name, &existing);
    if (look == EMBK_OK) {
        kprintf("EMBKFS: %s: \"%s\" already exists (object %lu)\n", dev, name, existing);
        return -EMBK_EEXIST;
    }
    if (look != -EMBK_ENOENT) return look;        /* dir missing / not a dir / corrupt */

    uint64_t new_oid = vol->next_oid;
    uint64_t new_gen = vol->generation + 1;

    /* (b) the new object's inode: empty, links 2 for a directory else 1.
     * btime/mtime/ctime/atime all equal at creation -- the object has never
     * been modified OR read since it was born, so "created"/"last
     * modified"/"last accessed" are the same instant, standard Unix
     * convention.
     *
     * atime scoping decision (v2.2): this is the ONLY one of the four
     * timestamp fields that is write-once rather than kept live. Updating
     * it on every read would mean the READ path has to become CoW-write-
     * capable (allocate a txn, rebuild the tree spine, bump the
     * superblock generation) purely to record that a read happened --
     * real cost for every single read, forever, to maintain a field whose
     * own reputation is bad enough that Linux's `relatime`/`noatime`
     * mount options exist specifically to avoid this exact tradeoff on
     * production filesystems. Not implemented here for the same reason
     * those exist; revisit only if something concretely needs a live
     * atime (see docs/EMBKFS_spec_v2.2.md's timestamps section). */
    uint64_t now_ns = rtc_now_ns();
    struct embk_inode_item ino;
    memset(&ino, 0, sizeof ino);
    ino.size       = 0;
    ino.blocks     = 0;
    ino.links      = is_dir ? 2 : 1;
    ino.mode       = mode;
    ino.atime      = now_ns;
    ino.btime      = now_ns;
    ino.mtime      = now_ns;
    ino.ctime      = now_ns;
    ino.generation = new_gen;
    embk_inode_set_writer_pid(&ino, current_thread ? current_process->pid : 0);

    /* (c) read the parent inode: its own CONTENT is changing (a new
     * directory entry is being added under it) regardless of whether the
     * new object is a file or a subdirectory, so mtime/ctime always move --
     * only the link-count bump (for the new child's own "..") is
     * directory-specific. Pre-v2.2 this block only ran for is_dir (link
     * count is the only thing a plain file creation used to need from its
     * parent), which meant a directory's own mtime never reflected new
     * FILES being created inside it -- a real gap, fixed here while this
     * exact code is already being touched for timestamps, not introduced
     * by them. Copy the inode out before probe is reused by the dir-entry
     * descent below. */
    const struct embk_item_header *pi =
        embkfs_find_item(vol, dir_oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!pi) { kprintf("EMBKFS: %s: parent object %lu has no inode\n", dev, dir_oid); return -EMBK_ENOENT; }
    const struct embk_inode_item *p = embk_item_data(probe, vol->block_size, pi, sizeof *p);
    if (!p) return -EMBK_EINVAL;
    struct embk_inode_item parent_ino = *p;
    if (is_dir) parent_ino.links += 1;          /* the new child's ".." */
    parent_ino.mtime      = now_ns;
    parent_ino.ctime      = now_ns;
    parent_ino.generation = new_gen;

    /* (d) the directory entry. Its key offset is the name hash; if that hash is
     *     already present (a collision chain), append a record to the existing
     *     item, otherwise start a fresh single-record item. */
    uint32_t hash = embk_crc32c(name, name_len, 0);
    const struct embk_item_header *de =
        embkfs_find_item(vol, dir_oid, EMBK_TYPE_DIR_ENTRY, hash, probe, sizeof probe);

    uint32_t       old_chain      = 0;
    const uint8_t *old_chain_data = NULL;
    if (de) {
        old_chain      = de->size;
        old_chain_data = embk_item_data(probe, vol->block_size, de, old_chain);
        if (!old_chain_data) return -EMBK_EINVAL;
    }
    uint32_t rec_len   = sizeof(struct embk_dir_entry_item) + (uint32_t)name_len;
    uint32_t new_chain = old_chain + rec_len;
    uint8_t *dirent = kmalloc(new_chain);
    if (!dirent) return -EMBK_ENOMEM;
    if (old_chain) memcpy(dirent, old_chain_data, old_chain);   /* keep existing records */
    struct embk_dir_entry_item *rec = (struct embk_dir_entry_item *)(dirent + old_chain);
    memset(rec, 0, sizeof *rec);
    uint8_t dtype = is_dir ? EMBKFS_DT_DIR
                           : (((mode & EMBKFS_S_IFMT) == EMBKFS_S_IFLNK) ? EMBKFS_DT_LNK : EMBKFS_DT_REG);
    rec->target_object_id = new_oid;
    rec->target_type      = dtype;
    rec->name_len         = (uint8_t)name_len;
    memcpy(dirent + old_chain + sizeof *rec, name, name_len);

    /* (e) one atomic commit carrying the new inode, the directory entry, and —
     *     for a directory — the parent inode with its bumped link count. They may
     *     land in the same leaf or different leaves; the engine routes each
     *     independently and rebuilds the shared spine once. */
    struct embk_put puts[3];
    uint32_t nputs = 0;
    puts[nputs++] = (struct embk_put){
        .key = { .object_id = new_oid, .type = EMBK_TYPE_INODE, .offset = 0 },
        .data = (const uint8_t *)&ino, .size = sizeof ino };
    puts[nputs++] = (struct embk_put){
        .key = { .object_id = dir_oid, .type = EMBK_TYPE_DIR_ENTRY, .offset = hash },
        .data = dirent, .size = new_chain };
    puts[nputs++] = (struct embk_put){
        .key = { .object_id = dir_oid, .type = EMBK_TYPE_INODE, .offset = 0 },
        .data = (const uint8_t *)&parent_ino, .size = sizeof parent_ino };

    /* Transaction: COW rebuilds a tree path whose old nodes become reclaimable
     * once committed. No data blocks are added, but inserting items can SPLIT a
     * full leaf (adding a node), so the free count comes from the txn delta. */
    int rc = embkfs_txn_apply_ops(vol, puts, nputs, new_gen);

    kfree(dirent);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: %s: make \"%s\" failed: %s\n", dev, name, embk_strerror(rc));
        return rc;
    }

    vol->next_oid += 1;          /* advance only after a durable commit */
    *out_oid = new_oid;
    kprintf("EMBKFS: %s: %s /%s -> object %lu (gen %lu)\n",
            dev, is_dir ? "mkdir" : "created", name, new_oid, vol->generation);
    return EMBK_OK;
}

/* Create an empty regular file `name` in directory `dir_oid`. Thin wrapper over
 * embkfs_make_object; data is written by a later embkfs_write_file call. */
static int embkfs_create(struct embkfs_volume *vol, uint64_t dir_oid,
                         const char *name, uint64_t *out_oid)
{
    return embkfs_make_object(vol, dir_oid, name, EMBKFS_S_IFREG | EMBKFS_PERM_FILE, out_oid);
}

/* Create an empty subdirectory `name` in directory `dir_oid`. The new directory
 * has no entries of its own yet (children are added by later create/mkdir calls
 * targeting *out_oid). */
static int embkfs_mkdir(struct embkfs_volume *vol, uint64_t dir_oid,
                        const char *name, uint64_t *out_oid)
{
    return embkfs_make_object(vol, dir_oid, name, EMBKFS_S_IFDIR | EMBKFS_PERM_DIR, out_oid);
}


/* Build the op that removes `name` from directory `dir_oid`'s entry chain: a
 * delete if it was the chain's only record, else a put of the shortened chain.
 * Shared by unlink and rmdir. On success *op is ready to hand to the engine and
 * *out_buf holds the new chain bytes the op points at (NULL for a delete) — the
 * caller must kfree(*out_buf) AFTER the commit. `probe` is a scratch node buffer.
 * Returns -EMBK_ENOENT if the name isn't in the chain. */
static int embkfs_dirent_remove_op(struct embkfs_volume *vol, uint64_t dir_oid,
                                   const char *name, size_t name_len,
                                   uint8_t *probe, size_t probe_sz,
                                   struct embk_put *op, uint8_t **out_buf)
{
    const char *dev = vol->dev->name;
    uint32_t hash = embk_crc32c(name, name_len, 0);
    const struct embk_item_header *de =
        embkfs_find_item(vol, dir_oid, EMBK_TYPE_DIR_ENTRY, hash, probe, probe_sz);
    if (!de) { kprintf("EMBKFS: %s: \"%s\" resolved but has no dir entry\n", dev, name); return -EMBK_EINVAL; }
    uint32_t chain_size = de->size;
    const uint8_t *chain = embk_item_data(probe, vol->block_size, de, chain_size);
    if (!chain) return -EMBK_EINVAL;

    uint8_t *newchain = kmalloc(chain_size ? chain_size : 1);   /* shrinks, never grows */
    if (!newchain) return -EMBK_ENOMEM;
    uint32_t newlen  = 0;
    bool     removed = false;
    for (uint32_t off = 0; off + sizeof(struct embk_dir_entry_item) <= chain_size; ) {
        const struct embk_dir_entry_item *r = (const struct embk_dir_entry_item *)(chain + off);
        uint32_t rl = sizeof *r + r->name_len;
        if (rl > chain_size - off) break;             /* malformed record guard */
        bool match = (!removed && r->name_len == name_len &&
                      memcmp(chain + off + sizeof *r, name, name_len) == 0);
        if (match) removed = true;                    /* drop this record */
        else { memcpy(newchain + newlen, chain + off, rl); newlen += rl; }
        off += rl;
    }
    if (!removed) { kfree(newchain); return -EMBK_ENOENT; }

    struct embk_key key = { .object_id = dir_oid, .type = EMBK_TYPE_DIR_ENTRY, .offset = hash };
    if (newlen == 0) {                                /* the name was the only record */
        kfree(newchain);
        *out_buf = NULL;
        *op = (struct embk_put){ .key = key, .del = true };
    } else {                                          /* other names shared the hash */
        *out_buf = newchain;
        *op = (struct embk_put){ .key = key, .data = newchain, .size = newlen };
    }
    return EMBK_OK;
}

/* Build the op that adds `name` -> target_oid into directory `dir_oid`'s hash
 * chain, creating or extending the item keyed by hash(name). Returns EEXIST if
 * that exact name is already present in the chain. */
static int embkfs_dirent_add_op(struct embkfs_volume *vol, uint64_t dir_oid,
                                const char *name, size_t name_len,
                                uint64_t target_oid, uint8_t target_type,
                                uint8_t *probe, size_t probe_sz,
                                struct embk_put *op, uint8_t **out_buf)
{
    uint32_t hash = embk_crc32c(name, name_len, 0);
    const struct embk_item_header *de =
        embkfs_find_item(vol, dir_oid, EMBK_TYPE_DIR_ENTRY, hash, probe, probe_sz);

    uint32_t old_chain = 0;
    const uint8_t *old_data = NULL;
    if (de) {
        old_chain = de->size;
        old_data = embk_item_data(probe, vol->block_size, de, old_chain);
        if (!old_data) return -EMBK_EINVAL;

        for (uint32_t off = 0; off + sizeof(struct embk_dir_entry_item) <= old_chain; ) {
            const struct embk_dir_entry_item *r = (const struct embk_dir_entry_item *)(old_data + off);
            uint32_t rl = sizeof *r + r->name_len;
            if (rl > old_chain - off) return -EMBK_EINVAL;
            if (r->name_len == name_len &&
                memcmp(old_data + off + sizeof *r, name, name_len) == 0)
                return -EMBK_EEXIST;
            off += rl;
        }
    }

    uint32_t rec_len = sizeof(struct embk_dir_entry_item) + (uint32_t)name_len;
    uint32_t new_chain = old_chain + rec_len;
    uint8_t *buf = kmalloc(new_chain);
    if (!buf) return -EMBK_ENOMEM;

    if (old_chain) memcpy(buf, old_data, old_chain);
    struct embk_dir_entry_item *rec = (struct embk_dir_entry_item *)(buf + old_chain);
    memset(rec, 0, sizeof *rec);
    rec->target_object_id = target_oid;
    rec->target_type = target_type;
    rec->name_len = (uint8_t)name_len;
    memcpy(buf + old_chain + sizeof *rec, name, name_len);

    *out_buf = buf;
    *op = (struct embk_put){
        .key = { .object_id = dir_oid, .type = EMBK_TYPE_DIR_ENTRY, .offset = hash },
        .data = buf,
        .size = new_chain,
    };
    return EMBK_OK;
}

/* Point an EXISTING dirent at a different object, in ONE put.
 *
 * The replace half of an atomic rename. add_op cannot serve: it refuses a name
 * that already exists (-EEXIST), and remove_op+add_op on the same key does NOT
 * compose -- each builds its buffer from the ORIGINAL on-disk chain, so the
 * second silently clobbers the first. Retargeting rewrites the record in place,
 * so the chain length never changes (same name, same length).
 *
 * -EMBK_ENOENT if the name isn't there: the caller looked it up already, so
 * this means the tree moved under us, not a routine miss. */
/* Defined further down (with the open-object table); rename's replace path needs
 * it to know whether a destination it is dropping is still held open. */
static bool embkfs_object_is_open(struct embkfs_volume *vol, uint64_t oid);

static int embkfs_dirent_retarget_op(struct embkfs_volume *vol, uint64_t dir_oid,
                                     const char *name, size_t name_len,
                                     uint64_t target_oid, uint8_t target_type,
                                     uint8_t *probe, size_t probe_sz,
                                     struct embk_put *op, uint8_t **out_buf)
{
    uint32_t hash = embk_crc32c(name, name_len, 0);
    const struct embk_item_header *de =
        embkfs_find_item(vol, dir_oid, EMBK_TYPE_DIR_ENTRY, hash, probe, probe_sz);
    if (!de) return -EMBK_ENOENT;

    uint32_t chain = de->size;
    const uint8_t *old_data = embk_item_data(probe, vol->block_size, de, chain);
    if (!old_data) return -EMBK_EINVAL;

    uint8_t *buf = kmalloc(chain ? chain : 1);
    if (!buf) return -EMBK_ENOMEM;
    memcpy(buf, old_data, chain);

    /* Walk OUR copy (probe gets reused by later find_item calls). */
    bool found = false;
    for (uint32_t off = 0; off + sizeof(struct embk_dir_entry_item) <= chain; ) {
        struct embk_dir_entry_item *r = (struct embk_dir_entry_item *)(buf + off);
        uint32_t rl = sizeof *r + r->name_len;
        if (rl > chain - off) { kfree(buf); return -EMBK_EINVAL; }
        if (r->name_len == name_len &&
            memcmp(buf + off + sizeof *r, name, name_len) == 0) {
            r->target_object_id = target_oid;
            r->target_type      = target_type;
            found = true;
            break;
        }
        off += rl;
    }
    if (!found) { kfree(buf); return -EMBK_ENOENT; }

    *out_buf = buf;
    *op = (struct embk_put){
        .key = { .object_id = dir_oid, .type = EMBK_TYPE_DIR_ENTRY, .offset = hash },
        .data = buf,
        .size = chain,
    };
    return EMBK_OK;
}

/* Read inode of oid into *out_ino and ensure it is a directory. */
static int embkfs_read_dir_inode(struct embkfs_volume *vol, uint64_t oid,
                                 uint8_t *probe, size_t probe_sz,
                                 struct embk_inode_item *out_ino)
{
    const struct embk_item_header *ii =
        embkfs_find_item(vol, oid, EMBK_TYPE_INODE, 0, probe, probe_sz);
    if (!ii) return -EMBK_ENOENT;
    const struct embk_inode_item *ino = embk_item_data(probe, vol->block_size, ii, sizeof *ino);
    if (!ino) return -EMBK_EINVAL;
    if ((ino->mode & EMBKFS_S_IFMT) != EMBKFS_S_IFDIR) return -EMBK_ENOTDIR;
    *out_ino = *ino;
    return EMBK_OK;
}

/* True if `needle_oid` appears anywhere under `dir_oid` (recursively through
 * directory entries). Depth-limited so a corrupted cycle cannot recurse forever. */
static int embkfs_dir_contains_oid_rec(struct embkfs_volume *vol, const struct embk_block_ptr *ptr,
                                       uint64_t dir_oid, uint64_t needle_oid,
                                       uint32_t depth, bool *found)
{
    if (*found) return EMBK_OK;
    if (depth > 64) return -EMBK_EINVAL;

    uint8_t *buf = kmalloc(vol->block_size);
    if (!buf) return -EMBK_ENOMEM;
    int rc = embkfs_read_node(vol, ptr, buf, vol->block_size);
    if (rc != EMBK_OK) { kfree(buf); return rc; }

    const struct embk_node_header *h = (const struct embk_node_header *)buf;
    const struct embk_key lo = { .object_id = dir_oid, .type = EMBK_TYPE_DIR_ENTRY, .offset = 0 };
    const struct embk_key hi = { .object_id = dir_oid, .type = EMBK_TYPE_DIR_ENTRY, .offset = UINT64_MAX };

    if (h->level == 0) {
        for (uint32_t i = 0; i < h->nritems && !*found; i++) {
            const struct embk_item_header *it = embk_leaf_item(buf, i);
            if (it->key.object_id != dir_oid || it->key.type != EMBK_TYPE_DIR_ENTRY) continue;

            const uint8_t *chain = embk_item_data(buf, vol->block_size, it, it->size);
            if (!chain) { kfree(buf); return -EMBK_EINVAL; }

            uint32_t left = it->size;
            while (left >= sizeof(struct embk_dir_entry_item) && !*found) {
                const struct embk_dir_entry_item *rec = (const struct embk_dir_entry_item *)chain;
                uint32_t rl = sizeof *rec + rec->name_len;
                if (rl > left) { kfree(buf); return -EMBK_EINVAL; }

                if (rec->target_object_id == needle_oid) {
                    *found = true;
                    break;
                }

                if (rec->target_type == EMBKFS_DT_DIR &&
                    rec->target_object_id != dir_oid &&
                    rec->target_object_id != EMBKFS_ROOT_OBJECT_ID) {
                    rc = embkfs_dir_contains_oid_rec(vol, &vol->root,
                                                     rec->target_object_id,
                                                     needle_oid,
                                                     depth + 1, found);
                    if (rc != EMBK_OK) { kfree(buf); return rc; }
                }

                chain += rl;
                left -= rl;
            }
        }
        kfree(buf);
        return EMBK_OK;
    }

    if (h->nritems == 0 || h->nritems > EMBKFS_MAX_SLOTS) { kfree(buf); return -EMBK_EINVAL; }
    const struct embk_internal_slot *slots =
        (const struct embk_internal_slot *)(buf + sizeof(struct embk_node_header));
    for (uint32_t i = 0; i < h->nritems && !*found; i++) {
        struct embk_key clo;
        memcpy(&clo, &slots[i].key, sizeof clo);
        if (embk_key_cmp(&clo, &hi) > 0) break;
        if (i + 1 < h->nritems) {
            struct embk_key chi;
            memcpy(&chi, &slots[i + 1].key, sizeof chi);
            if (embk_key_cmp(&chi, &lo) <= 0) continue;
        }
        struct embk_block_ptr cp;
        memcpy(&cp, &slots[i].ptr, sizeof cp);
        rc = embkfs_dir_contains_oid_rec(vol, &cp, dir_oid, needle_oid, depth, found);
        if (rc != EMBK_OK) { kfree(buf); return rc; }
    }
    kfree(buf);
    return EMBK_OK;
}

/* Rename/move a directory entry in one atomic transaction. For directory moves
 * across parents, update parent nlink counts and reject moves into own subtree. */
static int embkfs_rename(struct embkfs_volume *vol,
                         uint64_t old_dir_oid, const char *old_name,
                         uint64_t new_dir_oid, const char *new_name)
{
    const char *dev = vol->dev->name;
    static uint8_t probe[4096];

    if (vol->read_only) return -EMBK_EROFS;
    size_t old_len = strlen(old_name), new_len = strlen(new_name);
    if (old_len == 0 || old_len > 255 || new_len == 0 || new_len > 255) return -EMBK_EINVAL;
    if (old_dir_oid == new_dir_oid && old_len == new_len && memcmp(old_name, new_name, old_len) == 0)
        return EMBK_OK;

    uint64_t target;
    int rc = embkfs_lookup(vol, old_dir_oid, old_name, &target);
    if (rc != EMBK_OK) return rc;

    /* An existing destination is REPLACED, atomically, in the same commit that
     * moves the name -- POSIX rename semantics, and the reason this is a kernel
     * op at all. The libc used to fake it with unlink-then-rename, which loses
     * the destination if we die between the two. */
    uint64_t existing = 0;
    bool replacing = false;
    rc = embkfs_lookup(vol, new_dir_oid, new_name, &existing);
    if (rc == EMBK_OK) replacing = true;
    else if (rc != -EMBK_ENOENT) return rc;

    struct embk_inode_item old_parent;
    rc = embkfs_read_dir_inode(vol, old_dir_oid, probe, sizeof probe, &old_parent);
    if (rc != EMBK_OK) return rc;
    struct embk_inode_item new_parent;
    rc = embkfs_read_dir_inode(vol, new_dir_oid, probe, sizeof probe, &new_parent);
    if (rc != EMBK_OK) return rc;

    const struct embk_item_header *ti =
        embkfs_find_item(vol, target, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ti) return -EMBK_ENOENT;
    const struct embk_inode_item *ino = embk_item_data(probe, vol->block_size, ti, sizeof *ino);
    if (!ino) return -EMBK_EINVAL;
    bool is_dir = ((ino->mode & EMBKFS_S_IFMT) == EMBKFS_S_IFDIR);
    uint8_t ttype = is_dir ? EMBKFS_DT_DIR
                           : (((ino->mode & EMBKFS_S_IFMT) == EMBKFS_S_IFLNK) ? EMBKFS_DT_LNK : EMBKFS_DT_REG);

    if (is_dir && old_dir_oid != new_dir_oid) {
        if (new_dir_oid == target) return -EMBK_EINVAL;
        bool inside = false;
        rc = embkfs_dir_contains_oid_rec(vol, &vol->root, target, new_dir_oid, 0, &inside);
        if (rc != EMBK_OK) return rc;
        if (inside) return -EMBK_EINVAL;
    }

    /* --- the victim, when replacing ------------------------------------- */
    struct embk_inode_item vic;          /* the destination we are about to drop */
    bool vic_last = false, vic_defer = false;
    if (replacing) {
        if (existing == target) return EMBK_OK;   /* already the same object (hardlink) */

        const struct embk_item_header *vi =
            embkfs_find_item(vol, existing, EMBK_TYPE_INODE, 0, probe, sizeof probe);
        if (!vi) return -EMBK_ENOENT;
        const struct embk_inode_item *vp = embk_item_data(probe, vol->block_size, vi, sizeof *vp);
        if (!vp) return -EMBK_EINVAL;
        vic = *vp;
        bool vic_is_dir = ((vic.mode & EMBKFS_S_IFMT) == EMBKFS_S_IFDIR);

        /* POSIX type rules. Refused, not silently coerced: replacing a
         * directory with a file (or vice versa) is a caller bug, and the two
         * teardowns are not the same operation. */
        if (vic_is_dir != is_dir) return vic_is_dir ? -EMBK_EISDIR : -EMBK_ENOTDIR;
        if (vic_is_dir) {
            /* Replacing a directory means it must be EMPTY, and then its
             * removal is rmdir's job (link accounting for "..", etc). Not
             * built here: git never does it, and a half-right version of it
             * would be worse than an honest refusal. */
            return -EMBK_ENOTSUP;
        }

        vic_last  = (vic.links <= 1);
        vic_defer = vic_last && embkfs_object_is_open(vol, existing);
    }

    /* Both dirent ops must land on DIFFERENT keys, or they don't compose: each
     * builds its buffer from the ORIGINAL chain, so same-key ops clobber each
     * other. Same dir + different names that COLLIDE on the CRC32C name hash is
     * the only way that happens -- vanishingly rare, but silent corruption if
     * ignored, so refuse loudly instead. (Fixing it means one combined chain
     * rewrite; nothing needs it yet.) */
    if (old_dir_oid == new_dir_oid &&
        embk_crc32c(old_name, old_len, 0) == embk_crc32c(new_name, new_len, 0)) {
        kprintf("EMBKFS: %s: rename %s -> %s: name-hash collision in one directory "
                "is not supported\n", dev, old_name, new_name);
        return -EMBK_ENOTSUP;
    }

    struct embk_put remove_op, add_op;
    uint8_t *remove_buf = NULL;
    uint8_t *add_buf = NULL;

    rc = embkfs_dirent_remove_op(vol, old_dir_oid, old_name, old_len,
                                 probe, sizeof probe, &remove_op, &remove_buf);
    if (rc != EMBK_OK) return rc;

    /* Replacing -> RETARGET the existing name (add_op would refuse it with
     * -EEXIST); otherwise add it. Either way: ONE put on the destination key. */
    rc = replacing
       ? embkfs_dirent_retarget_op(vol, new_dir_oid, new_name, new_len,
                                   target, ttype, probe, sizeof probe,
                                   &add_op, &add_buf)
       : embkfs_dirent_add_op(vol, new_dir_oid, new_name, new_len,
                              target, ttype, probe, sizeof probe,
                              &add_op, &add_buf);
    if (rc != EMBK_OK) { kfree(remove_buf); return rc; }

    uint64_t new_gen = vol->generation + 1;

    /* When replacing, the victim's extents are freed in THIS SAME commit, so
     * the op count is no longer a fixed handful -- same shape as unlink's. */
    struct embk_extref *vic_ext = NULL;
    uint32_t vic_n = 0;
    if (replacing && vic_last && !vic_defer) {
        rc = embkfs_count_extents(vol, existing, &vic_n);
        if (rc != EMBK_OK) { kfree(remove_buf); kfree(add_buf); return rc; }
        if (vic_n) {
            vic_ext = kmalloc((uint64_t)vic_n * sizeof *vic_ext);
            if (!vic_ext) { kfree(remove_buf); kfree(add_buf); return -EMBK_ENOMEM; }
            uint32_t got = 0; bool over = false;
            rc = embkfs_collect_extents(vol, existing, vic_ext, vic_n, &got, &over);
            if (rc != EMBK_OK || over || got != vic_n) {
                kfree(vic_ext); kfree(remove_buf); kfree(add_buf); return -EMBK_EINVAL;
            }
            rc = embkfs_validate_extent_map(vol, vic_ext, vic_n, vic.size, "rename victim map");
            if (rc != EMBK_OK) { kfree(vic_ext); kfree(remove_buf); kfree(add_buf); return rc; }
        }
    }

    uint32_t ops_cap = 4 + 1 + vic_n;   /* 2 dirents + 2 parents + victim inode + its extents */
    struct embk_put *ops = kmalloc((uint64_t)ops_cap * sizeof *ops);
    if (!ops) { kfree(vic_ext); kfree(remove_buf); kfree(add_buf); return -EMBK_ENOMEM; }
    uint32_t nops = 0;
    ops[nops++] = remove_op;
    ops[nops++] = add_op;

    /* Every directory actually touched by this rename had its own CONTENT
     * change (a dirent left it and/or arrived in it) -- mtime/ctime always
     * move on whichever directory/directories that is, not only when a
     * moved SUBDIRECTORY's own ".." link-count needs adjusting (pre-v2.2
     * this whole block, link bump included, only ran for is_dir &&
     * different dirs, so a plain file rename never touched either parent
     * inode at all -- the same "content changed, parent inode untouched"
     * gap the creation path had, fixed here for the same reason). A
     * same-directory rename writes ONE parent update (old_dir_oid ==
     * new_dir_oid); a cross-directory rename writes one per directory. */
    uint64_t now_ns = rtc_now_ns();
    struct embk_inode_item old_parent_new = old_parent;
    old_parent_new.mtime      = now_ns;
    old_parent_new.ctime      = now_ns;
    old_parent_new.generation = new_gen;
    if (is_dir && old_dir_oid != new_dir_oid) {
        if (old_parent_new.links == 0) {
            kfree(ops); kfree(vic_ext); kfree(remove_buf); kfree(add_buf);
            return -EMBK_EINVAL;
        }
        old_parent_new.links -= 1;
    }
    ops[nops++] = (struct embk_put){
        .key = { .object_id = old_dir_oid, .type = EMBK_TYPE_INODE, .offset = 0 },
        .data = (const uint8_t *)&old_parent_new, .size = sizeof old_parent_new };

    if (new_dir_oid != old_dir_oid) {
        struct embk_inode_item new_parent_new = new_parent;
        new_parent_new.mtime      = now_ns;
        new_parent_new.ctime      = now_ns;
        new_parent_new.generation = new_gen;
        if (is_dir) new_parent_new.links += 1;
        ops[nops++] = (struct embk_put){
            .key = { .object_id = new_dir_oid, .type = EMBK_TYPE_INODE, .offset = 0 },
            .data = (const uint8_t *)&new_parent_new, .size = sizeof new_parent_new };
    }

    /* The victim, torn down in the SAME commit that repoints the name at
     * `target`. Mirrors unlink's three arms exactly -- including the DEFER one:
     * a destination someone still holds open keeps its inode and blocks (links
     * 0), and embkfs_object_put reclaims them at last close. Replacing a file
     * out from under a reader must not free blocks it is still reading. */
    struct embk_inode_item vic_upd;
    if (replacing) {
        if (vic_defer) {
            vic_upd = vic;
            vic_upd.links      = 0;
            vic_upd.ctime      = now_ns;   /* metadata-only: its data is untouched */
            vic_upd.generation = new_gen;
            ops[nops++] = (struct embk_put){
                .key = { .object_id = existing, .type = EMBK_TYPE_INODE, .offset = 0 },
                .data = (const uint8_t *)&vic_upd, .size = sizeof vic_upd };
        } else if (vic_last) {
            ops[nops++] = (struct embk_put){
                .key = { .object_id = existing, .type = EMBK_TYPE_INODE, .offset = 0 },
                .del = true };
            for (uint32_t i = 0; i < vic_n; i++)
                ops[nops++] = (struct embk_put){
                    .key = { .object_id = existing, .type = EMBK_TYPE_EXTENT,
                             .offset = vic_ext[i].offset },
                    .del = true };
        } else {
            vic_upd = vic;
            vic_upd.links     -= 1;        /* a hard link elsewhere still names it */
            vic_upd.ctime      = now_ns;
            vic_upd.generation = new_gen;
            ops[nops++] = (struct embk_put){
                .key = { .object_id = existing, .type = EMBK_TYPE_INODE, .offset = 0 },
                .data = (const uint8_t *)&vic_upd, .size = sizeof vic_upd };
        }
    }

    rc = embkfs_txn_apply_ops(vol, ops, nops, new_gen);

    kfree(ops);
    kfree(vic_ext);
    kfree(remove_buf);
    kfree(add_buf);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: %s: rename %s -> %s failed: %s\n", dev, old_name, new_name, embk_strerror(rc));
        return rc;
    }
    kprintf("EMBKFS: %s: rename %s -> %s (object %lu)\n", dev, old_name, new_name, target);
    return EMBK_OK;
}


/* ===================================================================
 * Snapshots (v2.2 Phase 5b) — public API.
 *
 * Taking a snapshot is O(1): the live root is already a durable, CoW-
 * immutable tree the instant a commit lands, so "freezing" it is just
 * recording its block_ptr as one more tree item. The real work (and the
 * plan's own "real dependency, not optional") is the ALLOCATOR side:
 * embkfs_txn_end() must not let a block a snapshot still needs get
 * reclaimed just because the LIVE tree stopped pointing at it.
 *
 * SIMPLIFICATION (documented, not silently worked around): rather than
 * true per-block reference counting -- which would mean tracking, for
 * every block, exactly which snapshots (if any) still reference it --
 * this is a coarser, conservative policy: while snapshot_count > 0, NO
 * freed block is reclaimed at all, regardless of whether it's actually
 * still needed by a snapshot or just happens to be freed while one
 * exists. This is always SAFE (never frees something a snapshot needs)
 * at the cost of NOT reclaiming space some frees could have returned
 * immediately; deleting the last snapshot triggers a full bitmap rebuild
 * (embkfs_bitmap_build(), which walks the live tree fresh) to recover
 * everything that's now genuinely free. True refcounting is a natural
 * v2.x follow-up, flagged here rather than silently approximated.
 * =================================================================== */

static int embkfs_snapshot_create_impl(struct embkfs_volume *vol, const char *name)
{
    if (!vol || !vol->mounted || !name) return -EMBK_EINVAL;
    if (vol->read_only) return -EMBK_EROFS;
    if (!name[0]) return -EMBK_EINVAL;

    if (embkfs_snapshot_find_slot(vol, name, NULL, NULL) == EMBK_OK) return -EMBK_EEXIST;

    struct embk_snapshot_item si;
    memset(&si, 0, sizeof si);
    embk_snapshot_name_pad(name, si.name);
    si.root       = vol->root;         /* captured by VALUE before this commit moves vol->root */
    si.generation = vol->generation;
    si.timestamp  = rtc_now_ns();

    /* v2.3 (SNAPREG): the registry is a plain block outside the tree, so
     * creating a snapshot writes ONE block and commits nothing. Note what that
     * removes: with the in-tree registry, the put that recorded the snapshot
     * was itself a CoW write that superseded the very tree the snapshot had
     * just frozen -- which is why the legacy path below has to bump
     * snapshot_count before reconciling its own frees. Here the frozen tree is
     * never rewritten, so that hazard does not exist. Order still matters for
     * a different reason: the count must be live before anything can free a
     * block, so the registry write comes first and the count follows it. */
    if (embk_snapreg_enabled(vol)) {
        struct embk_snapshot_item slots[EMBKFS_MAX_SNAPSHOTS];
        uint32_t n = 0;
        int rc = embkfs_snapreg_load(vol, slots, &n);
        if (rc != EMBK_OK) return rc;
        if (n >= EMBKFS_MAX_SNAPSHOTS) return -EMBK_ENOSPC;
        slots[n] = si;
        rc = embkfs_snapreg_store(vol, slots, n + 1);
        if (rc != EMBK_OK) {
            kprintf("EMBKFS: %s: snapshot create '%s' failed: %s\n",
                    vol->dev->name, name, embk_strerror(rc));
            return rc;
        }
        vol->snapshot_count = n + 1;
        kprintf("EMBKFS: %s: snapshot '%s' created (slot %u, gen %lu, %u snapshot%s retained)\n",
                vol->dev->name, name, n, si.generation,
                (unsigned int)vol->snapshot_count, vol->snapshot_count == 1 ? "" : "s");
        return EMBK_OK;
    }

    uint32_t slot;
    int rc = embkfs_snapshot_find_free_slot(vol, &slot);
    if (rc != EMBK_OK) return rc;

    uint64_t new_gen = vol->generation + 1;
    struct embk_txn txn;
    rc = embkfs_txn_begin(vol, &txn);
    if (rc != EMBK_OK) return rc;

    struct embk_put put = {
        .key  = { .object_id = EMBKFS_ROOT_OBJECT_ID, .type = EMBK_TYPE_SNAPSHOT, .offset = slot },
        .data = (const uint8_t *)&si, .size = sizeof si };

    struct embk_block_ptr new_root;
    rc = embkfs_cow_apply(vol, &vol->root, &put, 1, new_gen, &new_root);
    if (rc == EMBK_OK) {
        /* This commit's OWN CoW rebuild supersedes the pre-snapshot tree
         * nodes -- which is exactly what si.root (the snapshot being
         * created RIGHT NOW) points at. Bump the count BEFORE reconciling
         * this transaction's own frees, so embkfs_txn_end()'s hold-back
         * sees the new snapshot and protects them, instead of freeing the
         * very blocks the snapshot we're about to finish creating needs
         * (a real bug caught by test embkfs snapshot: without this, the
         * next write to reuse this space would silently corrupt the
         * "frozen" snapshot). Rolled back below if the commit fails. */
        vol->snapshot_count++;
        uint64_t new_free;
        rc = embkfs_txn_new_free(vol, &new_free);
        if (rc == EMBK_OK)
            rc = embkfs_commit(vol, &new_root, new_gen, new_free);
        if (rc != EMBK_OK) vol->snapshot_count--;
    }
    embkfs_txn_end(vol, &txn, rc == EMBK_OK);

    if (rc != EMBK_OK) {
        kprintf("EMBKFS: %s: snapshot create '%s' failed: %s\n", vol->dev->name, name, embk_strerror(rc));
        return rc;
    }
    kprintf("EMBKFS: %s: snapshot '%s' created (slot %u, gen %lu, %u snapshot%s retained)\n",
            vol->dev->name, name, slot, si.generation,
            (unsigned int)vol->snapshot_count, vol->snapshot_count == 1 ? "" : "s");
    return EMBK_OK;
}

static int embkfs_snapshot_delete_impl(struct embkfs_volume *vol, const char *name)
{
    if (!vol || !vol->mounted || !name) return -EMBK_EINVAL;
    if (vol->read_only) return -EMBK_EROFS;

    uint32_t slot;
    int rc = embkfs_snapshot_find_slot(vol, name, &slot, NULL);
    if (rc != EMBK_OK) return rc;

    if (embk_snapreg_enabled(vol)) {
        struct embk_snapshot_item slots[EMBKFS_MAX_SNAPSHOTS];
        uint32_t n = 0;
        rc = embkfs_snapreg_load(vol, slots, &n);
        if (rc != EMBK_OK) return rc;
        if (slot >= n) return -EMBK_ENOENT;
        for (uint32_t i = slot; i + 1 < n; i++) slots[i] = slots[i + 1];   /* keep it dense */
        rc = embkfs_snapreg_store(vol, slots, n - 1);
        if (rc != EMBK_OK) {
            kprintf("EMBKFS: %s: snapshot delete '%s' failed: %s\n",
                    vol->dev->name, name, embk_strerror(rc));
            return rc;
        }
        vol->snapshot_count = n - 1;
        kprintf("EMBKFS: %s: snapshot '%s' deleted (slot %u, %u snapshot%s remaining)\n",
                vol->dev->name, name, slot,
                (unsigned int)vol->snapshot_count, vol->snapshot_count == 1 ? "" : "s");
        if (vol->snapshot_count == 0) {
            embkfs_bitmap_build(vol);
            embkfs_free_index_rebuild(vol);
        }
        return EMBK_OK;
    }

    uint64_t new_gen = vol->generation + 1;
    struct embk_txn txn;
    rc = embkfs_txn_begin(vol, &txn);
    if (rc != EMBK_OK) return rc;

    struct embk_put put = {
        .key = { .object_id = EMBKFS_ROOT_OBJECT_ID, .type = EMBK_TYPE_SNAPSHOT, .offset = slot },
        .del = true };

    struct embk_block_ptr new_root;
    rc = embkfs_cow_apply(vol, &vol->root, &put, 1, new_gen, &new_root);
    if (rc == EMBK_OK) {
        uint64_t new_free;
        rc = embkfs_txn_new_free(vol, &new_free);
        if (rc == EMBK_OK)
            rc = embkfs_commit(vol, &new_root, new_gen, new_free);
    }
    embkfs_txn_end(vol, &txn, rc == EMBK_OK);

    if (rc != EMBK_OK) {
        kprintf("EMBKFS: %s: snapshot delete '%s' failed: %s\n", vol->dev->name, name, embk_strerror(rc));
        return rc;
    }
    if (vol->snapshot_count > 0) vol->snapshot_count--;
    kprintf("EMBKFS: %s: snapshot '%s' deleted (slot %u, %u snapshot%s remaining)\n",
            vol->dev->name, name, slot,
            (unsigned int)vol->snapshot_count, vol->snapshot_count == 1 ? "" : "s");

    if (vol->snapshot_count == 0) {
        /* Last snapshot gone: reclaim everything the hold-back policy
         * deferred, instead of waiting for the next remount. */
        embkfs_bitmap_build(vol);
        embkfs_free_index_rebuild(vol);
    }
    return EMBK_OK;
}

static int embkfs_snapshot_rollback_impl(struct embkfs_volume *vol, const char *name)
{
    if (!vol || !vol->mounted || !name) return -EMBK_EINVAL;
    if (vol->read_only) return -EMBK_EROFS;

    struct embk_snapshot_item si;
    int rc = embkfs_snapshot_find_slot(vol, name, NULL, &si);
    if (rc != EMBK_OK) return rc;

    /* Compute the post-rollback free count by temporarily pointing vol->root at
     * the snapshot's root and rebuilding the bitmap from it, plus every
     * snapshot the registry names. Pure in-memory/disk-READ work up to this
     * point -- nothing durable changes yet, so on any failure we just restore
     * vol->root and walk away.
     *
     * Note what is NOT here on a v2.3 volume: any handling of the registry.
     * Rollback swaps the root and leaves the registry block alone, so every
     * snapshot -- older AND newer than the target -- is still listed and still
     * protected afterwards. Rollback stops being a one-way door: you can roll
     * back to an earlier snapshot and then forward again to a later one,
     * because the later one still exists to roll forward TO. On a legacy
     * volume the registry rides inside the tree being replaced, so the newer
     * ones vanish with the abandoned future (see embkfs.h). */
    struct embk_block_ptr saved_root = vol->root;
    vol->root = si.root;
    rc = embkfs_bitmap_build(vol);
    if (rc != EMBK_OK) {
        vol->root = saved_root;
        embkfs_bitmap_build(vol);
        kprintf("EMBKFS: %s: snapshot rollback '%s' failed: %s\n", vol->dev->name, name, embk_strerror(rc));
        return rc;
    }

    uint64_t used = 0;
    for (uint64_t b = 0; b < vol->total_blocks; b++)
        if (embk_bm_test(vol->block_bitmap, b)) used++;
    uint64_t new_free = vol->total_blocks - used;

    uint64_t new_gen = vol->generation + 1;
    rc = embkfs_commit(vol, &si.root, new_gen, new_free);
    if (rc != EMBK_OK) {
        vol->root = saved_root;
        embkfs_bitmap_build(vol);
        kprintf("EMBKFS: %s: snapshot rollback '%s' commit failed: %s\n", vol->dev->name, name, embk_strerror(rc));
        return rc;
    }

    embkfs_free_index_rebuild(vol);

    /* vol->snapshot_count was already set authoritatively as a side effect of
     * the embkfs_bitmap_build() call above. On a v2.3 volume that count is
     * unchanged by the rollback (the registry did not move); on a legacy
     * volume it can be FEWER than before, if newer snapshots existed only in
     * the abandoned future. */

    kprintf("EMBKFS: %s: rolled back to snapshot '%s' (gen now %lu, %u snapshot%s retained)\n",
            vol->dev->name, name, vol->generation,
            (unsigned int)vol->snapshot_count, vol->snapshot_count == 1 ? "" : "s");
    return EMBK_OK;
}

int embkfs_snapshot_list(struct embkfs_volume *vol, struct embk_snapshot_item *out_items,
                         uint32_t max, uint32_t *out_n)
{
    if (!vol || !vol->mounted) return -EMBK_ENODEV;
    embkfs_lock();
    int rc = embkfs_snapshot_list_internal(vol, out_items, max, out_n);
    embkfs_unlock();
    return rc;
}


/* ===================================================================
 * Open-handle reference counting — unlink-while-open safety.
 *
 * In-memory ONLY: records how many open handles (fds) currently hold each
 * object alive. The fd layer brackets each open file with get...put. An
 * object is reclaimed only when BOTH counts are zero:
 *     links > 0  OR  opens > 0   -> object stays
 *     links == 0 AND opens == 0  -> free inode + extents + blocks
 * EMBKFS owns this because unlink (below the fd layer) must consult it; the
 * fd layer drives it from above and never learns what an "fd" is.
 *
 * Not on disk: a crash while a file is unlinked-but-open leaks that inode's
 * space (links==0, no dirent) — a space leak, not corruption. A future
 * mount-time sweep ("free every inode with links==0") reclaims it. =========*/
#define EMBKFS_MAX_OPEN_OBJECTS 64
struct embk_open_ref {
    struct embkfs_volume *vol;
    uint64_t              oid;
    uint32_t              count;   /* slot is free when count == 0 */
};
static struct embk_open_ref g_open_refs[EMBKFS_MAX_OPEN_OBJECTS];

static bool embkfs_object_is_open(struct embkfs_volume *vol, uint64_t oid)
{
    for (uint32_t i = 0; i < EMBKFS_MAX_OPEN_OBJECTS; i++)
        if (g_open_refs[i].count && g_open_refs[i].vol == vol &&
            g_open_refs[i].oid == oid)
            return true;
    return false;
}

/* Reclaim an ORPHAN object: no directory entry, links already 0. Frees its
 * inode + extents + data blocks in one transaction. Idempotent (a missing
 * inode is a no-op). Mirrors unlink's last-link free path, standalone. */
static int embkfs_destroy_object(struct embkfs_volume *vol, uint64_t oid)
{
    const char *dev = vol->dev->name;
    static uint8_t probe[4096];
    if (vol->read_only) return -EMBK_EROFS;

    uint64_t new_gen = vol->generation + 1;

    const struct embk_item_header *ti =
        embkfs_find_item(vol, oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ti) return EMBK_OK;                       /* already gone */
    const struct embk_inode_item *tino =
        embk_item_data(probe, vol->block_size, ti, sizeof *tino);
    if (!tino) return -EMBK_EINVAL;
    struct embk_inode_item ino = *tino;
    if (ino.links != 0) return -EMBK_EINVAL;       /* caller bug: not an orphan */

    struct embk_extref *old_ext = NULL;
    uint32_t old_n = 0;
    int rc = embkfs_count_extents(vol, oid, &old_n);
    if (rc != EMBK_OK) return rc;
    if (old_n) {
        old_ext = kmalloc((uint64_t)old_n * sizeof *old_ext);
        if (!old_ext) return -EMBK_ENOMEM;
        uint32_t got = 0; bool over = false;
        rc = embkfs_collect_extents(vol, oid, old_ext, old_n, &got, &over);
        if (rc != EMBK_OK || over || got != old_n) { kfree(old_ext); return -EMBK_EINVAL; }
        rc = embkfs_validate_extent_map(vol, old_ext, old_n, ino.size, "destroy old map");
        if (rc != EMBK_OK) { kfree(old_ext); return rc; }
    }

    struct embk_put *ops = kmalloc((uint64_t)(1 + old_n) * sizeof *ops);
    if (!ops) { kfree(old_ext); return -EMBK_ENOMEM; }
    uint32_t nops = 0;
    ops[nops++] = (struct embk_put){
        .key = { .object_id = oid, .type = EMBK_TYPE_INODE, .offset = 0 }, .del = true };
    for (uint32_t i = 0; i < old_n; i++)
        ops[nops++] = (struct embk_put){
            .key = { .object_id = oid, .type = EMBK_TYPE_EXTENT, .offset = old_ext[i].offset },
            .del = true };

    struct embk_txn txn;
    rc = embkfs_txn_begin(vol, &txn);
    if (rc != EMBK_OK) { kfree(ops); kfree(old_ext); return rc; }
    for (uint32_t i = 0; i < old_n; i++)
        embkfs_note_freed_run(vol, old_ext[i].disk_block, old_ext[i].length);

    struct embk_block_ptr new_root;
    rc = embkfs_cow_apply(vol, &vol->root, ops, nops, new_gen, &new_root);
    if (rc == EMBK_OK) {
        uint64_t new_free;
        rc = embkfs_txn_new_free(vol, &new_free);
        if (rc == EMBK_OK)
            rc = embkfs_commit(vol, &new_root, new_gen, new_free);
    }
    embkfs_txn_end(vol, &txn, rc == EMBK_OK);

    kfree(ops);
    kfree(old_ext);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: %s: destroy object %lu failed: %s\n", dev, oid, embk_strerror(rc));
        return rc;
    }
    kprintf("EMBKFS: %s: reclaimed orphan object %lu, gen now %lu, free %lu\n",
            dev, oid, vol->generation, vol->free_blocks);
    return EMBK_OK;
}

/* Return the first orphan inode (links==0) reachable from the current root.
 * Mount-time sweep only reclaims non-directories via embkfs_destroy_object,
 * which deletes inode+extents and data runs in one transaction. */
static int embkfs_find_orphan_inode_rec(struct embkfs_volume *vol,
                                        const struct embk_block_ptr *ptr,
                                        uint64_t *out_oid, bool *out_found)
{
    uint8_t *buf = kmalloc(vol->block_size);
    if (!buf) return -EMBK_ENOMEM;

    int rc = embkfs_read_node(vol, ptr, buf, vol->block_size);
    if (rc != EMBK_OK) {
        kfree(buf);
        return rc;
    }

    const struct embk_node_header *h = (const struct embk_node_header *)buf;
    if (h->level == 0) {
        for (uint32_t i = 0; i < h->nritems; i++) {
            const struct embk_item_header *it = embk_leaf_item(buf, i);
            if (it->key.type != EMBK_TYPE_INODE)
                continue;
            if (it->key.object_id == EMBKFS_ROOT_OBJECT_ID)
                continue;

            const struct embk_inode_item *ino =
                embk_item_data(buf, vol->block_size, it, sizeof *ino);
            if (!ino) {
                kfree(buf);
                return -EMBK_EINVAL;
            }
            if (ino->links != 0)
                continue;
            if ((ino->mode & EMBKFS_S_IFMT) == EMBKFS_S_IFDIR)
                continue;

            *out_oid = it->key.object_id;
            *out_found = true;
            kfree(buf);
            return EMBK_OK;
        }

        kfree(buf);
        *out_found = false;
        return EMBK_OK;
    }

    if (h->nritems == 0 || h->nritems > EMBKFS_MAX_SLOTS) {
        kfree(buf);
        return -EMBK_EINVAL;
    }

    const struct embk_internal_slot *slots =
        (const struct embk_internal_slot *)(buf + sizeof(struct embk_node_header));
    for (uint32_t i = 0; i < h->nritems; i++) {
        struct embk_block_ptr child;
        memcpy(&child, &slots[i].ptr, sizeof child);
        rc = embkfs_find_orphan_inode_rec(vol, &child, out_oid, out_found);
        if (rc != EMBK_OK) {
            kfree(buf);
            return rc;
        }
        if (*out_found) {
            kfree(buf);
            return EMBK_OK;
        }
    }

    kfree(buf);
    *out_found = false;
    return EMBK_OK;
}

/* Crash-recovery sweep: reclaim unreachable, unlinked files/symlinks left by a
 * previous crash in the unlink-while-open window (links==0, no dirent). */
static int embkfs_mount_orphan_sweep(struct embkfs_volume *vol)
{
    if (!vol) return -EMBK_EINVAL;
    if (vol->read_only) return EMBK_OK;

    uint32_t reclaimed = 0;
    for (;;) {
        uint64_t oid = 0;
        bool found = false;

        int rc = embkfs_find_orphan_inode_rec(vol, &vol->root, &oid, &found);
        if (rc != EMBK_OK)
            return rc;
        if (!found)
            break;

        rc = embkfs_destroy_object(vol, oid);
        if (rc != EMBK_OK)
            return rc;
        reclaimed++;
    }

    if (reclaimed) {
        kprintf("EMBKFS: %s: mount sweep reclaimed %u orphan object(s)\n",
                vol->dev->name, (unsigned)reclaimed);
    }
    return EMBK_OK;
}

/* fd layer calls this when a handle opens. */
int embkfs_object_get(struct embkfs_volume *vol, uint64_t oid)
{
    if (!vol) return -EMBK_EINVAL;
    for (uint32_t i = 0; i < EMBKFS_MAX_OPEN_OBJECTS; i++)
        if (g_open_refs[i].count && g_open_refs[i].vol == vol && g_open_refs[i].oid == oid) {
            g_open_refs[i].count++;
            return EMBK_OK;
        }
    for (uint32_t i = 0; i < EMBKFS_MAX_OPEN_OBJECTS; i++)
        if (g_open_refs[i].count == 0) {
            g_open_refs[i].vol = vol;
            g_open_refs[i].oid = oid;
            g_open_refs[i].count = 1;
            return EMBK_OK;
        }
    return -EMBK_ENOSPC;                            /* too many open objects */
}

/* fd layer calls this when a handle closes. On the LAST close, if the object
 * was unlinked while open (on-disk links == 0), reclaim it now. */
int embkfs_object_put(struct embkfs_volume *vol, uint64_t oid)
{
    if (!vol) return -EMBK_EINVAL;
    static uint8_t probe[4096];

    for (uint32_t i = 0; i < EMBKFS_MAX_OPEN_OBJECTS; i++) {
        if (g_open_refs[i].count && g_open_refs[i].vol == vol && g_open_refs[i].oid == oid) {
            if (--g_open_refs[i].count > 0)
                return EMBK_OK;                     /* other handles remain */
            g_open_refs[i].vol = NULL;
            g_open_refs[i].oid = 0;

            /* Last handle closed. Read the AUTHORITATIVE on-disk link count. */
            const struct embk_item_header *ti =
                embkfs_find_item(vol, oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
            if (!ti) return EMBK_OK;                /* inode already gone */
            const struct embk_inode_item *tino =
                embk_item_data(probe, vol->block_size, ti, sizeof *tino);
            if (!tino) return -EMBK_EINVAL;
            if (tino->links == 0)
                return embkfs_destroy_object(vol, oid);
            return EMBK_OK;                         /* still linked: keep it */
        }
    }
    return -EMBK_ENOENT;                            /* put with no matching get */
}


/* Remove the name `name` from directory `dir_oid`, as ONE atomic COW transaction.
 * Files only — a directory returns -EMBK_EISDIR (use rmdir).
 *
 * Two things change together, so a crash can't drop one without the other:
 *   - the directory entry: the name's record is removed from its hash chain
 *     (shortened, or the whole item deleted if it was the only record);
 *   - the target's link count: decremented. If it reaches zero the file is gone,
 *     so its INODE and EXTENT items are deleted and its data block(s) are freed;
 *     if a hard link remains, only the inode's count is rewritten.
 *
 * The transaction reclaims the freed data and every COW-orphaned node into the
 * in-memory bitmap on commit (see the transactional allocator). */
static int embkfs_unlink(struct embkfs_volume *vol, uint64_t dir_oid, const char *name)
{
    const char *dev = vol->dev->name;
    static uint8_t probe[4096];

    if (vol->read_only) return -EMBK_EROFS;
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -EMBK_EINVAL;

    /* 1. resolve the name (also validates dir_oid is a real directory) */
    uint64_t target;
    int rc = embkfs_lookup(vol, dir_oid, name, &target);
    if (rc != EMBK_OK) return rc;                 /* -EMBK_ENOENT if the name is absent */

    uint64_t new_gen = vol->generation + 1;

    /* 2. read the target inode: must be a regular file. Copy it out — probe is
     *    reused by the descents below. */
    const struct embk_item_header *ti =
        embkfs_find_item(vol, target, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ti) { kprintf("EMBKFS: %s: object %lu has no inode\n", dev, target); return -EMBK_ENOENT; }
    const struct embk_inode_item *tino = embk_item_data(probe, vol->block_size, ti, sizeof *tino);
    if (!tino) return -EMBK_EINVAL;
    struct embk_inode_item ino = *tino;
    if ((ino.mode & EMBKFS_S_IFMT) == EMBKFS_S_IFDIR) {
        kprintf("EMBKFS: %s: \"%s\" is a directory (use rmdir)\n", dev, name);
        return -EMBK_EISDIR;
    }
    bool last_link = (ino.links <= 1);
    
    /* If a handle holds this file open, defer the actual free to last close. */
    bool defer = last_link && embkfs_object_is_open(vol, target);


    /* 3. if this is the last link, collect all extents to free/delete. */
    struct embk_extref *old_ext = NULL;
    uint32_t old_n = 0;
    if (last_link && !defer) {
        rc = embkfs_count_extents(vol, target, &old_n);
        if (rc != EMBK_OK) return rc;
        if (old_n) {
            old_ext = kmalloc((uint64_t)old_n * sizeof *old_ext);
            if (!old_ext) return -EMBK_ENOMEM;
            uint32_t got = 0; bool over = false;
            rc = embkfs_collect_extents(vol, target, old_ext, old_n, &got, &over);
            if (rc != EMBK_OK || over || got != old_n) { kfree(old_ext); return -EMBK_EINVAL; }
            rc = embkfs_validate_extent_map(vol, old_ext, old_n, ino.size, "unlink old map");
            if (rc != EMBK_OK) { kfree(old_ext); return rc; }
        }
    }

    /* 4. build the directory-entry removal op (shortened chain, or item delete). */
    struct embk_put dirent_op;
    uint8_t        *chain_buf = NULL;
    rc = embkfs_dirent_remove_op(vol, dir_oid, name, name_len, probe, sizeof probe,
                                 &dirent_op, &chain_buf);
    if (rc != EMBK_OK) { kfree(old_ext); return rc; }

    /* 4b. the parent directory's own CONTENT changed (a dirent was removed
     * from it) -- mtime/ctime move on it, same reasoning as the create/
     * rename paths (pre-v2.2 this function never touched the parent's
     * inode at all, the same "content changed, parent inode untouched"
     * gap fixed there). Copy the inode out before `probe` is reused again. */
    uint64_t now_ns = rtc_now_ns();
    const struct embk_item_header *pi =
        embkfs_find_item(vol, dir_oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!pi) { kfree(chain_buf); kfree(old_ext); return -EMBK_ENOENT; }
    const struct embk_inode_item *pp = embk_item_data(probe, vol->block_size, pi, sizeof *pp);
    if (!pp) { kfree(chain_buf); kfree(old_ext); return -EMBK_EINVAL; }
    struct embk_inode_item parent_ino = *pp;
    parent_ino.mtime      = now_ns;
    parent_ino.ctime      = now_ns;
    parent_ino.generation = new_gen;

    /* 5. assemble the ops for one atomic commit. */
    uint32_t ops_cap = (last_link && !defer) ? (3 + old_n) : 3;   /* +1 for the parent inode update */
    struct embk_put *ops = kmalloc((uint64_t)ops_cap * sizeof *ops);
    if (!ops) { kfree(chain_buf); kfree(old_ext); return -EMBK_ENOMEM; }
    uint32_t nops = 0;

    struct embk_inode_item dec_ino;
    struct embk_inode_item orphan;                 /* NEW */

    if (defer) {                                   /* NEW arm */
        /* Name goes now; object stays. Write links=0 but keep inode+extents.
         * The blocks are reclaimed at last close, in embkfs_object_put.
         * links dropping to 0 is a metadata-only change to the TARGET
         * itself -- ctime moves, mtime does not (its data is untouched). */
        orphan = ino;
        orphan.links = 0;
        orphan.ctime = now_ns;
        orphan.generation = new_gen;
        ops[nops++] = (struct embk_put){
            .key = { .object_id = target, .type = EMBK_TYPE_INODE, .offset = 0 },
            .data = (const uint8_t *)&orphan, .size = sizeof orphan };
    } else if (last_link) {
        /* ... existing free path, unchanged ... */
        ops[nops++] = (struct embk_put){
            .key = { .object_id = target, .type = EMBK_TYPE_INODE, .offset = 0 }, .del = true };
        for (uint32_t i = 0; i < old_n; i++)
            ops[nops++] = (struct embk_put){
                .key = { .object_id = target, .type = EMBK_TYPE_EXTENT, .offset = old_ext[i].offset }, .del = true };

    } else {
        /* ... existing hard-link-remains path, unchanged ... */
        dec_ino = ino;
        dec_ino.links     -= 1;
        dec_ino.ctime      = now_ns;   /* metadata-only change to the TARGET, ctime not mtime */
        dec_ino.generation = new_gen;
        ops[nops++] = (struct embk_put){
            .key = { .object_id = target, .type = EMBK_TYPE_INODE, .offset = 0 },
            .data = (const uint8_t *)&dec_ino, .size = sizeof dec_ino };
    }
    ops[nops++] = dirent_op;                        /* unchanged: name removal */
    ops[nops++] = (struct embk_put){
        .key = { .object_id = dir_oid, .type = EMBK_TYPE_INODE, .offset = 0 },
        .data = (const uint8_t *)&parent_ino, .size = sizeof parent_ino };



    /* Transaction: the rebuilt tree path's old nodes, plus the freed file's data
     * run, are reclaimed once committed. */
    struct embk_txn txn;
    rc = embkfs_txn_begin(vol, &txn);
    if (rc != EMBK_OK) { kfree(ops); kfree(chain_buf); kfree(old_ext); return rc; }
    for (uint32_t i = 0; i < old_n; i++)
        embkfs_note_freed_run(vol, old_ext[i].disk_block, old_ext[i].length);

    struct embk_block_ptr new_root;
    rc = embkfs_cow_apply(vol, &vol->root, ops, nops, new_gen, &new_root);
    if (rc == EMBK_OK) {
        uint64_t new_free;
        rc = embkfs_txn_new_free(vol, &new_free);   /* frees data + emptied nodes */
        if (rc == EMBK_OK)
            rc = embkfs_commit(vol, &new_root, new_gen, new_free);
    }
    embkfs_txn_end(vol, &txn, rc == EMBK_OK);

    kfree(ops);
    kfree(chain_buf);
    kfree(old_ext);
    if (rc != EMBK_OK) { kprintf("EMBKFS: %s: unlink \"%s\" failed: %s\n", dev, name, embk_strerror(rc)); return rc; }

    
    kprintf("EMBKFS: %s: unlinked /%s (object %lu, %s), gen now %lu, free %lu\n",
            dev, name, target,
            defer ? "deferred, held open" : (last_link ? "freed" : "link remains"),
            vol->generation, vol->free_blocks);
    return EMBK_OK;
}


/* Set *found if the tree holds ANY directory entry for `dir_oid` — i.e. any key
 * with object_id == dir_oid and type == DIR_ENTRY. A pruned recursive walk: at an
 * internal node descend only into children whose key range can overlap the
 * dir-entry prefix [{dir_oid,DIR_ENTRY,0} .. {dir_oid,DIR_ENTRY,MAX}], so it
 * normally touches a single leaf. Correct regardless of how those entries fall
 * across leaves (it cannot miss one straddling a boundary, which a single
 * descend-and-scan could). Each recursion level owns its node buffer, so the
 * parent's slot array stays valid across the recursive call. */
static int embkfs_dir_has_entry(struct embkfs_volume *vol, const struct embk_block_ptr *ptr,
                                uint64_t dir_oid, bool *found)
{
    uint8_t *buf = kmalloc(vol->block_size);
    if (!buf) return -EMBK_ENOMEM;
    int rc = embkfs_read_node(vol, ptr, buf, vol->block_size);   /* verifies */
    if (rc != EMBK_OK) { kfree(buf); return rc; }

    const struct embk_node_header *h = (const struct embk_node_header *)buf;
    const struct embk_key lo = { .object_id = dir_oid, .type = EMBK_TYPE_DIR_ENTRY, .offset = 0 };
    const struct embk_key hi = { .object_id = dir_oid, .type = EMBK_TYPE_DIR_ENTRY, .offset = UINT64_MAX };

    if (h->level == 0) {
        for (uint32_t i = 0; i < h->nritems && !*found; i++) {
            const struct embk_item_header *it = embk_leaf_item(buf, i);
            if (it->key.object_id == dir_oid && it->key.type == EMBK_TYPE_DIR_ENTRY)
                *found = true;
        }
        kfree(buf);
        return EMBK_OK;
    }

    if (h->nritems == 0 || h->nritems > EMBKFS_MAX_SLOTS) { kfree(buf); return -EMBK_EINVAL; }
    const struct embk_internal_slot *slots =
        (const struct embk_internal_slot *)(buf + sizeof(struct embk_node_header));
    for (uint32_t i = 0; i < h->nritems && !*found; i++) {
        struct embk_key clo;                          /* child i covers [clo, chi) */
        memcpy(&clo, &slots[i].key, sizeof clo);
        if (embk_key_cmp(&clo, &hi) > 0) break;        /* child (and all later) start past prefix */
        if (i + 1 < h->nritems) {                      /* skip a child that ends at/before prefix */
            struct embk_key chi;
            memcpy(&chi, &slots[i + 1].key, sizeof chi);
            if (embk_key_cmp(&chi, &lo) <= 0) continue;
        }
        struct embk_block_ptr cp;
        memcpy(&cp, &slots[i].ptr, sizeof cp);
        rc = embkfs_dir_has_entry(vol, &cp, dir_oid, found);
        if (rc != EMBK_OK) { kfree(buf); return rc; }
    }
    kfree(buf);
    return EMBK_OK;
}


/* Remove the empty subdirectory `name` from directory `dir_oid`, as ONE atomic
 * COW transaction — the inverse of mkdir. The target must be a directory
 * (-EMBK_ENOTDIR otherwise) and must contain no entries (-EMBK_ENOTEMPTY). Three
 * items commit together: the target's inode is deleted, its directory entry is
 * removed from the parent's chain, and the parent's link count is decremented
 * (the removed child's ".." no longer points at it — undoing mkdir's bump). An
 * empty directory owns no data blocks, so the free count is unchanged; the
 * superseded tree nodes are reclaimed by the transaction. */
static int embkfs_rmdir(struct embkfs_volume *vol, uint64_t dir_oid, const char *name)
{
    const char *dev = vol->dev->name;
    static uint8_t probe[4096];

    if (vol->read_only) return -EMBK_EROFS;
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -EMBK_EINVAL;

    /* 1. resolve the name (also validates dir_oid is a real directory) */
    uint64_t target;
    int rc = embkfs_lookup(vol, dir_oid, name, &target);
    if (rc != EMBK_OK) return rc;

    uint64_t new_gen = vol->generation + 1;

    /* 2. the target must itself be a directory */
    const struct embk_item_header *ti =
        embkfs_find_item(vol, target, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ti) { kprintf("EMBKFS: %s: object %lu has no inode\n", dev, target); return -EMBK_ENOENT; }
    const struct embk_inode_item *tino = embk_item_data(probe, vol->block_size, ti, sizeof *tino);
    if (!tino) return -EMBK_EINVAL;
    if ((tino->mode & EMBKFS_S_IFMT) != EMBKFS_S_IFDIR) {
        kprintf("EMBKFS: %s: \"%s\" is not a directory\n", dev, name); return -EMBK_ENOTDIR;
    }

    /* 3. and it must be empty — no entries may live under it */
    bool has_child = false;
    rc = embkfs_dir_has_entry(vol, &vol->root, target, &has_child);
    if (rc != EMBK_OK) return rc;
    if (has_child) { kprintf("EMBKFS: %s: \"%s\" not empty\n", dev, name); return -EMBK_ENOTEMPTY; }

    /* 4. read the parent inode and drop its link count by one (undo mkdir). */
    const struct embk_item_header *pi =
        embkfs_find_item(vol, dir_oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!pi) { kprintf("EMBKFS: %s: parent object %lu has no inode\n", dev, dir_oid); return -EMBK_ENOENT; }
    const struct embk_inode_item *p = embk_item_data(probe, vol->block_size, pi, sizeof *p);
    if (!p) return -EMBK_EINVAL;
    struct embk_inode_item parent_ino = *p;
    if (parent_ino.links == 0) return -EMBK_EINVAL;
    parent_ino.links     -= 1;
    /* Parent's own content changed (a dirent was removed) -- mtime/ctime
     * both move, same reasoning as create/rename/unlink. */
    parent_ino.mtime      = rtc_now_ns();
    parent_ino.ctime      = parent_ino.mtime;
    parent_ino.generation = new_gen;

    /* 5. directory-entry removal op (probe reused; chain copied out internally) */
    struct embk_put dirent_op;
    uint8_t        *chain_buf = NULL;
    rc = embkfs_dirent_remove_op(vol, dir_oid, name, name_len, probe, sizeof probe,
                                 &dirent_op, &chain_buf);
    if (rc != EMBK_OK) return rc;

    /* 6. one atomic commit: delete target inode, rewrite parent inode, remove entry */
    struct embk_put ops[3];
    uint32_t nops = 0;
    ops[nops++] = (struct embk_put){
        .key = { .object_id = target, .type = EMBK_TYPE_INODE, .offset = 0 }, .del = true };
    ops[nops++] = (struct embk_put){
        .key = { .object_id = dir_oid, .type = EMBK_TYPE_INODE, .offset = 0 },
        .data = (const uint8_t *)&parent_ino, .size = sizeof parent_ino };
    ops[nops++] = dirent_op;

    rc = embkfs_txn_apply_ops(vol, ops, nops, new_gen);   /* may free a node if a leaf emptied/merged */

    kfree(chain_buf);
    if (rc != EMBK_OK) { kprintf("EMBKFS: %s: rmdir \"%s\" failed: %s\n", dev, name, embk_strerror(rc)); return rc; }

    kprintf("EMBKFS: %s: rmdir /%s (object %lu), gen now %lu\n", dev, name, target, vol->generation);
    return EMBK_OK;
}

static int embkfs_list_dir_rec(struct embkfs_volume *vol, const struct embk_block_ptr *ptr,
                               uint64_t dir_oid, embkfs_dirent_cb cb, void *ctx)
{
    uint8_t *buf = kmalloc(vol->block_size);
    if (!buf) return -EMBK_ENOMEM;
    int rc = embkfs_read_node(vol, ptr, buf, vol->block_size);
    if (rc != EMBK_OK) { kfree(buf); return rc; }

    const struct embk_node_header *h = (const struct embk_node_header *)buf;
    const struct embk_key lo = { .object_id = dir_oid, .type = EMBK_TYPE_DIR_ENTRY, .offset = 0 };
    const struct embk_key hi = { .object_id = dir_oid, .type = EMBK_TYPE_DIR_ENTRY, .offset = UINT64_MAX };

    if (h->level == 0) {
        for (uint32_t i = 0; i < h->nritems; i++) {
            const struct embk_item_header *it = embk_leaf_item(buf, i);
            if (it->key.object_id != dir_oid || it->key.type != EMBK_TYPE_DIR_ENTRY) continue;
            const uint8_t *chain = embk_item_data(buf, vol->block_size, it, it->size);
            if (!chain) { kfree(buf); return -EMBK_EINVAL; }
            uint32_t left = it->size;
            while (left >= sizeof(struct embk_dir_entry_item)) {
                const struct embk_dir_entry_item *rec = (const struct embk_dir_entry_item *)chain;
                uint32_t rl = sizeof *rec + rec->name_len;
                if (rl > left) { kfree(buf); return -EMBK_EINVAL; }
                rc = cb(rec->target_object_id, rec->target_type,
                        (const char *)(chain + sizeof *rec), rec->name_len, ctx);
                if (rc != EMBK_OK) { kfree(buf); return rc; }
                chain += rl;
                left -= rl;
            }
        }
        kfree(buf);
        return EMBK_OK;
    }

    if (h->nritems == 0 || h->nritems > EMBKFS_MAX_SLOTS) { kfree(buf); return -EMBK_EINVAL; }
    const struct embk_internal_slot *slots =
        (const struct embk_internal_slot *)(buf + sizeof(struct embk_node_header));
    for (uint32_t i = 0; i < h->nritems; i++) {
        struct embk_key clo;
        memcpy(&clo, &slots[i].key, sizeof clo);
        if (embk_key_cmp(&clo, &hi) > 0) break;
        if (i + 1 < h->nritems) {
            struct embk_key chi;
            memcpy(&chi, &slots[i + 1].key, sizeof chi);
            if (embk_key_cmp(&chi, &lo) <= 0) continue;
        }
        struct embk_block_ptr cp;
        memcpy(&cp, &slots[i].ptr, sizeof cp);
        rc = embkfs_list_dir_rec(vol, &cp, dir_oid, cb, ctx);
        if (rc != EMBK_OK) { kfree(buf); return rc; }
    }
    kfree(buf);
    return EMBK_OK;
}

static int embkfs_path_walk(struct embkfs_volume *vol, uint64_t start_dir_oid,
                            const char *path, uint64_t *out_oid);
static int embkfs_path_parent(struct embkfs_volume *vol, uint64_t start_dir_oid,
                              const char *path, uint64_t *out_parent,
                              char *out_name, size_t out_name_sz);

int embkfs_list_dir(struct embkfs_volume *vol, uint64_t dir_oid,
                    embkfs_dirent_cb cb, void *ctx)
{
    if (!vol || !cb) return -EMBK_EINVAL;
    static uint8_t probe[4096];
    const struct embk_item_header *di =
        embkfs_find_item(vol, dir_oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!di) return -EMBK_ENOENT;
    const struct embk_inode_item *dino = embk_item_data(probe, vol->block_size, di, sizeof *dino);
    if (!dino) return -EMBK_EINVAL;
    if ((dino->mode & EMBKFS_S_IFMT) != EMBKFS_S_IFDIR) return -EMBK_ENOTDIR;
    return embkfs_list_dir_rec(vol, &vol->root, dir_oid, cb, ctx);
}

#define EMBKFS_ITER_STOP 1

struct embkfs_page_ctx {
    uint64_t idx;
    uint64_t skip;
    uint64_t limit;
    uint64_t emitted;
    embkfs_dirent_cb user_cb;
    void *user_ctx;
};

static int embkfs_page_cb(uint64_t target_oid, uint8_t target_type,
                          const char *name, uint8_t name_len, void *ctx)
{
    struct embkfs_page_ctx *p = (struct embkfs_page_ctx *)ctx;
    if (p->idx < p->skip) { p->idx++; return EMBK_OK; }
    if (p->emitted >= p->limit) return EMBKFS_ITER_STOP;

    int rc = p->user_cb(target_oid, target_type, name, name_len, p->user_ctx);
    if (rc != EMBK_OK) return rc;

    p->idx++;
    p->emitted++;
    return EMBK_OK;
}

int embkfs_list_dir_page(struct embkfs_volume *vol, uint64_t dir_oid,
                         uint64_t start_index, uint64_t max_entries,
                         uint64_t *out_next_index, uint64_t *out_emitted,
                         embkfs_dirent_cb cb, void *ctx)
{
    if (!vol || !cb || !out_next_index || !out_emitted) return -EMBK_EINVAL;
    if (max_entries == 0) {
        *out_next_index = start_index;
        *out_emitted = 0;
        return EMBK_OK;
    }

    struct embkfs_page_ctx p = {
        .idx = 0,
        .skip = start_index,
        .limit = max_entries,
        .emitted = 0,
        .user_cb = cb,
        .user_ctx = ctx,
    };

    int rc = embkfs_list_dir(vol, dir_oid, embkfs_page_cb, &p);
    if (rc == EMBKFS_ITER_STOP) rc = EMBK_OK;
    *out_emitted = p.emitted;
    *out_next_index = start_index + p.emitted;
    return rc;
}

static int embkfs_count_cb(uint64_t target_oid, uint8_t target_type,
                           const char *name, uint8_t name_len, void *ctx)
{
    (void)target_oid; (void)target_type; (void)name; (void)name_len;
    uint64_t *n = (uint64_t *)ctx;
    if (*n == UINT64_MAX) return -EMBK_EINVAL;
    (*n)++;
    return EMBK_OK;
}

int embkfs_dir_entry_count(struct embkfs_volume *vol, uint64_t dir_oid,
                           uint64_t *out_count)
{
    if (!out_count) return -EMBK_EINVAL;
    *out_count = 0;
    return embkfs_list_dir(vol, dir_oid, embkfs_count_cb, out_count);
}

static int embkfs_nonempty_cb(uint64_t target_oid, uint8_t target_type,
                              const char *name, uint8_t name_len, void *ctx)
{
    (void)target_oid; (void)target_type; (void)name; (void)name_len; (void)ctx;
    return EMBKFS_ITER_STOP;
}

int embkfs_dir_is_empty(struct embkfs_volume *vol, uint64_t dir_oid,
                        bool *out_empty)
{
    if (!out_empty) return -EMBK_EINVAL;
    int rc = embkfs_list_dir(vol, dir_oid, embkfs_nonempty_cb, NULL);
    if (rc == EMBKFS_ITER_STOP) { *out_empty = false; return EMBK_OK; }
    if (rc != EMBK_OK) return rc;
    *out_empty = true;
    return EMBK_OK;
}

static int embkfs_inode_dtype(struct embkfs_volume *vol, uint64_t oid, uint8_t *out_type)
{
    static uint8_t probe[4096];
    const struct embk_item_header *ii =
        embkfs_find_item(vol, oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ii) return -EMBK_ENOENT;
    const struct embk_inode_item *ino = embk_item_data(probe, vol->block_size, ii, sizeof *ino);
    if (!ino) return -EMBK_EINVAL;

    uint32_t mode = ino->mode & EMBKFS_S_IFMT;
    if (mode == EMBKFS_S_IFDIR) *out_type = EMBKFS_DT_DIR;
    else if (mode == EMBKFS_S_IFLNK) *out_type = EMBKFS_DT_LNK;
    else *out_type = EMBKFS_DT_REG;
    return EMBK_OK;
}

int embkfs_dir_exists_name(struct embkfs_volume *vol, uint64_t dir_oid,
                           const char *name, bool *out_exists,
                           uint8_t *out_type, uint64_t *out_oid)
{
    if (!vol || !name || !out_exists) return -EMBK_EINVAL;
    uint64_t oid;
    int rc = embkfs_lookup(vol, dir_oid, name, &oid);
    if (rc == -EMBK_ENOENT) { *out_exists = false; return EMBK_OK; }
    if (rc != EMBK_OK) return rc;

    *out_exists = true;
    if (out_oid) *out_oid = oid;
    if (out_type) {
        rc = embkfs_inode_dtype(vol, oid, out_type);
        if (rc != EMBK_OK) return rc;
    }
    return EMBK_OK;
}

int embkfs_dir_exists_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                           const char *path, bool *out_exists,
                           uint8_t *out_type, uint64_t *out_oid)
{
    if (!vol || !path || !out_exists) return -EMBK_EINVAL;
    uint64_t oid;
    int rc = embkfs_path_walk(vol, start_dir_oid, path, &oid);
    if (rc == -EMBK_ENOENT) { *out_exists = false; return EMBK_OK; }
    if (rc != EMBK_OK) return rc;

    *out_exists = true;
    if (out_oid) *out_oid = oid;
    if (out_type) {
        rc = embkfs_inode_dtype(vol, oid, out_type);
        if (rc != EMBK_OK) return rc;
    }
    return EMBK_OK;
}

static bool embk_path_comp_eq(const char *comp, size_t len, const char *lit)
{
    size_t i = 0;
    while (lit[i]) i++;
    return len == i && memcmp(comp, lit, len) == 0;
}

static int embkfs_find_parent_dir_rec(struct embkfs_volume *vol, const struct embk_block_ptr *ptr,
                                      uint64_t child_oid, uint64_t *parent_oid, bool *found)
{
    if (*found) return EMBK_OK;

    uint8_t *buf = kmalloc(vol->block_size);
    if (!buf) return -EMBK_ENOMEM;
    int rc = embkfs_read_node(vol, ptr, buf, vol->block_size);
    if (rc != EMBK_OK) { kfree(buf); return rc; }

    const struct embk_node_header *h = (const struct embk_node_header *)buf;
    if (h->level == 0) {
        for (uint32_t i = 0; i < h->nritems && !*found; i++) {
            const struct embk_item_header *it = embk_leaf_item(buf, i);
            if (it->key.type != EMBK_TYPE_DIR_ENTRY) continue;

            const uint8_t *chain = embk_item_data(buf, vol->block_size, it, it->size);
            if (!chain) { kfree(buf); return -EMBK_EINVAL; }

            uint32_t left = it->size;
            while (left >= sizeof(struct embk_dir_entry_item) && !*found) {
                const struct embk_dir_entry_item *rec = (const struct embk_dir_entry_item *)chain;
                uint32_t rl = sizeof *rec + rec->name_len;
                if (rl > left) { kfree(buf); return -EMBK_EINVAL; }

                if (rec->target_type == EMBKFS_DT_DIR && rec->target_object_id == child_oid) {
                    *parent_oid = it->key.object_id;
                    *found = true;
                    break;
                }

                chain += rl;
                left -= rl;
            }
        }
        kfree(buf);
        return EMBK_OK;
    }

    if (h->nritems == 0 || h->nritems > EMBKFS_MAX_SLOTS) { kfree(buf); return -EMBK_EINVAL; }
    const struct embk_internal_slot *slots =
        (const struct embk_internal_slot *)(buf + sizeof(struct embk_node_header));
    for (uint32_t i = 0; i < h->nritems && !*found; i++) {
        struct embk_block_ptr cp;
        memcpy(&cp, &slots[i].ptr, sizeof cp);
        rc = embkfs_find_parent_dir_rec(vol, &cp, child_oid, parent_oid, found);
        if (rc != EMBK_OK) { kfree(buf); return rc; }
    }
    kfree(buf);
    return EMBK_OK;
}

static int embkfs_parent_dir_oid(struct embkfs_volume *vol, uint64_t child_oid, uint64_t *out_parent)
{
    if (child_oid == EMBKFS_ROOT_OBJECT_ID) {
        *out_parent = EMBKFS_ROOT_OBJECT_ID;
        return EMBK_OK;
    }

    bool found = false;
    uint64_t parent = EMBKFS_ROOT_OBJECT_ID;
    int rc = embkfs_find_parent_dir_rec(vol, &vol->root, child_oid, &parent, &found);
    if (rc != EMBK_OK) return rc;
    if (!found) return -EMBK_ENOENT;
    *out_parent = parent;
    return EMBK_OK;
}

static int embkfs_path_walk_norm(struct embkfs_volume *vol, uint64_t start_dir_oid,
                                 const char *path, uint64_t *out_oid)
{
    if (!vol || !path || !out_oid) return -EMBK_EINVAL;

    uint64_t cur = (*path == '/') ? EMBKFS_ROOT_OBJECT_ID : start_dir_oid;
    const char *s = path;
    while (*s == '/') s++;
    if (*s == '\0') { *out_oid = cur; return EMBK_OK; }

    for (;;) {
        const char *comp = s;
        size_t len = 0;
        while (s[len] && s[len] != '/') len++;
        if (len == 0 || len > 255) return -EMBK_EINVAL;

        int rc;
        if (embk_path_comp_eq(comp, len, ".")) {
            /* stay on current directory */
        } else if (embk_path_comp_eq(comp, len, "..")) {
            rc = embkfs_parent_dir_oid(vol, cur, &cur);
            if (rc != EMBK_OK) return rc;
        } else {
            char name[256];
            memcpy(name, comp, len);
            name[len] = '\0';
            rc = embkfs_lookup(vol, cur, name, &cur);
            if (rc != EMBK_OK) return rc;
        }

        s = comp + len;
        while (*s == '/') s++;
        if (*s == '\0') {
            *out_oid = cur;
            return EMBK_OK;
        }
    }
}

static int embkfs_path_parent_norm(struct embkfs_volume *vol, uint64_t start_dir_oid,
                                   const char *path, uint64_t *out_parent,
                                   char *out_name, size_t out_name_sz)
{
    if (!vol || !path || !out_parent || !out_name || out_name_sz < 256) return -EMBK_EINVAL;

    size_t plen = strlen(path);
    if (plen == 0 || (plen == 1 && path[0] == '/')) return -EMBK_EINVAL;

    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') slash = p;
    }

    const char *leaf = slash ? slash + 1 : path;
    size_t llen = strlen(leaf);
    if (llen == 0 || llen > 255) return -EMBK_EINVAL;
    if (embk_path_comp_eq(leaf, llen, ".") || embk_path_comp_eq(leaf, llen, ".."))
        return -EMBK_EINVAL;

    if (!slash) {
        *out_parent = start_dir_oid;
    } else if (slash == path) {
        *out_parent = EMBKFS_ROOT_OBJECT_ID;
    } else {
        char parent_path[1024];
        size_t parent_len = (size_t)(slash - path);
        if (parent_len >= sizeof parent_path) return -EMBK_EINVAL;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        int rc = embkfs_path_walk_norm(vol, start_dir_oid, parent_path, out_parent);
        if (rc != EMBK_OK) return rc;
    }

    memcpy(out_name, leaf, llen + 1);
    return EMBK_OK;
}

int embkfs_normalize_path(const char *path, char *out, size_t out_sz)
{
    if (!path || !out || out_sz == 0) return -EMBK_EINVAL;

    bool abs = (*path == '/');
    const char *parts[256];
    uint8_t lens[256];
    size_t n = 0;
    const char *s = path;

    while (*s == '/') s++;
    while (*s) {
        const char *comp = s;
        size_t len = 0;
        while (s[len] && s[len] != '/') len++;
        if (len == 0 || len > 255) return -EMBK_EINVAL;

        if (embk_path_comp_eq(comp, len, ".")) {
            /* collapse '.' */
        } else if (embk_path_comp_eq(comp, len, "..")) {
            /* For relative paths, preserve unresolved leading '..' components. */
            if (n > 0 && !embk_path_comp_eq(parts[n - 1], lens[n - 1], "..")) {
                n--;
            } else if (!abs) {
                if (n >= 256) return -EMBK_EINVAL;
                parts[n] = comp;
                lens[n] = (uint8_t)len;
                n++;
            }
        } else {
            if (n >= 256) return -EMBK_EINVAL;
            parts[n] = comp;
            lens[n] = (uint8_t)len;
            n++;
        }

        s = comp + len;
        while (*s == '/') s++;
    }

    size_t pos = 0;
    if (abs) {
        if (pos + 1 >= out_sz) return -EMBK_EINVAL;
        out[pos++] = '/';
    }

    for (size_t i = 0; i < n; i++) {
        if ((abs && pos > 1) || (!abs && pos > 0)) {
            if (pos + 1 >= out_sz) return -EMBK_EINVAL;
            out[pos++] = '/';
        }
        if (pos + lens[i] >= out_sz) return -EMBK_EINVAL;
        memcpy(out + pos, parts[i], lens[i]);
        pos += lens[i];
    }

    if (pos == 0) {
        if (out_sz < 2) return -EMBK_EINVAL;
        out[pos++] = '.';
    }
    out[pos] = '\0';
    return EMBK_OK;
}

static int embkfs_path_walk(struct embkfs_volume *vol, uint64_t start_dir_oid,
                            const char *path, uint64_t *out_oid)
{
    char norm[1024];
    int rc = embkfs_normalize_path(path, norm, sizeof norm);
    if (rc != EMBK_OK) return rc;
    return embkfs_path_walk_norm(vol, start_dir_oid, norm, out_oid);
}

static int embkfs_path_parent(struct embkfs_volume *vol, uint64_t start_dir_oid,
                              const char *path, uint64_t *out_parent,
                              char *out_name, size_t out_name_sz)
{
    char norm[1024];
    int rc = embkfs_normalize_path(path, norm, sizeof norm);
    if (rc != EMBK_OK) return rc;
    return embkfs_path_parent_norm(vol, start_dir_oid, norm, out_parent, out_name, out_name_sz);
}

int embkfs_lookup_name(struct embkfs_volume *vol, uint64_t dir_oid,
                       const char *name, uint64_t *out_oid)
{
    return embkfs_lookup(vol, dir_oid, name, out_oid);
}

int embkfs_lookup_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                       const char *path, uint64_t *out_oid)
{
    return embkfs_path_walk(vol, start_dir_oid, path, out_oid);
}

int embkfs_create_file(struct embkfs_volume *vol, uint64_t dir_oid,
                       const char *name, uint64_t *out_oid)
{
    return embkfs_create(vol, dir_oid, name, out_oid);
}

int embkfs_create_file_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                            const char *path, uint64_t *out_oid)
{
    char leaf[256];
    uint64_t parent;
    int rc = embkfs_path_parent(vol, start_dir_oid, path, &parent, leaf, sizeof leaf);
    if (rc != EMBK_OK) return rc;
    return embkfs_create(vol, parent, leaf, out_oid);
}

int embkfs_mkdir_name(struct embkfs_volume *vol, uint64_t dir_oid,
                      const char *name, uint64_t *out_oid)
{
    return embkfs_mkdir(vol, dir_oid, name, out_oid);
}

int embkfs_mkdir_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                      const char *path, uint64_t *out_oid)
{
    char leaf[256];
    uint64_t parent;
    int rc = embkfs_path_parent(vol, start_dir_oid, path, &parent, leaf, sizeof leaf);
    if (rc != EMBK_OK) return rc;
    return embkfs_mkdir(vol, parent, leaf, out_oid);
}

static int embkfs_read_object_prefix(struct embkfs_volume *vol, uint64_t oid,
                                     uint8_t *out, uint64_t want,
                                     uint64_t *out_total_len, uint32_t *out_mode);

/* Range read that skips the prefix entirely -- see its definition below. */
static int embkfs_read_object_range(struct embkfs_volume *vol, uint64_t oid,
                                    uint64_t offset, uint8_t *out, uint64_t len,
                                    uint64_t *out_read);

/* Get an object's extent map, from the (oid, generation)-keyed cache when
 * possible; collect + validate + install it on a miss.
 *
 * Collecting a map is TWO FULL B-TREE SCANS (count_extents, then
 * collect_extents), each reading a node per leaf holding this object's extents,
 * plus a kmalloc and an O(n) validate. read_object_range cached that;
 * read_object_prefix did NOT, and it is the path EVERY file <= EMBKFS_RCACHE_MAX
 * takes -- i.e. every app load -- so it paid both scans on every call. Measured:
 * ~95% of this workload's device reads are B-tree nodes, so uncached scans are
 * exactly the wrong thing to leave lying around.
 *
 * ⚠️ OWNERSHIP: the returned array is owned by the CACHE, never by the caller.
 * Callers must NOT kfree it -- doing so hands the next cache hit freed memory.
 * (That bug has already been made once here; it cost seven kfree sites.)
 * The pointer stays valid until the next miss on a DIFFERENT (oid, generation),
 * so a caller must not hold it across another object's read.
 *
 * `out_vfy` returns the parallel per-extent "already verified this generation"
 * array, or NULL if the caller doesn't need it. */
static int embkfs_extents_cached(struct embkfs_volume *vol, uint64_t oid,
                                 uint64_t inode_size, const char *where,
                                 struct embk_extref **out_ext, uint32_t *out_n,
                                 uint8_t **out_vfy);

/* Fetch an object's inode item, from the (oid, generation)-keyed cache when
 * possible. Finding an inode is a B-tree DESCENT THAT READS BLOCKS, and the read
 * path did two per call (the size probe in read_object_at + read_object_range's
 * own lookup) -- measured at ~2 of the ~3 device reads an 8 KB chunk cost, when
 * only one of them is the data. A write commit bumps `generation`, so a stale
 * entry can never be served. Returns a COPY, not a pointer into a shared probe
 * buffer, so callers can't be invalidated by the next lookup. */
static int embkfs_inode_cached(struct embkfs_volume *vol, uint64_t oid,
                               struct embk_inode_item *out)
{
    static uint8_t probe_i[4096];

    if (!vol || !out) return -EMBK_EINVAL;

    if (vol->icache_valid && vol->icache_oid == oid &&
        vol->icache_gen == vol->generation) {
        *out = vol->icache_ino;
        return EMBK_OK;
    }

    const struct embk_item_header *ii =
        embkfs_find_item(vol, oid, EMBK_TYPE_INODE, 0, probe_i, sizeof probe_i);
    if (!ii) return -EMBK_ENOENT;
    const struct embk_inode_item *ino =
        embk_item_data(probe_i, vol->block_size, ii, sizeof *ino);
    if (!ino) return -EMBK_EINVAL;

    vol->icache_ino   = *ino;          /* copy: probe_i is reused by the next call */
    vol->icache_oid   = oid;
    vol->icache_gen   = vol->generation;
    vol->icache_valid = true;
    *out = *ino;
    return EMBK_OK;
}

int embkfs_write_object(struct embkfs_volume *vol, uint64_t oid,
                        const uint8_t *data, uint64_t len)
{
    return embkfs_write_file(vol, oid, data, len);
}

/* Copy object oid's raw inode item out to *out (a full 128-byte struct
 * copy, not a live pointer into the read buffer) -- size/mode/links/
 * timestamps/generation, everything a `stat`-style caller needs. Read-
 * only, takes no lock beyond whatever a single tree lookup needs. */
int embkfs_stat_object(struct embkfs_volume *vol, uint64_t oid,
                       struct embk_inode_item *out)
{
    static uint8_t probe[4096];
    if (!vol || !out) return -EMBK_EINVAL;

    const struct embk_item_header *ii =
        embkfs_find_item(vol, oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ii) return -EMBK_ENOENT;
    const struct embk_inode_item *ino = embk_item_data(probe, vol->block_size, ii, sizeof *ino);
    if (!ino) return -EMBK_EINVAL;

    *out = *ino;
    return EMBK_OK;
}

int embkfs_read_object(struct embkfs_volume *vol, uint64_t oid,
                       uint8_t *buf, uint64_t buf_sz,
                       uint64_t *out_read)
{
    if (!vol || !buf || !out_read) return -EMBK_EINVAL;
    uint64_t total = 0;
    int rc = embkfs_read_object_prefix(vol, oid, buf, buf_sz, &total, NULL);
    if (rc != EMBK_OK) return rc;
    *out_read = (total < buf_sz) ? total : buf_sz;
    return EMBK_OK;
}

/* Cap on the whole-object read cache (embkfs_read_object_at): files up to this
 * size are cached for fast sequential reads; larger ones use the direct path. */
#define EMBKFS_RCACHE_MAX (8u * 1024u * 1024u)

/* Read-ahead window for objects too big for the whole-object cache above.
 * MUST be a power of two (the lookup masks the offset) and a multiple of the
 * 4096-byte fs block. 64 KB = 8 app-sized (8 KB) reads per fill, and matches
 * EMBKFS_IO_BATCH_BLOCKS below so one window is exactly one device request --
 * the two constants are meant to be kept equal.
 *
 * RELATED, ACROSS THE LANGUAGE BOUNDARY, to mkfs's EXTENT_MAX_BYTES
 * (tools/embkfs_mkfs/mkfs_embkfs.py, currently 1 MiB). An extent's checksum
 * covers the whole extent, so cold amplification is (2E - W)/E for extent E and
 * window W -- measured 194% at E=1MiB/W=64KiB, matching the model exactly.
 * Enlarging this window shrinks that; E <= W would reach 100%.
 *
 * But do NOT chase that number by shrinking extents: A/B'd, E=64KiB hit 101%
 * cold and cost ~85% MORE device time system-wide, because ~95% of device reads
 * on a real workload are B-TREE NODE reads and more extents means a bigger tree
 * for every COW rebuild to walk. See the long note at that constant. */
#define EMBKFS_WCACHE_WIN (64u * 1024u)

/* Blocks fetched per DEVICE request. The storage cost that dominates is PER
 * REQUEST -- ATA command setup plus a busy-wait for the DMA completion IRQ,
 * ~2.7 ms under TCG -- and it does NOT scale with transfer size. Reading one
 * 4 KB block per request therefore pinned throughput near 1.5 MB/s (a 2 GB read
 * would take ~23 minutes) even though the transfer itself is real bus-master
 * DMA: at 4 KB the setup IS the cost, so DMA had nothing to amortise over.
 *
 * 16 blocks = 64 KB, equal to EMBKFS_WCACHE_WIN so one window fill is exactly
 * ONE device request. This was 8 (32 KB) "to match block.c's block_bounce[64*512]
 * cap" -- but that cap only applies to buffers that FAIL buffer_dma_ok() and get
 * bounced, and iobatch is BSS precisely so it does not (see below). The bounce
 * buffer was never in this path, so 32 KB was a self-imposed limit. The real
 * per-command ceiling is the ATA driver's 255-sector chunk (~127 KB) in
 * ata_block_read(); 128 sectors stays comfortably inside it, and a contiguous
 * 64 KB span needs at most 2 of the 8 available PRD entries.
 *
 * iobatch lives in kernel BSS on purpose: buffer_dma_ok() requires the kernel
 * range and a <4 GB physical address, both true here, so the block layer DMAs
 * straight into it with NO bounce copy. (kmalloc memory would fail that test --
 * KHEAP_BASE sits below KERNEL_VIRTUAL_BASE, and heap pages are physically
 * scattered anyway, so KV2P on them is meaningless.) Sharing one static is
 * consistent with the probe[]/datablk[] statics already in this file -- every FS
 * op runs under embk_vfs.c's big lock. */
#define EMBKFS_IO_BATCH_BLOCKS 16
static uint8_t iobatch[EMBKFS_IO_BATCH_BLOCKS * 4096] __attribute__((aligned(16)));

int embkfs_read_object_at(struct embkfs_volume *vol, uint64_t oid,
                          uint64_t offset, uint8_t *buf, uint64_t len,
                          uint64_t *out_read)
{
    if (!vol || !buf || !out_read) return -EMBK_EINVAL;
    *out_read = 0;
    if (len == 0) return EMBK_OK;

    /* Fast path: whole-object read cache. This is the O(n^2) fix -- previously
     * EVERY call re-decoded the entire [0, offset+want) prefix, so a file read
     * sequentially in K chunks re-read growing prefixes (K*(K+1)/2 blocks). */
    if (vol->rcache_buf && vol->rcache_oid == oid && vol->rcache_gen == vol->generation) {
        uint64_t total = vol->rcache_len;
        if (offset >= total) return EMBK_OK;
        uint64_t want = total - offset;
        if (want > len) want = len;
        memcpy(buf, vol->rcache_buf + offset, want);
        *out_read = want;
        return EMBK_OK;
    }

    /* Size from the cached inode. This used to be
     * `embkfs_read_object_prefix(vol, oid, NULL, 0, &total, NULL)` -- want==0 so
     * it decoded nothing, but it still paid a full B-tree descent (block reads)
     * on EVERY read just to learn ino->size. That was one of the ~2 redundant
     * device reads per chunk. */
    struct embk_inode_item ino_at;
    int rc = embkfs_inode_cached(vol, oid, &ino_at);
    if (rc != EMBK_OK) return rc;
    /* Same type gate read_object_prefix applied -- keep refusing dirs/devices. */
    uint32_t mode_at = ino_at.mode & EMBKFS_S_IFMT;
    if (mode_at != EMBKFS_S_IFREG && mode_at != EMBKFS_S_IFLNK) return -EMBK_EINVAL;
    uint64_t total = ino_at.size;
    if (offset >= total) return EMBK_OK;

    uint64_t want = total - offset;
    if (want > len) want = len;

    /* Decode the whole object ONCE into the cache (capped so a huge file can't
     * pin unbounded RAM), then serve. On a miss for a >cap file, or on OOM,
     * fall back to the original direct prefix read below. */
    if (total <= EMBKFS_RCACHE_MAX) {
        uint8_t *nb = kmalloc(total ? total : 1);
        if (nb) {
            rc = embkfs_read_object_prefix(vol, oid, nb, total, NULL, NULL);
            if (rc != EMBK_OK) { kfree(nb); return rc; }
            if (vol->rcache_buf) kfree(vol->rcache_buf);
            vol->rcache_buf = nb;
            vol->rcache_oid = oid;
            vol->rcache_gen = vol->generation;
            vol->rcache_len = total;
            memcpy(buf, nb + offset, want);
            *out_read = want;
            return EMBK_OK;
        }
    }

    /* Too big for the whole-object cache: serve from a read-ahead WINDOW.
     *
     * Only serve a request that fits ENTIRELY inside the window. A partial serve
     * would mean returning a short read mid-file, and short reads on this path
     * have already cost us one silent data-loss bug -- not worth re-opening for
     * the one read in eight that straddles a boundary. Those fall through to the
     * direct range read below, which is exactly what every read did before. */
    uint64_t woff = offset & ~(uint64_t)(EMBKFS_WCACHE_WIN - 1);
    bool whit = vol->wcache_buf && vol->wcache_oid == oid &&
                vol->wcache_gen == vol->generation && vol->wcache_off == woff;

    if (!whit) {
        if (!vol->wcache_buf) vol->wcache_buf = kmalloc(EMBKFS_WCACHE_WIN);
        if (vol->wcache_buf) {
            uint64_t got = 0;
            rc = embkfs_read_object_range(vol, oid, woff, vol->wcache_buf,
                                          EMBKFS_WCACHE_WIN, &got);
            if (rc == EMBK_OK && got > 0) {
                vol->wcache_oid = oid;
                vol->wcache_gen = vol->generation;
                vol->wcache_off = woff;
                vol->wcache_len = got;
                whit = true;
            } else {
                /* Don't leave a half-filled window installed as valid. */
                vol->wcache_len = 0;
                vol->wcache_gen = 0;
            }
        }
    }

    if (whit && offset >= woff && (offset - woff) + want <= vol->wcache_len) {
        memcpy(buf, vol->wcache_buf + (offset - woff), want);
        *out_read = want;
        return EMBK_OK;
    }

    /* Window unusable (OOM, straddles the end, or read failed). Serve the range
     * DIRECTLY instead of decoding [0, offset+want) into a scratch buffer first.
     *
     * The old code here did exactly that -- kmalloc(offset+want) + a full prefix
     * decode, PER READ -- which made every read O(offset) in both time and
     * memory, so reading a >RCACHE_MAX file sequentially was O(n^2). That is what
     * froze CPython: python314.zip is 10.3 MB, over the 8 MB cap, and zipimport
     * reads its central directory at ~10 MB offsets. read_object_range() skips
     * the extents below `offset` without touching the disk. */
    return embkfs_read_object_range(vol, oid, offset, buf, want, out_read);
}

int embkfs_write_object_at(struct embkfs_volume *vol, uint64_t oid,
                           uint64_t offset, const uint8_t *data, uint64_t len,
                           uint64_t *out_written)
{
    if (!vol || (!data && len) || !out_written) return -EMBK_EINVAL;
    *out_written = 0;
    if (len == 0) return EMBK_OK;

    if (offset > UINT64_MAX - len) return -EMBK_EINVAL;

    uint64_t old_size = 0;
    int rc = embkfs_read_object_prefix(vol, oid, NULL, 0, &old_size, NULL);
    if (rc != EMBK_OK) return rc;

    uint64_t end = offset + len;
    uint64_t new_size = (end > old_size) ? end : old_size;

    uint8_t *buf = kmalloc(new_size ? new_size : 1);
    if (!buf) return -EMBK_ENOMEM;
    if (new_size) memset(buf, 0, new_size);

    if (old_size) {
        rc = embkfs_read_object_prefix(vol, oid, buf, old_size, NULL, NULL);
        if (rc != EMBK_OK) { kfree(buf); return rc; }
    }

    memcpy(buf + offset, data, len);
    rc = embkfs_write_file(vol, oid, buf, new_size);
    kfree(buf);
    if (rc != EMBK_OK) return rc;

    *out_written = len;
    return EMBK_OK;
}

int embkfs_append_object(struct embkfs_volume *vol, uint64_t oid,
                         const uint8_t *data, uint64_t len,
                         uint64_t *out_written)
{
    if (!vol || (!data && len) || !out_written) return -EMBK_EINVAL;
    uint64_t size = 0;
    int rc = embkfs_read_object_prefix(vol, oid, NULL, 0, &size, NULL);
    if (rc != EMBK_OK) return rc;
    return embkfs_write_object_at(vol, oid, size, data, len, out_written);
}

int embkfs_seek_object(struct embkfs_volume *vol, uint64_t oid,
                       int64_t current_offset, int whence, int64_t delta,
                       uint64_t *out_offset)
{
    if (!vol || !out_offset) return -EMBK_EINVAL;

    uint64_t size = 0;
    int rc = embkfs_read_object_prefix(vol, oid, NULL, 0, &size, NULL);
    if (rc != EMBK_OK) return rc;

    int64_t base;
    if (whence == EMBKFS_SEEK_SET) base = 0;
    else if (whence == EMBKFS_SEEK_CUR) {
        if (current_offset < 0) return -EMBK_EINVAL;
        base = current_offset;
    }
    else if (whence == EMBKFS_SEEK_END) {
        if (size > (uint64_t)INT64_MAX) return -EMBK_ERANGE;
        base = (int64_t)size;
    } else {
        return -EMBK_EINVAL;
    }

    if ((delta > 0 && base > INT64_MAX - delta) ||
        (delta < 0 && base < INT64_MIN - delta))
        return -EMBK_ERANGE;

    int64_t pos = base + delta;
    if (pos < 0) return -EMBK_EINVAL;
    *out_offset = (uint64_t)pos;
    return EMBK_OK;
}

int embkfs_unlink_name(struct embkfs_volume *vol, uint64_t dir_oid,
                       const char *name)
{
    return embkfs_unlink(vol, dir_oid, name);
}

int embkfs_unlink_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                       const char *path)
{
    char leaf[256];
    uint64_t parent;
    int rc = embkfs_path_parent(vol, start_dir_oid, path, &parent, leaf, sizeof leaf);
    if (rc != EMBK_OK) return rc;
    return embkfs_unlink(vol, parent, leaf);
}

int embkfs_remove_entry_name(struct embkfs_volume *vol, uint64_t dir_oid,
                             const char *name)
{
    if (!vol || !name) return -EMBK_EINVAL;

    uint64_t target;
    int rc = embkfs_lookup(vol, dir_oid, name, &target);
    if (rc != EMBK_OK) return rc;

    static uint8_t probe[4096];
    const struct embk_item_header *ii =
        embkfs_find_item(vol, target, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ii) return -EMBK_ENOENT;
    const struct embk_inode_item *ino = embk_item_data(probe, vol->block_size, ii, sizeof *ino);
    if (!ino) return -EMBK_EINVAL;

    if ((ino->mode & EMBKFS_S_IFMT) == EMBKFS_S_IFDIR)
        return embkfs_rmdir(vol, dir_oid, name);
    return embkfs_unlink(vol, dir_oid, name);
}

int embkfs_remove_entry_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                             const char *path)
{
    if (!vol || !path) return -EMBK_EINVAL;
    char leaf[256];
    uint64_t parent;
    int rc = embkfs_path_parent(vol, start_dir_oid, path, &parent, leaf, sizeof leaf);
    if (rc != EMBK_OK) return rc;
    return embkfs_remove_entry_name(vol, parent, leaf);
}

int embkfs_rmdir_name(struct embkfs_volume *vol, uint64_t dir_oid,
                      const char *name)
{
    return embkfs_rmdir(vol, dir_oid, name);
}

int embkfs_rmdir_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                      const char *path)
{
    char leaf[256];
    uint64_t parent;
    int rc = embkfs_path_parent(vol, start_dir_oid, path, &parent, leaf, sizeof leaf);
    if (rc != EMBK_OK) return rc;
    return embkfs_rmdir(vol, parent, leaf);
}

int embkfs_rename_name(struct embkfs_volume *vol,
                       uint64_t old_dir_oid, const char *old_name,
                       uint64_t new_dir_oid, const char *new_name)
{
    return embkfs_rename(vol, old_dir_oid, old_name, new_dir_oid, new_name);
}

int embkfs_rename_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                       const char *old_path, const char *new_path)
{
    char old_leaf[256], new_leaf[256];
    uint64_t old_parent, new_parent;

    int rc = embkfs_path_parent(vol, start_dir_oid, old_path, &old_parent, old_leaf, sizeof old_leaf);
    if (rc != EMBK_OK) return rc;
    rc = embkfs_path_parent(vol, start_dir_oid, new_path, &new_parent, new_leaf, sizeof new_leaf);
    if (rc != EMBK_OK) return rc;

    return embkfs_rename(vol, old_parent, old_leaf, new_parent, new_leaf);
}

/* DELIBERATELY NOT on the shared extent cache (unlike read_object_prefix and
 * read_object_range). Its only caller is embkfs_readlink_object(): symlink
 * targets are tiny single-extent objects and readlink is not a hot path, while
 * the ecache is SINGLE-SLOT -- routing this through it would let a symlink
 * evict a large file's extent map for no gain. It owns its own array, so the
 * kfree(ext) calls below are correct here and must NOT be "cleaned up" to match
 * the other two. */
static int embkfs_read_object_data(struct embkfs_volume *vol, uint64_t oid,
                                   uint8_t *out, uint64_t out_sz,
                                   uint64_t *out_len, uint32_t *out_mode)
{
    static uint8_t probe[4096];
    static uint8_t datablk[4096];

    const struct embk_item_header *ii =
        embkfs_find_item(vol, oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ii) return -EMBK_ENOENT;
    const struct embk_inode_item *ino = embk_item_data(probe, vol->block_size, ii, sizeof *ino);
    if (!ino) return -EMBK_EINVAL;

    uint32_t mode = ino->mode & EMBKFS_S_IFMT;
    if (mode != EMBKFS_S_IFREG && mode != EMBKFS_S_IFLNK) return -EMBK_EINVAL;
    if (out_mode) *out_mode = mode;
    if (out_len) *out_len = ino->size;
    if (ino->size == 0) return EMBK_OK;
    if (!out || out_sz < ino->size) return -EMBK_ERANGE;

    uint32_t en = 0;
    int rc = embkfs_count_extents(vol, oid, &en);
    if (rc != EMBK_OK) return rc;
    if (en == 0) return -EMBK_EINVAL;

    struct embk_extref *ext = kmalloc((uint64_t)en * sizeof *ext);
    if (!ext) return -EMBK_ENOMEM;
    uint32_t got = 0; bool over = false;
    rc = embkfs_collect_extents(vol, oid, ext, en, &got, &over);
    if (rc != EMBK_OK || over || got != en) { kfree(ext); return -EMBK_EINVAL; }
    rc = embkfs_validate_extent_map(vol, ext, en, ino->size, "read_object_data");
    if (rc != EMBK_OK) { kfree(ext); return rc; }

    uint64_t spb = vol->block_size / vol->dev->block_size;
    uint64_t pos = 0;
    for (uint32_t i = 0; i < en; i++) {
        if (ext[i].flags & EMBKFS_EXTENT_F_HOLE) {
            memset(out + pos, 0, ext[i].logical_size);
            pos += ext[i].logical_size;
            continue;
        }
        bool enc = (ext[i].flags & EMBKFS_EXTENT_F_ENCRYPTED) != 0;
        if (ext[i].flags & EMBKFS_EXTENT_F_COMPRESSED) {
            /* This simple LZ scheme has no random-access decode: producing
             * ANY logical byte requires the whole compressed blob, so read
             * + decrypt + verify + decompress it in full, then hand back
             * the slice the caller wants (v2.2 Phase 3/4). */
            uint64_t comp_size = ext[i].compressed_size;
            uint8_t *raw = kmalloc(comp_size ? comp_size : 1);
            if (!raw) { kfree(ext); return -EMBK_ENOMEM; }
            uint32_t csum = 0; uint64_t written = 0;
            for (uint64_t blk = 0; blk < ext[i].length; blk++) {
                uint64_t chunk = comp_size - written;
                if (chunk > vol->block_size) chunk = vol->block_size;
                rc = embk_block_read(vol->dev, (ext[i].disk_block + blk) * spb, spb, datablk);
                if (rc != EMBK_OK) { kfree(raw); kfree(ext); return rc; }
                /* Checksum the ON-DISK bytes (ciphertext when encrypted)
                 * BEFORE decrypting -- it must match what write time
                 * hashed, and decrypting first would hash plaintext
                 * instead, failing verification on every encrypted block. */
                csum = embk_crc32c(datablk, enc ? vol->block_size : chunk, csum);
                if (enc) {
                    aes_xts_decrypt(embkfs_xts(vol), ext[i].disk_block + blk, datablk, datablk, vol->block_size);
                }
                memcpy(raw + written, datablk, chunk);
                written += chunk;
            }
            if (csum != ext[i].checksum) { kfree(raw); kfree(ext); return -EMBK_EINVAL; }
            bool dok = embk_decompress(raw, (uint32_t)comp_size, out + pos, (uint32_t)ext[i].logical_size);
            kfree(raw);
            if (!dok) { kfree(ext); return -EMBK_EINVAL; }
            pos += ext[i].logical_size;
            continue;
        }
        uint32_t csum = 0;
        uint64_t written = 0;
        for (uint64_t blk = 0; blk < ext[i].length; blk++) {
            uint64_t chunk = ext[i].logical_size - written;
            if (chunk > vol->block_size) chunk = vol->block_size;
            rc = embk_block_read(vol->dev, (ext[i].disk_block + blk) * spb, spb, datablk);
            if (rc != EMBK_OK) { kfree(ext); return rc; }
            csum = embk_crc32c(datablk, enc ? vol->block_size : chunk, csum);
            if (enc) {
                aes_xts_decrypt(embkfs_xts(vol), ext[i].disk_block + blk, datablk, datablk, vol->block_size);
            }
            memcpy(out + pos, datablk, chunk);
            pos += chunk;
            written += chunk;
        }
        if (csum != ext[i].checksum) { kfree(ext); return -EMBK_EINVAL; }
    }
    kfree(ext);
    return EMBK_OK;
}

/* See the declaration above for the ownership contract. */
static int embkfs_extents_cached(struct embkfs_volume *vol, uint64_t oid,
                                 uint64_t inode_size, const char *where,
                                 struct embk_extref **out_ext, uint32_t *out_n,
                                 uint8_t **out_vfy)
{
    if (!vol || !out_ext || !out_n) return -EMBK_EINVAL;

    if (vol->ecache_ext && vol->ecache_oid == oid &&
        vol->ecache_gen == vol->generation) {
        g_efs_stat.ecache_hit++;
        *out_ext = vol->ecache_ext;
        *out_n   = vol->ecache_n;
        if (out_vfy) *out_vfy = vol->ecache_verified;
        return EMBK_OK;
    }

    g_efs_stat.ecache_miss++;
    uint32_t en = 0;
    int rc = embkfs_count_extents(vol, oid, &en);
    if (rc != EMBK_OK) return rc;
    if (en == 0) return -EMBK_EINVAL;

    struct embk_extref *ext = kmalloc((uint64_t)en * sizeof *ext);
    if (!ext) return -EMBK_ENOMEM;
    uint32_t got = 0; bool over = false;
    rc = embkfs_collect_extents(vol, oid, ext, en, &got, &over);
    if (rc != EMBK_OK || over || got != en) { kfree(ext); return -EMBK_EINVAL; }
    rc = embkfs_validate_extent_map(vol, ext, en, inode_size, where);
    if (rc != EMBK_OK) { kfree(ext); return rc; }

    uint8_t *vfy = kmalloc(en);
    if (!vfy) { kfree(ext); return -EMBK_ENOMEM; }
    memset(vfy, 0, en);                  /* nothing verified yet */

    /* Install last: until this point `ext` is still ours to free, and after it
     * the cache owns it. Free the OLD entry only now, so an early return above
     * can never have dropped a live cache. */
    if (vol->ecache_ext) kfree(vol->ecache_ext);
    if (vol->ecache_verified) kfree(vol->ecache_verified);
    vol->ecache_ext      = ext;
    vol->ecache_verified = vfy;
    vol->ecache_n        = en;
    vol->ecache_oid      = oid;
    vol->ecache_gen      = vol->generation;

    *out_ext = ext;
    *out_n   = en;
    if (out_vfy) *out_vfy = vfy;
    return EMBK_OK;
}

static int embkfs_read_object_prefix(struct embkfs_volume *vol, uint64_t oid,
                                     uint8_t *out, uint64_t want,
                                     uint64_t *out_total_len, uint32_t *out_mode)
{
    static uint8_t probe[4096];
    static uint8_t datablk[4096];

    g_efs_stat.prefix_calls++;

    const struct embk_item_header *ii =
        embkfs_find_item(vol, oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ii) return -EMBK_ENOENT;
    const struct embk_inode_item *ino = embk_item_data(probe, vol->block_size, ii, sizeof *ino);
    if (!ino) return -EMBK_EINVAL;

    uint32_t mode = ino->mode & EMBKFS_S_IFMT;
    if (mode != EMBKFS_S_IFREG && mode != EMBKFS_S_IFLNK) return -EMBK_EINVAL;
    if (out_mode) *out_mode = mode;
    if (out_total_len) *out_total_len = ino->size;
    if (want == 0 || ino->size == 0) return EMBK_OK;
    if (!out) return -EMBK_EINVAL;

    uint64_t need = (want < ino->size) ? want : ino->size;

    /* Extent map from the shared (oid, generation) cache; was an uncached
     * count_extents + collect_extents + validate here. The array below is OWNED
     * BY THE CACHE: nothing in this function may kfree(ext).
     *
     * ⚠️ DON'T EXPECT SPEED FROM THIS -- measured identical (5401 device reads,
     * 5152 node reads, before and after). This path is COLD BY CONSTRUCTION:
     * read_object_at fills rcache with the WHOLE object for anything
     * <= EMBKFS_RCACHE_MAX and then serves from RAM, so prefix runs about ONCE
     * per file per generation, and the one collect it does is the one a cache
     * miss would have done anyway. ("No cache here" was a true observation and a
     * false problem.) What the sharing buys is one less copy of the install
     * block, and no per-call kmalloc of the extent array. */
    struct embk_extref *ext;
    uint32_t en = 0;
    int rc = embkfs_extents_cached(vol, oid, ino->size, "read_object_prefix",
                                   &ext, &en, NULL);
    if (rc != EMBK_OK) return rc;

    uint64_t spb = vol->block_size / vol->dev->block_size;
    uint64_t pos = 0;
    for (uint32_t i = 0; i < en && pos < need; i++) {
        if (ext[i].flags & EMBKFS_EXTENT_F_HOLE) {
            uint64_t take = need - pos;
            if (take > ext[i].logical_size) take = ext[i].logical_size;
            memset(out + pos, 0, take);
            pos += take;
            continue;
        }
        bool enc = (ext[i].flags & EMBKFS_EXTENT_F_ENCRYPTED) != 0;
        if (ext[i].flags & EMBKFS_EXTENT_F_COMPRESSED) {
            /* No random-access decode: even a partial prefix needs the
             * WHOLE compressed blob read + decrypted + verified +
             * decompressed first, then we hand back only the slice
             * actually wanted (v2.2 Phase 3/4). */
            uint64_t comp_size = ext[i].compressed_size;
            uint8_t *raw = kmalloc(comp_size ? comp_size : 1);
            if (!raw) { return -EMBK_ENOMEM; }
            uint32_t csum = 0; uint64_t written = 0;
            for (uint64_t blk = 0; blk < ext[i].length; blk++) {
                uint64_t chunk = comp_size - written;
                if (chunk > vol->block_size) chunk = vol->block_size;
                rc = embk_block_read(vol->dev, (ext[i].disk_block + blk) * spb, spb, datablk);
                if (rc != EMBK_OK) { kfree(raw); return rc; }
                csum = embk_crc32c(datablk, enc ? vol->block_size : chunk, csum);
                if (enc) {
                    aes_xts_decrypt(embkfs_xts(vol), ext[i].disk_block + blk, datablk, datablk, vol->block_size);
                }
                memcpy(raw + written, datablk, chunk);
                written += chunk;
            }
            if (csum != ext[i].checksum) { kfree(raw); return -EMBK_EINVAL; }
            uint8_t *logical = kmalloc(ext[i].logical_size ? ext[i].logical_size : 1);
            bool dok = logical && embk_decompress(raw, (uint32_t)comp_size, logical, (uint32_t)ext[i].logical_size);
            kfree(raw);
            if (!dok) { kfree(logical); return -EMBK_EINVAL; }
            uint64_t take = need - pos;
            if (take > ext[i].logical_size) take = ext[i].logical_size;
            memcpy(out + pos, logical, take);
            kfree(logical);
            pos += take;
            continue;
        }
        uint32_t csum = 0;
        uint64_t written = 0;
        /* NOTE: reads every block of this extent even once `pos` has
         * already reached `need` for THIS extent's own share -- the
         * checksum was computed at write time over the WHOLE extent, so
         * verifying it requires the whole thing regardless of how much of
         * it the caller actually wants back.
         *
         * BATCHED: one device request per EMBKFS_IO_BATCH_BLOCKS blocks, not per
         * block. The per-request cost (command setup + a busy-wait for the DMA
         * IRQ) is ~2.7 ms under TCG and is paid PER REQUEST, not per byte -- so
         * issuing 4 KB at a time capped throughput near 1.5 MB/s no matter that
         * the transfer itself is DMA. This loop fills rcache for EVERY file
         * <= EMBKFS_RCACHE_MAX, i.e. every app load, so it is the hot one. */
        for (uint64_t blk = 0; blk < ext[i].length; ) {
            uint64_t nb = ext[i].length - blk;
            if (nb > EMBKFS_IO_BATCH_BLOCKS) nb = EMBKFS_IO_BATCH_BLOCKS;

            /* iobatch is kernel BSS: in the kernel range and under the 32-bit
             * DMA limit, so the block layer DMAs straight into it -- no bounce
             * copy. nb*block_size never exceeds block.c's 32 KB bounce cap. */
            rc = embk_block_read(vol->dev, (ext[i].disk_block + blk) * spb,
                                 (uint32_t)(nb * spb), iobatch);
            if (rc != EMBK_OK) { return rc; }

            for (uint64_t j = 0; j < nb; j++) {
                uint8_t *blkbuf = iobatch + j * vol->block_size;
                uint64_t chunk = ext[i].logical_size - written;
                if (chunk > vol->block_size) chunk = vol->block_size;
                /* Per-block CRC/decrypt order is unchanged -- the checksum is
                 * defined over the blocks in sequence, so batching the fetch
                 * must not reorder this. */
                csum = embk_crc32c(blkbuf, enc ? vol->block_size : chunk, csum);
                if (enc) {
                    aes_xts_decrypt(embkfs_xts(vol), ext[i].disk_block + blk + j,
                                    blkbuf, blkbuf, vol->block_size);
                }
                if (pos < need) {
                    uint64_t take = need - pos;
                    if (take > chunk) take = chunk;
                    memcpy(out + pos, blkbuf, take);
                    pos += take;
                }
                written += chunk;
            }
            blk += nb;
        }
        if (csum != ext[i].checksum) { return -EMBK_EINVAL; }
    }

    /* No kfree(ext) anywhere in here: the ecache owns the array. */
    return EMBK_OK;
}

/* Read [offset, offset+len) of an object WITHOUT decoding the prefix before it.
 *
 * This is the real fix for the O(n^2) that EMBKFS_RCACHE_MAX only papered over.
 * read_object_prefix() always starts at logical 0, so serving a read at offset N
 * cost O(N) time AND an O(N) kmalloc -- every call. Files <= RCACHE_MAX hid it
 * behind the whole-object cache; anything BIGGER fell back to that path and a
 * sequential read became O(n^2). CPython's 10.3 MB python314.zip (555-entry
 * central directory read at ~10 MB offsets) is what exposed it.
 *
 * The walk below is the same one read_object_prefix does, with one change that
 * is the whole point: an extent lying entirely BELOW `offset` is skipped with
 * ZERO I/O -- no block reads, no decrypt, no checksum, no decompress. Cost
 * becomes O(#extents) to locate + O(len) to deliver.
 *
 * Integrity is preserved for everything we actually return: an extent we TOUCH
 * is still read in full and checksum-verified, because (per the note in
 * read_object_prefix) the checksum was computed at write time over the WHOLE
 * extent. We simply don't verify extents whose bytes the caller isn't asking
 * for -- which is what any range read does, and what the cached path effectively
 * did too once it was warm.
 */
static int embkfs_read_object_range(struct embkfs_volume *vol, uint64_t oid,
                                    uint64_t offset, uint8_t *out, uint64_t len,
                                    uint64_t *out_read)
{
    static uint8_t probe_r[4096];
    static uint8_t datablk_r[4096];

    if (!vol || !out || !out_read) return -EMBK_EINVAL;
    *out_read = 0;
    if (len == 0) return EMBK_OK;

    /* Cached inode: this was a B-tree descent (block reads) on EVERY call. */
    struct embk_inode_item ino_c;
    int irc = embkfs_inode_cached(vol, oid, &ino_c);
    if (irc != EMBK_OK) return irc;

    uint32_t mode = ino_c.mode & EMBKFS_S_IFMT;
    if (mode != EMBKFS_S_IFREG && mode != EMBKFS_S_IFLNK) return -EMBK_EINVAL;
    if (ino_c.size == 0 || offset >= ino_c.size) return EMBK_OK;   /* EOF: 0 bytes */

    uint64_t avail_total = ino_c.size - offset;
    if (len > avail_total) len = avail_total;

    /* Extent map from the shared (oid, generation) cache -- see
     * embkfs_extents_cached(). Collecting one costs two full B-tree scans + a
     * kmalloc + an O(n) validate; doing that per read is what dominated once the
     * prefix decode was gone. The array is OWNED BY THE CACHE: every `return`
     * below must NOT kfree(ext), or the next hit serves freed memory. */
    int rc;
    struct embk_extref *ext;
    uint32_t en = 0;
    uint8_t *vfy;                            /* parallel to ext[], same keying */
    rc = embkfs_extents_cached(vol, oid, ino_c.size, "read_object_range",
                               &ext, &en, &vfy);
    if (rc != EMBK_OK) return rc;

    uint64_t spb = vol->block_size / vol->dev->block_size;
    /* Find the first extent overlapping `offset` by BINARY SEARCH rather than
     * walking from extent 0. The walk was O(offset/extent_size) CPU on EVERY
     * read: harmless while mkfs emitted one extent per file, but it silently
     * caps how small extents can get -- and small extents are exactly what makes
     * the first-touch verify cheap. At 64 KB extents a read near the end of a
     * 2 GB file would have walked 32,768 entries, per read, reintroducing the
     * O(n^2) this rebuild just removed (as CPU instead of I/O, which the profile
     * says is now the scarce one).
     *
     * Exact because embkfs_validate_extent_map() has already proven the map
     * tiles the file contiguously from 0, so ext[] is sorted by .offset and
     * ext[i].offset is precisely the logical start of extent i. */
    uint32_t first = 0;
    {
        uint32_t lo = 0, hi = en;
        while (lo < hi) {                    /* first i with offset < end(i) */
            uint32_t mid = lo + (hi - lo) / 2;
            if (ext[mid].offset + ext[mid].logical_size <= offset) lo = mid + 1;
            else                                                   hi = mid;
        }
        first = lo;
    }

    uint64_t elog = (first < en) ? ext[first].offset : 0;  /* logical start of ext[i] */
    uint64_t done = 0;    /* bytes delivered into out[] */

    for (uint32_t i = first; i < en && done < len; i++) {
        uint64_t esz = ext[i].logical_size;

        /* Kept as defence, not as the mechanism: `first` already lands on the
         * right extent, so this is false on entry and costs nothing. It only
         * fires if the map were ever not what validate proved it to be. */
        if (elog + esz <= offset) { elog += esz; continue; }

        uint64_t skip_in = (offset > elog) ? (offset - elog) : 0;   /* into this extent */
        uint64_t take = esz - skip_in;
        if (take > len - done) take = len - done;

        if (ext[i].flags & EMBKFS_EXTENT_F_HOLE) {
            memset(out + done, 0, take);
            done += take; elog += esz;
            continue;
        }

        bool enc = (ext[i].flags & EMBKFS_EXTENT_F_ENCRYPTED) != 0;

        if (ext[i].flags & EMBKFS_EXTENT_F_COMPRESSED) {
            /* Compressed extents have no random access: the whole blob must be
             * read+decrypted+verified+decompressed before any slice of it
             * exists. That is per-EXTENT work though, not per-FILE -- the
             * extents before `offset` were already skipped above. */
            uint64_t comp_size = ext[i].compressed_size;
            uint8_t *raw = kmalloc(comp_size ? comp_size : 1);
            if (!raw) { return -EMBK_ENOMEM; }
            uint32_t csum = 0; uint64_t written = 0;
            for (uint64_t blk = 0; blk < ext[i].length; blk++) {
                uint64_t chunk = comp_size - written;
                if (chunk > vol->block_size) chunk = vol->block_size;
                rc = embk_block_read(vol->dev, (ext[i].disk_block + blk) * spb, spb, datablk_r);
                if (rc != EMBK_OK) { kfree(raw); return rc; }
                csum = embk_crc32c(datablk_r, enc ? vol->block_size : chunk, csum);
                if (enc) {
                    aes_xts_decrypt(embkfs_xts(vol), ext[i].disk_block + blk, datablk_r, datablk_r, vol->block_size);
                }
                memcpy(raw + written, datablk_r, chunk);
                written += chunk;
            }
            if (csum != ext[i].checksum) { kfree(raw); return -EMBK_EINVAL; }
            uint8_t *logical = kmalloc(esz ? esz : 1);
            bool dok = logical && embk_decompress(raw, (uint32_t)comp_size, logical, (uint32_t)esz);
            kfree(raw);
            if (!dok) { kfree(logical); return -EMBK_EINVAL; }
            memcpy(out + done, logical + skip_in, take);   /* slice, not prefix */
            kfree(logical);
            done += take; elog += esz;
            continue;
        }

        /* Plain (possibly encrypted) extent.
         *
         * ALREADY VERIFIED in this generation -> touch only the blocks that
         * actually overlap the request. This is the case that matters: mkfs
         * gives a file ONE extent, so without this a 10 MB extent was fully
         * re-read and re-CRC'd to serve every 8 KB, i.e. O(filesize) PER READ. */
        if (vfy[i]) {
            uint64_t first = skip_in / vol->block_size;         /* block index */
            uint64_t want_end = skip_in + take;                 /* extent-relative */
            /* One block per device request was the whole steady-state cost: every
             * request pays ~2.7ms of command setup + IRQ wait regardless of size,
             * so an 8 KB read cost 2 requests (~5.4ms) to move 8 KB -- measured at
             * ~690 ms/MB, which is exactly what the profile showed. Batch like the
             * verify loop below. `last` is the exclusive block bound: ceil to a
             * block, clamped to the extent, so we never read past what was asked
             * for and `esz - bstart` can never underflow. */
            uint64_t last = (want_end + vol->block_size - 1) / vol->block_size;
            if (last > ext[i].length) last = ext[i].length;
            for (uint64_t blk = first; blk < last; ) {
                uint64_t nb = last - blk;
                if (nb > EMBKFS_IO_BATCH_BLOCKS) nb = EMBKFS_IO_BATCH_BLOCKS;
                rc = embk_block_read(vol->dev, (ext[i].disk_block + blk) * spb,
                                     (uint32_t)(nb * spb), iobatch);
                if (rc != EMBK_OK) { return rc; }
                for (uint64_t j = 0; j < nb; j++) {
                    uint64_t b = blk + j;
                    uint64_t bstart = b * vol->block_size;
                    uint64_t chunk = esz - bstart;
                    if (chunk > vol->block_size) chunk = vol->block_size;
                    uint8_t *blkbuf = iobatch + j * vol->block_size;
                    /* XTS is keyed by ABSOLUTE disk block, so batching must keep
                     * using each block's own index -- not the batch's first. */
                    if (enc) {
                        aes_xts_decrypt(embkfs_xts(vol), ext[i].disk_block + b,
                                        blkbuf, blkbuf, vol->block_size);
                    }
                    uint64_t lo = bstart > skip_in ? bstart : skip_in;
                    uint64_t hi_a = bstart + chunk, hi_b = want_end;
                    uint64_t hi = hi_a < hi_b ? hi_a : hi_b;
                    if (hi > lo) {
                        memcpy(out + done + (lo - skip_in), blkbuf + (lo - bstart), hi - lo);
                    }
                }
                blk += nb;
            }
            done += take; elog += esz;
            continue;
        }

        /* FIRST touch of this extent: read every block and verify the checksum
         * (it was computed at write time over the WHOLE extent, so there is no
         * cheaper way to establish trust), copying out only what was asked for.
         * Then remember it, so the reads above can be cheap. */
        /* BATCHED, same reasoning as read_object_prefix: one device request per
         * EMBKFS_IO_BATCH_BLOCKS blocks. This is the once-per-extent verify, so
         * for a single-extent 10 MB file it is 2,647 blocks -- at one request
         * each that alone was ~7 s. */
        uint32_t csum = 0;
        uint64_t written = 0;
        for (uint64_t blk = 0; blk < ext[i].length; ) {
            uint64_t nb = ext[i].length - blk;
            if (nb > EMBKFS_IO_BATCH_BLOCKS) nb = EMBKFS_IO_BATCH_BLOCKS;
            rc = embk_block_read(vol->dev, (ext[i].disk_block + blk) * spb,
                                 (uint32_t)(nb * spb), iobatch);
            if (rc != EMBK_OK) { return rc; }

            for (uint64_t j = 0; j < nb; j++) {
                uint8_t *blkbuf = iobatch + j * vol->block_size;
                uint64_t chunk = esz - written;
                if (chunk > vol->block_size) chunk = vol->block_size;
                csum = embk_crc32c(blkbuf, enc ? vol->block_size : chunk, csum);
                if (enc) {
                    aes_xts_decrypt(embkfs_xts(vol), ext[i].disk_block + blk + j,
                                    blkbuf, blkbuf, vol->block_size);
                }
                /* Intersect this block's logical span [written, written+chunk)
                 * with the wanted span [skip_in, skip_in+take), extent-relative. */
                uint64_t lo = written > skip_in ? written : skip_in;
                uint64_t hi_a = written + chunk, hi_b = skip_in + take;
                uint64_t hi = hi_a < hi_b ? hi_a : hi_b;
                if (hi > lo) {
                    memcpy(out + done + (lo - skip_in), blkbuf + (lo - written), hi - lo);
                }
                written += chunk;
            }
            blk += nb;
        }
        if (csum != ext[i].checksum) { return -EMBK_EINVAL; }
        vfy[i] = 1;                     /* verified for this (oid, generation) */
        done += take; elog += esz;
    }

    /* ext is the CACHE's array now -- must NOT be freed here. */
    *out_read = done;
    return EMBK_OK;
}

int embkfs_link_name(struct embkfs_volume *vol, uint64_t target_oid,
                     uint64_t new_dir_oid, const char *new_name)
{
    static uint8_t probe[4096];
    if (!vol || !new_name) return -EMBK_EINVAL;
    if (vol->read_only) return -EMBK_EROFS;
    size_t name_len = strlen(new_name);
    if (name_len == 0 || name_len > 255) return -EMBK_EINVAL;

    uint64_t existing;
    int rc = embkfs_lookup(vol, new_dir_oid, new_name, &existing);
    if (rc == EMBK_OK) return -EMBK_EEXIST;
    if (rc != -EMBK_ENOENT) return rc;

    struct embk_inode_item new_parent;
    rc = embkfs_read_dir_inode(vol, new_dir_oid, probe, sizeof probe, &new_parent);
    if (rc != EMBK_OK) return rc;

    const struct embk_item_header *ti =
        embkfs_find_item(vol, target_oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ti) return -EMBK_ENOENT;
    const struct embk_inode_item *ino = embk_item_data(probe, vol->block_size, ti, sizeof *ino);
    if (!ino) return -EMBK_EINVAL;
    uint32_t mode = ino->mode & EMBKFS_S_IFMT;
    if (mode == EMBKFS_S_IFDIR) return -EMBK_EPERM;
    uint8_t ttype = (mode == EMBKFS_S_IFLNK) ? EMBKFS_DT_LNK : EMBKFS_DT_REG;

    struct embk_inode_item upd = *ino;
    uint64_t new_gen = vol->generation + 1;
    uint64_t now_ns  = rtc_now_ns();
    upd.links     += 1;
    upd.ctime      = now_ns;   /* metadata-only change to the TARGET, ctime not mtime */
    upd.generation = new_gen;

    struct embk_put add_op;
    uint8_t *add_buf = NULL;
    rc = embkfs_dirent_add_op(vol, new_dir_oid, new_name, name_len,
                              target_oid, ttype, probe, sizeof probe,
                              &add_op, &add_buf);
    if (rc != EMBK_OK) return rc;

    /* The directory gaining the new name had its own content change too --
     * `new_parent` was already read above (to validate new_dir_oid is a
     * real directory) but, pre-v2.2, was never written back; same gap
     * fixed the same way at every other namespace-mutating call site. */
    new_parent.mtime      = now_ns;
    new_parent.ctime      = now_ns;
    new_parent.generation = new_gen;

    struct embk_put ops[3];
    uint32_t nops = 0;
    ops[nops++] = (struct embk_put){
        .key = { .object_id = target_oid, .type = EMBK_TYPE_INODE, .offset = 0 },
        .data = (const uint8_t *)&upd, .size = sizeof upd };
    ops[nops++] = add_op;
    ops[nops++] = (struct embk_put){
        .key = { .object_id = new_dir_oid, .type = EMBK_TYPE_INODE, .offset = 0 },
        .data = (const uint8_t *)&new_parent, .size = sizeof new_parent };

    rc = embkfs_txn_apply_ops(vol, ops, nops, new_gen);
    kfree(add_buf);
    return rc;
}

/* chmod: set the PERMISSION bits of an object's mode.
 *
 * REAL, not a stub: EMBKFS inodes have carried a mode since v1 -- there was
 * simply never a road from userspace to change it. git init is the caller that
 * exposed the gap (it chmods a lockfile to probe core.filemode, and dies if that
 * fails).
 *
 * The FILE-TYPE bits (EMBKFS_S_IFMT) are preserved from the existing inode and
 * the caller's are ignored: chmod changes permissions, never what a thing IS.
 * A caller passing S_IFDIR at a regular file must not be able to turn it into a
 * directory by accident.
 *
 * ctime, not mtime: the inode changed, the CONTENT did not -- the same POSIX
 * pairing embkfs_link_name observes just above. */
int embkfs_chmod_object(struct embkfs_volume *vol, uint64_t oid, uint32_t mode)
{
    static uint8_t probe[4096];
    if (!vol) return -EMBK_EINVAL;
    if (vol->read_only) return -EMBK_EROFS;

    const struct embk_item_header *ii =
        embkfs_find_item(vol, oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
    if (!ii) return -EMBK_ENOENT;
    const struct embk_inode_item *ino =
        embk_item_data(probe, vol->block_size, ii, sizeof *ino);
    if (!ino) return -EMBK_EINVAL;

    struct embk_inode_item upd = *ino;
    uint64_t new_gen = vol->generation + 1;
    upd.mode       = (ino->mode & EMBKFS_S_IFMT) | (mode & ~EMBKFS_S_IFMT);
    upd.ctime      = rtc_now_ns();
    upd.generation = new_gen;

    struct embk_put op = {
        .key = { .object_id = oid, .type = EMBK_TYPE_INODE, .offset = 0 },
        .data = (const uint8_t *)&upd, .size = sizeof upd };

    return embkfs_txn_apply_ops(vol, &op, 1, new_gen);
}

int embkfs_link_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                     const char *target_path, const char *new_path)
{
    if (!vol || !target_path || !new_path) return -EMBK_EINVAL;
    uint64_t target_oid;
    int rc = embkfs_path_walk(vol, start_dir_oid, target_path, &target_oid);
    if (rc != EMBK_OK) return rc;
    char leaf[256];
    uint64_t parent;
    rc = embkfs_path_parent(vol, start_dir_oid, new_path, &parent, leaf, sizeof leaf);
    if (rc != EMBK_OK) return rc;
    return embkfs_link_name(vol, target_oid, parent, leaf);
}

int embkfs_symlink_name(struct embkfs_volume *vol, uint64_t dir_oid,
                        const char *name, const char *target_path,
                        uint64_t *out_oid)
{
    if (!vol || !name || !target_path || !out_oid) return -EMBK_EINVAL;
    uint64_t oid;
    int rc = embkfs_make_object(vol, dir_oid, name, EMBKFS_S_IFLNK | EMBKFS_PERM_LNK, &oid);
    if (rc != EMBK_OK) return rc;

    size_t tlen = strlen(target_path);
    rc = embkfs_write_file(vol, oid, (const uint8_t *)target_path, tlen);
    if (rc != EMBK_OK) {
        embkfs_unlink(vol, dir_oid, name); /* best-effort rollback */
        return rc;
    }

    *out_oid = oid;
    return EMBK_OK;
}

int embkfs_symlink_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                        const char *link_path, const char *target_path,
                        uint64_t *out_oid)
{
    if (!vol || !link_path || !target_path || !out_oid) return -EMBK_EINVAL;
    char leaf[256];
    uint64_t parent;
    int rc = embkfs_path_parent(vol, start_dir_oid, link_path, &parent, leaf, sizeof leaf);
    if (rc != EMBK_OK) return rc;
    return embkfs_symlink_name(vol, parent, leaf, target_path, out_oid);
}

int embkfs_readlink_object(struct embkfs_volume *vol, uint64_t oid,
                           char *buf, size_t buf_sz, uint64_t *out_len)
{
    if (!vol || !buf || !out_len) return -EMBK_EINVAL;
    uint32_t mode = 0;
    int rc = embkfs_read_object_data(vol, oid, (uint8_t *)buf, buf_sz, out_len, &mode);
    if (rc != EMBK_OK) return rc;
    if (mode != EMBKFS_S_IFLNK) return -EMBK_EINVAL;
    if (*out_len >= buf_sz) return -EMBK_ERANGE;
    buf[*out_len] = '\0';
    return EMBK_OK;
}

int embkfs_readlink_path(struct embkfs_volume *vol, uint64_t start_dir_oid,
                         const char *path, char *buf, size_t buf_sz,
                         uint64_t *out_len)
{
    if (!vol || !path || !buf || !out_len) return -EMBK_EINVAL;
    uint64_t oid;
    int rc = embkfs_path_walk(vol, start_dir_oid, path, &oid);
    if (rc != EMBK_OK) return rc;
    return embkfs_readlink_object(vol, oid, buf, buf_sz, out_len);
}

int embkfs_truncate_object(struct embkfs_volume *vol, uint64_t oid,
                           uint64_t new_size)
{
    if (!vol) return -EMBK_EINVAL;

    uint64_t old_size = 0;
    uint32_t mode = 0;
    int rc = embkfs_read_object_prefix(vol, oid, NULL, 0, &old_size, &mode);
    if (rc != EMBK_OK) return rc;
    if (mode != EMBKFS_S_IFREG && mode != EMBKFS_S_IFLNK) return -EMBK_EINVAL;

    uint8_t *new_buf = NULL;
    if (new_size > 0) {
        new_buf = kmalloc(new_size);
        if (!new_buf) return -EMBK_ENOMEM;
        uint64_t copy = (old_size < new_size) ? old_size : new_size;
        if (copy) {
            rc = embkfs_read_object_prefix(vol, oid, new_buf, copy, NULL, NULL);
            if (rc != EMBK_OK) { kfree(new_buf); return rc; }
        }
        if (new_size > copy) memset(new_buf + copy, 0, new_size - copy);
    }

    rc = embkfs_write_file(vol, oid, new_buf, new_size);
    kfree(new_buf);
    return rc;
}

int embkfs_resize_object(struct embkfs_volume *vol, uint64_t oid,
                         uint64_t new_size)
{
    return embkfs_truncate_object(vol, oid, new_size);
}

static void embkfs_path_norm_smoke(struct embkfs_volume *vol)
{
    char norm[256];
    static const char *const samples[] = {
        "/./hello.txt",
        "/a//b/../c/",
        "./renamed.txt",
        "../../hello.txt"
    };

    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        int rc = embkfs_normalize_path(samples[i], norm, sizeof norm);
        if (rc == EMBK_OK)
            kprintf("EMBKFS: %s: normalize '%s' -> '%s'\n", vol->dev->name, samples[i], norm);
        else
            kprintf("EMBKFS: %s: normalize '%s' failed (%d)\n", vol->dev->name, samples[i], rc);
    }

    uint64_t a, b;
    int r1 = embkfs_lookup_path(vol, EMBKFS_ROOT_OBJECT_ID, "/./hello.txt", &a);
    int r2 = embkfs_lookup_path(vol, EMBKFS_ROOT_OBJECT_ID, "/hello.txt", &b);
    if (r1 == EMBK_OK && r2 == EMBK_OK && a == b)
        kprintf("EMBKFS: %s: path normalize lookup smoke OK (/./hello.txt == /hello.txt -> %lu)\n",
                vol->dev->name, a);
    else
        kprintf("EMBKFS: %s: path normalize lookup smoke rc1=%d rc2=%d oid1=%lu oid2=%lu\n",
                vol->dev->name, r1, r2, a, b);
}

static int embkfs_run_path_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    embkfs_path_norm_smoke(g_embkfs_live);
    return EMBK_OK;
}

static int embkfs_run_allocator_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;

    kprintf("EMBKFS: %s: allocator tiny stress: begin\n", vol->dev->name);

    int rc = EMBK_OK;
    bool ok = true;

    /* Phase 1: contiguous alloc/free, merge via adjacent frees, and double-free detection. */
    uint64_t astart = 0, agot = 0;
    rc = embkfs_alloc_run(vol, 3, &astart, &agot);
    if (rc == EMBK_OK) {
        if (agot >= 3) {
            int r1 = embkfs_free_run(vol, astart, 1);
            int r2 = embkfs_free_run(vol, astart + 1, 2);
            if (r1 != EMBK_OK || r2 != EMBK_OK) ok = false;

            uint64_t mstart = 0, mgot = 0;
            int rm = embkfs_alloc_run(vol, 3, &mstart, &mgot);
            if (rm != EMBK_OK || mgot != 3) {
                ok = false;
            } else {
                int rf = embkfs_free_run(vol, mstart, mgot);
                if (rf != EMBK_OK) ok = false;
                int rdf = embkfs_free_run(vol, mstart, 1);
                if (rdf != -EMBK_EEXIST) ok = false;
            }
        } else {
            int rf = embkfs_free_run(vol, astart, agot);
            if (rf != EMBK_OK) ok = false;
        }
    } else if (rc != -EMBK_ENOSPC) {
        ok = false;
    }

    /* Phase 2: exhaust allocator and require graceful ENOSPC, then restore. */
    uint32_t cap = vol->free_ext_n + 4;
    if (cap < 8) cap = 8;
    struct embk_run *taken = kmalloc((uint64_t)cap * sizeof *taken);
    if (!taken) return -EMBK_ENOMEM;

    uint32_t tn = 0;
    for (;;) {
        uint64_t st = 0, got = 0;
        rc = embkfs_alloc_run(vol, vol->total_blocks, &st, &got);
        if (rc == -EMBK_ENOSPC) break;
        if (rc != EMBK_OK || got == 0) { ok = false; break; }
        if (tn >= cap) { ok = false; break; }
        taken[tn].start = st;
        taken[tn].len = got;
        tn++;
    }
    if (rc != -EMBK_ENOSPC) ok = false;

    for (uint32_t i = 0; i < tn; i++) {
        int rf = embkfs_free_run(vol, taken[i].start, taken[i].len);
        if (rf != EMBK_OK) ok = false;
    }

    /* Post-restore sanity: at least one block should be allocatable then freeable. */
    uint64_t st = 0, got = 0;
    rc = embkfs_alloc_run(vol, 1, &st, &got);
    if (rc == EMBK_OK && got == 1) {
        if (embkfs_free_run(vol, st, 1) != EMBK_OK) ok = false;
    } else {
        ok = false;
    }

    kfree(taken);

    if (!ok) {
        kprintf("EMBKFS: %s: allocator tiny stress: FAIL\n", vol->dev->name);
        return -EMBK_EINVAL;
    }

    kprintf("EMBKFS: %s: allocator tiny stress: OK\n", vol->dev->name);
    return EMBK_OK;
}

static void embk_tree_case_name(char *out, const char *prefix, uint32_t idx)
{
    out[0] = prefix[0];
    out[1] = (char)('0' + ((idx / 100) % 10));
    out[2] = (char)('0' + ((idx / 10) % 10));
    out[3] = (char)('0' + (idx % 10));
    out[4] = '\0';
}

static void embk_tree_case_path(char *out, const char *prefix, uint32_t idx)
{
    out[0] = '/'; out[1] = 't'; out[2] = 's'; out[3] = 't'; out[4] = 'r'; out[5] = 'e'; out[6] = 'e'; out[7] = '/';
    out[8] = prefix[0];
    out[9] = (char)('0' + ((idx / 100) % 10));
    out[10] = (char)('0' + ((idx / 10) % 10));
    out[11] = (char)('0' + (idx % 10));
    out[12] = '\0';
}

static int embkfs_run_tree_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;
    if (vol->read_only) return -EMBK_EROFS;

    kprintf("EMBKFS: %s: tree churn stress: begin\n", vol->dev->name);

    int rc = EMBK_OK;
    uint64_t dir_oid = 0;
    uint64_t file_oid = 0;
    char path[32];
    char name[8];

    /* Best-effort cleanup from prior interrupted runs. */
    for (uint32_t i = 0; i < 140; i++) {
        embk_tree_case_path(path, "f", i);
        embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, path);
        embk_tree_case_path(path, "g", i);
        embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, path);
    }
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstree");

    rc = embkfs_mkdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstree", &dir_oid);
    if (rc != EMBK_OK && rc != -EMBK_EEXIST) return rc;
    if (rc == -EMBK_EEXIST) {
        rc = embkfs_lookup_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstree", &dir_oid);
        if (rc != EMBK_OK) return rc;
    }

    /* Insert wave #1: drive leaf/internal splits. */
    for (uint32_t i = 0; i < 120; i++) {
        embk_tree_case_name(name, "f", i);
        rc = embkfs_create_file(vol, dir_oid, name, &file_oid);
        if (rc != EMBK_OK && rc != -EMBK_EEXIST) goto fail;
    }

    /* Delete sparse subset: trigger underflow pressure. */
    for (uint32_t i = 1; i < 120; i += 2) {
        embk_tree_case_name(name, "f", i);
        rc = embkfs_unlink_name(vol, dir_oid, name);
        if (rc != EMBK_OK && rc != -EMBK_ENOENT) goto fail;
    }

    /* Insert wave #2: force sibling borrow/redistribution and re-splits. */
    for (uint32_t i = 0; i < 120; i++) {
        embk_tree_case_name(name, "g", i);
        rc = embkfs_create_file(vol, dir_oid, name, &file_oid);
        if (rc != EMBK_OK && rc != -EMBK_EEXIST) goto fail;
    }

    /* Full teardown: force merges and eventual root-level shrink. */
    for (uint32_t i = 0; i < 120; i++) {
        embk_tree_case_name(name, "f", i);
        rc = embkfs_unlink_name(vol, dir_oid, name);
        if (rc != EMBK_OK && rc != -EMBK_ENOENT) goto fail;
        embk_tree_case_name(name, "g", i);
        rc = embkfs_unlink_name(vol, dir_oid, name);
        if (rc != EMBK_OK && rc != -EMBK_ENOENT) goto fail;
    }

    rc = embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstree");
    if (rc != EMBK_OK) goto fail;

    kprintf("EMBKFS: %s: tree churn stress: OK\n", vol->dev->name);
    return EMBK_OK;

fail:
    kprintf("EMBKFS: %s: tree churn stress: FAIL (%d)\n", vol->dev->name, rc);
    return rc;
}

static int embkfs_run_object_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;
    if (vol->read_only) return -EMBK_EROFS;

    kprintf("EMBKFS: %s: object io stress: begin\n", vol->dev->name);

    int rc = EMBK_OK;
    bool ok = true;
    uint64_t dir_oid = 0;
    uint64_t file_oid = 0;
    uint64_t got = 0;
    uint64_t wr = 0;

    static const uint8_t abc[] = { 'a', 'b', 'c' };
    static const uint8_t tail[] = { 'T', 'A', 'I', 'L' };
    static const uint8_t one[] = { 'Z' };
    uint8_t buf[32];

    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstio/io");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstio");

    rc = embkfs_mkdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstio", &dir_oid);
    if (rc != EMBK_OK && rc != -EMBK_EEXIST) return rc;
    if (rc == -EMBK_EEXIST) {
        rc = embkfs_lookup_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstio", &dir_oid);
        if (rc != EMBK_OK) return rc;
    }

    rc = embkfs_create_file_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstio/io", &file_oid);
    if (rc == -EMBK_EEXIST) {
        rc = embkfs_lookup_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstio/io", &file_oid);
        if (rc != EMBK_OK) return rc;
    } else if (rc != EMBK_OK) {
        return rc;
    }

    rc = embkfs_write_object(vol, file_oid, abc, sizeof abc);
    if (rc != EMBK_OK) ok = false;

    memset(buf, 0, sizeof buf);
    got = 0;
    if (ok) {
        rc = embkfs_read_object_at(vol, file_oid, 1, buf, 2, &got);
        if (rc != EMBK_OK || got != 2 || buf[0] != 'b' || buf[1] != 'c') ok = false;
    }

    uint64_t pos = 0;
    if (ok) {
        rc = embkfs_seek_object(vol, file_oid, 0, EMBKFS_SEEK_END, -1, &pos);
        if (rc != EMBK_OK || pos != 2) ok = false;
    }
    if (ok) {
        rc = embkfs_seek_object(vol, file_oid, -1, EMBKFS_SEEK_CUR, 1, &pos);
        if (rc != -EMBK_EINVAL) ok = false;
    }

    if (ok) {
        rc = embkfs_write_object_at(vol, file_oid, vol->block_size + 2, one, sizeof one, &wr);
        if (rc != EMBK_OK || wr != 1) ok = false;
    }
    memset(buf, 0xA5, sizeof buf);
    got = 0;
    if (ok) {
        rc = embkfs_read_object_at(vol, file_oid, vol->block_size, buf, 3, &got);
        if (rc != EMBK_OK || got != 3 || buf[0] != 0 || buf[1] != 0 || buf[2] != 'Z') ok = false;
    }

    if (ok) {
        rc = embkfs_append_object(vol, file_oid, tail, sizeof tail, &wr);
        if (rc != EMBK_OK || wr != sizeof tail) ok = false;
    }

    if (ok) {
        rc = embkfs_truncate_object(vol, file_oid, 2);
        if (rc != EMBK_OK) ok = false;
    }
    memset(buf, 0, sizeof buf);
    got = 0;
    if (ok) {
        rc = embkfs_read_object(vol, file_oid, buf, sizeof buf, &got);
        if (rc != EMBK_OK || got != 2 || buf[0] != 'a' || buf[1] != 'b') ok = false;
    }

    if (ok) {
        rc = embkfs_resize_object(vol, file_oid, 6);
        if (rc != EMBK_OK) ok = false;
    }
    memset(buf, 0xA5, sizeof buf);
    got = 0;
    if (ok) {
        rc = embkfs_read_object(vol, file_oid, buf, sizeof buf, &got);
        if (rc != EMBK_OK || got != 6) ok = false;
        if (ok && (buf[0] != 'a' || buf[1] != 'b' || buf[2] != 0 || buf[3] != 0 || buf[4] != 0 || buf[5] != 0)) ok = false;
    }

    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstio/io");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstio");

    if (!ok) {
        kprintf("EMBKFS: %s: object io stress: FAIL (rc=%d)\n", vol->dev->name, rc);
        return (rc == EMBK_OK) ? -EMBK_EINVAL : rc;
    }

    kprintf("EMBKFS: %s: object io stress: OK\n", vol->dev->name);
    return EMBK_OK;
}

/* SHRINKING WRITES, over whatever extent shape the volume produces -- the test
 * that should have existed when the "extent-supersede" bug was first reported.
 *
 * That bug (docs/TODO.md, and the comment above embkfs_write_file) said a
 * shrinking write failed with -EINVAL when the object's prior data HAPPENED to
 * land as one multi-block extent rather than several single-block ones, and
 * called itself "100% reproducible" via `test embkfs timestamps` followed by
 * `test embkfs obj`. The word doing the damage is HAPPENED: the trigger was
 * never the tests, it was the free-block bitmap state they ran into, which is
 * incidental to everything -- image contents, what mkfs packed, what ran
 * before. A repro resting on allocator luck stops reproducing the moment the
 * image changes, and then nobody can tell whether the bug was fixed or merely
 * hidden. Which is exactly what happened: the sequence now passes, WITH the
 * triggering shape present (see the note on the fix below).
 *
 * The lesson shapes this test. A first draft demanded a specific extent count
 * per case and skipped when it did not get one -- which reproduced the very
 * flaw it was written to replace (7 of 9 cases went unexercised on the first
 * run, and the suite honestly reported "try again" rather than green). The
 * fix is to stop testing a SHAPE and start testing the INVARIANT:
 *
 *     a shrinking write succeeds, and the surviving bytes are unchanged,
 *     WHATEVER extent shape the prior contents happened to take.
 *
 * Every case therefore runs against whatever the allocator gives, records the
 * shape it actually got, and passes or fails on behaviour alone. Shape
 * coverage is reported separately: the suite says whether it ever managed to
 * exercise the reported trigger (a single extent spanning >1 block), so a run
 * that never hit it cannot be mistaken for one that did.
 *
 * Every case reads the data back. A shrink that "succeeds" and loses the
 * surviving bytes is the worse of the two bugs, and only a content check
 * catches it. */
struct embk_shrink_stats {
    int pass, fail;
    int saw_single_multiblock;   /* the reported trigger: n==1 over >1 block */
    int saw_multi_extent;        /* the shape it was contrasted against      */
    uint32_t max_extents;
};

static int embk_shrink_case(struct embkfs_volume *vol, uint64_t oid, const char *tag,
                            uint8_t *buf, uint64_t write_len, uint64_t new_len,
                            struct embk_shrink_stats *st)
{
    /* A recognisable, non-repeating pattern: byte i is (i*31+7). Never all-zero
     * across a block, so hole synthesis stays out of the way unless a case asks
     * for it, and a misplaced byte is obvious rather than plausible. */
    for (uint64_t i = 0; i < write_len; i++) buf[i] = (uint8_t)(i * 31 + 7);

    int rc = embkfs_write_object(vol, oid, buf, write_len);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: shrink %s: initial write of %lu failed: %s\n",
                tag, write_len, embk_strerror(rc));
        st->fail++;
        return rc;
    }

    /* Record the shape we actually got -- the case is defined by its behaviour,
     * but the shape is what tells a reader which variant was covered. */
    uint32_t n = 0;
    rc = embkfs_count_extents(vol, oid, &n);
    if (rc != EMBK_OK) { st->fail++; return rc; }
    uint64_t blocks = (write_len + vol->block_size - 1) / vol->block_size;
    if (n == 1 && blocks > 1) st->saw_single_multiblock++;
    if (n > 1)                st->saw_multi_extent++;
    if (n > st->max_extents)  st->max_extents = n;

    rc = embkfs_truncate_object(vol, oid, new_len);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: shrink %s: %lu -> %lu (%u extent%s in) FAILED: %s\n",
                tag, write_len, new_len, n, n == 1 ? "" : "s", embk_strerror(rc));
        st->fail++;
        return rc;
    }

    /* the survivors must be the original bytes, and the size exact */
    uint64_t got = 0;
    memset(buf, 0xA5, new_len ? new_len : 1);
    rc = embkfs_read_object(vol, oid, buf, new_len, &got);
    if (rc != EMBK_OK) { st->fail++; return rc; }
    if (got != new_len) {
        kprintf("EMBKFS: shrink %s: read back %lu bytes, want %lu\n", tag, got, new_len);
        st->fail++;
        return -EMBK_EINVAL;
    }
    for (uint64_t i = 0; i < new_len; i++) {
        if (buf[i] != (uint8_t)(i * 31 + 7)) {
            kprintf("EMBKFS: shrink %s: byte %lu is 0x%x, want 0x%x -- DATA LOST\n",
                    tag, i, buf[i], (uint8_t)(i * 31 + 7));
            st->fail++;
            return -EMBK_EINVAL;
        }
    }
    kprintf("EMBKFS: shrink %s: %lu -> %lu (%u extent%s in) OK\n",
            tag, write_len, new_len, n, n == 1 ? "" : "s");
    st->pass++;
    return EMBK_OK;
}

static int embkfs_run_shrink_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;
    if (vol->read_only) return -EMBK_EROFS;

    const uint64_t bs = vol->block_size;
    kprintf("EMBKFS: %s: shrinking writes: begin (block_size %lu)\n", vol->dev->name, bs);

    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstshrink/f");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstshrink");

    uint64_t dir_oid = 0, oid = 0;
    int rc = embkfs_mkdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstshrink", &dir_oid);
    if (rc != EMBK_OK) return rc;
    rc = embkfs_create_file_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstshrink/f", &oid);
    if (rc != EMBK_OK) return rc;

    uint8_t *buf = kmalloc(bs * 4 + 16);
    if (!buf) return -EMBK_ENOMEM;

    struct embk_shrink_stats st = { 0, 0, 0, 0, 0 };
    int last = EMBK_OK;
    #define SHRINK(tag, wl, nl) do {                                    \
        int r = embk_shrink_case(vol, oid, (tag), buf, (wl), (nl), &st); \
        if (r != EMBK_OK) last = r;                                     \
    } while (0)

    /* (a) THE REPORTED TRIGGER's exact geometry: bs+7 bytes (the original was
     * 4103 on a 4096 volume) shrunk to 2 bytes. */
    SHRINK("a/bs+7->2",        bs + 7,     2);

    /* (b) shrink landing exactly on a block boundary */
    SHRINK("b/2bs+5->1bs",     bs * 2 + 5, bs);

    /* (c) truncate-to-empty: every extent deleted, none created */
    SHRINK("c/2bs+5->0",       bs * 2 + 5, 0);

    /* (d) a 4-block object cut to three very different sizes */
    SHRINK("d/4bs->3",         bs * 4,     3);
    SHRINK("d/4bs->bs+1",      bs * 4,     bs + 1);
    SHRINK("d/4bs->3bs",       bs * 4,     bs * 3);

    /* (e) off-by-one either side of a block boundary -- the arithmetic most
     * likely to be wrong in extent-splitting code */
    SHRINK("e/2bs->bs-1",      bs * 2,     bs - 1);
    SHRINK("e/2bs->bs+1",      bs * 2,     bs + 1);
    SHRINK("e/2bs->bs",        bs * 2,     bs);

    /* (f) single block down to one byte, and to zero */
    SHRINK("f/bs->1",          bs,         1);
    SHRINK("f/bs->0",          bs,         0);

    /* (g) a HOLE-bearing file: the middle block is all zero, so write_file
     * synthesises a logical-only extent (length 0, owning no blocks). Shrinking
     * across that boundary runs the supersede path against an extent that has
     * no run to free -- a case the byte-pattern cases cannot reach. */
    {
        for (uint64_t i = 0; i < bs * 3; i++) buf[i] = (uint8_t)(i * 31 + 7);
        memset(buf + bs, 0, bs);                       /* middle block: a hole */
        int r = embkfs_write_object(vol, oid, buf, bs * 3);
        uint32_t n = 0;
        if (r == EMBK_OK) r = embkfs_count_extents(vol, oid, &n);
        if (r == EMBK_OK) r = embkfs_truncate_object(vol, oid, bs + 4);  /* cut inside the hole */
        if (r == EMBK_OK) {
            uint64_t got = 0;
            memset(buf, 0xA5, bs + 4);
            r = embkfs_read_object(vol, oid, buf, bs + 4, &got);
            if (r == EMBK_OK && got != bs + 4) r = -EMBK_EINVAL;
            for (uint64_t i = 0; r == EMBK_OK && i < bs; i++)
                if (buf[i] != (uint8_t)(i * 31 + 7)) r = -EMBK_EINVAL;   /* real data */
            for (uint64_t i = bs; r == EMBK_OK && i < bs + 4; i++)
                if (buf[i] != 0) r = -EMBK_EINVAL;                       /* hole reads zero */
        }
        kprintf("EMBKFS: shrink g/hole 3bs->bs+4: %u extents in, %s\n",
                n, r == EMBK_OK ? "OK" : embk_strerror(r));
        if (r == EMBK_OK) st.pass++; else { st.fail++; last = r; }
    }
    #undef SHRINK

    kfree(buf);
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstshrink/f");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstshrink");

    /* Shape coverage is REPORTED, never asserted -- the volume decides it, and
     * a run that never saw the reported trigger must not read like one that
     * did. Both shapes appearing across a run is the useful signal: it means
     * the invariant held on the shape that used to fail AND on its opposite. */
    kprintf("EMBKFS: %s: shrinking writes: %d passed, %d failed "
            "(shapes seen: single-multiblock %d, multi-extent %d, max %u extents)\n",
            vol->dev->name, st.pass, st.fail,
            st.saw_single_multiblock, st.saw_multi_extent, st.max_extents);
    if (!st.saw_single_multiblock)
        kprintf("EMBKFS: %s: shrinking writes: NOTE -- the reported trigger shape "
                "(one extent over >1 block) did not occur this run; free space was "
                "too fragmented to produce it\n", vol->dev->name);

    if (st.fail) return (last == EMBK_OK) ? -EMBK_EINVAL : last;
    return EMBK_OK;
}

/* v2.2: real RTC-sourced timestamps (Phase 0). Proves creation sets
 * btime==mtime==ctime==atime, a later content write moves mtime/ctime
 * forward while btime stays fixed, and a parent directory's own mtime
 * moves when a child is created inside it (the create-time gap fixed
 * alongside this feature -- see embkfs_make_object's comment). RTC
 * resolution is one second (rtc.h), so this test needs a real ~1.5s delay
 * between the two stats to guarantee a visible difference -- not flaky
 * because it isn't racing anything, just waiting out the clock's own
 * granularity. */
static int embkfs_run_timestamp_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;
    if (vol->read_only) return -EMBK_EROFS;

    kprintf("EMBKFS: %s: timestamps: begin\n", vol->dev->name);

    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstts/f");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstts");

    uint64_t dir_oid = 0, file_oid = 0;
    int rc = embkfs_mkdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstts", &dir_oid);
    if (rc != EMBK_OK) { kprintf("EMBKFS: %s: timestamps: mkdir failed: %s\n", vol->dev->name, embk_strerror(rc)); return rc; }

    struct embk_inode_item dir_before;
    rc = embkfs_stat_object(vol, dir_oid, &dir_before);
    if (rc != EMBK_OK) return rc;

    /* RTC resolution is one second (rtc.h) -- without a real delay here,
     * mkdir and the create right below can (and, run back-to-back with no
     * I/O in between, usually do) land in the exact same RTC second, which
     * would make "did the parent's mtime advance" indistinguishable from
     * "did nothing happen" by timestamp alone. Same reasoning as the delay
     * before the write step further down. */
    pit_delay_ms(500); pit_delay_ms(500); pit_delay_ms(500); pit_delay_ms(500);

    rc = embkfs_create_file_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstts/f", &file_oid);
    if (rc != EMBK_OK) { kprintf("EMBKFS: %s: timestamps: create failed: %s\n", vol->dev->name, embk_strerror(rc)); return rc; }

    struct embk_inode_item after_create;
    rc = embkfs_stat_object(vol, file_oid, &after_create);
    if (rc != EMBK_OK) return rc;

    bool ok = true;
    if (after_create.btime == 0 || after_create.mtime == 0 ||
        after_create.ctime == 0 || after_create.atime == 0) {
        kprintf("EMBKFS: %s: timestamps: FAIL zero timestamp at creation (a=%lu b=%lu m=%lu c=%lu)\n",
                vol->dev->name, (unsigned long)after_create.atime, (unsigned long)after_create.btime,
                (unsigned long)after_create.mtime, (unsigned long)after_create.ctime);
        ok = false;
    }
    if (ok && (after_create.btime != after_create.mtime || after_create.mtime != after_create.ctime ||
               after_create.ctime != after_create.atime)) {
        kprintf("EMBKFS: %s: timestamps: FAIL creation timestamps not all equal (a=%lu b=%lu m=%lu c=%lu)\n",
                vol->dev->name, (unsigned long)after_create.atime, (unsigned long)after_create.btime,
                (unsigned long)after_create.mtime, (unsigned long)after_create.ctime);
        ok = false;
    }

    /* The parent directory's own mtime must have moved too -- a new dirent
     * was added under it (the gap this v2.2 pass also closed). */
    struct embk_inode_item dir_after_create;
    if (ok) {
        rc = embkfs_stat_object(vol, dir_oid, &dir_after_create);
        if (rc != EMBK_OK) { ok = false; }
        else if (dir_after_create.mtime <= dir_before.mtime) {
            kprintf("EMBKFS: %s: timestamps: FAIL parent dir mtime did not advance on child create (%lu -> %lu)\n",
                    vol->dev->name, (unsigned long)dir_before.mtime, (unsigned long)dir_after_create.mtime);
            ok = false;
        }
    }

    /* Wait out the RTC's own 1-second resolution, then write real content. */
    if (ok) {
        pit_delay_ms(500); pit_delay_ms(500); pit_delay_ms(500); pit_delay_ms(500);
        static const uint8_t data[] = { 'h', 'i' };
        rc = embkfs_write_object(vol, file_oid, data, sizeof data);
        if (rc != EMBK_OK) { kprintf("EMBKFS: %s: timestamps: write failed: %s\n", vol->dev->name, embk_strerror(rc)); ok = false; }
    }

    struct embk_inode_item after_write;
    if (ok) {
        rc = embkfs_stat_object(vol, file_oid, &after_write);
        if (rc != EMBK_OK) { ok = false; }
        else {
            if (after_write.btime != after_create.btime) {
                kprintf("EMBKFS: %s: timestamps: FAIL btime changed on write (%lu -> %lu)\n",
                        vol->dev->name, (unsigned long)after_create.btime, (unsigned long)after_write.btime);
                ok = false;
            }
            if (ok && after_write.mtime <= after_create.mtime) {
                kprintf("EMBKFS: %s: timestamps: FAIL mtime did not advance on write (%lu -> %lu)\n",
                        vol->dev->name, (unsigned long)after_create.mtime, (unsigned long)after_write.mtime);
                ok = false;
            }
            if (ok && after_write.ctime <= after_create.ctime) {
                kprintf("EMBKFS: %s: timestamps: FAIL ctime did not advance on write (%lu -> %lu)\n",
                        vol->dev->name, (unsigned long)after_create.ctime, (unsigned long)after_write.ctime);
                ok = false;
            }
        }
    }

    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstts/f");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstts");

    if (!ok) {
        kprintf("EMBKFS: %s: timestamps: FAIL\n", vol->dev->name);
        return -EMBK_EINVAL;
    }
    kprintf("EMBKFS: %s: timestamps: OK (created@%lu -> written@%lu)\n",
            vol->dev->name, (unsigned long)after_create.mtime, (unsigned long)after_write.mtime);
    return EMBK_OK;
}

/* Phase 1 (v2.2): two independently-mounted EMBKFS volumes must stay fully
 * independent -- listing one must not disturb the other, and a write's CoW
 * commit (which bumps the writing volume's superblock generation) must not
 * leak into the other volume's generation counter. Needs a real 2nd EMBKFS
 * device at boot (`make run-multivol`); with only one volume mounted this
 * degrades to a SKIP rather than a FAIL, since most boots (plain `make run`)
 * only attach one EMBKFS-formatted drive. */
static int embkfs_run_multivol_selftests_impl(void)
{
    uint32_t n = embkfs_volume_count();
    if (n < 2) {
        kprintf("EMBKFS: multivol: SKIP (only %u volume(s) mounted -- boot with a "
                "2nd EMBKFS-formatted drive, e.g. `make run-multivol`, to exercise this)\n",
                (unsigned int)n);
        return EMBK_OK;
    }

    struct embkfs_volume *a = embkfs_volume_at(0);
    struct embkfs_volume *b = embkfs_volume_at(1);
    if (!a || !b || !a->mounted || !b->mounted) return -EMBK_ENODEV;
    if (a->dev == b->dev) {
        kprintf("EMBKFS: multivol: FAIL volumes 0 and 1 alias the same device\n");
        return -EMBK_EINVAL;
    }

    kprintf("EMBKFS: multivol: begin (%s + %s)\n", a->dev->name, b->dev->name);

    /* Each volume's tree must be independently walkable. */
    uint64_t count_a = 0, count_b = 0;
    int rc = embkfs_dir_entry_count(a, EMBKFS_ROOT_OBJECT_ID, &count_a);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: multivol: FAIL listing %s: %s\n", a->dev->name, embk_strerror(rc));
        return rc;
    }
    rc = embkfs_dir_entry_count(b, EMBKFS_ROOT_OBJECT_ID, &count_b);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: multivol: FAIL listing %s: %s\n", b->dev->name, embk_strerror(rc));
        return rc;
    }

    if (a->read_only || b->read_only) {
        kprintf("EMBKFS: multivol: OK (read-only, listed %s=%lu entries %s=%lu entries; "
                "skipped the write-isolation check)\n",
                a->dev->name, (unsigned long)count_a, b->dev->name, (unsigned long)count_b);
        return EMBK_OK;
    }

    uint64_t gen_b_before = b->generation;
    uint64_t gen_a_before = a->generation;

    embkfs_unlink_path(a, EMBKFS_ROOT_OBJECT_ID, "/tstmv");
    uint64_t file_oid = 0;
    rc = embkfs_create_file_path(a, EMBKFS_ROOT_OBJECT_ID, "/tstmv", &file_oid);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: multivol: FAIL create on %s: %s\n", a->dev->name, embk_strerror(rc));
        return rc;
    }
    static const uint8_t data[] = { 'm', 'v' };
    rc = embkfs_write_object(a, file_oid, data, sizeof data);
    bool ok = (rc == EMBK_OK);
    if (!ok) {
        kprintf("EMBKFS: multivol: FAIL write on %s: %s\n", a->dev->name, embk_strerror(rc));
    }

    if (ok && a->generation <= gen_a_before) {
        kprintf("EMBKFS: multivol: FAIL %s generation did not advance on its own write (%lu -> %lu)\n",
                a->dev->name, (unsigned long)gen_a_before, (unsigned long)a->generation);
        ok = false;
    }
    if (ok && b->generation != gen_b_before) {
        kprintf("EMBKFS: multivol: FAIL write to %s changed %s's generation (%lu -> %lu)\n",
                a->dev->name, b->dev->name, (unsigned long)gen_b_before, (unsigned long)b->generation);
        ok = false;
    }

    embkfs_unlink_path(a, EMBKFS_ROOT_OBJECT_ID, "/tstmv");

    if (!ok) {
        kprintf("EMBKFS: multivol: FAIL\n");
        return -EMBK_EINVAL;
    }
    kprintf("EMBKFS: multivol: OK (%s gen %lu -> %lu, %s gen unchanged at %lu)\n",
            a->dev->name, (unsigned long)gen_a_before, (unsigned long)a->generation,
            b->dev->name, (unsigned long)b->generation);
    return EMBK_OK;
}

/* Integration-level compression selftest (v2.2 Phase 3): writes real files
 * through the live filesystem (not just embk_compress_run_selftests()'s
 * standalone round-trip) and checks block usage actually shrank for
 * compressible data via the allocator's free-block count -- there's no
 * direct API to peek at an extent's flags from here, but "used fewer
 * blocks than the naive uncompressed byte count requires" can ONLY happen
 * if compression actually ran, so it's an equally rigorous proof. Also
 * covers the low-value case that must NOT shrink (proving the "only keep
 * it if it helps" policy actually runs, not just exists). */
static int embkfs_run_compress_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;
    if (vol->read_only) return -EMBK_EROFS;

    kprintf("EMBKFS: %s: compress: begin\n", vol->dev->name);
    bool ok = true;

    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstcz");

    /* Case 1: highly compressible -- a short repeating pattern filling 4
     * blocks (16384 bytes) worth of logical size. A real compressor should
     * get this down to well under 1 block, so total blocks used must be
     * strictly less than the naive ceil(len/block_size) == 4. */
    {
        uint32_t len = (uint32_t)(4 * vol->block_size);
        uint8_t *buf = kmalloc(len);
        if (!buf) { ok = false; }
        else {
            static const char pat[] = "EmbLinkOS-EMBKFS-v2.2-compress-test-pattern-";
            uint32_t patlen = (uint32_t)(sizeof(pat) - 1);
            for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)pat[i % patlen];

            uint64_t free_before = vol->free_blocks;
            uint64_t oid = 0;
            int rc = embkfs_create_file_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstcz", &oid);
            if (rc != EMBK_OK) { kprintf("EMBKFS: compress: FAIL create: %s\n", embk_strerror(rc)); ok = false; }
            if (ok) {
                rc = embkfs_write_object(vol, oid, buf, len);
                if (rc != EMBK_OK) { kprintf("EMBKFS: compress: FAIL write: %s\n", embk_strerror(rc)); ok = false; }
            }
            uint64_t naive_blocks = (len + vol->block_size - 1) / vol->block_size;
            uint64_t blocks_used = (ok && free_before >= vol->free_blocks) ? free_before - vol->free_blocks : naive_blocks;
            if (ok && blocks_used >= naive_blocks) {
                kprintf("EMBKFS: compress: FAIL repeating pattern used %lu blocks, expected fewer than %lu\n",
                        (unsigned long)blocks_used, (unsigned long)naive_blocks);
                ok = false;
            }
            if (ok) {
                uint8_t *round = kmalloc(len);
                uint64_t got = 0;
                rc = round ? embkfs_read_object(vol, oid, round, len, &got) : -EMBK_ENOMEM;
                if (rc != EMBK_OK || got != len || memcmp(round, buf, len) != 0) {
                    kprintf("EMBKFS: compress: FAIL read-back mismatch on repeating pattern\n");
                    ok = false;
                }
                kfree(round);
            }
            kfree(buf);
        }
        embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstcz");
    }

    /* Case 2: pseudo-random, incompressible, and above the size threshold
     * that would even be attempted -- block usage must match the naive
     * uncompressed count exactly (compression correctly declined). */
    if (ok) {
        uint32_t len = (uint32_t)(3 * vol->block_size + 111);
        uint8_t *buf = kmalloc(len);
        if (!buf) { ok = false; }
        else {
            uint32_t x = 0xA5A5F00Du;
            for (uint32_t i = 0; i < len; i++) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                buf[i] = (uint8_t)x;
            }

            uint64_t free_before = vol->free_blocks;
            uint64_t oid = 0;
            int rc = embkfs_create_file_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstcz", &oid);
            if (rc != EMBK_OK) { kprintf("EMBKFS: compress: FAIL create (case 2): %s\n", embk_strerror(rc)); ok = false; }
            if (ok) {
                rc = embkfs_write_object(vol, oid, buf, len);
                if (rc != EMBK_OK) { kprintf("EMBKFS: compress: FAIL write (case 2): %s\n", embk_strerror(rc)); ok = false; }
            }
            uint64_t naive_blocks = (len + vol->block_size - 1) / vol->block_size;
            uint64_t blocks_used = (ok && free_before >= vol->free_blocks) ? free_before - vol->free_blocks : naive_blocks;
            if (ok && blocks_used != naive_blocks) {
                kprintf("EMBKFS: compress: FAIL incompressible data used %lu blocks, expected exactly %lu "
                        "(compression should have declined)\n",
                        (unsigned long)blocks_used, (unsigned long)naive_blocks);
                ok = false;
            }
            if (ok) {
                uint8_t *round = kmalloc(len);
                uint64_t got = 0;
                rc = round ? embkfs_read_object(vol, oid, round, len, &got) : -EMBK_ENOMEM;
                if (rc != EMBK_OK || got != len || memcmp(round, buf, len) != 0) {
                    kprintf("EMBKFS: compress: FAIL read-back mismatch on random data\n");
                    ok = false;
                }
                kfree(round);
            }
            kfree(buf);
        }
        embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstcz");
    }

    kprintf("EMBKFS: compress: %s\n", ok ? "OK" : "FAIL");
    return ok ? EMBK_OK : -EMBK_EINVAL;
}

/* Self-healing dual-superblock repair (v2.2 Phase 5a): corrupts the LIVE
 * volume's on-disk BACKUP superblock (a single bit-flip in its checksum
 * field, invalidating it without touching the primary), then re-mounts
 * the same device into a scratch volume struct -- exercising the exact
 * mount-time repair path a real boot would run, not a synthetic stand-in
 * for it. Confirms the backup comes back byte-identical to the primary,
 * and that a SECOND remount (both copies now agreeing) still mounts
 * cleanly. Corrupting/repairing the on-disk backup doesn't touch the
 * already-mounted live volume's in-memory state (root/generation), so
 * this is safe to run against g_embkfs_live without disturbing it. */
static int embkfs_run_selfheal_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;
    if (vol->read_only) return -EMBK_EROFS;

    kprintf("EMBKFS: %s: selfheal: begin\n", vol->dev->name);
    bool ok = true;

    struct embk_block_device *dev = vol->dev;
    uint64_t spb = vol->block_size / dev->block_size;
    uint64_t sb_lba = EMBKFS_SB_OFFSET / dev->block_size;
    uint64_t backup_lba = (dev->block_count >= spb) ? dev->block_count - spb : 0;

    uint8_t *primary_snapshot = kmalloc(vol->block_size);
    uint8_t *corrupted = kmalloc(vol->block_size);
    if (!primary_snapshot || !corrupted) {
        kfree(primary_snapshot); kfree(corrupted);
        return -EMBK_ENOMEM;
    }

    int rc = embk_block_read(dev, sb_lba, spb, primary_snapshot);
    if (rc == EMBK_OK) rc = embk_block_read(dev, backup_lba, spb, corrupted);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: selfheal: FAIL reading superblock copies: %s\n", embk_strerror(rc));
        kfree(primary_snapshot); kfree(corrupted);
        return rc;
    }

    /* Flip a bit in the backup's checksum field (superblock body offset
     * 152) -- invalidates it without corrupting anything else, a
     * realistic single-bit-rot scenario. */
    corrupted[152] ^= 0xFF;
    rc = embk_block_write(dev, backup_lba, spb, corrupted);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: selfheal: FAIL corrupting backup for the test: %s\n", embk_strerror(rc));
        kfree(primary_snapshot); kfree(corrupted);
        return rc;
    }

    /* Re-mount: the real mount-time self-heal path should detect the
     * invalid backup and rewrite it from the (still-valid) primary.
     * NOTE: if the live volume were ENCRYPTED, this remount would block
     * on a second passphrase prompt (embkfs_mount()'s normal behavior) --
     * fine for this selftest, which only ever runs against the plain test
     * images, but worth knowing if this is ever pointed at an encrypted
     * volume from an automated (non-interactive) test run. */
    struct embkfs_volume *scratch = kmalloc(sizeof *scratch);
    if (!scratch) { kfree(primary_snapshot); kfree(corrupted); return -EMBK_ENOMEM; }
    rc = embkfs_mount(dev, scratch);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: selfheal: FAIL remount after corrupting backup: %s\n", embk_strerror(rc));
        ok = false;
    }
    kfree(scratch);

    if (ok) {
        uint8_t *backup_after = kmalloc(vol->block_size);
        rc = backup_after ? embk_block_read(dev, backup_lba, spb, backup_after) : -EMBK_ENOMEM;
        if (rc != EMBK_OK || memcmp(backup_after, primary_snapshot, vol->block_size) != 0) {
            kprintf("EMBKFS: selfheal: FAIL backup was not repaired to match the primary\n");
            ok = false;
        }
        kfree(backup_after);
    }

    /* Remount again: both copies should now agree, so this must still
     * mount cleanly (proves the repair didn't just paper over the
     * detection -- the volume is genuinely consistent afterward). */
    if (ok) {
        struct embkfs_volume *scratch2 = kmalloc(sizeof *scratch2);
        if (!scratch2) { ok = false; }
        else {
            rc = embkfs_mount(dev, scratch2);
            if (rc != EMBK_OK) {
                kprintf("EMBKFS: selfheal: FAIL remount after repair: %s\n", embk_strerror(rc));
                ok = false;
            }
            kfree(scratch2);
        }
    }

    kfree(primary_snapshot);
    kfree(corrupted);

    kprintf("EMBKFS: %s: selfheal: %s\n", dev->name, ok ? "OK" : "FAIL");
    return ok ? EMBK_OK : -EMBK_EINVAL;
}

/* Snapshots (v2.2 Phase 5b): write, snapshot, overwrite, rollback, confirm
 * the ORIGINAL bytes return -- and along the way, confirm the overwritten-
 * away block is HELD (not reclaimed) by both the txn_end hold-back AND a
 * full bitmap rebuild (the same computation a remount performs), proving
 * the allocator-side half of this phase, not just the registry bookkeeping. */
static int embkfs_run_snapshot_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;
    if (vol->read_only) return -EMBK_EROFS;

    kprintf("EMBKFS: %s: snapshot: begin\n", vol->dev->name);
    bool ok = true;

    embkfs_snapshot_delete(vol, "tstsnap1");   /* best-effort: prior failed run */
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstsnap");

    static const uint8_t content_a[] = "Original content, before the snapshot.\n";
    static const uint8_t content_b[] = "Overwritten content, after the snapshot -- must NOT survive rollback.\n";

    uint64_t oid = 0;
    int rc = embkfs_create_file_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstsnap", &oid);
    if (rc != EMBK_OK) { kprintf("EMBKFS: snapshot: FAIL create: %s\n", embk_strerror(rc)); return rc; }
    rc = embkfs_write_object(vol, oid, content_a, sizeof content_a - 1);
    if (rc != EMBK_OK) { kprintf("EMBKFS: snapshot: FAIL initial write: %s\n", embk_strerror(rc)); return rc; }

    rc = embkfs_snapshot_create(vol, "tstsnap1");
    if (rc != EMBK_OK) { kprintf("EMBKFS: snapshot: FAIL create snapshot: %s\n", embk_strerror(rc)); return rc; }

    uint64_t free_before_overwrite = vol->free_blocks;
    rc = embkfs_write_object(vol, oid, content_b, sizeof content_b - 1);
    if (rc != EMBK_OK) {
        kprintf("EMBKFS: snapshot: FAIL overwrite: %s\n", embk_strerror(rc));
        embkfs_snapshot_delete(vol, "tstsnap1");
        return rc;
    }

    /* The old (content_a) extent must be HELD, not reclaimed, while the
     * snapshot retains it -- the live free count must not have grown
     * despite the overwrite superseding that extent. */
    if (vol->free_blocks > free_before_overwrite) {
        kprintf("EMBKFS: snapshot: FAIL old extent was reclaimed despite a retained snapshot "
                "(free %lu -> %lu)\n", (unsigned long)free_before_overwrite, (unsigned long)vol->free_blocks);
        ok = false;
    }

    /* Force a full bitmap rebuild -- the same computation a remount would
     * do -- and confirm it independently agrees the block is still held
     * (mount-time protection, not just the txn_end hold-back). */
    if (ok) {
        rc = embkfs_bitmap_build(vol);
        if (rc != EMBK_OK) {
            kprintf("EMBKFS: snapshot: FAIL bitmap rebuild: %s\n", embk_strerror(rc));
            ok = false;
        } else {
            uint64_t used = 0;
            for (uint64_t b = 0; b < vol->total_blocks; b++)
                if (embk_bm_test(vol->block_bitmap, b)) used++;
            uint64_t rebuilt_free = vol->total_blocks - used;
            if (rebuilt_free > free_before_overwrite) {
                kprintf("EMBKFS: snapshot: FAIL bitmap rebuild freed a snapshot-held block "
                        "(free %lu -> %lu)\n", (unsigned long)free_before_overwrite, (unsigned long)rebuilt_free);
                ok = false;
            }
            embkfs_free_index_rebuild(vol);
        }
    }

    /* Rollback and confirm the ORIGINAL bytes come back. */
    if (ok) {
        rc = embkfs_snapshot_rollback(vol, "tstsnap1");
        if (rc != EMBK_OK) { kprintf("EMBKFS: snapshot: FAIL rollback: %s\n", embk_strerror(rc)); ok = false; }
    }
    if (ok) {
        uint64_t rolled_oid = 0;
        rc = embkfs_lookup_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstsnap", &rolled_oid);
        if (rc != EMBK_OK) {
            kprintf("EMBKFS: snapshot: FAIL lookup after rollback: %s\n", embk_strerror(rc));
            ok = false;
        } else {
            uint8_t buf[128];
            uint64_t got = 0;
            rc = embkfs_read_object(vol, rolled_oid, buf, sizeof buf, &got);
            if (rc != EMBK_OK || got != sizeof content_a - 1 || memcmp(buf, content_a, got) != 0) {
                kprintf("EMBKFS: snapshot: FAIL rollback did not restore the original content\n");
                ok = false;
            }
        }
    }

    /* Cleanup -- best-effort. On a LEGACY volume the snapshot's own registry
     * entry is expected to be gone post-rollback (it lived in the tree that
     * was just replaced); on a v2.3 volume it is expected to still be there.
     * Either way a failing delete here is not a test failure. */
    embkfs_snapshot_delete(vol, "tstsnap1");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstsnap");

    kprintf("EMBKFS: %s: snapshot: %s\n", vol->dev->name, ok ? "OK" : "FAIL");
    return ok ? EMBK_OK : -EMBK_EINVAL;
}

/* ROLLBACK IS NOT A ONE-WAY DOOR (v2.3, EMBKFS_INCOMPAT_SNAPREG).
 *
 * The defect this asserts against, in one sentence: with the registry stored
 * as items INSIDE the versioned tree, rolling back to an older snapshot
 * reverted the registry too, so every snapshot taken AFTER the target silently
 * stopped existing -- you could go back, but never forward again.
 *
 * The shape of the test is the shape of the bug:
 *
 *   write A, snapshot "s1"
 *   write B, snapshot "s2"      <- taken AFTER s1; the one that used to vanish
 *   write C                     <- live content, belongs to no snapshot
 *   rollback to s1              -> content is A
 *   s2 MUST STILL EXIST         <- the assertion; this is the whole fix
 *   rollback to s2              -> content is B     (forward again)
 *   rollback to s1              -> content is A     (and back; not one-shot)
 *
 * Rolling FORWARD to s2 after having rolled back to s1 is the part that could
 * not previously happen at all, and it checks more than the registry: s2's
 * frozen tree must still be intact on disk, which means bitmap_build kept its
 * blocks marked in-use across a rollback that made them unreachable from the
 * live root. A registry entry pointing at recycled blocks would pass a
 * "snapshot still listed" check and fail this one.
 *
 * On a volume WITHOUT the feature bit this test SKIPS rather than fails: the
 * old behaviour is correct for the old format, and reporting a legacy volume
 * as broken would be a lie in the other direction. */
static int embkfs_run_snapreg_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;
    if (vol->read_only) return -EMBK_EROFS;

    if (!embk_snapreg_enabled(vol)) {
        kprintf("EMBKFS: %s: snapreg: SKIP -- volume has no INCOMPAT_SNAPREG "
                "(legacy in-tree registry; rollback-drops-newer is correct there)\n",
                vol->dev->name);
        return -EMBK_ENOTSUP;
    }

    kprintf("EMBKFS: %s: snapreg: begin (registry block %lu, outside the tree)\n",
            vol->dev->name, EMBKFS_SNAPREG_BLOCK);
    bool ok = true;

    static const uint8_t A[] = "A: the oldest content, frozen by s1.\n";
    static const uint8_t B[] = "B: written after s1, frozen by s2.\n";
    static const uint8_t C[] = "C: live content, held by no snapshot at all.\n";

    embkfs_snapshot_delete(vol, "sr1");        /* best-effort: prior failed run */
    embkfs_snapshot_delete(vol, "sr2");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstsnapreg");

    uint64_t oid = 0;
    int rc = embkfs_create_file_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstsnapreg", &oid);
    if (rc != EMBK_OK) { kprintf("EMBKFS: snapreg: FAIL create: %s\n", embk_strerror(rc)); return rc; }

    #define SR_STEP(what, expr) do { if (ok) { rc = (expr); \
        if (rc != EMBK_OK) { kprintf("EMBKFS: snapreg: FAIL %s: %s\n", (what), embk_strerror(rc)); ok = false; } } } while (0)

    SR_STEP("write A",   embkfs_write_object(vol, oid, A, sizeof A - 1));
    SR_STEP("create s1", embkfs_snapshot_create(vol, "sr1"));
    SR_STEP("write B",   embkfs_write_object(vol, oid, B, sizeof B - 1));
    SR_STEP("create s2", embkfs_snapshot_create(vol, "sr2"));
    SR_STEP("write C",   embkfs_write_object(vol, oid, C, sizeof C - 1));

    /* Read the file through a fresh path lookup and compare against `want`.
     * Looking the path up each time (rather than reusing oid) matters: a
     * rollback replaces the whole tree, and the object id is only meaningful
     * relative to the tree that is live now. */
    #define SR_EXPECT(tag, want, wantlen) do { if (ok) {                        \
        uint64_t o2 = 0; uint8_t rb[128]; uint64_t g = 0;                       \
        rc = embkfs_lookup_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstsnapreg", &o2);\
        if (rc != EMBK_OK) { kprintf("EMBKFS: snapreg: FAIL lookup %s: %s\n",   \
                                     (tag), embk_strerror(rc)); ok = false; }   \
        else {                                                                  \
            rc = embkfs_read_object(vol, o2, rb, sizeof rb, &g);                \
            if (rc != EMBK_OK || g != (wantlen) || memcmp(rb, (want), g) != 0) {\
                kprintf("EMBKFS: snapreg: FAIL %s: content is not what the "    \
                        "snapshot froze (got %lu bytes)\n", (tag), g);          \
                ok = false;                                                     \
            } else kprintf("EMBKFS: snapreg: %s content OK\n", (tag));          \
        } } } while (0)

    SR_EXPECT("live=C", C, sizeof C - 1);

    /* (1) back to the OLDER snapshot */
    SR_STEP("rollback to s1", embkfs_snapshot_rollback(vol, "sr1"));
    SR_EXPECT("after rollback to s1: A", A, sizeof A - 1);

    /* (2) THE ASSERTION: s2 was taken after s1 and must have survived. */
    if (ok) {
        struct embk_snapshot_item si;
        rc = embkfs_snapshot_find_slot(vol, "sr2", NULL, &si);
        if (rc != EMBK_OK) {
            kprintf("EMBKFS: snapreg: FAIL -- 's2' did not survive the rollback to 's1' "
                    "(%s). This is the exact defect the out-of-tree registry fixes.\n",
                    embk_strerror(rc));
            ok = false;
        } else {
            kprintf("EMBKFS: snapreg: 's2' survived the rollback to 's1' (root block %lu)\n",
                    si.root.block);
        }
    }
    if (ok) {
        uint32_t n = 0;
        embkfs_snapshot_list_internal(vol, NULL, 0, &n);
        if (n != 2) {
            kprintf("EMBKFS: snapreg: FAIL -- registry lists %u snapshot(s) after rollback, want 2\n", n);
            ok = false;
        }
    }

    /* (3) FORWARD again -- impossible before, and it proves s2's frozen tree
     * is still physically intact, not merely still named. */
    SR_STEP("rollback FORWARD to s2", embkfs_snapshot_rollback(vol, "sr2"));
    SR_EXPECT("after rollback to s2: B", B, sizeof B - 1);

    /* (4) and back again, to show it is not a single extra step */
    SR_STEP("rollback BACK to s1", embkfs_snapshot_rollback(vol, "sr1"));
    SR_EXPECT("after second rollback to s1: A", A, sizeof A - 1);

    /* (5) the registry survives a full bitmap rebuild -- the same computation
     * a remount performs -- so this is durable, not an in-memory illusion. */
    if (ok) {
        rc = embkfs_bitmap_build(vol);
        uint32_t n = 0;
        embkfs_snapshot_list_internal(vol, NULL, 0, &n);
        if (rc != EMBK_OK || n != 2 || vol->snapshot_count != 2) {
            kprintf("EMBKFS: snapreg: FAIL -- after bitmap rebuild: rc=%s, %u listed, count %u (want 2/2)\n",
                    embk_strerror(rc), n, (unsigned)vol->snapshot_count);
            ok = false;
        } else {
            kprintf("EMBKFS: snapreg: registry intact across a full bitmap rebuild (2 snapshots)\n");
        }
        embkfs_free_index_rebuild(vol);
    }
    #undef SR_STEP
    #undef SR_EXPECT

    embkfs_snapshot_delete(vol, "sr2");
    embkfs_snapshot_delete(vol, "sr1");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstsnapreg");

    kprintf("EMBKFS: %s: snapreg: %s\n", vol->dev->name,
            ok ? "OK -- rollback preserves snapshots on both sides of the target" : "FAIL");
    return ok ? EMBK_OK : -EMBK_EINVAL;
}

/* Process-provenance (v2.2 Phase 5c): confirms a created/written object's
 * writer_pid matches current_process->pid at the time of the call.
 * Testing from two genuinely DIFFERENT processes would need real
 * multi-process spawning (out of scope for this unit-level check) --
 * what's actually being verified is the mechanism itself (reading
 * current_process->pid at every content-mutating call site), which
 * generalizes trivially to whichever process is really calling; a
 * dedicated cross-process check belongs in a scheduler/process-level
 * test, not here. */
static int embkfs_run_provenance_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;
    if (vol->read_only) return -EMBK_EROFS;

    kprintf("EMBKFS: %s: provenance: begin\n", vol->dev->name);
    bool ok = true;

    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstprov");

    uint32_t expected_pid = current_thread ? current_process->pid : 0;

    uint64_t oid = 0;
    int rc = embkfs_create_file_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstprov", &oid);
    if (rc != EMBK_OK) { kprintf("EMBKFS: provenance: FAIL create: %s\n", embk_strerror(rc)); return rc; }

    struct embk_inode_item ino;
    rc = embkfs_stat_object(vol, oid, &ino);
    if (rc != EMBK_OK || embk_inode_writer_pid(&ino) != expected_pid) {
        kprintf("EMBKFS: provenance: FAIL writer_pid after create: got %u, expected %u\n",
                (unsigned int)embk_inode_writer_pid(&ino), (unsigned int)expected_pid);
        ok = false;
    }

    if (ok) {
        static const uint8_t data[] = { 'p', 'r', 'o', 'v' };
        rc = embkfs_write_object(vol, oid, data, sizeof data);
        if (rc != EMBK_OK) { kprintf("EMBKFS: provenance: FAIL write: %s\n", embk_strerror(rc)); ok = false; }
    }
    if (ok) {
        rc = embkfs_stat_object(vol, oid, &ino);
        if (rc != EMBK_OK || embk_inode_writer_pid(&ino) != expected_pid) {
            kprintf("EMBKFS: provenance: FAIL writer_pid after write: got %u, expected %u\n",
                    (unsigned int)embk_inode_writer_pid(&ino), (unsigned int)expected_pid);
            ok = false;
        }
    }

    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstprov");

    kprintf("EMBKFS: %s: provenance: %s\n", vol->dev->name, ok ? "OK" : "FAIL");
    return ok ? EMBK_OK : -EMBK_EINVAL;
}

/* Verified-root boot check (v2.2 Phase 5d): crafts a superblock with
 * VERIFIED_ROOT set on the LIVE device (never touching the already-
 * mounted g_embkfs_live's in-memory state) and re-mounts into a scratch
 * volume struct -- the exact mount-time code path a real boot runs.
 * Case 1: a correctly-signed superblock must mount. Case 2: the SAME
 * superblock with one bit of the authenticated root flipped (the stored
 * HMAC now stale) must be refused with EMBK_EACCES specifically, not
 * merely "some error". Restores the original superblock bytes afterward
 * either way, so this never leaves the live volume's on-disk state
 * altered. */
static int embkfs_run_verifyboot_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;
    if (vol->read_only) return -EMBK_EROFS;

    kprintf("EMBKFS: %s: verifyboot: begin\n", vol->dev->name);
    bool ok = true;

    struct embk_block_device *dev = vol->dev;
    uint64_t spb = vol->block_size / dev->block_size;
    uint64_t sb_lba = EMBKFS_SB_OFFSET / dev->block_size;
    uint64_t backup_lba = (dev->block_count >= spb) ? dev->block_count - spb : 0;

    uint8_t *original = kmalloc(vol->block_size);
    uint8_t *work = kmalloc(vol->block_size);
    if (!original || !work) { kfree(original); kfree(work); return -EMBK_ENOMEM; }

    int rc = embk_block_read(dev, sb_lba, spb, original);
    if (rc != EMBK_OK) { kprintf("EMBKFS: verifyboot: FAIL reading superblock: %s\n", embk_strerror(rc)); kfree(original); kfree(work); return rc; }

    /* Case 1: correctly-signed superblock must mount. */
    memcpy(work, original, vol->block_size);
    struct embk_superblock *sb = (struct embk_superblock *)work;
    sb->feature_incompat |= EMBKFS_INCOMPAT_VERIFIED_ROOT;
    struct embk_verify_header *vh = (struct embk_verify_header *)(work + EMBKFS_VERIFY_HEADER_OFFSET);
    vh->magic = EMBKFS_VERIFY_HEADER_MAGIC;
    embkfs_verify_root_hmac(&sb->root, sb->generation, vh->hmac);
    sb->checksum = embk_crc32c(work, EMBKFS_SB_BODY_SIZE, 0);

    rc = embk_block_write(dev, sb_lba, spb, work);
    if (rc == EMBK_OK) rc = embk_block_write(dev, backup_lba, spb, work);
    if (rc != EMBK_OK) { kprintf("EMBKFS: verifyboot: FAIL writing test superblock: %s\n", embk_strerror(rc)); ok = false; }

    if (ok) {
        struct embkfs_volume *scratch = kmalloc(sizeof *scratch);
        if (!scratch) { ok = false; }
        else {
            rc = embkfs_mount(dev, scratch);
            if (rc != EMBK_OK) {
                kprintf("EMBKFS: verifyboot: FAIL mount with a CORRECT hmac was refused: %s\n", embk_strerror(rc));
                ok = false;
            }
            kfree(scratch);
        }
    }

    /* Case 2: same signed superblock, but one bit of the AUTHENTICATED
     * root flipped after signing (the stored HMAC is now stale) -- must
     * be refused, specifically with EMBK_EACCES. */
    if (ok) {
        sb->root.checksum ^= 0xFFFFFFFFu;
        sb->checksum = embk_crc32c(work, EMBKFS_SB_BODY_SIZE, 0);
        rc = embk_block_write(dev, sb_lba, spb, work);
        if (rc == EMBK_OK) rc = embk_block_write(dev, backup_lba, spb, work);
        if (rc != EMBK_OK) { kprintf("EMBKFS: verifyboot: FAIL writing tampered superblock: %s\n", embk_strerror(rc)); ok = false; }
    }
    if (ok) {
        struct embkfs_volume *scratch = kmalloc(sizeof *scratch);
        if (!scratch) { ok = false; }
        else {
            rc = embkfs_mount(dev, scratch);
            if (rc == EMBK_OK) {
                kprintf("EMBKFS: verifyboot: FAIL mount with a TAMPERED root was accepted\n");
                ok = false;
            } else if (rc != -EMBK_EACCES) {
                kprintf("EMBKFS: verifyboot: FAIL tampered root rejected with the wrong error: %s\n",
                        embk_strerror(rc));
                ok = false;
            }
            kfree(scratch);
        }
    }

    /* Always restore the ORIGINAL on-disk bytes, regardless of outcome --
     * the live volume must find its real superblock unchanged afterward. */
    embk_block_write(dev, sb_lba, spb, original);
    embk_block_write(dev, backup_lba, spb, original);
    kfree(original);
    kfree(work);

    kprintf("EMBKFS: %s: verifyboot: %s\n", dev->name, ok ? "OK" : "FAIL");
    return ok ? EMBK_OK : -EMBK_EINVAL;
}

static int embkfs_run_namespace_selftests_impl(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    struct embkfs_volume *vol = g_embkfs_live;
    if (vol->read_only) return -EMBK_EROFS;

    kprintf("EMBKFS: %s: namespace stress: begin\n", vol->dev->name);

    int rc = EMBK_OK;
    bool ok = true;
    uint64_t oid = 0;

    static const uint8_t payload[] = { 'N', 'S', 'D', 'A', 'T', 'A' };
    char linkbuf[64];
    uint64_t l = 0;
    uint8_t rbuf[16];
    uint64_t nread = 0;

    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/f");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/f2");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/l");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/l2");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/x");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/y");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/a/sub");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/a");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/b");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns");

    rc = embkfs_mkdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns", &oid);
    if (rc != EMBK_OK && rc != -EMBK_EEXIST) return rc;
    rc = embkfs_mkdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/a", &oid);
    if (rc != EMBK_OK && rc != -EMBK_EEXIST) return rc;
    rc = embkfs_mkdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/a/sub", &oid);
    if (rc != EMBK_OK && rc != -EMBK_EEXIST) return rc;
    rc = embkfs_mkdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/b", &oid);
    if (rc != EMBK_OK && rc != -EMBK_EEXIST) return rc;

    /* Must reject moving a directory into its own subtree. */
    rc = embkfs_rename_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/a", "/tstns/a/sub/a");
    if (rc != -EMBK_EINVAL) ok = false;

    /* Hard-linking a directory must be denied. */
    if (ok) {
        rc = embkfs_link_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/a", "/tstns/a_link");
        if (rc != -EMBK_EPERM) ok = false;
    }

    rc = embkfs_create_file_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/f", &oid);
    if (rc == -EMBK_EEXIST) {
        rc = embkfs_lookup_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/f", &oid);
        if (rc != EMBK_OK) ok = false;
    } else if (rc != EMBK_OK) {
        ok = false;
    }

    if (ok) {
        rc = embkfs_write_object(vol, oid, payload, sizeof payload);
        if (rc != EMBK_OK) ok = false;
    }

    /* Hardlink should preserve object and readability after original unlink. */
    if (ok) {
        rc = embkfs_link_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/f", "/tstns/f2");
        if (rc != EMBK_OK) ok = false;
    }
    if (ok) {
        rc = embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/f");
        if (rc != EMBK_OK) ok = false;
    }
    if (ok) {
        uint64_t f2 = 0;
        rc = embkfs_lookup_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/f2", &f2);
        if (rc != EMBK_OK) ok = false;
        if (ok) {
            memset(rbuf, 0, sizeof rbuf);
            nread = 0;
            rc = embkfs_read_object(vol, f2, rbuf, sizeof rbuf, &nread);
            if (rc != EMBK_OK || nread != sizeof payload || memcmp(rbuf, payload, sizeof payload) != 0) ok = false;
        }
    }

    /* Symlink create/readlink/rename roundtrip. */
    if (ok) {
        rc = embkfs_symlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/l", "/tstns/f2", &oid);
        if (rc != EMBK_OK) ok = false;
    }
    if (ok) {
        memset(linkbuf, 0, sizeof linkbuf);
        l = 0;
        rc = embkfs_readlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/l", linkbuf, sizeof linkbuf, &l);
        if (rc != EMBK_OK || l != 9 || memcmp(linkbuf, "/tstns/f2", 9) != 0) ok = false;
    }
    if (ok) {
        rc = embkfs_rename_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/l", "/tstns/l2");
        if (rc != EMBK_OK) ok = false;
    }
    if (ok) {
        memset(linkbuf, 0, sizeof linkbuf);
        l = 0;
        rc = embkfs_readlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/l2", linkbuf, sizeof linkbuf, &l);
        if (rc != EMBK_OK || l != 9 || memcmp(linkbuf, "/tstns/f2", 9) != 0) ok = false;
    }

    /* Rename into an existing destination must fail. */
    if (ok) {
        rc = embkfs_create_file_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/x", &oid);
        if (rc != EMBK_OK && rc != -EMBK_EEXIST) ok = false;
    }
    if (ok) {
        rc = embkfs_create_file_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/y", &oid);
        if (rc != EMBK_OK && rc != -EMBK_EEXIST) ok = false;
    }
    if (ok) {
        rc = embkfs_rename_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/x", "/tstns/y");
        if (rc != -EMBK_EEXIST) ok = false;
    }

    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/f");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/f2");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/l");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/l2");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/x");
    embkfs_unlink_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/y");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/a/sub");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/a");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns/b");
    embkfs_rmdir_path(vol, EMBKFS_ROOT_OBJECT_ID, "/tstns");

    if (!ok) {
        kprintf("EMBKFS: %s: namespace stress: FAIL (rc=%d)\n", vol->dev->name, rc);
        return (rc == EMBK_OK) ? -EMBK_EINVAL : rc;
    }

    kprintf("EMBKFS: %s: namespace stress: OK\n", vol->dev->name);
    return EMBK_OK;
}


/* (Re)build the in-memory free-space bitmap from the live tree: reserve the
 * fixed metadata that lives OUTSIDE the tree, then walk the tree and mark every
 * referenced block. Also recomputes vol->next_oid (the object-id high-water
 * mark). The bitmap buffer must already be allocated. This is the exact source
 * of truth — used at mount, and as the transactional allocator's overflow
 * backstop — so its result always matches what a fresh mount would compute. */
static int embkfs_bitmap_build(struct embkfs_volume *vol)
{
    uint64_t nbytes = (vol->total_blocks + 7) >> 3;
    memset(vol->block_bitmap, 0, nbytes);          /* everything free to start */

    /* Block 0 is reserved forever: an all-zero block_ptr (block=0) is the
     * "points at nothing" null sentinel, so no real node or data block may ever
     * live there — reserving it keeps embkfs_alloc_block from minting a pointer
     * that's indistinguishable from null. The rest of the pre-superblock region
     * (blocks 1..15) stays free, matching the formatter. */
    embk_bm_set(vol->block_bitmap, 0);

    /* Fixed metadata outside the tree: primary + backup superblock, and (v2.3)
     * the snapshot registry. The registry MUST be marked here: it is not
     * reachable from the tree walk below, so without this the allocator would
     * see a free block and hand out the one place the snapshot list lives. */
    embk_bm_set(vol->block_bitmap, EMBKFS_SB_OFFSET / vol->block_size);
    embk_bm_set(vol->block_bitmap, vol->total_blocks - 1);
    if (embk_snapreg_enabled(vol))
        embk_bm_set(vol->block_bitmap, EMBKFS_SNAPREG_BLOCK);

    /* Mark every block the live tree references — and, in the same walk, record
     * the highest object_id in use (accumulated into vol->next_oid). */
    vol->next_oid = 0;
    int rc = embkfs_mark_tree(vol, &vol->root);
    if (rc != EMBK_OK) return rc;

    /* Also protect every retained snapshot's frozen tree (v2.2 Phase 5b) --
     * a block can be unreachable from the LIVE tree yet still be exactly
     * what a snapshot needs.
     *
     * With SNAPREG (v2.3) this reads the out-of-tree registry, which is the
     * same list no matter what vol->root currently points at -- including
     * during embkfs_snapshot_rollback()'s temporary root swap. That is
     * precisely what makes snapshots survive a rollback: their blocks stay
     * marked in-use because the registry still names them. On a legacy volume
     * it reads vol->root's OWN in-tree registry instead, which is why rollback
     * there loses the snapshots the abandoned future was holding. */
    struct embk_snapshot_item snaps[EMBKFS_MAX_SNAPSHOTS];
    uint32_t n_snaps = 0;
    embkfs_snapshot_list_internal(vol, snaps, EMBKFS_MAX_SNAPSHOTS, &n_snaps);
    if (n_snaps > EMBKFS_MAX_SNAPSHOTS) n_snaps = EMBKFS_MAX_SNAPSHOTS;
    for (uint32_t i = 0; i < n_snaps; i++) {
        rc = embkfs_mark_tree(vol, &snaps[i].root);
        if (rc != EMBK_OK) return rc;
    }
    vol->snapshot_count = n_snaps;   /* authoritative recount, not incremental */

    /* Turn the high-water mark into the next free id (>= the first user id). */
    vol->next_oid = (vol->next_oid + 1 < EMBKFS_FIRST_USER_OBJID)
                      ? EMBKFS_FIRST_USER_OBJID : vol->next_oid + 1;
    return EMBK_OK;
}

/* Allocate the bitmap and build it at mount, then check it against the
 * superblock's free hint (the oracle that guards both the format and our walk). */
static int embkfs_alloc_init(struct embkfs_volume *vol)
{
    const char *dev = vol->dev->name;
    uint64_t nbytes = (vol->total_blocks + 7) >> 3;

    embkfs_free_index_clear(vol);
    if (vol->block_bitmap) {
        kfree(vol->block_bitmap);
        vol->block_bitmap = NULL;
    }

    vol->block_bitmap = kmalloc(nbytes);
    if (!vol->block_bitmap) { kprintf("EMBKFS: %s: bitmap alloc failed\n", dev); return -EMBK_ENOMEM; }

    int rc = embkfs_bitmap_build(vol);
    if (rc != EMBK_OK) { kfree(vol->block_bitmap); vol->block_bitmap = NULL; return rc; }
    rc = embkfs_free_index_rebuild(vol);
    if (rc != EMBK_OK) {
        embkfs_free_index_clear(vol);
        kfree(vol->block_bitmap);
        vol->block_bitmap = NULL;
        return rc;
    }
    kprintf("EMBKFS: %s: next object id %lu\n", dev, vol->next_oid);

    /* Oracle: our computed free count must match the superblock's. */
    uint64_t used = 0;
    for (uint64_t b = 0; b < vol->total_blocks; b++)
        if (embk_bm_test(vol->block_bitmap, b)) used++;
    uint64_t freeb = vol->total_blocks - used;
    kprintf("EMBKFS: %s: allocator built: %lu used, %lu free  (superblock says %lu)%s\n",
            dev, used, freeb, vol->free_blocks,
            freeb == vol->free_blocks ? "  -- OK" : "  -- MISMATCH");
    return EMBK_OK;
}



/* Finish mounting `vol` (already superblock-validated by embkfs_mount()):
 * verify the root node, build the allocator's free-space index, and sweep
 * for orphans left by an unclean shutdown. Shared by every slot in
 * embkfs_init()'s mount loop below -- previously this was inline, single-
 * volume-only code; factored out (v2.2 Phase 1) so mounting a second,
 * third, fourth volume is the exact same sequence, not a copy of it.
 * Returns true on success (vol is fully usable); false leaves vol mounted
 * at the superblock level but NOT usable (matches the original code's
 * "return early, but g_embkfs_live was already set" ordering bug this
 * refactor also avoids reproducing per-volume). */
static bool embkfs_finish_mount(struct embkfs_volume *vol)
{
    static uint8_t rootbuf[4096];   /* one scratch buffer, reused per volume --
                                      * mounting is boot-time-sequential, never
                                      * concurrent across volumes */
    if (embkfs_read_node(vol, &vol->root, rootbuf, sizeof rootbuf) != EMBK_OK)
        return false;
    const struct embk_node_header *h = (const struct embk_node_header *)rootbuf;
    kprintf("EMBKFS: %s: root node OK  level %u (%s)  nritems %u\n",
            vol->dev->name, (unsigned int)h->level,
            h->level == 0 ? "LEAF" : "internal", (unsigned int)h->nritems);

    embkfs_alloc_init(vol);

    int sweep_rc = embkfs_mount_orphan_sweep(vol);
    if (sweep_rc != EMBK_OK) {
        kprintf("EMBKFS: %s: mount orphan sweep failed: %s\n",
                vol->dev->name, embk_strerror(sweep_rc));
        return false;
    }
    return true;
}

void embkfs_init(void)
{
    kprintf("\n=== EMBKFS init ===\n");

    g_embkfs_live = NULL;
    g_embkfs_volume_count = 0;

    /* Build the CRC32C table before anything verifies a checksum. */
    embk_crc32c_init();

    /* Probe EVERY block device for an EMBKFS superblock; mount EVERY one
     * that validates, up to EMBKFS_MAX_VOLUMES (v2.2 Phase 1 -- previously
     * this loop `break`d on the first match, so a second EMBKFS-formatted
     * device sharing the machine with the first, e.g. a USB stick
     * alongside the boot disk, was silently never even looked at again).
     * Order matches embk_block_count()'s own registration order, which is
     * itself boot-sequential (ATA/AHCI before USB enumeration, per
     * main.c) -- so index 0 (the primary, aliased by g_embkfs_live) is
     * deterministically "whichever EMBKFS volume was found earliest in
     * boot", normally the internal disk. */
    for (uint32_t i = 0; i < embk_block_count() && g_embkfs_volume_count < EMBKFS_MAX_VOLUMES; i++) {
        struct embk_block_device *d = embk_block_get(i);
        struct embkfs_volume *slot = &g_embkfs_volumes[g_embkfs_volume_count];
        if (embkfs_mount(d, slot) != EMBK_OK)
            continue;
        if (!embkfs_finish_mount(slot))
            continue;
        g_embkfs_volume_count++;
    }

    if (g_embkfs_volume_count == 0) {
        kprintf("EMBKFS: no volume found on any block device\n");
        return;
    }

    g_embkfs_live = &g_embkfs_volumes[0];
    if (g_embkfs_volume_count > 1) {
        kprintf("EMBKFS: %u volumes mounted (primary: %s)\n",
                (unsigned int)g_embkfs_volume_count, g_embkfs_live->dev->name);
    }

    /* Boot path now does init/mount only. Demo and stress routines are
     * command-driven via the selftest command module. */
}

#if 0
    if (h->level == 0)                 /* flat-image diagnostic only */
        embkfs_leaf_dump(&vol, rootbuf);
    
    /* Lookups DESCEND from vol.root, so this works at any tree depth. */
    static const char *const names[] = { "hello.txt", "wgyehkb.txt", "illoeuw.txt" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        uint64_t oid;
        if (embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, names[i], &oid) == EMBK_OK) {
            kprintf("EMBKFS: %s: /%s -> object %lu\n", vol.dev->name, names[i], oid);
            embkfs_dump_file(&vol, oid, names[i]);
        } else {
            kprintf("EMBKFS: %s: /%s not found\n", vol.dev->name, names[i]);
        }
    }

    /* Data write into an existing file, read back with end-to-end verification. */
    static const uint8_t newmsg[] = "EMBKFS copy-on-write rewrote this block. Generation 2 now!\n\n";
    if (embkfs_write_file(&vol, 2, newmsg, sizeof newmsg - 1) == EMBK_OK) {
        uint64_t oid;
        if (embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "hello.txt", &oid) == EMBK_OK)
            embkfs_dump_file(&vol, oid, "hello.txt (after write)");
    }

    /* A length-CHANGING write: shorter contents update the inode size AND the
     * extent together, so size and data stay consistent. */
    static const uint8_t msg3[] = "Shorter: a length-changing write updates size + extent.\n";
    if (embkfs_write_file(&vol, 2, msg3, sizeof msg3 - 1) == EMBK_OK) {
        uint64_t oid;
        if (embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "hello.txt", &oid) == EMBK_OK)
            embkfs_dump_file(&vol, oid, "hello.txt (after length-changing write)");
    }

    /* --- Full namespace + data write path: create an empty file, write bytes
     *     into it (empty -> sized: allocates a data block and sets inode size +
     *     extent in one commit), then read it back. Re-running on the persisted
     *     image hits EEXIST and just reads the contents written last time. */
    {
        uint64_t newoid;
        int crc = embkfs_create(&vol, EMBKFS_ROOT_OBJECT_ID, "created.txt", &newoid);
        if (crc == EMBK_OK) {
            static const uint8_t body[] = "Hello from a file CREATED and WRITTEN by the kernel!\n";
            if (embkfs_write_file(&vol, newoid, body, sizeof body - 1) == EMBK_OK)
                embkfs_dump_file(&vol, newoid, "created.txt (created, then written)");
        } else if (crc == -EMBK_EEXIST) {
            uint64_t exoid;
            kprintf("EMBKFS: %s: /created.txt already present (persisted) — reading it back\n",
                    vol.dev->name);
            if (embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "created.txt", &exoid) == EMBK_OK)
                embkfs_dump_file(&vol, exoid, "created.txt (persisted from prior run)");
        }

        /* Rename smoke test: move created.txt -> renamed.txt and verify lookup. */
        if (embkfs_rename(&vol, EMBKFS_ROOT_OBJECT_ID, "created.txt",
                          EMBKFS_ROOT_OBJECT_ID, "renamed.txt") == EMBK_OK) {
            uint64_t roid;
            if (embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "renamed.txt", &roid) == EMBK_OK)
                embkfs_dump_file(&vol, roid, "renamed.txt (after rename)");
        }
    }

    embkfs_path_norm_smoke(&vol);

    /* --- Nested namespace: mkdir a subdirectory, then create+write a file INSIDE
     *     it, then resolve the file by walking root -> subdir -> file (manual path
     *     resolution, one component at a time — there is no path parser yet). On a
     *     persisted image the mkdir hits EEXIST and we just re-walk to read it. */
    {
        uint64_t docs_oid;
        int dr = embkfs_mkdir(&vol, EMBKFS_ROOT_OBJECT_ID, "docs", &docs_oid);
        if (dr == -EMBK_EEXIST)
            (void)embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "docs", &docs_oid);

        if (dr == EMBK_OK || dr == -EMBK_EEXIST) {
            uint64_t note_oid;
            int nr = embkfs_create(&vol, docs_oid, "notes.txt", &note_oid);
            if (nr == EMBK_OK) {
                static const uint8_t note[] = "A file living inside /docs — nested directories work.\n";
                if (embkfs_write_file(&vol, note_oid, note, sizeof note - 1) == EMBK_OK)
                    embkfs_dump_file(&vol, note_oid, "docs/notes.txt (nested create+write)");
            } else if (nr == -EMBK_EEXIST) {
                /* Re-resolve through the slash-path walker. */
                uint64_t n;
                if (embkfs_lookup_path(&vol, EMBKFS_ROOT_OBJECT_ID, "/docs/notes.txt", &n) == EMBK_OK)
                    embkfs_dump_file(&vol, n, "docs/notes.txt (persisted, re-walked)");
            }
        }
    }

    /* --- rmdir: refuse a non-empty directory, then create an empty one and
     *     remove it. /docs holds notes.txt, so rmdir must reject it; empty.d is
     *     made and removed within the boot (idempotent across re-runs). The
     *     parent's link count rises on mkdir and falls on rmdir — the verifier
     *     confirms root's nlink afterwards. */
    {
        int r1 = embkfs_rmdir(&vol, EMBKFS_ROOT_OBJECT_ID, "docs");
        kprintf("EMBKFS: %s: rmdir /docs (non-empty) -> %s\n", vol.dev->name,
                r1 == -EMBK_ENOTEMPTY ? "ENOTEMPTY (refused)" : embk_strerror(r1));

        uint64_t e;
        int mk = embkfs_mkdir(&vol, EMBKFS_ROOT_OBJECT_ID, "empty.d", &e);
        if (mk == EMBK_OK || mk == -EMBK_EEXIST) {
            int r2 = embkfs_rmdir(&vol, EMBKFS_ROOT_OBJECT_ID, "empty.d");
            uint64_t gone;
            int look = embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "empty.d", &gone);
            kprintf("EMBKFS: %s: rmdir /empty.d (empty) -> %s, lookup -> %s\n", vol.dev->name,
                    r2 == EMBK_OK ? "OK" : embk_strerror(r2),
                    look == -EMBK_ENOENT ? "gone" : "STILL THERE?!");
        }
    }

    /* --- Removal: create a scratch file, write to it (consuming a data block),
     *     then unlink it. Confirm the name is gone and the freed block is back in
     *     the count. Self-contained and idempotent: the file is created and
     *     removed within one boot, so re-runs start clean. */
    {
        uint64_t scratch;
        int cr = embkfs_create(&vol, EMBKFS_ROOT_OBJECT_ID, "scratch.tmp", &scratch);
        if (cr == EMBK_OK) {
            static const uint8_t s[] = "temporary file, about to be unlinked\n";
            embkfs_write_file(&vol, scratch, s, sizeof s - 1);
        }
        uint64_t free_before = vol.free_blocks;
        if (embkfs_unlink(&vol, EMBKFS_ROOT_OBJECT_ID, "scratch.tmp") == EMBK_OK) {
            uint64_t gone;
            int look = embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "scratch.tmp", &gone);
            kprintf("EMBKFS: %s: after unlink scratch.tmp lookup -> %s  (free %lu -> %lu)\n",
                    vol.dev->name,
                    look == -EMBK_ENOENT ? "ENOENT (gone)" : "STILL PRESENT?!",
                    free_before, vol.free_blocks);
        }
    }

    /* --- unlink-while-open: a held-open file survives unlink; its blocks are
     *     retained until last close, then reclaimed. */
    {
        uint64_t oid;
        if (embkfs_create(&vol, EMBKFS_ROOT_OBJECT_ID, "openlink.tmp", &oid) == EMBK_OK) {
            static const uint8_t s[] = "held open across unlink\n";
            embkfs_write_file(&vol, oid, s, sizeof s - 1);

            embkfs_object_get(&vol, oid);                 /* simulate fd open */
            uint64_t free_held = vol.free_blocks;

            embkfs_unlink(&vol, EMBKFS_ROOT_OBJECT_ID, "openlink.tmp");

            uint64_t gone;
            bool name_gone   = (embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "openlink.tmp", &gone) == -EMBK_ENOENT);
            bool blocks_held = (vol.free_blocks == free_held);   /* NOT freed yet */

            uint8_t rb[64]; uint64_t got = 0;
            int rd = embkfs_read_object_at(&vol, oid, 0, rb, sizeof rb, &got);

            embkfs_object_put(&vol, oid);                 /* simulate last close */
            bool blocks_freed = (vol.free_blocks > free_held);   /* now reclaimed */

            kprintf("EMBKFS: %s: unlink-while-open: name=%s held=%s read=%s freed-on-close=%s\n",
                    vol.dev->name,
                    name_gone    ? "gone"            : "PRESENT?!",
                    blocks_held  ? "blocks retained" : "FREED EARLY?!",
                    (rd == EMBK_OK && got > 0) ? "readable" : "LOST?!",
                    blocks_freed ? "yes"             : "NO?!");
        }
    }

    /* --- Collision-chain shrink: wgyehkb.txt and illoeuw.txt share one name
     *     hash, so they live as two records in a single DIR_ENTRY item. Unlink
     *     one and the OTHER must still resolve — the chain shrinks (item kept),
     *     it is not deleted. Guarded by existence, so it runs once then no-ops on
     *     the persisted image. */
    {
        uint64_t a;
        if (embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "wgyehkb.txt", &a) == EMBK_OK &&
            embkfs_unlink(&vol, EMBKFS_ROOT_OBJECT_ID, "wgyehkb.txt") == EMBK_OK) {
            uint64_t x;
            int s1 = embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "wgyehkb.txt", &x);
            int s2 = embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "illoeuw.txt", &x);
            kprintf("EMBKFS: %s: collision-chain unlink: wgyehkb -> %s, illoeuw -> %s\n",
                    vol.dev->name,
                    s1 == -EMBK_ENOENT ? "gone" : "STILL THERE?!",
                    s2 == EMBK_OK ? "still resolves" : "LOST?!");
        }
    }

    /* --- Leaf split/merge: create many files in one directory so its items
     *     overflow a single leaf and the tree GROWS leaves (and, with enough
     *     files, levels). Every name must still resolve afterwards — the proof
     *     that splits kept the tree correctly ordered. Then remove them all,
     *     exercising the merge/empty path. Idempotent: EEXIST/absent are fine. */
    {
        uint64_t bdir;
        int mr = embkfs_mkdir(&vol, EMBKFS_ROOT_OBJECT_ID, "bulk", &bdir);
        if (mr == -EMBK_EEXIST) (void)embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "bulk", &bdir);
        if (mr == EMBK_OK || mr == -EMBK_EEXIST) {
            const uint32_t N = 120;
            char nm[8] = { 'f', 0, 0, 0, 0 };
            uint64_t free0 = vol.free_blocks;
            uint32_t made = 0;
            for (uint32_t i = 0; i < N; i++) {
                nm[1] = '0' + (char)((i / 100) % 10);
                nm[2] = '0' + (char)((i / 10) % 10);
                nm[3] = '0' + (char)(i % 10);
                uint64_t o;
                int c = embkfs_create(&vol, bdir, nm, &o);
                if (c == EMBK_OK || c == -EMBK_EEXIST) made++; else break;
            }
            /* verify every name resolves through the (now split) tree */
            uint32_t resolved = 0;
            for (uint32_t i = 0; i < N; i++) {
                nm[1] = '0' + (char)((i / 100) % 10);
                nm[2] = '0' + (char)((i / 10) % 10);
                nm[3] = '0' + (char)(i % 10);
                uint64_t o;
                if (embkfs_lookup(&vol, bdir, nm, &o) == EMBK_OK) resolved++;
            }
            kprintf("EMBKFS: %s: bulk: %u/%u created, %u/%u resolve after splits, free %lu -> %lu\n",
                    vol.dev->name, made, N, resolved, N, free0, vol.free_blocks);

            /* tear them back down to exercise delete/merge, then re-check empty */
            uint32_t removed = 0;
            for (uint32_t i = 0; i < N; i++) {
                nm[1] = '0' + (char)((i / 100) % 10);
                nm[2] = '0' + (char)((i / 10) % 10);
                nm[3] = '0' + (char)(i % 10);
                if (embkfs_unlink(&vol, bdir, nm) == EMBK_OK) removed++;
            }
            int rd = embkfs_rmdir(&vol, EMBKFS_ROOT_OBJECT_ID, "bulk");
            kprintf("EMBKFS: %s: bulk: %u/%u removed, rmdir /bulk -> %s, free now %lu\n",
                    vol.dev->name, removed, N, rd == EMBK_OK ? "OK (merged empty)" : embk_strerror(rd),
                    vol.free_blocks);
        }

        /* A PERSISTENT, deeply split tree so the external verifier can validate
         * the routing invariant on a grown tree — enough files that the root
         * itself splits and the tree gains a LEVEL (>72 leaves). Idempotent:
         * re-creates hit EEXIST and the files simply remain; stops cleanly if the
         * small demo volume runs out of space. */
        uint64_t gdir;
        int gr = embkfs_mkdir(&vol, EMBKFS_ROOT_OBJECT_ID, "grown", &gdir);
        if (gr == -EMBK_EEXIST) (void)embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "grown", &gdir);
        if (gr == EMBK_OK || gr == -EMBK_EEXIST) {
            char nm[8] = { 'g', 0, 0, 0, 0, 0 };
            uint32_t made = 0;
            for (uint32_t i = 0; i < 800; i++) {
                nm[1] = '0' + (char)((i / 1000) % 10);
                nm[2] = '0' + (char)((i / 100) % 10);
                nm[3] = '0' + (char)((i / 10) % 10);
                nm[4] = '0' + (char)(i % 10);
                uint64_t o;
                int c = embkfs_create(&vol, gdir, nm, &o);
                if (c == EMBK_OK || c == -EMBK_EEXIST) made++; else break;   /* ENOSPC: stop */
            }
            /* sample-verify resolution across the deep tree */
            uint32_t resolved = 0, sampled = 0;
            for (uint32_t i = 0; i < made; i += 17) {
                nm[1] = '0' + (char)((i / 1000) % 10);
                nm[2] = '0' + (char)((i / 100) % 10);
                nm[3] = '0' + (char)((i / 10) % 10);
                nm[4] = '0' + (char)(i % 10);
                uint64_t o; sampled++;
                if (embkfs_lookup(&vol, gdir, nm, &o) == EMBK_OK) resolved++;
            }
            kprintf("EMBKFS: %s: grown: %u files present, %u/%u sampled resolve (deep split tree)\n",
                    vol.dev->name, made, resolved, sampled);
        }
    }

    /* --- Allocator durability stress: rewrite one file FAR more times than the
     *     volume has free blocks. Each rewrite supersedes a data block plus a
     *     tree path (~4 blocks); without in-session reclamation the bitmap would
     *     fill after ~60 writes and alloc would fail. With the transactional
     *     allocator, superseded blocks are reclaimed at each commit, so free
     *     stays flat and every write succeeds. Finally cross-check the bitmap's
     *     used-bit count against the free counter — they must agree. */
    {
        uint64_t oid;
        if (embkfs_lookup(&vol, EMBKFS_ROOT_OBJECT_ID, "hello.txt", &oid) == EMBK_OK) {
            const uint32_t rounds = 400;            /* >> 248 free blocks on the demo image */
            uint64_t free_start = vol.free_blocks;
            static uint8_t msg[48];
            uint32_t ok = 0;
            for (uint32_t i = 0; i < rounds; i++) {
                for (int k = 0; k < 40; k++) msg[k] = (uint8_t)('A' + ((i + k) % 26));
                if (embkfs_write_file(&vol, oid, msg, 40) != EMBK_OK) break;   /* same length each round */
                ok++;
            }
            /* independent oracle: count used bits, compare to total - free */
            uint64_t used = 0;
            for (uint64_t b = 0; b < vol.total_blocks; b++)
                if (embk_bm_test(vol.block_bitmap, b)) used++;
            kprintf("EMBKFS: %s: stress: %u/%u rewrites OK, free %lu -> %lu, "
                    "bitmap used %lu vs total-free %lu  %s\n",
                    vol.dev->name, ok, rounds, free_start, vol.free_blocks,
                    used, vol.total_blocks - vol.free_blocks,
                    (ok == rounds && used == vol.total_blocks - vol.free_blocks)
                        ? "-- OK (reclaimed)" : "-- FAIL");
        }
    }

}
#endif

int embkfs_run_boot_diagnostics(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted)
        return -EMBK_ENODEV;

    struct embkfs_volume *vol = g_embkfs_live;
    static uint8_t rootbuf[4096];

    int rc = embkfs_read_node(vol, &vol->root, rootbuf, sizeof rootbuf);
    if (rc != EMBK_OK)
        return rc;

    const struct embk_node_header *h = (const struct embk_node_header *)rootbuf;
    kprintf("EMBKFS: %s: diag root level %u (%s) nritems %u\n",
            vol->dev->name, (unsigned int)h->level,
            h->level == 0 ? "LEAF" : "internal", (unsigned int)h->nritems);

    if (h->level == 0)
        embkfs_leaf_dump(vol, rootbuf);

    static const char *const names[] = { "hello.txt", "wgyehkb.txt", "illoeuw.txt" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        uint64_t oid;
        if (embkfs_lookup(vol, EMBKFS_ROOT_OBJECT_ID, names[i], &oid) == EMBK_OK) {
            kprintf("EMBKFS: %s: /%s -> object %lu\n", vol->dev->name, names[i], oid);
            embkfs_dump_file(vol, oid, names[i]);
        } else {
            kprintf("EMBKFS: %s: /%s not found\n", vol->dev->name, names[i]);
        }
    }

    embkfs_path_norm_smoke(vol);
    return EMBK_OK;
}


/* ---- Locked public entry points -------------------------------------------
 * Each of these is reachable DIRECTLY from the kernel console, bypassing the
 * VFS bridge that used to be the only place the big lock was taken. Taking it
 * here is what closes that gap. The lock is recursive by owner thread, so an
 * _impl that calls another public entry point underneath costs a depth++ and
 * nothing else. */
int embkfs_run_path_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_path_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_allocator_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_allocator_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_tree_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_tree_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_object_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_object_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_shrink_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_shrink_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_timestamp_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_timestamp_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_multivol_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_multivol_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_compress_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_compress_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_selfheal_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_selfheal_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_snapshot_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_snapshot_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_snapreg_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_snapreg_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_provenance_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_provenance_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_verifyboot_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_verifyboot_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_run_namespace_selftests(void)
{
    embkfs_lock();
    int rc = embkfs_run_namespace_selftests_impl();
    embkfs_unlock();
    return rc;
}

int embkfs_snapshot_create(struct embkfs_volume *vol, const char *name)
{
    embkfs_lock();
    int rc = embkfs_snapshot_create_impl(vol, name);
    embkfs_unlock();
    return rc;
}

int embkfs_snapshot_delete(struct embkfs_volume *vol, const char *name)
{
    embkfs_lock();
    int rc = embkfs_snapshot_delete_impl(vol, name);
    embkfs_unlock();
    return rc;
}

int embkfs_snapshot_rollback(struct embkfs_volume *vol, const char *name)
{
    embkfs_lock();
    int rc = embkfs_snapshot_rollback_impl(vol, name);
    embkfs_unlock();
    return rc;
}

