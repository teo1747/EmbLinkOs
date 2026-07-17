/* ==========================================================================
 * sval.c -- the structured-value SDK. See sval.h.
 * Uses the newlib POSIX layer (read/write/fstat) -- so any program linking
 * this is a newlib program, not a freestanding -nostdlib one. That's fine:
 * the shell, ls, the terminal are all newlib apps.
 * ========================================================================== */
#include "sval.h"
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>       /* snprintf for scalar formatting */
#include <stdint.h>

/* VFS_DT_FIFO / VFS_DT_CHAR as newlib's fstat reports them. The kernel maps
 * its vfs_stat.type into st_mode; a pipe is S_IFIFO. We check S_ISFIFO rather
 * than a raw EmbLink type constant so this rides on the standard stat shape
 * the newlib _fstat stub already fills. */

/* ==========================================================================
 * Mode detection
 * ========================================================================== */
static bool fd_is_fifo(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return false;   /* EBADF (unopened fd 3) -> not structured */
    return S_ISFIFO(st.st_mode);
}
bool sval_structured_out(void) { return fd_is_fifo(SVAL_FD_OUT); }
bool sval_structured_in(void)  { return fd_is_fifo(SVAL_FD_IN); }

/* ==========================================================================
 * Emit
 * ========================================================================== */
/* Write every byte or fail. A frame is all-or-nothing: a partial one is a
 * protocol error at the far end ("corrupt structured output"), so a write
 * that stops early must never be mistaken for success.
 *
 * Only EPIPE is terminal (the reader hung up -- a normal end of a pipeline).
 * Anything else is treated as transient and retried: the kernel's user-copy
 * can still surface a spurious fault, and giving up on one would truncate an
 * otherwise-fine frame. Bounded so a genuinely broken fd can't spin forever
 * -- a real EPIPE exits immediately, so the cap only bites on the pathological
 * case. (Mirrors the retry/EPIPE split in the shell's extern feed loop.) */
#define WRITE_ALL_RETRIES 64

static int write_all(int fd, const uint8_t *p, size_t n) {
    size_t off = 0;
    int stalls = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            stalls = 0;                      /* progress: reset the budget */
            continue;
        }
        if (w < 0 && errno == EPIPE) {
            return -1;                       /* reader gone: terminal, stop now */
        }
        if (++stalls > WRITE_ALL_RETRIES) {
            return -1;                       /* not making progress; give up */
        }
        /* EINTR, a transient fault, or a 0-byte return: retry this offset. */
    }
    return 0;
}

int sval_emit_fd(int fd, const struct value *v) {
    struct wire_buf frame;
    wire_buf_init(&frame);
    if (wire_serialize(v, &frame)) { wire_buf_free(&frame); return -1; }
    int rc = write_all(fd, frame.data, frame.len);
    wire_buf_free(&frame);
    return rc;
}
int sval_emit(const struct value *v) { return sval_emit_fd(SVAL_FD_OUT, v); }

/* ==========================================================================
 * Reader
 * ========================================================================== */
void sval_reader_init(struct sval_reader *r) { wire_reader_init(&r->wr); }
void sval_reader_free(struct sval_reader *r) { wire_reader_free(&r->wr); }

int sval_reader_feed(struct sval_reader *r, const uint8_t *data, size_t n) {
    return wire_reader_feed(&r->wr, data, n);
}
int sval_reader_next(struct sval_reader *r, struct value *out) {
    return wire_reader_next(&r->wr, out);
}

int sval_read_blocking(struct sval_reader *r, int fd, struct value *out) {
    /* First, maybe a whole value is already buffered from a previous read that
     * delivered more than one frame -- drain that before touching the fd. */
    int rc = sval_reader_next(r, out);
    if (rc != 0) return rc;   /* 1 = value, <0 = protocol error */

    uint8_t tmp[512];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;                       /* read error */
        }
        if (n == 0) return 0;                /* EOF -- pipe's writers all closed */
        if (sval_reader_feed(r, tmp, (size_t)n)) return -1;   /* OOM */
        rc = sval_reader_next(r, out);
        if (rc != 0) return rc;              /* 1 = value, <0 = protocol error */
        /* rc == 0: partial frame, loop and read more */
    }
}

/* ==========================================================================
 * Human rendering
 * ========================================================================== */

/* Render a filesize as a human unit. Binary-ish (1024) since it's disk/memory
 * sizes; picks the largest unit that keeps the number >= 1. */
static size_t format_filesize(int64_t bytes, char *buf, size_t cap) {
    const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0) return (size_t)snprintf(buf, cap, "%lld B", (long long)bytes);
    return (size_t)snprintf(buf, cap, "%.1f %s", v, units[u]);
}

/* Render a DATE (epoch seconds) as a compact readable stamp. No timezone
 * handling and no libc time (freestanding-ish target) -- a plain UTC
 * decomposition. Good enough to READ; the shell isn't a calendar app. */
static size_t format_date(int64_t epoch, char *buf, size_t cap) {
    /* days since 1970-01-01, civil-from-days (Howard Hinnant's algorithm). */
    int64_t days = epoch / 86400;
    int64_t secs = epoch % 86400;
    if (secs < 0) { secs += 86400; days -= 1; }
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365*yoe + yoe/4 - yoe/100);
    int64_t mp = (5*doy + 2) / 153;
    int64_t d = doy - (153*mp + 2)/5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    int hh = (int)(secs / 3600), mm = (int)((secs % 3600) / 60), ss = (int)(secs % 60);
    return (size_t)snprintf(buf, cap, "%04lld-%02lld-%02lld %02d:%02d:%02d",
                            (long long)y, (long long)m, (long long)d, hh, mm, ss);
}

