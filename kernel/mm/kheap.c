#include "mm/kheap.h"
#include "arch/x86_64/cpu/spinlock.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "include/kprintf.h"
#include "drivers/char/serial.h"
#include <stdint.h>



// Block header. Every allocation has this in front of the user data.
// Layout in memory:
//
//   +----+-------+---------+----------+--------+-------------+----+
//   | sz | flags | prev    | next     | canary | user_data...| tail|
//   +----+-------+---------+----------+--------+-------------+----+
//   ^                                          ^
//   block                                      returned to user
//
// size includes the header itself. So a 64-byte user allocation
// gives size = sizeof(block_t) + 64 + 8 (tail canary).
//
// We use a doubly-linked list of ALL blocks (free and used) sorted
// by address. This makes coalescing on free O(1).

typedef struct block {
    uint64_t size;      // total size of the block (including header and canaries)
    uint64_t flags;     // bit 0 = free/used
    struct block *prev; // previous block in memory (NULL if this is the first block)
    struct block *next; // next block in memory (NULL if this is the last block)
    uint64_t canary;    // KHEAP_CANARY_HEAD for integrity check
} block_t;


#define BLOCK_FREE      0x0
#define BLOCK_USED      0x1


// Minimum useful block size (must hold a free block + some user data)
#define MIN_BLOCK_SIZE (sizeof(block_t) + KHEAP_ALIGNMENT + sizeof(uint64_t))


/* Slab pools: O(1) alloc/free for fixed sizes (16..1024). Rebuilt after the
 * first attempt was disabled for two fatal bugs -- documented here because the
 * fixes ARE the design:
 *
 *   1. DISCRIMINATION BY RANGE, not by a magic byte. The old version tagged each
 *      object [0xAA][pool] and hoped kfree could tell slab from general by that
 *      byte -- but 0xAA occurs in ordinary data, so it collided (this is the
 *      "metadata collision" the TODO named). Instead, every slab region records
 *      its [start,end) in g_slab_ranges[]; kfree/krealloc look a pointer up
 *      there. A range hit is DEFINITIVE -- it cannot false-positive on general
 *      data the way a byte can.
 *
 *   2. INTRUSIVE free list, not a side array. The old free_list was a void*[]
 *      grown with kheap_alloc() -- called from *under* heap_lock, which re-locks
 *      the same spinlock and SELF-DEADLOCKS. A free object now stores the next
 *      pointer in its OWN first 8 bytes (obj_size >= 16 >= sizeof(void*)), so
 *      push/pop are O(1) and allocate nothing.
 *
 * A slab region is just a permanent general allocation (kheap_alloc_locked)
 * subdivided into objects -- no manual block-list surgery, and the region keeps
 * the general allocator's canaries. Objects have NO per-object header. */
typedef struct slab_pool {
    uint64_t obj_size;      // 16, 32, 64, ...
    void    *free_head;     // intrusive: *(void**)obj == next free object
    uint64_t total_objs;    // objects ever carved into this pool
    uint64_t used_objs;     // currently handed out
} slab_pool_t;

/* Registry of slab-owned address ranges. kheap_free()/kheap_realloc() consult it
 * to route a pointer: a hit means "this is a slab object of pool N", a miss means
 * "general block". Fixed and bounded; if it fills, grow just declines and the
 * size falls back to the general allocator (honest degradation, never unsafe). */
#define SLAB_MAX_RANGES 128
#define SLAB_REGION_BYTES (8u * 1024u)   // per grow; keeps range count low
typedef struct slab_range { uintptr_t start, end; uint8_t pool_idx; } slab_range_t;
static slab_range_t g_slab_ranges[SLAB_MAX_RANGES];
static int g_slab_range_count;

// Size classes and their pools. Declared here (not lower down) so the lookup
// helpers below can see slab_sizes[].
static const uint64_t slab_sizes[KHEAP_SLAB_SIZES] = {
    16, 32, 64, 128, 256, 512, 1024
};
static slab_pool_t slab_pools[KHEAP_SLAB_SIZES];

