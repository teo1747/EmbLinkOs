/* ==========================================================================
 * eval.c -- see eval.h. Scope, the expression evaluator (with the shell's
 * ONE coercion site), and the materializing pipeline runner.
 * ========================================================================== */
#include "eval/eval.h"
#include "builtins/builtins.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Scope
 * ------------------------------------------------------------------------- */
void scope_init(struct scope *s, struct scope *parent) {
    memset(s, 0, sizeof(*s));
    s->parent = parent;
}

void scope_free(struct scope *s) {
    for (size_t i = 0; i < s->count; i++) {
        free(s->names[i]);
        value_free(&s->values[i]);
    }
    free(s->names);
    free(s->values);
    memset(s, 0, sizeof(*s));
}

int scope_bind(struct scope *s, const char *name, struct value v) {
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->names[i], name) == 0) {      /* rebind: replace in place */
            value_free(&s->values[i]);
            s->values[i] = v;
            return 0;
        }
    }
    if (s->count == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4;
        char **nn = (char **)realloc(s->names, nc * sizeof(*nn));
        if (!nn) { value_free(&v); return -1; }
        s->names = nn;
        struct value *nv = (struct value *)realloc(s->values, nc * sizeof(*nv));
        if (!nv) { value_free(&v); return -1; }
        s->values = nv;
        s->cap = nc;
    }
    size_t n = strlen(name);
    char *copy = (char *)malloc(n + 1);
    if (!copy) { value_free(&v); return -1; }
    memcpy(copy, name, n + 1);
    s->names[s->count]  = copy;
    s->values[s->count] = v;
    s->count++;
    return 0;
}