size_t sval_format_scalar(const struct value *v, char *buf, size_t cap) {
    if (cap == 0) return 0;
    switch (v->type) {
    case VAL_NULL:   buf[0] = '\0'; return 0;   /* empty cell (not snprintf(""):
                                                 * -Wformat-zero-length) */
    case VAL_INT:    return (size_t)snprintf(buf, cap, "%lld", (long long)v->u.i);
    case VAL_FLOAT:  return (size_t)snprintf(buf, cap, "%g", v->u.f);
    case VAL_BOOL:   return (size_t)snprintf(buf, cap, "%s", v->u.b ? "true" : "false");
    case VAL_FILESIZE: return format_filesize(v->u.i, buf, cap);
    case VAL_DATE:     return format_date(v->u.i, buf, cap);
    case VAL_STRING:
    case VAL_PATH: {
        size_t n = v->u.s.len < cap - 1 ? v->u.s.len : cap - 1;
        memcpy(buf, v->u.s.bytes, n);
        buf[n] = '\0';
        return n;
    }
    case VAL_LIST:   return (size_t)snprintf(buf, cap, "[list]");
    case VAL_RECORD: return (size_t)snprintf(buf, cap, "{record}");
    case VAL_TABLE:  return (size_t)snprintf(buf, cap, "[table]");
    case VAL_ERROR:  return (size_t)snprintf(buf, cap, "[error: %s]", v->u.s.bytes);
    default:         return (size_t)snprintf(buf, cap, "[unknown]");
    }
    return 0;
}

static int puts_fd(int fd, const char *s) {
    size_t n = strlen(s);
    return write_all(fd, (const uint8_t *)s, n);
}

/* Table renderer: two passes over the rows -- pass 1 measures the max text
 * width of each column (across all rows + the header name), pass 2 prints an
 * aligned grid. Columns are the union of the first row's field names (rows are
 * assumed homogeneous for rendering; a missing field prints blank). */
static int print_table(const struct table *t, int fd) {
    if (t->count == 0) { return puts_fd(fd, "(empty table)\n"); }

    struct record *hdr = &t->rows[0];
    size_t ncols = hdr->count;
    if (ncols == 0) { return puts_fd(fd, "(no columns)\n"); }

    /* column widths (capped so one pathological cell can't blow the layout) */
    #define CELL_MAX 40
    size_t *w = (size_t *)calloc(ncols, sizeof(size_t));
    if (!w) return -1;
    for (size_t c = 0; c < ncols; c++) w[c] = strlen(hdr->names[c]);

    char cell[CELL_MAX + 1];
    for (size_t r = 0; r < t->count; r++) {
        struct record *row = &t->rows[r];
        for (size_t c = 0; c < ncols; c++) {
            const struct value *cv = record_field(row, hdr->names[c]);
            size_t len = cv ? sval_format_scalar(cv, cell, sizeof cell) : 0;
            if (len > CELL_MAX) len = CELL_MAX;
            if (len > w[c]) w[c] = len;
        }
    }

    /* header */
    for (size_t c = 0; c < ncols; c++) {
        char pad[CELL_MAX + 2];
        int k = snprintf(pad, sizeof pad, "%-*s%s", (int)w[c], hdr->names[c],
                         c + 1 < ncols ? "  " : "");
        (void)k;
        if (puts_fd(fd, pad)) { free(w); return -1; }
    }
    puts_fd(fd, "\n");
    /* underline */
    for (size_t c = 0; c < ncols; c++) {
        for (size_t i = 0; i < w[c]; i++) puts_fd(fd, "-");
        if (c + 1 < ncols) puts_fd(fd, "  ");
    }
    puts_fd(fd, "\n");
    /* rows */
    for (size_t r = 0; r < t->count; r++) {
        struct record *row = &t->rows[r];
        for (size_t c = 0; c < ncols; c++) {
            const struct value *cv = record_field(row, hdr->names[c]);
            if (cv) sval_format_scalar(cv, cell, sizeof cell); else cell[0] = '\0';
            char pad[CELL_MAX + 4];
            snprintf(pad, sizeof pad, "%-*s%s", (int)w[c], cell, c + 1 < ncols ? "  " : "");
            if (puts_fd(fd, pad)) { free(w); return -1; }
        }
        puts_fd(fd, "\n");
    }
    free(w);
    return 0;
}

int sval_print(const struct value *v, int fd) {
    char cell[128];
    switch (v->type) {
    case VAL_LIST:
        for (size_t i = 0; i < v->u.list.count; i++) {
            if (sval_print(&v->u.list.items[i], fd)) return -1;   /* one per line;
                                                                   * scalars add \n below */
        }
        return 0;
    case VAL_RECORD: {
        struct record *r = v->u.record;
        for (size_t i = 0; i < r->count; i++) {
            char line[256];
            sval_format_scalar(&r->values[i], cell, sizeof cell);
            snprintf(line, sizeof line, "%s: %s\n", r->names[i], cell);
            if (puts_fd(fd, line)) return -1;
        }
        return 0;
    }
    case VAL_TABLE:
        return print_table(v->u.table, fd);
    default:
        sval_format_scalar(v, cell, sizeof cell);
        if (puts_fd(fd, cell)) return -1;
        return puts_fd(fd, "\n");
    }
}