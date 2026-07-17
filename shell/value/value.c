/* ==========================================================================
 * value.c -- implementation of the structured shell's value type.
 * See value.h for the ownership model (value semantics; every Value owns its
 * data; operations copy; free is a clean recursive walk).
 *
 * Freestanding-friendly: depends only on malloc/free/realloc, memcpy, strlen,
 * strcmp (all present in the newlib port). No stdio here -- printing is a
 * separate concern (the renderer), not the value layer's job.
 * ========================================================================== */
#include "value.h"
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Small growable-array helper. Records/lists/tables all grow the same way:
 * double the capacity (from a floor of 4) when full. Centralised so the grow
 * policy lives in one place.
 * -------------------------------------------------------------------------- */
static size_t grow_cap(size_t cap) { return cap < 4 ? 4 : cap * 2; }

/* --------------------------------------------------------------------------
 * Construction
 * -------------------------------------------------------------------------- */
struct value value_null(void)          { struct value v; v.type = VAL_NULL; return v; }
struct value value_int(int64_t i)      { struct value v; v.type = VAL_INT;  v.u.i = i; return v; }
struct value value_float(double f)     { struct value v; v.type = VAL_FLOAT; v.u.f = f; return v; }
struct value value_bool(bool b)        { struct value v; v.type = VAL_BOOL; v.u.b = b; return v; }
struct value value_filesize(int64_t n) { struct value v; v.type = VAL_FILESIZE; v.u.i = n; return v; }
struct value value_date(int64_t s)     { struct value v; v.type = VAL_DATE; v.u.i = s; return v; }

/* STRING/PATH/ERROR share the owned-bytes representation; only the tag
 * differs. (Defined BEFORE the error constructors below that call it --
 * an implicit declaration of a struct-returning function is a compile
 * error, not just a warning.) */
static struct value make_bytes(enum value_type t, const char *s, size_t n) {
    struct value v; v.type = t;
    v.u.s.bytes = (char *)malloc(n + 1);   /* +1: keep a NUL so callers that
                                            * treat it as a C-string are safe,
                                            * even though `len` is authoritative */
    if (!v.u.s.bytes) { v.type = VAL_NULL; return v; }   /* OOM -> NULL value,
                                                          * never a half-built one */
    if (n) memcpy(v.u.s.bytes, s, n);
    v.u.s.bytes[n] = '\0';
    v.u.s.len = n;
    return v;
}
struct value value_string(const char *s)             { return make_bytes(VAL_STRING, s, s ? strlen(s) : 0); }
struct value value_string_n(const char *s, size_t n) { return make_bytes(VAL_STRING, s, n); }
struct value value_path(const char *p)               { return make_bytes(VAL_PATH,   p, p ? strlen(p) : 0); }
struct value value_path_n(const char *p, size_t n)   { return make_bytes(VAL_PATH,   p, n); }

struct value value_error(const char *msg)          { return make_bytes(VAL_ERROR, msg, msg ? strlen(msg) : 0); }
struct value value_error_n(const char *m, size_t n){ return make_bytes(VAL_ERROR, m, n); }
bool value_is_error(const struct value *v)         { return v->type == VAL_ERROR; }
const char *value_error_msg(const struct value *v) { return v->type == VAL_ERROR ? v->u.s.bytes : ""; }

struct value value_list(void) {
    struct value v; v.type = VAL_LIST;
    v.u.list.items = NULL; v.u.list.count = 0; v.u.list.cap = 0;
    return v;
}

void value_list_push(struct value *list, struct value item) {
    if (list->type != VAL_LIST) { value_free(&item); return; }  /* misuse -> don't leak item */
    if (list->u.list.count == list->u.list.cap) {
        size_t nc = grow_cap(list->u.list.cap);
        struct value *ni = (struct value *)realloc(list->u.list.items, nc * sizeof(struct value));
        if (!ni) { value_free(&item); return; }  /* OOM: drop the pushee, list intact */
        list->u.list.items = ni;
        list->u.list.cap = nc;
    }
    list->u.list.items[list->u.list.count++] = item;   /* MOVE: ownership transfers in */
}

