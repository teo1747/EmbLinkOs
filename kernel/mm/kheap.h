#ifndef __KHEAP__H__
#define __KHEAP__H__


#include <stdint.h>

// Re-export the public allocation API (kmalloc/kcalloc/krealloc/kfree) so
// existing callers that only include this engine header keep working.
#include "../include/kmalloc.h"


// Heap virtual base
#define KHEAP_BASE 0xFFFFFF8000000000ULL

// Alignment for all returned pointers (16 bytes for any SSE types TYPE)
#define KHEAP_ALIGNMENT 16

// Canary value for detecting heap overflows (magic number)
#define KHEAP_CANARY_HEAD 0xDEADBEEFCAFEBABEULL
#define KHEAP_CANARY_TAIL 0xCAFEBABEDEADBEEFULL


/*
 * Low-level heap engine API.
 *
 * These are the real allocator entry points implemented in kheap.c. The
 * public kmalloc/kcalloc/krealloc/kfree names (declared in kmalloc.h and
 * implemented in kmalloc.c) are thin wrappers over these.
 */

// Initialize the kernel heap. Must be called before any other heap call.
void kheap_init(void);

// Allocate at least 'size' bytes, aligned to KHEAP_ALIGNMENT. NULL on failure.
void *kheap_alloc(uint64_t size);

// Allocate and zero a 'count' x 'size' array. NULL on failure or overflow.
void *kheap_calloc(uint64_t count, uint64_t size);

// Resize an allocation to 'new_size' bytes. Returns the (possibly moved)
// pointer, or NULL on failure (the original block is left untouched).
void *kheap_realloc(void *ptr, uint64_t new_size);

// Free a block returned by the allocator. NULL is a no-op.
void kheap_free(void *ptr);

// Print stats: total bytes, used, free, largest free block, block count
void kheap_stats(void);

// Walk the heap and validate canary, panics on corruption
void kheap_check(void);

#endif /* __KHEAP__H__ */