// pool_idx if ptr lies in a slab region, else -1. The whole discrimination.
static int slab_range_lookup(void *ptr) {
    uintptr_t a = (uintptr_t)ptr;
    for (int i = 0; i < g_slab_range_count; i++) {
        if (a >= g_slab_ranges[i].start && a < g_slab_ranges[i].end)
            return g_slab_ranges[i].pool_idx;
    }
    return -1;
}

// Smallest size class that fits `size`, or -1 if too big for any pool. Rounds
// UP (so kmalloc(20) uses the 32 pool) -- exact-match-only made slabs never fire.
static inline int slab_class_for(uint64_t size) {
    for (int i = 0; i < KHEAP_SLAB_SIZES; i++)
        if (size <= slab_sizes[i]) return i;
    return -1;
}


static spinlock_t heap_lock = SPINLOCK_INIT; // Spinlock to protect heap data structures in case of concurrent access from multiple cores or interrupts

// Heap state
static block_t *heap_head = 0; // pointer to the first block in the heap
static uint64_t  heap_end = 0; // next virtual address beyond the heap
static uint64_t total_size = 0; // total size of the heap in bytes (including all blocks and canaries)
static uint64_t used_size = 0;  // total used bytes (excluding free blocks and canaries)
static uint64_t block_count = 0; // total number of blocks (free + used)

// Round up size to next multiple of KHEAP_ALIGNMENT(align must be power of 2)
static inline uint64_t align_up(uint64_t size, uint64_t align) {
    return (size + align - 1) & ~(align - 1);
}

// Get pointer to the canary at the end of the block
static inline uint64_t *block_tail_canary(block_t *block) {
    return (uint64_t *)((uint8_t *)block + block->size - sizeof(uint64_t));
}

// Get pointer to the user data area of the block (where the caller can write)
static inline void *block_user_data(block_t *block) {
    return (void *)((uint8_t *)block + sizeof(block_t));
}

// Get pointer to the block header from a user data pointer
static inline block_t *block_from_user(void *ptr) {
    return (block_t *)((uint8_t *)ptr - sizeof(block_t));
}

// Compute pointer to where the next block would start in memory (used for coalescing)
static inline block_t *block_next_addr(block_t *block) {
    return (block_t *)((uint8_t *)block + block->size);
}

// Find slab pool index for a given size, or -1 if no exact match
// Allocate one more page and append it to the heap. Returns pointer to the new block or NULL on failure.
static uint64_t heap_grow(uint64_t bytes) {
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE; // Round up to full pages
    uint64_t new_block_addr = heap_end;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            serial_write_string("kheap: PMM exhausted during grow\n");
            return 0; // Out of memory
        }
        if (vmm_map(heap_end, phys, VMM_WRITABLE) < 0) {
            serial_write_string("kheap: VMM failed to map page during grow\n");
            return 0; // Failed to map
        }
        heap_end += PAGE_SIZE;
    }
    total_size += pages * PAGE_SIZE;


    return new_block_addr;
}

// Initialize slab pools
static void kheap_slab_init(void) {
    for (int i = 0; i < KHEAP_SLAB_SIZES; i++) {
        slab_pools[i].obj_size = slab_sizes[i];
        slab_pools[i].free_head = 0;
        slab_pools[i].total_objs = 0;
        slab_pools[i].used_objs = 0;
    }
    g_slab_range_count = 0;
}

static void *kheap_alloc_locked(uint64_t size);   /* grow carves regions from it */

/* Grow a pool: carve one region of SLAB_REGION_BYTES worth of objects and thread
 * them onto the intrusive free list. The region is a PERMANENT general
 * allocation -- we never free it, we subdivide it -- so it keeps the general
 * allocator's canaries and needs no manual block-list surgery. Its [start,end)
 * is registered so kfree can route objects back here by RANGE. */