/* --- records --- */
struct value value_record(void) {
    struct value v; v.type = VAL_RECORD;
    v.u.record = (struct record *)calloc(1, sizeof(struct record));
    if (!v.u.record) v.type = VAL_NULL;
    return v;
}

static int record_find(const struct record *r, const char *name) {
    for (size_t i = 0; i < r->count; i++)
        if (strcmp(r->names[i], name) == 0) return (int)i;
    return -1;
}

void value_record_set(struct value *rec, const char *name, struct value val) {
    if (rec->type != VAL_RECORD || !rec->u.record) { value_free(&val); return; }
    struct record *r = rec->u.record;

    int idx = record_find(r, name);
    if (idx >= 0) {                       /* replace existing field (unique names) */
        value_free(&r->values[idx]);      /* free the old value first -- no leak */
        r->values[idx] = val;             /* MOVE new one in */
        return;
    }

    if (r->count == r->cap) {
        size_t nc = grow_cap(r->cap);
        char **nn = (char **)realloc(r->names, nc * sizeof(char *));
        if (!nn) { value_free(&val); return; }
        r->names = nn;
        struct value *nv = (struct value *)realloc(r->values, nc * sizeof(struct value));
        if (!nv) { value_free(&val); return; }   /* names grew, values didn't:
                                                  * harmless -- count unchanged,
                                                  * the extra name cap is unused */
        r->values = nv;
        r->cap = nc;
    }
    size_t n = strlen(name);
    char *namecopy = (char *)malloc(n + 1);
    if (!namecopy) { value_free(&val); return; }
    memcpy(namecopy, name, n + 1);
    r->names[r->count]  = namecopy;
    r->values[r->count] = val;            /* MOVE */
    r->count++;
}

const struct value *value_record_get(const struct value *rec, const char *name) {
    if (rec->type != VAL_RECORD || !rec->u.record) return NULL;
    int idx = record_find(rec->u.record, name);
    return idx < 0 ? NULL : &rec->u.record->values[idx]; /* BORROWED, not cloned */
}

/* Same lookup, but on a bare struct record* -- what a table row is stored as
 * (rows live BY VALUE in the table, not wrapped in a struct value). The table
 * renderer and the evaluator's row scope both need this shape. */
const struct value *record_field(const struct record *r, const char *name) {
    if (!r) return NULL;
    int idx = record_find(r, name);
    return idx < 0 ? NULL : &r->values[idx];             /* BORROWED, not cloned */
}

/* --- tables --- */
struct value value_table(void) {
    struct value v; v.type = VAL_TABLE;
    v.u.table = (struct table *)calloc(1, sizeof(struct table));
    if (!v.u.table) v.type = VAL_NULL;
    return v;
}

void value_table_push_row(struct value *table, struct value row) {
    if (table->type != VAL_TABLE || !table->u.table || row.type != VAL_RECORD) {
        value_free(&row);   /* a table holds only records; misuse frees the row */
        return;
    }
    struct table *t = table->u.table;
    if (t->count == t->cap) {
        size_t nc = grow_cap(t->cap);
        struct record *nr = (struct record *)realloc(t->rows, nc * sizeof(struct record));
        if (!nr) { value_free(&row); return; }
        t->rows = nr;
        t->cap = nc;
    }
    /* The row's Value wrapper is VAL_RECORD holding a struct record*; the table
     * stores rows as struct record BY VALUE, so move the record contents out of
     * the wrapper and free the (now-empty) wrapper shell. */
    t->rows[t->count++] = *row.u.record;
    free(row.u.record);    /* the record STRUCT, not its contents (moved above) */
    row.type = VAL_NULL;   /* wrapper consumed */
}

/* --------------------------------------------------------------------------
 * value_free -- recursive, idempotent.
 * -------------------------------------------------------------------------- */
static void record_free_contents(struct record *r) {
    for (size_t i = 0; i < r->count; i++) {
        free(r->names[i]);
        value_free(&r->values[i]);
    }
    free(r->names);
    free(r->values);
    r->names = NULL; r->values = NULL; r->count = r->cap = 0;
}

