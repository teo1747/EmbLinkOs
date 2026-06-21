#include "embkfs.h"

/*
 * Read-only EMBKFS mount. Fills in over the next steps; for now this TU
 * exists only so the compiler evaluates the _Static_asserts in embkfs.h
 * and proves our structs match the on-disk spec.
 */
void embkfs_init(void)
{
    /* TODO: superblock parse + verify (next step, after CRC32C) */
}