static void kheap_slab_grow_locked(slab_pool_t *pool) {
    if (g_slab_range_count >= SLAB_MAX_RANGES)
        return;   /* registry full: decline, and the size falls back to general */

    uint64_t objs = SLAB_REGION_BYTES / pool->obj_size;
    if (objs < KHEAP_SLAB_MIN_OBJS) objs = KHEAP_SLAB_MIN_OBJS;

    /* kheap_alloc_locked, NOT kheap_alloc: we already hold heap_lock, and the
     * public wrapper would re-lock the same spinlock and self-deadlock -- the
     * exact bug that disabled the first slab. This region request is > 1024, so
     * it goes to the general path, not recursively back into a slab. */
    uint8_t *region = (uint8_t *)kheap_alloc_locked(objs * pool->obj_size);
    if (!region)
        return;   /* OOM: pool stays empty, caller falls back to general */

    g_slab_ranges[g_slab_range_count].start    = (uintptr_t)region;
    g_slab_ranges[g_slab_range_count].end      = (uintptr_t)region + objs * pool->obj_size;
    g_slab_ranges[g_slab_range_count].pool_idx = (uint8_t)(pool - slab_pools);
    g_slab_range_count++;

    for (uint64_t i = 0; i < objs; i++) {
        void *obj = region + i * pool->obj_size;
        *(void **)obj = pool->free_head;   /* intrusive push */
        pool->free_head = obj;
    }
    pool->total_objs += objs;
}

// O(1) alloc: pop the intrusive free list, growing the pool if empty.
static void *kheap_slab_alloc_locked(slab_pool_t *pool) {
    if (!pool->free_head) {
        kheap_slab_grow_locked(pool);
        if (!pool->free_head)
            return NULL;   // couldn't grow (OOM or registry full)
    }
    void *obj = pool->free_head;
    pool->free_head = *(void **)obj;   /* intrusive pop */
    pool->used_objs++;
    return obj;
}

// O(1) free: push onto the intrusive free list. The caller has already confirmed
// (by range lookup) that `obj` belongs to `pool`.
static void kheap_slab_free_locked(slab_pool_t *pool, void *obj) {
    *(void **)obj = pool->free_head;
    pool->free_head = obj;
    pool->used_objs--;
}

void kheap_init(void) {
    serial_write_string("\n=== KHeap init ===\n");

    heap_end = KHEAP_BASE;
    total_size = 0;
    used_size = 0;
    block_count = 0;

    // Initialize slab pools
    kheap_slab_init();

    // Initially grow the heap by one page to create the first block
    uint64_t base = heap_grow(PAGE_SIZE);
    if (!base) {
        serial_write_string("kheap: Failed to initialize heap\n");
        while (1) {} // Halt if we can't initialize the heap
    }

    // Create the initial free block that spans the entire first page
    block_t *initial_block = (block_t *)base;
    initial_block->size = PAGE_SIZE;
    initial_block->flags = BLOCK_FREE;
    initial_block->prev = 0;
    initial_block->next = 0;
    initial_block->canary = 0;
    *block_tail_canary(initial_block) = KHEAP_CANARY_TAIL;

    heap_head = initial_block;
    block_count = 1;

   kprintf("Heap initialized at %p, %u bytes\n",
                                    (void*)KHEAP_BASE, (unsigned int)PAGE_SIZE);
}