void value_free(struct value *v) {
    switch (v->type) {
    case VAL_STRING:
    case VAL_PATH:
    case VAL_ERROR:   /* same owned-bytes arm as STRING (see value.h) */
        free(v->u.s.bytes);
        break;
    case VAL_LIST:
        for (size_t i = 0; i < v->u.list.count; i++)
            value_free(&v->u.list.items[i]);
        free(v->u.list.items);
        break;
    case VAL_RECORD:
        if (v->u.record) { record_free_contents(v->u.record); free(v->u.record); }
        break;
    case VAL_TABLE:
        if (v->u.table) {
            for (size_t i = 0; i < v->u.table->count; i++)
                record_free_contents(&v->u.table->rows[i]);
            free(v->u.table->rows);
            free(v->u.table);
        }
        break;
    default:   /* NULL/INT/FLOAT/BOOL/FILESIZE/DATE: nothing owned */
        break;
    }
    v->type = VAL_NULL;   /* idempotent: a second free is a no-op */
}

/* --------------------------------------------------------------------------
 * value_clone -- deep copy, shares nothing. The backbone of value semantics.
 * -------------------------------------------------------------------------- */
static struct record record_clone_contents(const struct record *r) {
    struct record out; out.names = NULL; out.values = NULL; out.count = 0; out.cap = 0;
    if (r->count == 0) return out;
    out.names  = (char **)malloc(r->count * sizeof(char *));
    out.values = (struct value *)malloc(r->count * sizeof(struct value));
    if (!out.names || !out.values) { free(out.names); free(out.values);
                                     out.names = NULL; out.values = NULL; return out; }
    out.cap = out.count = r->count;
    for (size_t i = 0; i < r->count; i++) {
        size_t n = strlen(r->names[i]);
        out.names[i] = (char *)malloc(n + 1);
        if (out.names[i]) memcpy(out.names[i], r->names[i], n + 1);
        out.values[i] = value_clone(&r->values[i]);
    }
    return out;
}

/* Wrap a bare table row (struct record, stored by value) as an owned
 * VAL_RECORD deep copy -- what where/select/first need to carry kept rows
 * into their output table. */
struct value record_clone_value(const struct record *r) {
    struct value out;
    out.type = VAL_RECORD;
    out.u.record = (struct record *)malloc(sizeof(struct record));
    if (!out.u.record) return value_null();
    *out.u.record = record_clone_contents(r);
    return out;
}

struct value value_clone(const struct value *v) {
    switch (v->type) {
    case VAL_NULL:  return value_null();
    case VAL_INT:   return value_int(v->u.i);
    case VAL_FLOAT: return value_float(v->u.f);
    case VAL_BOOL:  return value_bool(v->u.b);
    case VAL_FILESIZE: return value_filesize(v->u.i);
    case VAL_DATE:  return value_date(v->u.i);
    case VAL_STRING: return value_string_n(v->u.s.bytes, v->u.s.len);
    case VAL_PATH:   return make_bytes(VAL_PATH,  v->u.s.bytes, v->u.s.len);
    case VAL_ERROR:  return make_bytes(VAL_ERROR, v->u.s.bytes, v->u.s.len);
    case VAL_LIST: {
        struct value out = value_list();
        for (size_t i = 0; i < v->u.list.count; i++) {
            struct value c = value_clone(&v->u.list.items[i]);
            value_list_push(&out, c);
        }
        return out;
    }
    case VAL_RECORD: {
        struct value out; out.type = VAL_RECORD;
        out.u.record = (struct record *)malloc(sizeof(struct record));
        if (!out.u.record) return value_null();
        *out.u.record = record_clone_contents(v->u.record);
        return out;
    }
    case VAL_TABLE: {
        struct value out; out.type = VAL_TABLE;
        out.u.table = (struct table *)calloc(1, sizeof(struct table));
        if (!out.u.table) return value_null();
        struct table *t = v->u.table;
        if (t->count) {
            out.u.table->rows = (struct record *)malloc(t->count * sizeof(struct record));
            if (!out.u.table->rows) { free(out.u.table); return value_null(); }
            out.u.table->cap = out.u.table->count = t->count;
            for (size_t i = 0; i < t->count; i++)
                out.u.table->rows[i] = record_clone_contents(&t->rows[i]);
        }
        return out;
    }
    }
    return value_null();   /* unreachable, silences -Wreturn-type on some builds */
}

