/* ==========================================================================
 * tally.c -> /tally.elf -- the reference EXTERNAL pipeline CONSUMER.
 *
 * Proves the INPUT half of the external contract: when a stage feeds an
 * external (`ls | tally`), the shell pipes the previous stage's serialized
 * value into the child's fd 0; this program detects that
 * (sval_structured_in: fstat(0) says FIFO), reads EVERY frame to EOF,
 * counts rows/items, and emits a {rows} record on fd 3.
 *
 * It is deliberately an external re-implementation of the `count` builtin,
 * which makes it SELF-CHECKING: `ls | tally | get rows` must equal
 * `ls | count` -- same data, one path through a builtin, one through the
 * whole spawn/INSTALL_OBJ/serialize/deserialize machinery.
 *
 * Materialize-shaped on purpose (read ALL input, then emit): that's the
 * deadlock-free shape the extern contract documents.
 * ========================================================================== */
#include "sval/sval.h"
#include <unistd.h>
#include <string.h>

int main(void) {
    if (!sval_structured_in()) {
        static const char msg[] = "tally: no structured input (use it in a pipeline: ls | tally)\n";
        write(1, msg, sizeof msg - 1);
        return 1;
    }

    struct sval_reader rd;
    sval_reader_init(&rd);
    int64_t rows = 0, frames = 0;
    struct value v;
    int rc;
    while ((rc = sval_read_blocking(&rd, SVAL_FD_IN, &v)) == 1) {
        frames++;
        switch (v.type) {
        case VAL_TABLE: rows += (int64_t)v.u.table->count; break;
        case VAL_LIST:  rows += (int64_t)v.u.list.count;   break;
        case VAL_NULL:  break;
        default:        rows += 1; break;
        }
        value_free(&v);
    }
    sval_reader_free(&rd);
    if (rc < 0) {
        static const char msg[] = "tally: corrupt structured input\n";
        write(2, msg, sizeof msg - 1);
        return 1;
    }

    struct value out = value_record();
    value_record_set(&out, "rows",   value_int(rows));
    value_record_set(&out, "frames", value_int(frames));

    int erc;
    if (sval_structured_out())
        erc = sval_emit(&out);
    else
        erc = sval_print(&out, 1);
    value_free(&out);
    return erc == 0 ? 0 : 1;
}