static void *kheap_alloc_locked(uint64_t size) {
    if (size == 0) {
        return 0; // Don't allocate zero bytes
    }

    // Small requests go to a slab pool (O(1), less fragmentation). On slab OOM /
    // registry-full, slab_alloc returns NULL and we fall through to the general
    // first-fit path below -- the slab is an optimisation, never a requirement.
    int cls = slab_class_for(size);
    if (cls >= 0) {
        void *obj = kheap_slab_alloc_locked(&slab_pools[cls]);
        if (obj)
            return obj;
    }

    // Calculate total block size needed (user data + header + tail canary), aligned to KHEAP_ALIGNMENT

    uint64_t aligned_size = align_up(size, KHEAP_ALIGNMENT);
    uint64_t total_block_size = aligned_size + sizeof(block_t) + sizeof(uint64_t); // user data + header + tail canary

    // First-fit search for a free block large enough to hold the requested size

    block_t *current = heap_head;
    while (current) {
        if (!(current->flags & BLOCK_USED) && current->size >= total_block_size) {
            // Found a free block large enough. Should we split it?
            uint64_t leftover = current->size - total_block_size;

            if (leftover >= MIN_BLOCK_SIZE) {
                // Split the block into an allocated block and a smaller free block
                block_t *new_block = (block_t *)((uint8_t *)current + total_block_size);
                new_block->size = leftover;
                new_block->flags = BLOCK_FREE;
                new_block->prev = current;
                new_block->next = current->next;
                new_block->canary = 0;
                *block_tail_canary(new_block) = KHEAP_CANARY_TAIL;

                if (current->next) {
                    current->next->prev = new_block;
                }
                current->next = new_block;
                current->size = total_block_size; // Resize current block to the allocated size
                block_count++;
            }

            // Mark the current block as used and set canaries
            current->flags = BLOCK_USED;
            current->canary = KHEAP_CANARY_HEAD;
            *block_tail_canary(current) = KHEAP_CANARY_TAIL;

            used_size += current->size - sizeof(block_t) - sizeof(uint64_t); // Only count user data size
            return block_user_data(current);
        }
        current = current->next;
    }

    // No suitable block found, need to grow the heap
    uint64_t new_block_addr = heap_grow(total_block_size);
    if (!new_block_addr) {
        return 0; // Failed to grow heap
    }

    // Append the new region as a free block, then recursively call kheap_alloc to allocate from it (this will handle splitting if the new block is larger than needed)
    // first find the last block to link this on.
    block_t *last = heap_head;
    while (last->next) {
        last = last->next;
    }
    // Create a new block in the newly allocated space
    uint64_t actual_pages = (total_block_size + PAGE_SIZE - 1) / PAGE_SIZE; // How many pages we actually allocated
    block_t *new_block = (block_t *)new_block_addr;
    new_block->size = actual_pages * PAGE_SIZE;
    new_block->flags = BLOCK_FREE;
    new_block->prev = last;
    new_block->next = 0;
    new_block->canary = 0; // will be set in kheap_alloc call
    *block_tail_canary(new_block) = KHEAP_CANARY_TAIL;
    last->next = new_block;
    block_count++;

    // Recursively call kheap_alloc to allocate from the new block (this will handle splitting if the new block is larger than needed)
    return kheap_alloc_locked(size);
}


static void kheap_free_locked(void *ptr) {
    if (!ptr) {
        return; // No operation on NULL pointer
    }

    // Slab object? Decide by RANGE, not by a byte in the object -- the range
    // registry cannot false-positive the way the old magic byte did. A hit means
    // this pointer is definitively a slab object; route it to its pool and stop
    // (it has no block_t header, so the general path below would misread it).
    int cls = slab_range_lookup(ptr);
    if (cls >= 0) {
        kheap_slab_free_locked(&slab_pools[cls], ptr);
        return;
    }

    block_t *block = block_from_user(ptr);

    // Validate canaries to detect corruption
    if (block->canary != KHEAP_CANARY_HEAD ) {
        serial_write_string("kfree: Heap canary corruption detected during free\n");
        kheap_check(); // This will panic with details about the corruption
        return;
    }
    if (*block_tail_canary(block) != KHEAP_CANARY_TAIL) {
        serial_write_string("kfree: Heap corruption detected (tail canary) during free\n");
        kheap_check(); // This will panic with details about the corruption
        return;
 }

    if (!(block->flags & BLOCK_USED)) {
        serial_write_string("kfree: Double free! or invalid free detected\n");
        return;
    }

    block->flags &= ~BLOCK_USED;  // Clear USED flag, keep other flags
    used_size -= block->size - sizeof(block_t) - sizeof(uint64_t); // Only count user data size
    block->canary = 0; // Clear canarys to catch use-after-free

    // Coalesce with NEXT first
    if (block->next && !(block->next->flags & BLOCK_USED)) {
        block_t *next = block->next;
        block->size += next->size;
        block->next = next->next;
        if (next->next) {
            next->next->prev = block;
        }
        *block_tail_canary(block) = KHEAP_CANARY_TAIL;
        block_count--;
    }

    // Then coalesce with PREVIOUS
    if (block->prev && !(block->prev->flags & BLOCK_USED)) {
        block_t *prev = block->prev;
        prev->size += block->size;
        prev->next = block->next;
        if (block->next) {
            block->next->prev = prev;
        }
        *block_tail_canary(prev) = KHEAP_CANARY_TAIL;
        block_count--;
    }
}