/* --------------------------------------------------------------------------
 * value_equal -- strict structural equality. NO cross-type coercion (INT 1 !=
 * FLOAT 1.0 != FILESIZE 1): coercion is the EVALUATOR's job (see value.h).
 * -------------------------------------------------------------------------- */
static bool record_equal(const struct record *a, const struct record *b) {
    if (a->count != b->count) return false;
    /* Field ORDER-independent: a record is a set of named fields, so equality
     * matches by name, not position. (Two records built by setting the same
     * fields in different orders are equal -- the intuitive result.) */
    for (size_t i = 0; i < a->count; i++) {
        int j = record_find(b, a->names[i]);
        if (j < 0) return false;
        if (!value_equal(&a->values[i], &b->values[j])) return false;
    }
    return true;
}

bool value_equal(const struct value *a, const struct value *b) {
    if (a->type != b->type) return false;   /* strict: no coercion here */
    switch (a->type) {
    case VAL_NULL:  return true;
    case VAL_INT:
    case VAL_FILESIZE:
    case VAL_DATE:  return a->u.i == b->u.i;
    case VAL_FLOAT: return a->u.f == b->u.f;   /* exact; the evaluator can offer
                                                * tolerance-based compare if ever needed */
    case VAL_BOOL:  return a->u.b == b->u.b;
    case VAL_STRING:
    case VAL_PATH:
    case VAL_ERROR:   /* two errors are equal iff same message */
        return a->u.s.len == b->u.s.len &&
               memcmp(a->u.s.bytes, b->u.s.bytes, a->u.s.len) == 0;
    case VAL_LIST:
        if (a->u.list.count != b->u.list.count) return false;
        for (size_t i = 0; i < a->u.list.count; i++)   /* lists ARE order-sensitive */
            if (!value_equal(&a->u.list.items[i], &b->u.list.items[i])) return false;
        return true;
    case VAL_RECORD: return record_equal(a->u.record, b->u.record);
    case VAL_TABLE:
        if (a->u.table->count != b->u.table->count) return false;
        for (size_t i = 0; i < a->u.table->count; i++)   /* tables ARE row-order-sensitive */
            if (!record_equal(&a->u.table->rows[i], &b->u.table->rows[i])) return false;
        return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * value_compare -- ordering for `where size > x` and `sort`. Same-type only;
 * cross-type (and unorderable types) return VALUE_CMP_UNORDERED and the
 * evaluator decides. Numeric coercion (int vs filesize) is NOT done here.
 * -------------------------------------------------------------------------- */
static int cmp_i64(int64_t a, int64_t b) { return a < b ? -1 : (a > b ? 1 : 0); }
static int cmp_f64(double a, double b)   { return a < b ? -1 : (a > b ? 1 : 0); }

int value_compare(const struct value *a, const struct value *b) {
    if (a->type != b->type) return VALUE_CMP_UNORDERED;
    switch (a->type) {
    case VAL_INT:
    case VAL_FILESIZE:
    case VAL_DATE:  return cmp_i64(a->u.i, b->u.i);
    case VAL_FLOAT: return cmp_f64(a->u.f, b->u.f);
    case VAL_STRING:
    case VAL_PATH: {
        size_t n = a->u.s.len < b->u.s.len ? a->u.s.len : b->u.s.len;
        int c = memcmp(a->u.s.bytes, b->u.s.bytes, n);
        if (c) return c < 0 ? -1 : 1;
        return cmp_i64((int64_t)a->u.s.len, (int64_t)b->u.s.len);  /* prefix -> shorter first */
    }
    default:   /* NULL/BOOL/LIST/RECORD/TABLE: no natural order */
        return VALUE_CMP_UNORDERED;
    }
}