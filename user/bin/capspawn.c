/* capspawn.c -- drives the capability attenuation test across the SYSCALL
 * boundary, which is the thing ring 1 adds that `test caps` (kernel-internal)
 * could not reach.
 *
 * This process is spawned by the kernel selftest with a LIMITED set: {FS,NET}.
 * That limit is the whole point -- it lets us prove not just that a parent can
 * attenuate a child, but that a parent CANNOT grant what it does not itself
 * hold (monotonicity, EMBX §5.2). A test run from a full-authority parent could
 * never show the refusal.
 *
 * Steps, each exiting a distinct nonzero code on failure so the kernel side can
 * say which one broke:
 *   1. embk_getcaps() must report exactly {FS,NET} -- proves the kernel seeded
 *      this process's set and getcaps reads it.
 *   2. spawn capchild with SET_CAPS {FS}: succeeds, capchild exits 2 -- proves
 *      the SET_CAPS action attenuates across the syscall.
 *   3. spawn capchild with SET_CAPS {FS,NET}: succeeds, capchild exits 6 --
 *      a parent granting its whole set is fine.
 *   4. spawn capchild with SET_CAPS {GPU}: MUST be refused, because this parent
 *      does not hold GPU. A negative return is the pass here.
 * Exit 0 only if all four hold.
 */
#include "embk.h"

#define FS   EMBK_CAP_BIT(EMBK_CAP_FILESYSTEM)   /* 2 */
#define NET  EMBK_CAP_BIT(EMBK_CAP_NETWORK)      /* 4 */
#define GPU  EMBK_CAP_BIT(EMBK_CAP_GPU)          /* 8 */

/* spawn capchild with a requested cap mask; return its exit code, or the
 * negative spawn error if the spawn itself was refused. */
static int spawn_capchild(unsigned mask) {
    char *argv[] = { (char *)"/data/apps/capchild/capchild.elf", 0 };
    struct embk_spawn_file_action act;
    embk_action_set_caps(&act, mask);
    int64_t h = embk_spawn(argv[0], argv, &act, 1);
    if (h < 0) return (int)h;                 /* refused (e.g. -EMBK_EPERM) */
    return embk_wait((int)h);            /* capchild's exit == its caps */
}

int main(void) {
    /* (1) our own set is exactly what the kernel granted us */
    unsigned long mine = embk_getcaps();
    if (mine != (FS | NET)) return 11;

    /* (2) attenuate down to {FS}: capchild should report 2 */
    if (spawn_capchild(FS) != (int)FS) return 12;

    /* (3) grant our whole set {FS,NET}: capchild should report 6 */
    if (spawn_capchild(FS | NET) != (int)(FS | NET)) return 13;

    /* (4) ask for {GPU} we do not hold: the spawn MUST fail */
    if (spawn_capchild(GPU) >= 0) return 14;

    return 0;   /* all four held */
}