void *kheap_alloc(uint64_t size) {
    spin_lock(&heap_lock);
    void *ptr = kheap_alloc_locked(size);
    spin_unlock(&heap_lock);
    return ptr;
}


void kheap_free(void *ptr) {
    spin_lock(&heap_lock);
    kheap_free_locked(ptr);
    spin_unlock(&heap_lock);
}



void *kheap_calloc(uint64_t count, uint64_t size) {
    uint64_t total_size = count * size;
    void *ptr = kheap_alloc(total_size);
    if (ptr) {
        // Zero-initialize the allocated memory
        for (uint64_t i = 0; i < total_size; i++) {
            ((uint8_t *)ptr)[i] = 0;
        }
    }
    return ptr;
}

// Forward declaration for kheap_try_expand_locked
static bool kheap_try_expand_locked(block_t *block, uint64_t new_size);


void *kheap_realloc(void *ptr, uint64_t new_size) {
    if (!ptr) {
        return kheap_alloc(new_size); // realloc with NULL ptr is just malloc
    }
    if (new_size == 0) {
        kheap_free(ptr); // realloc to size 0 is just free
        return 0;
    }

    spin_lock(&heap_lock);

    // Slab object? It has no block_t header, so the general path below would
    // misread block->size. Handle by class: if the new size still fits this
    // class's object, the pointer is unchanged; otherwise allocate fresh, copy
    // the object's worth of bytes, and free the old back to its pool.
    int cls = slab_range_lookup(ptr);
    if (cls >= 0) {
        uint64_t obj_size = slab_sizes[cls];
        if (new_size <= obj_size) {
            spin_unlock(&heap_lock);
            return ptr;
        }
        void *new_ptr = kheap_alloc_locked(new_size);   /* under the lock already */
        if (new_ptr) {
            uint8_t *s = (uint8_t *)ptr, *d = (uint8_t *)new_ptr;
            for (uint64_t i = 0; i < obj_size; i++) d[i] = s[i];
            kheap_slab_free_locked(&slab_pools[cls], ptr);
        }
        spin_unlock(&heap_lock);   /* new_ptr==NULL: original intact, per realloc contract */
        return new_ptr;
    }

    block_t *block = block_from_user(ptr);
    uint64_t old_user_size = block->size - sizeof(block_t) - sizeof(uint64_t);

    if (new_size <= old_user_size) {
        // New size is smaller or equal to the old size, no need to allocate a new block
        spin_unlock(&heap_lock);
        return ptr;
    }

    // Try in-place expansion by coalescing with next block if it's free
    if (kheap_try_expand_locked(block, new_size)) {
        spin_unlock(&heap_lock);
        return ptr;  // Expanded in-place, pointer unchanged
    }

    spin_unlock(&heap_lock);

    // Need to grow the block. Allocate a new one and copy.
    void *new_ptr = kheap_alloc(new_size);
    if (!new_ptr) {
        return 0; // Failed to allocate new block
    }

    // Copy old data to new block
    uint8_t *src = (uint8_t *)ptr;
    uint8_t *dst = (uint8_t *)new_ptr;
    for (uint64_t i = 0; i < old_user_size ; i++) {
        dst[i] = src[i];
    }
    kheap_free(ptr); // Free the old block
    return new_ptr;
}

