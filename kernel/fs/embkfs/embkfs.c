#include "embkfs.h"
#include "crc32c.h"
#include "../../block/block.h"
#include "../../include/kmalloc.h"   /* kmalloc / kfree — the Phase 8 heap */
#include "../../include/kprintf.h"
#include "../../include/errno.h"
#include "../../include/kstring.h"   /* memcmp, strlen */

static struct embkfs_volume *g_embkfs_live = NULL;

struct embkfs_volume *embkfs_live_volume(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted)
        return NULL;
    return g_embkfs_live;
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

    /* per-block node lists */
    const uint64_t *blk = committed ? t->freed   : t->alloc;
    uint32_t        bn  = committed ? t->freed_n : t->alloc_n;
    for (uint32_t i = 0; i < bn; i++) {
        int rc = embkfs_free_run(vol, blk[i], 1);
        if (rc != EMBK_OK) free_err = true;
    }

    /* data-run lists */
    const struct embk_run *run = committed ? t->frun   : t->arun;
    uint32_t               rn  = committed ? t->frun_n : t->arun_n;
    for (uint32_t i = 0; i < rn; i++) {
        int rc = embkfs_free_run(vol, run[i].start, run[i].len);
        if (rc != EMBK_OK) free_err = true;
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
    uint64_t offset;        /* key offset = file byte position of this run */
    uint64_t disk_block;    /* first disk block of the run                */
    uint64_t length;        /* run length in blocks                       */
    uint64_t logical_size;  /* valid bytes in this extent                 */
    uint32_t checksum;      /* CRC32C over this extent's logical bytes     */
    uint32_t flags;         /* EMBKFS_EXTENT_F_*                          */
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
            out[*n].offset       = it->key.offset;
            out[*n].disk_block   = ext->disk_block;
            out[*n].length       = ext->length;
            out[*n].logical_size = ext->logical_size;
            out[*n].checksum     = (uint32_t)ext->checksum;
            out[*n].flags        = ext->flags;
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
    sb->checksum = embk_crc32c(sbbuf, EMBKFS_SB_BODY_SIZE, 0);   /* body [0..152) */

    rc = embk_block_write(vol->dev, sb_block * spb, spb, sbbuf);
    if (rc != EMBK_OK) return rc;
    return embk_block_write(vol->dev, (vol->total_blocks - 1) * spb, spb, sbbuf);
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

    /* Validated. Record the mount state. root is a 32-byte struct copied by
     * value — our entry point into the metadata tree next step. */
    vol->dev          = dev;
    vol->block_size   = sb->block_size;
    vol->total_blocks = sb->total_blocks;
    vol->free_blocks  = sb->free_blocks;
    vol->generation   = sb->generation;
    vol->root         = sb->root;
    vol->read_only    = read_only;
    vol->mounted      = true;
    vol->txn          = NULL;          /* no transaction in flight */
    vol->block_bitmap = NULL;
    vol->free_ext     = NULL;
    vol->free_ext_n   = 0;
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
int embkfs_read_node(struct embkfs_volume *vol,
                     const struct embk_block_ptr *ptr,
                     uint8_t *buf, size_t buf_size)
{
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
 * that pass a small length still compile. */
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

        /* write this run block-by-block, threading the CRC over logical bytes */
        uint32_t csum = 0; uint64_t written = 0;
        for (uint64_t blk = 0; blk < got && rc == EMBK_OK; blk++) {
            uint64_t chunk = ext_bytes - written;
            if (chunk > vol->block_size) chunk = vol->block_size;
            memset(datablk, 0, vol->block_size);              /* zero-pad a partial last block */
            if (chunk) memcpy(datablk, newdata + foff + written, chunk);
            rc = embk_block_write(vol->dev, (start + blk) * spb, spb, datablk);
            if (rc == EMBK_OK) { csum = embk_crc32c(datablk, chunk, csum); written += chunk; }
        }
        if (rc != EMBK_OK) break;

        struct embk_extent_item *e = &exts[new_n];
        memset(e, 0, sizeof *e);
        e->disk_block   = start;
        e->length       = got;
        e->logical_size = ext_bytes;
        e->checksum     = csum;                               /* over logical bytes (spec 9.3) */
        e->generation   = new_gen;

        new_off[new_n] = foff;
        if (nputs >= puts_cap) { rc = -EMBK_EINVAL; break; }
        puts[nputs++] = (struct embk_put){
            .key  = { .object_id = oid, .type = EMBK_TYPE_EXTENT, .offset = foff },
            .data = (const uint8_t *)e, .size = sizeof *e };
        new_n++;

        total_blocks += got;
        foff         += ext_bytes;
        remaining    -= ext_bytes;
    }

    if (rc == EMBK_OK) {
        /* 4. updated inode: new size, block count, generation. */
        ino.size       = len;
        ino.blocks     = total_blocks;
        ino.generation = new_gen;
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

        /* 6. one atomic commit: inode + all extent puts/deletes route together. */
        if (rc == EMBK_OK) {
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

    /* (b) the new object's inode: empty, links 2 for a directory else 1. */
    struct embk_inode_item ino;
    memset(&ino, 0, sizeof ino);
    ino.size       = 0;
    ino.blocks     = 0;
    ino.links      = is_dir ? 2 : 1;
    ino.mode       = mode;
    ino.generation = new_gen;

    /* (c) for a directory, read the parent inode so we can bump its link count.
     *     Copy it out before probe is reused by the dir-entry descent below. */
    struct embk_inode_item parent_ino;
    bool have_parent = false;
    if (is_dir) {
        const struct embk_item_header *pi =
            embkfs_find_item(vol, dir_oid, EMBK_TYPE_INODE, 0, probe, sizeof probe);
        if (!pi) { kprintf("EMBKFS: %s: parent object %lu has no inode\n", dev, dir_oid); return -EMBK_ENOENT; }
        const struct embk_inode_item *p = embk_item_data(probe, vol->block_size, pi, sizeof *p);
        if (!p) return -EMBK_EINVAL;
        parent_ino = *p;
        parent_ino.links      += 1;          /* the new child's ".." */
        parent_ino.generation  = new_gen;
        have_parent = true;
    }

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
    if (have_parent)
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

    uint64_t existing;
    rc = embkfs_lookup(vol, new_dir_oid, new_name, &existing);
    if (rc == EMBK_OK) return -EMBK_EEXIST;
    if (rc != -EMBK_ENOENT) return rc;

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

    struct embk_put remove_op, add_op;
    uint8_t *remove_buf = NULL;
    uint8_t *add_buf = NULL;

    rc = embkfs_dirent_remove_op(vol, old_dir_oid, old_name, old_len,
                                 probe, sizeof probe, &remove_op, &remove_buf);
    if (rc != EMBK_OK) return rc;

    rc = embkfs_dirent_add_op(vol, new_dir_oid, new_name, new_len,
                              target, ttype, probe, sizeof probe,
                              &add_op, &add_buf);
    if (rc != EMBK_OK) { kfree(remove_buf); return rc; }

    uint64_t new_gen = vol->generation + 1;
    struct embk_put ops[4];
    uint32_t nops = 0;
    ops[nops++] = remove_op;
    ops[nops++] = add_op;

    struct embk_inode_item old_parent_new;
    struct embk_inode_item new_parent_new;
    if (is_dir && old_dir_oid != new_dir_oid) {
        old_parent_new = old_parent;
        new_parent_new = new_parent;
        if (old_parent_new.links == 0) { kfree(remove_buf); kfree(add_buf); return -EMBK_EINVAL; }
        old_parent_new.links -= 1;
        new_parent_new.links += 1;
        old_parent_new.generation = new_gen;
        new_parent_new.generation = new_gen;

        ops[nops++] = (struct embk_put){
            .key = { .object_id = old_dir_oid, .type = EMBK_TYPE_INODE, .offset = 0 },
            .data = (const uint8_t *)&old_parent_new, .size = sizeof old_parent_new };
        ops[nops++] = (struct embk_put){
            .key = { .object_id = new_dir_oid, .type = EMBK_TYPE_INODE, .offset = 0 },
            .data = (const uint8_t *)&new_parent_new, .size = sizeof new_parent_new };
    }

    rc = embkfs_txn_apply_ops(vol, ops, nops, new_gen);

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

    /* 5. assemble the ops for one atomic commit. */
    uint32_t ops_cap = (last_link && !defer) ? (2 + old_n) : 2;
    struct embk_put *ops = kmalloc((uint64_t)ops_cap * sizeof *ops);
    if (!ops) { kfree(chain_buf); kfree(old_ext); return -EMBK_ENOMEM; }
    uint32_t nops = 0;

    struct embk_inode_item dec_ino;
    struct embk_inode_item orphan;                 /* NEW */

    if (defer) {                                   /* NEW arm */
        /* Name goes now; object stays. Write links=0 but keep inode+extents.
         * The blocks are reclaimed at last close, in embkfs_object_put. */
        orphan = ino;
        orphan.links = 0;
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
        dec_ino.generation = new_gen;
        ops[nops++] = (struct embk_put){
            .key = { .object_id = target, .type = EMBK_TYPE_INODE, .offset = 0 },
            .data = (const uint8_t *)&dec_ino, .size = sizeof dec_ino };
    }
    ops[nops++] = dirent_op;                        /* unchanged: name removal */



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

int embkfs_write_object(struct embkfs_volume *vol, uint64_t oid,
                        const uint8_t *data, uint64_t len)
{
    return embkfs_write_file(vol, oid, data, len);
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

int embkfs_read_object_at(struct embkfs_volume *vol, uint64_t oid,
                          uint64_t offset, uint8_t *buf, uint64_t len,
                          uint64_t *out_read)
{
    if (!vol || !buf || !out_read) return -EMBK_EINVAL;
    *out_read = 0;
    if (len == 0) return EMBK_OK;

    uint64_t total = 0;
    int rc = embkfs_read_object_prefix(vol, oid, NULL, 0, &total, NULL);
    if (rc != EMBK_OK) return rc;
    if (offset >= total) return EMBK_OK;

    uint64_t want = total - offset;
    if (want > len) want = len;

    uint64_t need_prefix = offset + want;
    uint8_t *tmp = kmalloc(need_prefix ? need_prefix : 1);
    if (!tmp) return -EMBK_ENOMEM;
    rc = embkfs_read_object_prefix(vol, oid, tmp, need_prefix, NULL, NULL);
    if (rc == EMBK_OK) {
        memcpy(buf, tmp + offset, want);
        *out_read = want;
    }
    kfree(tmp);
    return rc;
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
        uint32_t csum = 0;
        uint64_t written = 0;
        for (uint64_t blk = 0; blk < ext[i].length; blk++) {
            uint64_t chunk = ext[i].logical_size - written;
            if (chunk > vol->block_size) chunk = vol->block_size;
            rc = embk_block_read(vol->dev, (ext[i].disk_block + blk) * spb, spb, datablk);
            if (rc != EMBK_OK) { kfree(ext); return rc; }
            memcpy(out + pos, datablk, chunk);
            pos += chunk;
            written += chunk;
            csum = embk_crc32c(datablk, chunk, csum);
        }
        if (csum != ext[i].checksum) { kfree(ext); return -EMBK_EINVAL; }
    }
    kfree(ext);
    return EMBK_OK;
}

static int embkfs_read_object_prefix(struct embkfs_volume *vol, uint64_t oid,
                                     uint8_t *out, uint64_t want,
                                     uint64_t *out_total_len, uint32_t *out_mode)
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
    if (out_total_len) *out_total_len = ino->size;
    if (want == 0 || ino->size == 0) return EMBK_OK;
    if (!out) return -EMBK_EINVAL;

    uint64_t need = (want < ino->size) ? want : ino->size;

    uint32_t en = 0;
    int rc = embkfs_count_extents(vol, oid, &en);
    if (rc != EMBK_OK) return rc;
    if (en == 0) return -EMBK_EINVAL;

    struct embk_extref *ext = kmalloc((uint64_t)en * sizeof *ext);
    if (!ext) return -EMBK_ENOMEM;
    uint32_t got = 0; bool over = false;
    rc = embkfs_collect_extents(vol, oid, ext, en, &got, &over);
    if (rc != EMBK_OK || over || got != en) { kfree(ext); return -EMBK_EINVAL; }
    rc = embkfs_validate_extent_map(vol, ext, en, ino->size, "read_object_prefix");
    if (rc != EMBK_OK) { kfree(ext); return rc; }

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
        uint32_t csum = 0;
        uint64_t written = 0;
        for (uint64_t blk = 0; blk < ext[i].length && pos < need; blk++) {
            uint64_t chunk = ext[i].logical_size - written;
            if (chunk > vol->block_size) chunk = vol->block_size;
            rc = embk_block_read(vol->dev, (ext[i].disk_block + blk) * spb, spb, datablk);
            if (rc != EMBK_OK) { kfree(ext); return rc; }
            csum = embk_crc32c(datablk, chunk, csum);
            uint64_t take = need - pos;
            if (take > chunk) take = chunk;
            memcpy(out + pos, datablk, take);
            pos += take;
            written += chunk;
        }
        if (csum != ext[i].checksum) { kfree(ext); return -EMBK_EINVAL; }
    }

    kfree(ext);
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
    upd.links += 1;
    upd.generation = new_gen;

    struct embk_put add_op;
    uint8_t *add_buf = NULL;
    rc = embkfs_dirent_add_op(vol, new_dir_oid, new_name, name_len,
                              target_oid, ttype, probe, sizeof probe,
                              &add_op, &add_buf);
    if (rc != EMBK_OK) return rc;

    struct embk_put ops[2];
    uint32_t nops = 0;
    ops[nops++] = (struct embk_put){
        .key = { .object_id = target_oid, .type = EMBK_TYPE_INODE, .offset = 0 },
        .data = (const uint8_t *)&upd, .size = sizeof upd };
    ops[nops++] = add_op;

    rc = embkfs_txn_apply_ops(vol, ops, nops, new_gen);
    kfree(add_buf);
    return rc;
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

int embkfs_run_path_selftests(void)
{
    if (!g_embkfs_live || !g_embkfs_live->mounted) return -EMBK_ENODEV;
    embkfs_path_norm_smoke(g_embkfs_live);
    return EMBK_OK;
}

int embkfs_run_allocator_selftests(void)
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

int embkfs_run_tree_selftests(void)
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

int embkfs_run_object_selftests(void)
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

int embkfs_run_namespace_selftests(void)
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

    /* Fixed metadata outside the tree: primary + backup superblock. */
    embk_bm_set(vol->block_bitmap, EMBKFS_SB_OFFSET / vol->block_size);
    embk_bm_set(vol->block_bitmap, vol->total_blocks - 1);

    /* Mark every block the live tree references — and, in the same walk, record
     * the highest object_id in use (accumulated into vol->next_oid). */
    vol->next_oid = 0;
    int rc = embkfs_mark_tree(vol, &vol->root);
    if (rc != EMBK_OK) return rc;

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



void embkfs_init(void)
{
    kprintf("\n=== EMBKFS init ===\n");

    g_embkfs_live = NULL;

    /* Build the CRC32C table before anything verifies a checksum. */
    embk_crc32c_init();

    /* Probe every block device for an EMBKFS superblock; mount the first one
     * (same probe-each-disk pattern as the FAT32 mount). */
    static struct embkfs_volume vol;
    bool mounted = false;
    for (uint32_t i = 0; i < embk_block_count(); i++) {
        struct embk_block_device *d = embk_block_get(i);
        if (embkfs_mount(d, &vol) == EMBK_OK) {
            mounted = true;
            break;
        }
    }
    if (!mounted) {
        kprintf("EMBKFS: no volume found on any block device\n");
        return;
    }
    g_embkfs_live = &vol;


    /* Read + verify the root node — it may now be a leaf OR an internal node. */
    static uint8_t rootbuf[4096];
    if (embkfs_read_node(&vol, &vol.root, rootbuf, sizeof rootbuf) != EMBK_OK)
        return;
    const struct embk_node_header *h = (const struct embk_node_header *)rootbuf;
    kprintf("EMBKFS: %s: root node OK  level %u (%s)  nritems %u\n",
            vol.dev->name, (unsigned int)h->level,
            h->level == 0 ? "LEAF" : "internal", (unsigned int)h->nritems);
    
    embkfs_alloc_init(&vol);

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
