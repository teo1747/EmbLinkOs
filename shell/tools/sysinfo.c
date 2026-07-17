/* ==========================================================================
 * sysinfo.c -> /sysinfo.elf -- the reference EXTERNAL pipeline PRODUCER.
 *
 * The proof that "drop a .elf on the image and it's a pipeline stage" is
 * real: this program is NOT a shell builtin. When the shell runs `sysinfo`,
 * the evaluator spawns it with a pipe INSTALL_OBJ'd as its fd 3; this
 * program notices (sval_structured_out: fstat(3) says FIFO), emits ONE
 * record as a wire frame, and exits. The shell collects the frame and the
 * record flows on down the pipeline like any builtin's output:
 *
 *     sysinfo                      -> the record, pretty-printed
 *     sysinfo | get processes      -> an int
 *
 * Run OUTSIDE a pipeline (no fd 3 pipe -- e.g. `run /sysinfo.elf` at the
 * kernel console), it pretty-prints to fd 1 instead: the two-face shape
 * every structured tool has.
 * ========================================================================== */
#include "sval/sval.h"
#include "embk.h"

/* Proves crt0 runs .init_array -- the SAME mechanism C++ global constructors
 * use, so this regression-covers the crt0 half of C++ support with a plain-C
 * program (no g++ needed). If the constructor never runs, `ctor_ran` stays
 * false and the emitted record says so; `test extern` then makes it visible.
 * Before crt0 walked the array, this silently stayed false. */
static bool g_ctor_ran = false;

__attribute__((constructor))
static void sysinfo_ctor(void) { g_ctor_ran = true; }

int main(void) {
    struct embk_proc_info info[64];
    int n = embk_proc_list(info, 64);
    int64_t procs = 0, kthreads = 0, blocked = 0;
    for (int i = 0; i < (n > 0 ? n : 0); i++) {
        if (info[i].is_kthread) kthreads++; else procs++;
        if (info[i].state == 3) blocked++;
    }

    struct value r = value_record();
    value_record_set(&r, "os",        value_string("EmbLink"));
    value_record_set(&r, "processes", value_int(procs));
    value_record_set(&r, "kthreads",  value_int(kthreads));
    value_record_set(&r, "blocked",   value_int(blocked));
    value_record_set(&r, "uptime_s",  value_int((int64_t)(embk_uptime_ms() / 1000)));
    value_record_set(&r, "ctor_ran",  value_bool(g_ctor_ran));   /* crt0 .init_array */

    int rc;
    if (sval_structured_out())
        rc = sval_emit(&r);        /* a pipeline stage: one frame on fd 3 */
    else
        rc = sval_print(&r, 1);    /* a human is watching: pretty text */
    value_free(&r);
    return rc == 0 ? 0 : 1;
}
