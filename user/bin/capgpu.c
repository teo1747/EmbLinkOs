/* capgpu.c -- the witness for ring 3: is the GPU surface handle gated on the
 * capability?
 *
 * It asks the kernel for a small surface -- the fundamental GPU handle a UI
 * program needs -- and reports the outcome as its exit code:
 *   2  the surface was granted (this process holds EMBK_CAP_GPU)
 *   0  the surface was refused (this process does not hold it)
 * `test capgate` spawns this program twice, once seeded WITHOUT GPU and once
 * WITH, and checks the two exit codes -- proving the gate bites exactly when it
 * should and never otherwise.
 */
#include "embk.h"

int main(void) {
    struct embk_surface_info info;
    /* 2 buffers: surfaces are double-buffered (SURFACE_MIN_BUFFERS), so a
     * request of 1 fails validation for a reason unrelated to the cap gate. */
    int h = embk_surface_create(64, 64, EMBK_PIXFMT_BGRA8888_PRE, 2, &info);
    if (h >= 0) return 2;      /* granted */
    if (h == -1) return 0;     /* -EMBK_EPERM: gated (the expected denial) */
    return 3;                  /* some OTHER failure -- distinct so it can't be
                                * mistaken for the clean gate denial */
}
