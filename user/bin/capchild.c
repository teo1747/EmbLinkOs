/* capchild.c -- the witness for the capability-attenuation test.
 *
 * It does exactly one thing: exit with its OWN capability set as the exit code,
 * so the parent that spawned it can read (via wait) precisely what the kernel
 * granted this child. If attenuation across the spawn syscall works, a child
 * spawned with a SET_CAPS action requesting {FILESYSTEM} exits 2 (bit 1), and
 * one spawned with {FILESYSTEM,NETWORK} exits 6 -- the parent checks the number.
 *
 * A cap set is a small bitmask (bits 1..9), so it fits an exit code with room
 * to spare; no need to print anything. */
#include "embk.h"

int main(void) {
    return (int)embk_getcaps();
}
