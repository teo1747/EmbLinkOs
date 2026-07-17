/* ==========================================================================
 * sval.h -- the structured-value SDK. The thin layer a program (ls.elf, a
 * builtin, the terminal) actually calls, so nobody touches wire_serialize /
 * wire_reader directly.
 *
 * The fd convention (locked earlier): structured output goes to fd 3, human
 * text to fd 1. A program discovers whether it's IN a structured pipeline by
 * whether fd 3 (out) / fd 0 (in) is actually a pipe -- the fd's existence IS
 * the signal, no flag, no env var. So a program's shape is:
 *
 *     if (sval_structured_out())  sval_emit(&table);        // pipeline: emit
 *     else                        sval_print(&table, 1);    // interactive: pretty
 *
 * ls can do BOTH (structured on 3, pretty on 1) since they're different fds.
 * ========================================================================== */
#ifndef __EMBK_SVAL_H__
#define __EMBK_SVAL_H__

#include "value/value.h"
#include "wire/wire.h"
#include <stdint.h>
#include <stddef.h>

#define SVAL_FD_IN   0   /* structured input arrives here when fd 0 is a pipe */
#define SVAL_FD_OUT  3   /* structured output goes here when fd 3 is a pipe   */

/* -------------------------------------------------------------------------
 * Mode detection -- is this program in a structured pipeline?
 * Implemented by fstat'ing the fd and checking it's a FIFO (VFS_DT_FIFO).
 * An unopened fd 3 fstats to EBADF -> not structured. A console fd fstats to
 * VFS_DT_CHAR -> not structured (a human is watching). Only a real pipe says
 * "the next/prev stage speaks wire frames."
 * ------------------------------------------------------------------------- */
bool sval_structured_out(void);   /* should I emit structured data on fd 3? */
bool sval_structured_in(void);    /* is fd 0 a structured input pipe?        */

/* -------------------------------------------------------------------------
 * Emit -- serialize one value to a frame and write it to fd 3, handling
 * partial writes (a pipe may accept fewer bytes than offered; loop until the
 * whole frame is out). Returns 0, or negative on serialize-OOM / write error
 * (EPIPE if the downstream reader has gone away -- a normal, non-fatal end of
 * a pipeline the producer should stop on, not crash on).
 * ------------------------------------------------------------------------- */
int sval_emit(const struct value *v);
int sval_emit_fd(int fd, const struct value *v);   /* emit to an explicit fd */

/* -------------------------------------------------------------------------
 * Reader. The PUMP is the primitive; the BLOCKING form is a wrapper over it.
 *
 * PUMP model (for a program multiplexing fd 0 with other input -- the
 * terminal watching a pipe AND the keyboard): the caller owns the read()
 * loop and feeds raw bytes in; the reader parses and hands back whole values.
 *
 *     struct sval_reader rd; sval_reader_init(&rd);
 *     ... when fd 0 is readable:
 *         ssize_t n = read(0, tmp, sizeof tmp);
 *         if (n > 0) sval_reader_feed(&rd, tmp, n);
 *         struct value v;
 *         while (sval_reader_next(&rd, &v) == 1) { use(&v); value_free(&v); }
 *
 * BLOCKING model (for a straight-line consumer -- `where`, `select`): the SDK
 * owns the read() loop; the caller just pulls values until EOF.
 *
 *     struct sval_reader rd; sval_reader_init(&rd);
 *     struct value v;
 *     while (sval_read_blocking(&rd, 0, &v) == 1) { use(&v); value_free(&v); }
 *     // returns 0 at EOF, <0 on protocol error
 * ------------------------------------------------------------------------- */
struct sval_reader {
    struct wire_reader wr;   /* the frame decoder underneath */
};
void sval_reader_init(struct sval_reader *r);
void sval_reader_free(struct sval_reader *r);

int  sval_reader_feed(struct sval_reader *r, const uint8_t *data, size_t n);
     /* -> 0 ok, <0 OOM. Pump: hand raw stream bytes in. */
int  sval_reader_next(struct sval_reader *r, struct value *out);
     /* -> 1 a value came out (caller frees), 0 need more bytes, <0 protocol err */

int  sval_read_blocking(struct sval_reader *r, int fd, struct value *out);
     /* -> 1 got a value, 0 clean EOF, <0 read/protocol error. Loops read() +
      *    next() internally until a value pops or the pipe hits EOF. */

/* -------------------------------------------------------------------------
 * Human rendering -- print a value as readable text to `fd` (normally 1).
 * The humble ancestor of the terminal's real table grid; enough that the
 * interactive (non-piped) path shows something legible today.
 *   - scalars print inline (ints, "1.5 MB" for filesize, ISO-ish for date)
 *   - a LIST prints one item per line
 *   - a RECORD prints "name: value" lines
 *   - a TABLE prints an aligned column grid (header row + rows)
 * Returns 0, or negative on write error.
 * ------------------------------------------------------------------------- */
int sval_print(const struct value *v, int fd);

/* Format a single SCALAR value into `buf` (NUL-terminated, truncated to cap);
 * returns the length written. Exposed because the table renderer and future
 * callers both need "one cell as text". Non-scalars render as a short
 * placeholder ("[list]", "{record}", "[table]") -- nesting inside a grid cell
 * is the renderer's problem to expand, not this helper's. */
size_t sval_format_scalar(const struct value *v, char *buf, size_t cap);

#endif /* __EMBK_SVAL_H__ */