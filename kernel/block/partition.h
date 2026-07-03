#ifndef _PARTITION_H_
#define _PARTITION_H_

#include "block.h"

// Partition support: an MBR (DOS) partition table parser that exposes each
// primary partition as its own block device. A partition device delegates
// reads/writes to its parent disk, offset by the partition's starting LBA and
// bounded by its length — so a filesystem mounted on "sda1" can never address
// sectors outside that partition.
//
// Naming follows the Linux convention: the N-th partition of disk "sda" is
// "sda1", "sda2", ... (1-based).

// Scan one whole-disk device for an MBR partition table and register a child
// block device for each non-empty primary partition. Returns the number of
// partitions registered (0 if the disk has no valid MBR), or a negative
// EMBK_E* code on a read error.
int embk_partition_scan(struct embk_block_device *disk);

// Scan every whole-disk device currently in the block registry. Call this once,
// after all drivers have registered their disks, before mounting filesystems.
// Newly created partition devices are appended to the registry; this only scans
// the disks that existed when it was called (it never recurses into partitions).
void embk_partition_scan_all(void);

#endif /* _PARTITION_H_ */
