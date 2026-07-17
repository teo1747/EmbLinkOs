/* ==========================================================================
 * parse.h -- tokens -> AST. Two node families (the sketch's contract):
 * pipeline/command structure, and expression trees. One top-level statement
 * wrapper distinguishes `let name = expr` from a pipeline.
 *
 * Locked parsing decisions (see shell-architecture-sketch / docs/SHELL.md):
 *   - A command argument is ALWAYS an expression. No command has a bespoke
 *     arg parser; `where`'s arg is the expr `size > 1mb`, `ls`'s arg is the
 *     expr `"/foo"` or the column-ident `/foo`.
 *   - A bare ident parses to EXPR_COLUMN uniformly; whether it means "the
 *     current row's field" or "a path-ish word" is the EVALUATOR's (or the
 *     builtin's) call. Keeps the parser context-free.
 *   - Pratt/precedence-climbing with the locked table (lowest->highest):
 *       or < and < comparisons(non-chaining) < +- < * / < unary(not,-) < .()
 *   - Comparisons do NOT chain: `a < b < c` is a parse error, not `(a<b)<c`.
 *   - Ownership: the AST owns everything it points at (names are copied out
 *     of the token slices; literal Values are moved in). stmt_free() is a
 *     complete teardown. Tokens can be freed as soon as parse() returns.
 * ========================================================================== */
#ifndef __EMBK_PARSE_H__
#define __EMBK_PARSE_H__

#include "lex/lex.h"
#include "value/value.h"
#include <stddef.h>

/* --- expressions --- */
enum expr_kind {
    EXPR_LITERAL,   /* a literal Value (int/float/string/filesize/bool) */
    EXPR_VAR,       /* $ident -- explicit variable reference */
    EXPR_COLUMN,    /* bare ident -- current row's field (or a bare word the
                     * builtin interprets, e.g. a path argument to ls) */
    EXPR_FIELD,     /* expr.ident -- field access on a record value */
    EXPR_UNARY,     /* not expr, - expr */
    EXPR_BINARY,    /* expr OP expr */
};

struct expr {
    enum expr_kind kind;
    size_t line, col;      /* of the node's first token, for error messages */
    union {
        struct value literal;                             /* EXPR_LITERAL (owned) */
        struct { char *name; } var;                       /* EXPR_VAR / EXPR_COLUMN (owned) */
        struct { struct expr *obj; char *field; } field;  /* EXPR_FIELD (owned) */
        struct { int op; struct expr *operand; } unary;   /* op = a TOK_* */
        struct { int op; struct expr *lhs, *rhs; } binary;
    } u;
};

/* --- pipeline / command --- */
struct command {
    char         *name;    /* "ls", "where", ... (owned) */
    struct expr **args;    /* each arg is an expression (owned) */
    size_t        nargs;
    size_t        line, col;
};

struct pipeline {
    struct command **stages;   /* left-to-right; stage[i] feeds stage[i+1] */
    size_t           nstages;
};

/* --- statement: one shell line --- */
enum stmt_kind { STMT_PIPELINE, STMT_LET };
struct stmt {
    enum stmt_kind kind;
    union {
        struct pipeline *pipe;                          /* STMT_PIPELINE */
        struct { char *name; struct expr *expr; } let;  /* STMT_LET (owned) */
    } u;
};

/* Parse one line's tokens (from lex(); must end in TOK_EOF). On success
 * returns an owned stmt. On error returns NULL and writes a human message
 * ("3:14: expected expression after '|'") into err (NUL-terminated,
 * truncated to errcap). A TOK_ERROR from the lexer is surfaced the same way. */
struct stmt *parse(const struct token *toks, size_t ntoks, char *err, size_t errcap);

void expr_free(struct expr *e);          /* recursive; NULL-safe */
void pipeline_free(struct pipeline *p);  /* NULL-safe */
void stmt_free(struct stmt *s);          /* NULL-safe */

#endif /* __EMBK_PARSE_H__ */