// Try to expand a block in-place by coalescing with the next block if it's free
static bool kheap_try_expand_locked(block_t *block, uint64_t new_size) {
    if (!block->next || (block->next->flags & BLOCK_USED)) {
        return false;  // Can't expand: no next block or it's not free
    }

    uint64_t aligned_new_size = align_up(new_size, KHEAP_ALIGNMENT);
    uint64_t new_total_size = aligned_new_size + sizeof(block_t) + sizeof(uint64_t);
    uint64_t combined_size = block->size + block->next->size;

    if (combined_size < new_total_size) {
        return false;  // Can't expand: next block + current not large enough
    }

    // Coalesce with next block
    block_t *next = block->next;
    uint64_t new_block_size = combined_size - (new_total_size - block->size);
    block->size = new_total_size;

    if (new_block_size >= MIN_BLOCK_SIZE) {
        // Split off the leftover
        block_t *leftover = (block_t *)((uint8_t *)block + new_total_size);
        leftover->size = new_block_size;
        leftover->flags = BLOCK_FREE;
        leftover->prev = block;
        leftover->next = next->next;
        leftover->canary = 0;
        *block_tail_canary(leftover) = KHEAP_CANARY_TAIL;
        if (next->next) {
            next->next->prev = leftover;
        }
        block->next = leftover;
    } else {
        // Absorb the entire next block
        block->size = combined_size;
        block->next = next->next;
        if (next->next) {
            next->next->prev = block;
        }
        block_count--;
    }

    *block_tail_canary(block) = KHEAP_CANARY_TAIL;
    return true;  // Successfully expanded in-place
}


void kheap_stats(void) {
    uint64_t free_blocks = 0;
    uint64_t largest_free = 0;
    uint64_t used_blocks = 0;


    block_t *current = heap_head;
    while (current) {
        if (!(current->flags & BLOCK_USED)) {
            free_blocks++;
            if (current->size > largest_free) {
                largest_free = current->size;
            }
        } else {
            used_blocks++;
        }
        current = current->next;
    }
    kprintf("=== Heap stats ===\n");
    kprintf("   Total size: %u bytes\n", (unsigned int)total_size);
    kprintf("   Used size: %u bytes\n", (unsigned int)used_size);
    kprintf("   Free size: %u bytes\n", (unsigned int)(total_size - used_size));
    kprintf("   Largest free block: %u bytes\n", (unsigned int)largest_free);
    kprintf("   Total blocks: %u\n", (unsigned int)block_count);
    kprintf("   Used blocks: %u\n", (unsigned int)used_blocks);
    kprintf("   Free blocks: %u\n", (unsigned int)free_blocks);

}

void kheap_check(void) {
    block_t *current = heap_head;
    uint64_t i = 0;
    while (current) {
        if ((current->flags & BLOCK_USED) && current->canary != KHEAP_CANARY_HEAD) {
            serial_write_string("kHeap_check: corruption detected: head canary mismatch at block ");
            serial_write_hex((uint64_t)current);
            serial_write_string("\n");
            while (1) {} // Halt on corruption
        }
        if (*block_tail_canary(current) != KHEAP_CANARY_TAIL) {
            serial_write_string("Heap corruption detected: tail canary mismatch at block ");
            serial_write_hex((uint64_t)current);
            serial_write_string("\n");
            while (1) {} // Halt on corruption
        }
        current = current->next;
        i++;
    }
}

void kheap_slab_stats(void) {
    kprintf("=== Slab Pool Stats ===\n");
    for (int i = 0; i < KHEAP_SLAB_SIZES; i++) {
        slab_pool_t *pool = &slab_pools[i];
        kprintf("  %u-byte pool: %lu used / %lu total (%lu free)\n",
                (unsigned int)pool->obj_size,
                pool->used_objs,
                pool->total_objs,
                pool->total_objs - pool->used_objs);
    }
    kprintf("  slab ranges: %d / %d\n", g_slab_range_count, SLAB_MAX_RANGES);
}