const struct value *scope_lookup(const struct scope *s, const char *name) {
    for (; s; s = s->parent) {
        for (size_t i = 0; i < s->count; i++)
            if (strcmp(s->names[i], name) == 0) return &s->values[i];
        if (s->row) {
            const struct value *f = record_field(s->row, name);
            if (f) return f;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Truthiness -- STRICT (settled decision, see eval.h).
 * ------------------------------------------------------------------------- */
int value_truthy(const struct value *v) {
    if (v->type == VAL_BOOL) return v->u.b ? 1 : 0;
    if (v->type == VAL_NULL) return 0;
    return -1;
}

/* -------------------------------------------------------------------------
 * Coercion helpers -- the numeric family and the text family.
 * ------------------------------------------------------------------------- */
static bool is_numeric(const struct value *v) {
    return v->type == VAL_INT || v->type == VAL_FILESIZE ||
           v->type == VAL_DATE || v->type == VAL_FLOAT;
}
static bool is_text(const struct value *v) {
    return v->type == VAL_STRING || v->type == VAL_PATH;
}
static double as_f64(const struct value *v) {
    return v->type == VAL_FLOAT ? v->u.f : (double)v->u.i;
}

static const char *type_name(enum value_type t) {
    switch (t) {
    case VAL_NULL: return "null";      case VAL_INT: return "int";
    case VAL_FLOAT: return "float";    case VAL_BOOL: return "bool";
    case VAL_STRING: return "string";  case VAL_LIST: return "list";
    case VAL_RECORD: return "record";  case VAL_TABLE: return "table";
    case VAL_FILESIZE: return "filesize"; case VAL_DATE: return "date";
    case VAL_PATH: return "path";      case VAL_ERROR: return "error";
    }
    return "?";
}

static struct value errf(const char *fmt, const char *a, const char *b) {
    char msg[128];
    snprintf(msg, sizeof msg, fmt, a, b);
    return value_error(msg);
}

/* Compare with coercion: 0 filled into *out on success (-1/0/1), -1 if the
 * pair has no meaningful order (caller builds the error). */
static int coerced_compare(const struct value *a, const struct value *b, int *out) {
    /* Nulls ORDER first (SQL's NULLS FIRST): gives sort-by a total order
     * over columns with missing cells. (where's comparison arm never gets
     * here with a null -- it short-circuits to false, deliberately: "null
     * sorts before 100kb" must not make `where size < 100kb` match dirs.) */
    if (a->type == VAL_NULL || b->type == VAL_NULL) {
        *out = (a->type == b->type) ? 0 : (a->type == VAL_NULL ? -1 : 1);
        return 0;
    }
    if (is_numeric(a) && is_numeric(b)) {
        if (a->type == VAL_FLOAT || b->type == VAL_FLOAT) {
            double x = as_f64(a), y = as_f64(b);
            *out = x < y ? -1 : (x > y ? 1 : 0);
        } else {
            *out = a->u.i < b->u.i ? -1 : (a->u.i > b->u.i ? 1 : 0);
        }
        return 0;
    }
    if (is_text(a) && is_text(b)) {
        size_t n = a->u.s.len < b->u.s.len ? a->u.s.len : b->u.s.len;
        int c = n ? memcmp(a->u.s.bytes, b->u.s.bytes, n) : 0;
        if (c) { *out = c < 0 ? -1 : 1; return 0; }
        *out = a->u.s.len < b->u.s.len ? -1 : (a->u.s.len > b->u.s.len ? 1 : 0);
        return 0;
    }
    int c = value_compare(a, b);
    if (c == VALUE_CMP_UNORDERED) return -1;
    *out = c;
    return 0;
}

int eval_compare_values(const struct value *a, const struct value *b, int *out) {
    return coerced_compare(a, b, out);
}

/* Equality with coercion: numerics compare numerically across tags, text
 * compares as bytes across STRING/PATH; everything else is strict
 * value_equal (cross-type -> not equal, never an error: asking "is 1 equal
 * to \"a\"" has a perfectly good answer, namely no). */
static bool coerced_equal(const struct value *a, const struct value *b) {
    if (is_numeric(a) && is_numeric(b)) {
        if (a->type == VAL_FLOAT || b->type == VAL_FLOAT)
            return as_f64(a) == as_f64(b);
        return a->u.i == b->u.i;
    }
    if (is_text(a) && is_text(b))
        return a->u.s.len == b->u.s.len &&
               (a->u.s.len == 0 || memcmp(a->u.s.bytes, b->u.s.bytes, a->u.s.len) == 0);
    return value_equal(a, b);
}

/* Arithmetic result tag: FLOAT contaminates; INT is the "neutral" element
 * that adopts the richer side's tag; two different rich tags don't mix. */
static int arith_result_type(enum value_type a, enum value_type b, enum value_type *out) {
    if (a == VAL_FLOAT || b == VAL_FLOAT) { *out = VAL_FLOAT; return 0; }
    if (a == b)       { *out = a; return 0; }
    if (a == VAL_INT) { *out = b; return 0; }
    if (b == VAL_INT) { *out = a; return 0; }
    return -1;   /* filesize op date: meaningless */
}

static struct value eval_binary(int op, struct value L, struct value R) {
    /* errors flow through untouched */
    if (L.type == VAL_ERROR) { value_free(&R); return L; }
    if (R.type == VAL_ERROR) { value_free(&L); return R; }

    struct value out = value_null();

    switch (op) {
    case TOK_EQ: case TOK_NE: {
        bool eq = coerced_equal(&L, &R);
        out = value_bool(op == TOK_EQ ? eq : !eq);
        break;
    }
    case TOK_LT: case TOK_GT: case TOK_LE: case TOK_GE: {
        /* SQL semantics for missing data: a comparison against NULL is
         * FALSE, not an error -- `ls / | where size > 100kb` must simply
         * not match directories (whose size is null), not abort. */
        if (L.type == VAL_NULL || R.type == VAL_NULL) {
            out = value_bool(false);
            break;
        }
        int c;
        if (coerced_compare(&L, &R, &c) != 0) {
            out = errf("can't compare %s to %s", type_name(L.type), type_name(R.type));
            break;
        }
        bool r = (op == TOK_LT) ? c < 0 : (op == TOK_GT) ? c > 0
               : (op == TOK_LE) ? c <= 0 : c >= 0;
        out = value_bool(r);
        break;
    }
    case TOK_MATCH: {   /* substring match over text (regex is future work) */
        if (!is_text(&L) || !is_text(&R)) {
            out = errf("=~ needs text on both sides (got %s and %s)",
                       type_name(L.type), type_name(R.type));
            break;
        }
        bool found = false;
        if (R.u.s.len == 0) found = true;
        else if (R.u.s.len <= L.u.s.len) {
            for (size_t i = 0; i + R.u.s.len <= L.u.s.len; i++) {
                if (memcmp(L.u.s.bytes + i, R.u.s.bytes, R.u.s.len) == 0) { found = true; break; }
            }
        }
        out = value_bool(found);
        break;
    }
    case TOK_PLUS: case TOK_MINUS: case TOK_STAR: case TOK_SLASH: {
        if (!is_numeric(&L) || !is_numeric(&R)) {
            out = errf("arithmetic needs numbers (got %s and %s)",
                       type_name(L.type), type_name(R.type));
            break;
        }
        enum value_type rt;
        if (arith_result_type(L.type, R.type, &rt) != 0) {
            out = errf("can't mix %s and %s in arithmetic",
                       type_name(L.type), type_name(R.type));
            break;
        }
        if (rt == VAL_FLOAT) {
            double x = as_f64(&L), y = as_f64(&R);
            double r = (op == TOK_PLUS) ? x + y : (op == TOK_MINUS) ? x - y
                     : (op == TOK_STAR) ? x * y : (y == 0.0 ? 0.0 : x / y);
            if (op == TOK_SLASH && y == 0.0) out = value_error("division by zero");
            else out = value_float(r);
        } else {
            int64_t x = L.u.i, y = R.u.i;
            if (op == TOK_SLASH && y == 0) { out = value_error("division by zero"); break; }
            int64_t r = (op == TOK_PLUS) ? x + y : (op == TOK_MINUS) ? x - y
                      : (op == TOK_STAR) ? x * y : x / y;
            out.type = rt;   /* INT / FILESIZE / DATE all carry .u.i */
            out.u.i = r;
        }
        break;
    }
    default:
        out = value_error("unknown operator");
        break;
    }

    value_free(&L);
    value_free(&R);
    return out;
}

/* -------------------------------------------------------------------------
 * expr_eval
 * ------------------------------------------------------------------------- */
const char *expr_as_word(const struct expr *e) {
    if (e->kind == EXPR_COLUMN) return e->u.var.name;
    if (e->kind == EXPR_LITERAL &&
        (e->u.literal.type == VAL_STRING || e->u.literal.type == VAL_PATH))
        return e->u.literal.u.s.bytes;   /* NUL-terminated by make_bytes */
    return NULL;
}

struct value expr_eval(const struct expr *e, struct scope *env) {
    switch (e->kind) {
    case EXPR_LITERAL:
        return value_clone(&e->u.literal);

    case EXPR_COLUMN: {
        const struct value *v = scope_lookup(env, e->u.var.name);
        if (v) return value_clone(v);
        /* bare-word fallback (settled decision): an unresolved bare word IS
         * its own text -- `echo hi`, `ls /data` without quoting. $vars are
         * the strict spelling. */
        return value_string(e->u.var.name);
    }
    case EXPR_VAR: {
        const struct value *v = scope_lookup(env, e->u.var.name);
        if (v) return value_clone(v);
        return errf("undefined variable '$%s'%s", e->u.var.name, "");
    }
    case EXPR_FIELD: {
        struct value obj = expr_eval(e->u.field.obj, env);
        if (obj.type == VAL_ERROR) return obj;
        if (obj.type != VAL_RECORD) {
            struct value err = errf("'.%s' needs a record (got %s)",
                                    e->u.field.field, type_name(obj.type));
            value_free(&obj);
            return err;
        }
        const struct value *f = value_record_get(&obj, e->u.field.field);
        struct value out = f ? value_clone(f)
                             : errf("record has no field '%s'%s", e->u.field.field, "");
        value_free(&obj);
        return out;
    }
    case EXPR_UNARY: {
        struct value v = expr_eval(e->u.unary.operand, env);
        if (v.type == VAL_ERROR) return v;
        if (e->u.unary.op == TOK_NOT) {
            int t = value_truthy(&v);
            if (t < 0) {
                struct value err = errf("'not' expected a boolean, got %s%s",
                                        type_name(v.type), "");
                value_free(&v);
                return err;
            }
            value_free(&v);
            return value_bool(!t);
        }
        /* unary minus */
        if (v.type == VAL_INT || v.type == VAL_FILESIZE || v.type == VAL_DATE) {
            v.u.i = -v.u.i;
            return v;
        }
        if (v.type == VAL_FLOAT) { v.u.f = -v.u.f; return v; }
        {
            struct value err = errf("unary '-' expected a number, got %s%s",
                                    type_name(v.type), "");
            value_free(&v);
            return err;
        }
    }
    case EXPR_BINARY: {
        int op = e->u.binary.op;
        /* and/or short-circuit BEFORE the rhs is evaluated */
        if (op == TOK_AND || op == TOK_OR) {
            struct value L = expr_eval(e->u.binary.lhs, env);
            if (L.type == VAL_ERROR) return L;
            int lt = value_truthy(&L);
            value_free(&L);
            if (lt < 0) return value_error(op == TOK_AND
                ? "'and' expected booleans" : "'or' expected booleans");
            if (op == TOK_AND && !lt) return value_bool(false);
            if (op == TOK_OR  &&  lt) return value_bool(true);
            struct value R = expr_eval(e->u.binary.rhs, env);
            if (R.type == VAL_ERROR) return R;
            int rt = value_truthy(&R);
            value_free(&R);
            if (rt < 0) return value_error(op == TOK_AND
                ? "'and' expected booleans" : "'or' expected booleans");
            return value_bool(rt != 0);
        }
        return eval_binary(op, expr_eval(e->u.binary.lhs, env),
                               expr_eval(e->u.binary.rhs, env));
    }
    }
    return value_error("internal: unknown expression kind");
}

/* -------------------------------------------------------------------------
 * Pipeline runner -- materialize v1: each stage is Value -> Value.
 * ------------------------------------------------------------------------- */
struct value pipeline_run(const struct pipeline *pl, struct scope *top) {
    struct value cur = value_null();   /* first stage gets NULL as input */

    for (size_t i = 0; i < pl->nstages; i++) {
        const struct command *cmd = pl->stages[i];
        builtin_fn fn = builtin_lookup(cmd->name);
        struct value next;
        if (fn) {
            next = fn(cmd, cur, top);           /* stage takes ownership of cur */
        } else {
            next = extern_stage_run(cmd, cur, top);
        }
        cur = next;
        if (cur.type == VAL_ERROR) break;       /* abort remaining stages */
    }
    return cur;
}
