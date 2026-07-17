/* ==========================================================================
 * wire.c -- see wire.h for the format. Three layers:
 *   1. primitive codec: varint (unsigned), zigzag+varint (signed), LE double
 *   2. value serialize/deserialize: the clone-shaped recursive walk
 *   3. wire_reader: the incremental frame decoder that handles pipe boundaries
 * ========================================================================== */
#include "wire.h"
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Layer 1: primitive codec
 * ========================================================================== */

/* --- output buffer --- */
void wire_buf_init(struct wire_buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
void wire_buf_free(struct wire_buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static int buf_reserve(struct wire_buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 32;
    while (nc < b->len + extra) nc *= 2;
    uint8_t *nd = (uint8_t *)realloc(b->data, nc);
    if (!nd) return -1;
    b->data = nd; b->cap = nc;
    return 0;
}
static int buf_put_u8(struct wire_buf *b, uint8_t x) {
    if (buf_reserve(b, 1)) return -1;
    b->data[b->len++] = x;
    return 0;
}
static int buf_put_bytes(struct wire_buf *b, const void *p, size_t n) {
    if (buf_reserve(b, n)) return -1;
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

/* --- unsigned varint (LEB128): 7 bits/byte, high bit = "more follows" --- */
static int buf_put_uvarint(struct wire_buf *b, uint64_t x) {
    do {
        uint8_t byte = x & 0x7F;
        x >>= 7;
        if (x) byte |= 0x80;
        if (buf_put_u8(b, byte)) return -1;
    } while (x);
    return 0;
}

/* --- zigzag: map signed -> unsigned so small |n| stays small.
 *     0->0, -1->1, 1->2, -2->3, 2->4, ...  Arithmetic shift is required
 *     for the sign replication; on x86_64 (>>  of a signed int64) that's an
 *     arithmetic shift, which is what we want. --- */
static uint64_t zigzag_encode(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); }
static int64_t  zigzag_decode(uint64_t u) { return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); }

static int buf_put_svarint(struct wire_buf *b, int64_t v) {
    return buf_put_uvarint(b, zigzag_encode(v));
}

/* --- double: fixed 8 bytes, explicit little-endian.
 *     Reinterpret the bit pattern as u64 via memcpy (NOT a union/cast --
 *     memcpy is the strict-aliasing-safe reinterpret), then emit LE. --- */
static int buf_put_double(struct wire_buf *b, double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    uint8_t le[8];
    for (int i = 0; i < 8; i++) { le[i] = (uint8_t)(bits & 0xFF); bits >>= 8; }
    return buf_put_bytes(b, le, 8);
}

/* --- decode side: a cursor over a fixed byte range. Every read is
 *     bounds-checked; a truncated/malformed input fails cleanly rather than
 *     reading past the buffer. --- */
struct rd { const uint8_t *p; size_t len, pos; };

static int rd_u8(struct rd *r, uint8_t *out) {
    if (r->pos >= r->len) return -1;
    *out = r->p[r->pos++];
    return 0;
}
static int rd_uvarint(struct rd *r, uint64_t *out) {
    uint64_t v = 0; int shift = 0;
    for (;;) {
        if (shift >= 64) return -1;           /* overflow: >9 continuation bytes */
        uint8_t byte;
        if (rd_u8(r, &byte)) return -1;        /* truncated */
        v |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    *out = v;
    return 0;
}
static int rd_svarint(struct rd *r, int64_t *out) {
    uint64_t u;
    if (rd_uvarint(r, &u)) return -1;
    *out = zigzag_decode(u);
    return 0;
}
static int rd_double(struct rd *r, double *out) {
    if (r->pos + 8 > r->len) return -1;
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++) bits |= (uint64_t)r->p[r->pos++] << (i * 8);
    memcpy(out, &bits, 8);
    return 0;
}
static int rd_bytes(struct rd *r, const uint8_t **out, size_t n) {
    if (r->pos + n > r->len) return -1;        /* truncated */
    *out = r->p + r->pos;
    r->pos += n;
    return 0;
}

/* ==========================================================================
 * Layer 2: value <-> payload (the recursive walk, clone-shaped)
 * ========================================================================== */

static int ser_value(const struct value *v, struct wire_buf *b);   /* fwd */

static int ser_bytes_field(struct wire_buf *b, uint8_t tag, const char *bytes, size_t len) {
    if (buf_put_u8(b, tag)) return -1;
    if (buf_put_uvarint(b, len)) return -1;
    return buf_put_bytes(b, bytes, len);
}

static int ser_value(const struct value *v, struct wire_buf *b) {
    switch (v->type) {
    case VAL_NULL:  return buf_put_u8(b, WT_NULL);
    case VAL_INT:      if (buf_put_u8(b, WT_INT))      return -1; return buf_put_svarint(b, v->u.i);
    case VAL_FILESIZE: if (buf_put_u8(b, WT_FILESIZE)) return -1; return buf_put_svarint(b, v->u.i);
    case VAL_DATE:     if (buf_put_u8(b, WT_DATE))     return -1; return buf_put_svarint(b, v->u.i);
    case VAL_FLOAT:    if (buf_put_u8(b, WT_FLOAT))    return -1; return buf_put_double(b, v->u.f);
    case VAL_BOOL:     if (buf_put_u8(b, WT_BOOL))     return -1; return buf_put_u8(b, v->u.b ? 1 : 0);
    case VAL_STRING:   return ser_bytes_field(b, WT_STRING, v->u.s.bytes, v->u.s.len);
    case VAL_PATH:     return ser_bytes_field(b, WT_PATH,   v->u.s.bytes, v->u.s.len);
    case VAL_LIST:
        if (buf_put_u8(b, WT_LIST)) return -1;
        if (buf_put_uvarint(b, v->u.list.count)) return -1;
        for (size_t i = 0; i < v->u.list.count; i++)
            if (ser_value(&v->u.list.items[i], b)) return -1;
        return 0;
    case VAL_RECORD: {
        if (buf_put_u8(b, WT_RECORD)) return -1;
        struct record *r = v->u.record;
        if (buf_put_uvarint(b, r->count)) return -1;
        for (size_t i = 0; i < r->count; i++) {
            size_t nlen = strlen(r->names[i]);
            if (buf_put_uvarint(b, nlen)) return -1;         /* field name */
            if (buf_put_bytes(b, r->names[i], nlen)) return -1;
            if (ser_value(&r->values[i], b)) return -1;      /* field value */
        }
        return 0;
    }
    case VAL_TABLE: {
        if (buf_put_u8(b, WT_TABLE)) return -1;
        struct table *t = v->u.table;
        if (buf_put_uvarint(b, t->count)) return -1;
        for (size_t i = 0; i < t->count; i++) {
            /* a row is a record; serialize its {count, (name,value)*} inline.
             * We don't wrap it in a WT_RECORD tag -- context already says
             * "these are rows" -- but the SHAPE matches a record body so the
             * deserializer reuses the same row reader. */
            struct record *r = &t->rows[i];
            if (buf_put_uvarint(b, r->count)) return -1;
            for (size_t j = 0; j < r->count; j++) {
                size_t nlen = strlen(r->names[j]);
                if (buf_put_uvarint(b, nlen)) return -1;
                if (buf_put_bytes(b, r->names[j], nlen)) return -1;
                if (ser_value(&r->values[j], b)) return -1;
            }
        }
        return 0;
    }
    case VAL_ERROR:
        return -1;   /* errors abort the pipeline BEFORE the sink; one crossing
                      * the wire means a shell bug -- reject, don't invent a tag */
    }
    return -1;   /* unknown type */
}

int wire_serialize(const struct value *v, struct wire_buf *out) {
    /* Serialize the payload into a scratch buffer first, THEN prepend the
     * frame length -- we can't know the frame's length until it's built, and
     * a varint length can't be back-patched in place (its width depends on
     * the value). So: build payload, then build final = [varint len][payload].
     */
    struct wire_buf payload; wire_buf_init(&payload);
    if (ser_value(v, &payload)) { wire_buf_free(&payload); return -1; }

    if (buf_put_uvarint(out, payload.len)) { wire_buf_free(&payload); return -1; }
    if (buf_put_bytes(out, payload.data, payload.len)) { wire_buf_free(&payload); return -1; }
    wire_buf_free(&payload);
    return 0;
}

/* --- deserialize --- */
static int de_value(struct rd *r, struct value *out);   /* fwd */

/* Read one record BODY: [uvarint count]({name}{value})*  -- shared by
 * WT_RECORD and each table row (identical shape, see serializer note). */
static int de_record_body(struct rd *r, struct value *out) {
    uint64_t count;
    if (rd_uvarint(r, &count)) return -1;
    struct value rec = value_record();
    if (rec.type != VAL_RECORD) return -1;          /* OOM */
    for (uint64_t i = 0; i < count; i++) {
        uint64_t nlen;
        if (rd_uvarint(r, &nlen)) { value_free(&rec); return -1; }
        const uint8_t *nbytes;
        if (rd_bytes(r, &nbytes, nlen)) { value_free(&rec); return -1; }
        /* names need NUL-termination for value_record_set's strcmp/strlen;
         * copy into a small stack/heap buffer. Field names are short. */
        char *name = (char *)malloc(nlen + 1);
        if (!name) { value_free(&rec); return -1; }
        if (nlen) memcpy(name, nbytes, nlen);
        name[nlen] = '\0';
        struct value fv;
        if (de_value(r, &fv)) { free(name); value_free(&rec); return -1; }
        value_record_set(&rec, name, fv);            /* MOVES fv in */
        free(name);
    }
    *out = rec;
    return 0;
}

static int de_value(struct rd *r, struct value *out) {
    uint8_t tag;
    if (rd_u8(r, &tag)) return -1;
    switch (tag) {
    case WT_NULL:  *out = value_null();  return 0;
    case WT_BOOL: { uint8_t b; if (rd_u8(r, &b)) return -1; *out = value_bool(b != 0); return 0; }
    case WT_INT:      { int64_t i; if (rd_svarint(r, &i)) return -1; *out = value_int(i);      return 0; }
    case WT_FILESIZE: { int64_t i; if (rd_svarint(r, &i)) return -1; *out = value_filesize(i); return 0; }
    case WT_DATE:     { int64_t i; if (rd_svarint(r, &i)) return -1; *out = value_date(i);     return 0; }
    case WT_FLOAT:    { double d;  if (rd_double(r, &d))  return -1; *out = value_float(d);    return 0; }
    case WT_STRING:
    case WT_PATH: {
        uint64_t len;
        if (rd_uvarint(r, &len)) return -1;
        const uint8_t *bytes;
        if (rd_bytes(r, &bytes, len)) return -1;
        *out = (tag == WT_STRING) ? value_string_n((const char *)bytes, len)
                                  : value_path_n((const char *)bytes, len);
        return (out->type == VAL_NULL && len) ? -1 : 0;   /* OOM guard */
    }
    case WT_LIST: {
        uint64_t count;
        if (rd_uvarint(r, &count)) return -1;
        struct value list = value_list();
        for (uint64_t i = 0; i < count; i++) {
            struct value item;
            if (de_value(r, &item)) { value_free(&list); return -1; }
            value_list_push(&list, item);
        }
        *out = list;
        return 0;
    }
    case WT_RECORD:
        return de_record_body(r, out);
    case WT_TABLE: {
        uint64_t rows;
        if (rd_uvarint(r, &rows)) return -1;
        struct value tbl = value_table();
        if (tbl.type != VAL_TABLE) return -1;
        for (uint64_t i = 0; i < rows; i++) {
            struct value row;
            if (de_record_body(r, &row)) { value_free(&tbl); return -1; }   /* row = a record */
            value_table_push_row(&tbl, row);
        }
        *out = tbl;
        return 0;
    }
    default:
        return -1;   /* unknown tag -- reject rather than guess */
    }
}

int wire_deserialize(const uint8_t *payload, size_t len, struct value *out) {
    *out = value_null();
    struct rd r = { payload, len, 0 };
    struct value v;
    if (de_value(&r, &v)) return -1;
    if (r.pos != len) { value_free(&v); return -1; }   /* trailing garbage in the
                                                        * frame -> reject: a frame
                                                        * holds EXACTLY one value */
    *out = v;
    return 0;
}

/* ==========================================================================
 * Layer 3: wire_reader -- incremental frame decode across pipe boundaries
 * ========================================================================== */

void wire_reader_init(struct wire_reader *r) { r->buf = NULL; r->len = 0; r->cap = 0; r->consumed = 0; }
void wire_reader_free(struct wire_reader *r) { free(r->buf); r->buf = NULL; r->len = r->cap = r->consumed = 0; }

int wire_reader_feed(struct wire_reader *r, const uint8_t *data, size_t n) {
    /* Compact consumed prefix first so the buffer doesn't grow unboundedly
     * across a long-lived stream of many small frames. */
    if (r->consumed) {
        memmove(r->buf, r->buf + r->consumed, r->len - r->consumed);
        r->len -= r->consumed;
        r->consumed = 0;
    }
    if (r->len + n > r->cap) {
        size_t nc = r->cap ? r->cap : 256;
        while (nc < r->len + n) nc *= 2;
        uint8_t *nb = (uint8_t *)realloc(r->buf, nc);
        if (!nb) return -1;
        r->buf = nb; r->cap = nc;
    }
    memcpy(r->buf + r->len, data, n);
    r->len += n;
    return 0;
}

int wire_reader_next(struct wire_reader *r, struct value *out) {
    const uint8_t *p = r->buf + r->consumed;
    size_t avail = r->len - r->consumed;

    /* 1. Decode the frame-length varint -- but ONLY if the full varint has
     *    arrived. A varint's width isn't known until its terminating byte, so
     *    scan for high-bit-clear within `avail`; if we run out first, we don't
     *    have the length yet -> return 0 (need more bytes). */
    uint64_t frame_len = 0;
    int shift = 0;
    size_t hdr = 0;
    for (;;) {
        if (hdr >= avail) return 0;               /* length varint incomplete */
        if (shift >= 64) return -1;               /* malformed: absurd varint */
        uint8_t byte = p[hdr++];
        frame_len |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }

    if (frame_len > WIRE_FRAME_MAX) return -1;     /* refuse to allocate a hostile size */

    /* 2. Do we have the whole payload yet? */
    if (avail - hdr < frame_len) return 0;         /* payload incomplete -> more bytes */

    /* 3. Parse exactly frame_len payload bytes into a value. */
    int rc = wire_deserialize(p + hdr, (size_t)frame_len, out);
    if (rc) return -1;                              /* malformed payload */

    r->consumed += hdr + (size_t)frame_len;        /* advance past this frame */
    return 1;
}