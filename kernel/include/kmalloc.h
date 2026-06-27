#ifndef __KMALLOC_H__
#define __KMALLOC_H__

#include <stdint.h>

/*
 * Phase 8 kernel heap allocation API.
 *
 * This header exposes the legacy kmalloc/kfree interface used by older
 * kernel components, while the underlying implementation lives in the
 * kernel heap allocator.
 */

void *kmalloc(uint64_t size);
void *kcalloc(uint64_t count, uint64_t size);
void *krealloc(void *ptr, uint64_t new_size);
void kfree(void *ptr);

#endif /* __KMALLOC_H__ */
