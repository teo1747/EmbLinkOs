/*
 * kmalloc.c — public kernel allocation API.
 *
 * These are the names the rest of the kernel calls (kmalloc/kcalloc/
 * krealloc/kfree). They are thin, well-defined wrappers over the heap
 * engine in kheap.c. Keeping the public API here decouples callers from
 * the engine's internal naming and gives us one place to enforce the
 * standard libc-style contracts (NULL handling, overflow checks).
 */

#include "../include/kmalloc.h"
#include "kheap.h"

void *kmalloc(uint64_t size) {
    // A zero-byte request returns NULL; the engine treats 0 as "no alloc".
    return kheap_alloc(size);
}

void kfree(void *ptr) {
    // Freeing NULL is a documented no-op.
    kheap_free(ptr);
}

void *kcalloc(uint64_t count, uint64_t size) {
    // Guard against count * size overflowing uint64_t. Without this an
    // overflow would yield a small allocation while the caller believes
    // it got count*size bytes — a classic heap-overflow primitive.
    if (count != 0 && size > (UINT64_MAX / count)) {
        return 0;
    }
    return kheap_calloc(count, size);
}

void *krealloc(void *ptr, uint64_t new_size) {
    // realloc(NULL, n) == malloc(n) and realloc(p, 0) == free(p); both
    // cases are handled by the engine.
    return kheap_realloc(ptr, new_size);
}
