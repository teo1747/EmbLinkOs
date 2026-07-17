/* ==========================================================================
 * wire.h -- serialize a Value to bytes and back, for transport over fd 3.
 *
 * The format (locked decisions):
 *   - Every emitted value is ONE FRAME: [varint frame_len][payload].
 *   - Inside a frame, the value is TLV: [u8 type][type-specific body].
 *   - LENGTHS/COUNTS (string len, list count, record field count): plain
 *     unsigned varint (LEB128) -- always >= 0, usually small.
 *   - SIGNED int payloads (INT, FILESIZE, DATE): zigzag + varint, so a small
 *     negative stays ~1 byte instead of the 10 a raw-unsigned varint of a
 *     negative int64 would cost.
 *   - DOUBLE (FLOAT): fixed 8 bytes, little-endian. Varint is meaningless for
 *     a mantissa/exponent bit pattern.
 *   - Everything multi-byte is EXPLICIT little-endian, never a native memcpy
 *     -- so the format survives a file round-trip, an ARM64 port, and a
 *     hexdump-the-pipe debugging session.
 *
 * Serialization is structurally the SAME recursive walk as value_clone(): a
 * depth-first descent producing an independent copy, just into a byte buffer
 * instead of fresh heap structs. That symmetry is the whole point of value
 * semantics -- the in-process path and the wire path are one shape.
 * ========================================================================== */
#ifndef __EMBK_WIRE_H__
#define __EMBK_WIRE_H__

#include "value/value.h"
#include <stdint.h>
#include <stddef.h>

/* On-wire type tags. STABLE: these are the format, never renumber -- a change
 * silently breaks any peer built against the old numbers. New types append. */
enum wire_tag {
    WT_NULL     = 0,
    WT_INT      = 1,
    WT_FLOAT    = 2,
    WT_BOOL     = 3,
    WT_STRING   = 4,
    WT_LIST     = 5,
    WT_RECORD   = 6,
    WT_TABLE    = 7,
    WT_FILESIZE = 8,
    WT_DATE     = 9,
    WT_PATH     = 10,
};

/* A frame larger than this is rejected rather than allocated -- a malformed or
 * hostile stream must not be able to make us try to malloc gigabytes off a
 * varint that claims a huge length. Far above any real shell message. */
#define WIRE_FRAME_MAX (16u * 1024u * 1024u)

/* -------------------------------------------------------------------------
 * A growable output buffer the serializer appends into. Caller owns `data`
 * and frees it with wire_buf_free(). Kept dead simple -- the whole thing is
 * "append bytes, grow when full".
 * ------------------------------------------------------------------------- */
struct wire_buf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
};
void wire_buf_init(struct wire_buf *b);
void wire_buf_free(struct wire_buf *b);

/* -------------------------------------------------------------------------
 * Serialize: value -> a single complete FRAME (length prefix + payload) in `out`.
 * Returns 0 on success, negative on OOM. `out` must be a fresh/empty wire_buf;
 * on success out->data / out->len are a ready-to-write() frame.
 * ------------------------------------------------------------------------- */
int wire_serialize(const struct value *v, struct wire_buf *out);

/* -------------------------------------------------------------------------
 * Deserialize: a complete frame's PAYLOAD bytes -> a Value. `payload`/`len`
 * are exactly the bytes after the frame-length prefix (the frame reader
 * below strips that). Returns 0 and fills *out on success; negative on a
 * malformed/truncated payload (out is left VAL_NULL). The caller owns *out.
 * ------------------------------------------------------------------------- */
int wire_deserialize(const uint8_t *payload, size_t len, struct value *out);

/* -------------------------------------------------------------------------
 * The incremental frame reader -- the ONLY part that knows about pipe
 * read()-boundaries. A value's frame is [varint len][payload], and a varint's
 * width isn't known until its terminating byte, so reading a frame off a byte
 * stream is a small state machine, not "read N then M". This buffers partial
 * input across calls and hands back exactly one complete frame's payload when
 * enough bytes have arrived.
 *
 * Usage:
 *   struct wire_reader r; wire_reader_init(&r);
 *   for (;;) {
 *       ssize_t n = read(fd3, tmp, sizeof tmp);
 *       if (n <= 0) break;                       // EOF or error
 *       wire_reader_feed(&r, tmp, n);            // hand raw bytes in
 *       struct value v;
 *       while (wire_reader_next(&r, &v) == 1) {  // 1 = a complete value came out
 *           ... use v ...  value_free(&v);
 *       }
 *   }
 *   wire_reader_free(&r);
 * ------------------------------------------------------------------------- */
struct wire_reader {
    uint8_t *buf;       /* accumulated unconsumed bytes */
    size_t   len, cap;
    size_t   consumed;  /* bytes at the front already turned into values;
                         * compacted away lazily (see wire_reader_next) */
};
void wire_reader_init(struct wire_reader *r);
void wire_reader_free(struct wire_reader *r);

/* Append raw stream bytes. Returns 0, or negative on OOM. */
int  wire_reader_feed(struct wire_reader *r, const uint8_t *data, size_t n);

/* Try to pull ONE complete value out of the buffered bytes.
 *   returns 1  -> *out is a complete value (caller frees it)
 *   returns 0  -> not enough bytes yet; feed more and retry
 *   returns <0 -> protocol error (frame too large, malformed payload):
 *                 the stream is unrecoverable, the caller should stop.
 */
int  wire_reader_next(struct wire_reader *r, struct value *out);

#endif /* __EMBK_WIRE_H__ */