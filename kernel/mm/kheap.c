#include "kheap.h"
#include "../cpu/spinlock.h"
#include "pmm.h"
#include "vmm.h"
#include "../include/kprintf.h"
#include "../drivers/serial.h"
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

void kheap_init(void) {
    serial_write_string("\n=== KHeap init ===\n");

    heap_end = KHEAP_BASE;
    total_size = 0;
    used_size = 0;
    block_count = 0;

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

    // Calculate total block size needed (user data + header + tail canary), aligned to KHEAP_ALIGNMENT
    
    uint64_t aligned_size = align_up(size, KHEAP_ALIGNMENT);
    uint64_t total_block_size = aligned_size + sizeof(block_t) + sizeof(uint64_t); // user data + header + tail canary
    
    // First-fit search for a free block large enough to hold the requested size
    
    block_t *current = heap_head;
    while (current) {
        if (current->flags == BLOCK_FREE && current->size >= total_block_size) {
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

    if (block->flags != BLOCK_USED) {
        serial_write_string("kfree: Double free! or invalid free detected\n");
        return;
    }

    block->flags = BLOCK_FREE;
    used_size -= block->size - sizeof(block_t) - sizeof(uint64_t); // Only count user data size
    block->canary = 0; // Clear canarys to catch use-after-free

    // Coalesce with NEXT first
    if (block->next && block->next->flags == BLOCK_FREE) {
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
    if (block->prev && block->prev->flags == BLOCK_FREE) {
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


void *kheap_realloc(void *ptr, uint64_t new_size) {
    if (!ptr) {
        return kheap_alloc(new_size); // realloc with NULL ptr is just malloc
    }
    if (new_size == 0) {
        kheap_free(ptr); // realloc to size 0 is just free
        return 0;
    }

    block_t *block = block_from_user(ptr);
    uint64_t old_user_size = block->size - sizeof(block_t) - sizeof(uint64_t);

    if (new_size <= old_user_size) {
        // New size is smaller or equal to the old size, no need to allocate a new block
        return ptr;
    }
    // Need to grow the block. Check if we can expand into the next block if it's free and has enough space.
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


void kheap_stats(void) {
    uint64_t free_blocks = 0;
    uint64_t largest_free = 0;
    uint64_t used_blocks = 0;


    block_t *current = heap_head;
    while (current) {
        if (current->flags == BLOCK_FREE) {
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
        if (current->flags == BLOCK_USED && current->canary != KHEAP_CANARY_HEAD) {
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