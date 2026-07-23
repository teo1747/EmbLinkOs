/* capreload.c -- the REFUSE half of the EMBX capability test.
 *
 * `test embx` seeds this process with only {NETWORK}, then it tries to spawn
 * capchild.embx, which DECLARES {FILESYSTEM}. Because this process does not
 * hold FILESYSTEM, the EMBX loader's step-9 check must deny the spawn before
 * anything is mapped -- the spawn returns a negative error.
 *
 * Exit 0 iff the denial happened (the expected result); nonzero if the load
 * was wrongly allowed. A single exit code carries the verdict back to the
 * kernel test, same pattern as capspawn/capchild.
 */
#include "embk.h"

int main(void) {
    char *argv[] = { (char *)"/data/apps/capchildx/capchild.embx", 0 };
    int64_t h = embk_spawn(argv[0], argv, 0, 0);   /* no file actions */
    if (h >= 0) {
        /* Wrongly allowed: the EMBX declared a capability we do not hold and
         * the loader let it through. Reap it and fail. */
        embk_wait((int)h);
        return 1;
    }
    return 0;   /* denied, exactly as step 9 requires */
}
