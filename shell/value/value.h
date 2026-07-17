/* ==========================================================================
 * value.h -- the structured shell's value type.
 *
 * Value semantics throughout: every Value OWNS its data. Operations that
 * "keep" a value clone it; nothing is ever shared. So value_free() is a clean
 * recursive walk that can never double-free, and serialization (value ->
 * bytes -> value across fd 3) has the SAME ownership shape as an in-process
 * clone -- the wire path and the in-process path are the same walk.
 *
 * This is a DELIBERATE non-use of refcounting: a shell pipeline's data is
 * small (a dir listing, a process table -- hundreds of rows), so the copy
 * cost is irrelevant, and in exchange there are zero aliasing/free-order
 * bugs. (Refcounting would be right for a gigabyte dataframe engine; it is
 * not right here.)
 * ========================================================================== */
#ifndef __EMBK_VALUE_H__
#define __EMBK_VALUE_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

enum value_type {
    VAL_NULL = 0,   /* the absence of a value (an empty cell, a missing field) */
    VAL_INT,        /* int64  */
    VAL_FLOAT,      /* double */
    VAL_BOOL,
    VAL_STRING,     /* owned, length-prefixed bytes (NOT necessarily NUL-terminated
                     * internally -- may hold arbitrary text; len is authoritative) */
    VAL_LIST,       /* ordered sequence of values, any mix of types */
    VAL_RECORD,     /* named fields; a row */
    VAL_TABLE,      /* a sequence of RECORD rows, rendered as a grid.
                     * Storage-wise almost a List-of-Records, but a DISTINCT
                     * type: "this is tabular" vs "this is a list that happens
                     * to hold records" is a real semantic difference the
                     * renderer and where/select care about. */

    /* --- richer types: these are what make `ls | where size > 1mb` work
     * WITHOUT string-parsing hacks. Each carries a primitive payload plus a
     * type tag that changes how it PRINTS and COMPARES, not how it's stored. */
    VAL_FILESIZE,   /* int64 bytes; prints "1.5 MB", compares numerically */
    VAL_DATE,       /* int64 epoch seconds; prints human-readable, compares numerically */
    VAL_PATH,       /* owned string bytes; prints as a path, semantically a
                     * filesystem path (so a future command could resolve it) */
    VAL_ERROR,      /* owned message string, reuses the .s arm */
};

struct value;
struct record;
struct table;

struct value {
    enum value_type type;
    union {
        int64_t i;      /* INT, FILESIZE (bytes), DATE (epoch seconds) */
        double  f;      /* FLOAT */
        bool    b;      /* BOOL */
        struct { char *bytes; size_t len; } s;   /* STRING, PATH (owned) */
        struct { struct value *items; size_t count; size_t cap; } list;  /* LIST */
        struct record *record;   /* RECORD (owned) */
        struct table  *table;    /* TABLE  (owned) */
    } u;
};


/* --------------------------------------------------------------------------
 * Error values: a Value that is an error message. The evaluator can return
 * these to the shell, which prints them and aborts the pipeline. (The shell
 * never sees a NULL pointer -- errors are always a Value, so the pipeline
 * can be aborted cleanly without a segfault.) */
struct value value_error(const char *msg);       /* construct */
struct value value_error_n(const char *m, size_t n);
bool         value_is_error(const struct value *v);
const char  *value_error_msg(const struct value *v);   /* borrowed; "" if not an error */

/* A record: parallel arrays of owned field-name strings and owned values.
 * Parallel arrays (not an array of {name,value} pairs) purely so a future
 * "project these columns" can memcpy contiguous ranges; at shell scale it's
 * a wash, chosen for cache-friendliness of the common full-row walk. */
struct record {
    char        **names;   /* names[i]: owned NUL-terminated field name */
    struct value *values;  /* values[i]: owned value */
    size_t        count;
    size_t        cap;
};

/* A table: rows, each a full self-describing Record (see header rationale --
 * rows own their own field names, so a row survives extraction from its
 * table and heterogeneous rows are representable). */
struct table {
    struct record *rows;   /* each row is a full Record, owned */
    size_t         count;
    size_t         cap;
};

/* -------- construction (all return a Value BY VALUE; caller owns it) -------- */
struct value value_null(void);
struct value value_int(int64_t i);
struct value value_float(double f);
struct value value_bool(bool b);
struct value value_string(const char *s);              /* copies s (NUL-terminated source) */
struct value value_string_n(const char *s, size_t n);  /* copies n bytes */
struct value value_filesize(int64_t bytes);
struct value value_date(int64_t epoch_seconds);
struct value value_path(const char *p);
struct value value_path_n(const char *p, size_t n);     /* copies n bytes */

struct value value_list(void);                          /* empty list */
void         value_list_push(struct value *list, struct value v);  /* MOVES v in (takes ownership) */

struct value value_record(void);                        /* empty record */
void         value_record_set(struct value *rec, const char *name, struct value v);
                    /* MOVES v in. If `name` already exists, frees the old
                     * value and replaces it (a record has unique field names). */
const struct value *value_record_get(const struct value *rec, const char *name);
                    /* borrowed pointer into rec, or NULL. Does NOT clone --
                     * read-only peek; clone it yourself if you need to keep it. */
const struct value *record_field(const struct record *r, const char *name);
                    /* same peek on a bare struct record* (a table row is stored
                     * by value, unwrapped) -- the renderer + evaluator's shape. */

struct value value_table(void);                         /* empty table */
void         value_table_push_row(struct value *table, struct value record_row);
                    /* MOVES the record in as a new row. record_row MUST be a
                     * VAL_RECORD (asserted; a table holds only records). */

/* -------- the three operations value semantics require -------- */

/* Deep copy: the returned value shares NOTHING with `v`. This is what
 * "operations copy" is built on -- where/select/etc. clone the rows they keep.
 * Also structurally identical to what serialization does, one address space over. */
struct value value_clone(const struct value *v);

/* Deep-copy a bare table row (rows are stored by value, unwrapped) into an
 * owned VAL_RECORD -- how where/select/first carry kept rows into their
 * output table. */
struct value record_clone_value(const struct record *r);

/* Recursive free. Safe on any Value including nested lists/records/tables.
 * After this, *v is left as VAL_NULL (idempotent: freeing twice is a no-op,
 * a cheap guard against the double-free that value semantics already makes
 * structurally impossible but which a buggy CALLER could still attempt). */
void value_free(struct value *v);

/* Deep structural equality -- needed for `where field == x`. Two values are
 * equal iff same type AND same payload, recursively. Cross-type is NEVER
 * equal here (VAL_INT 1 != VAL_FLOAT 1.0) -- coercion is the EVALUATOR's job,
 * not the value layer's, so this stays a pure structural check. See §note. */
bool value_equal(const struct value *a, const struct value *b);

/* Ordering for `where size > 1mb` and `sort`. Returns <0, 0, >0. Defined only
 * for MEANINGFULLY-ORDERED same-type pairs (numbers incl. filesize/date;
 * strings/paths lexicographically). Returns VALUE_CMP_UNORDERED for pairs
 * with no natural order (two records, int vs string, ...) -- the evaluator
 * decides what to do with "unorderable" rather than this layer inventing one. */
#define VALUE_CMP_UNORDERED 0x7FFFFFFF
int value_compare(const struct value *a, const struct value *b);

#endif /* __EMBK_VALUE_H__ */