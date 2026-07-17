/* ==========================================================================
 * parse.c -- see parse.h. A Pratt (precedence-climbing) expression parser
 * plus the flat pipeline/command grammar around it:
 *
 *   line     := "let" IDENT "=" expr  |  pipeline
 *   pipeline := command ( "|" command )*
 *   command  := IDENT expr*                 (args end at '|' / EOF / ')')
 *   expr     := the Pratt grammar over the locked precedence table
 *
 * One documented consequence of "args are expressions" + maximal-munch:
 * `echo a - b` is ONE argument (the subtraction a-b), not three. Quote or
 * parenthesize when a literal word sequence is wanted.
 * ========================================================================== */
#include "parse/parse.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Parser state: a cursor over the token array + the error sink. All error
 * paths funnel through perr() so every message carries line:col.
 * ------------------------------------------------------------------------- */
struct parser {
    const struct token *toks;
    size_t              n, pos;
    char               *err;
    size_t              errcap;
    bool                failed;
};

static const struct token *pk(struct parser *P)  { return &P->toks[P->pos]; }
static const struct token *adv(struct parser *P) { return &P->toks[P->pos < P->n - 1 ? P->pos++ : P->pos]; }

static void perr(struct parser *P, const struct token *at, const char *msg) {
    if (P->failed) return;               /* keep the FIRST error */
    P->failed = true;
    snprintf(P->err, P->errcap, "%zu:%zu: %s", at->line, at->col, msg);
}

static char *copy_slice(const char *s, size_t n) {
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* -------------------------------------------------------------------------
 * AST teardown (needed by every error path below, so defined first).
 * ------------------------------------------------------------------------- */
void expr_free(struct expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_LITERAL: value_free(&e->u.literal); break;
    case EXPR_VAR:
    case EXPR_COLUMN:  free(e->u.var.name); break;
    case EXPR_FIELD:   expr_free(e->u.field.obj); free(e->u.field.field); break;
    case EXPR_UNARY:   expr_free(e->u.unary.operand); break;
    case EXPR_BINARY:  expr_free(e->u.binary.lhs); expr_free(e->u.binary.rhs); break;
    }
    free(e);
}

static void command_free(struct command *c) {
    if (!c) return;
    free(c->name);
    for (size_t i = 0; i < c->nargs; i++) expr_free(c->args[i]);
    free(c->args);
    free(c);
}

void pipeline_free(struct pipeline *p) {
    if (!p) return;
    for (size_t i = 0; i < p->nstages; i++) command_free(p->stages[i]);
    free(p->stages);
    free(p);
}

void stmt_free(struct stmt *s) {
    if (!s) return;
    if (s->kind == STMT_PIPELINE) pipeline_free(s->u.pipe);
    else { free(s->u.let.name); expr_free(s->u.let.expr); }
    free(s);
}

/* -------------------------------------------------------------------------
 * Node constructors. Every one can fail (OOM) -> perr + NULL; callers free
 * whatever they already own on NULL.
 * ------------------------------------------------------------------------- */
static struct expr *expr_new(struct parser *P, enum expr_kind k, const struct token *at) {
    struct expr *e = (struct expr *)calloc(1, sizeof(*e));
    if (!e) { perr(P, at, "out of memory"); return NULL; }
    e->kind = k;
    e->line = at->line;
    e->col  = at->col;
    return e;
}

/* -------------------------------------------------------------------------
 * The precedence table (parse.h's contract). Binding powers: an infix
 * operator binds its right side at (bp + 1) for left-assoc. Comparisons get
 * NON_CHAIN marking: after one comparison at that level, another comparison
 * immediately following is rejected.
 * ------------------------------------------------------------------------- */
enum { BP_NONE = 0, BP_OR = 1, BP_AND = 2, BP_CMP = 3, BP_ADD = 4, BP_MUL = 5,
       BP_UNARY = 6, BP_POSTFIX = 7 };

static int infix_bp(enum tok_type t) {
    switch (t) {
    case TOK_OR:  return BP_OR;
    case TOK_AND: return BP_AND;
    case TOK_EQ: case TOK_NE: case TOK_LT: case TOK_GT:
    case TOK_LE: case TOK_GE: case TOK_MATCH: return BP_CMP;
    case TOK_PLUS: case TOK_MINUS: return BP_ADD;
    case TOK_STAR: case TOK_SLASH: return BP_MUL;
    case TOK_DOT: return BP_POSTFIX;
    default: return BP_NONE;
    }
}
static bool is_comparison(enum tok_type t) {
    return t == TOK_EQ || t == TOK_NE || t == TOK_LT || t == TOK_GT ||
           t == TOK_LE || t == TOK_GE || t == TOK_MATCH;
}

static struct expr *parse_expr(struct parser *P, int min_bp);

/* --- prefix position: literals, names, unary ops, parens --- */
static struct expr *parse_prefix(struct parser *P) {
    const struct token *t = pk(P);
    switch (t->type) {
    case TOK_INT: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_LITERAL, t);
        if (e) e->u.literal = value_int(t->int_val);
        return e;
    }
    case TOK_FLOAT: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_LITERAL, t);
        if (e) e->u.literal = value_float(t->float_val);
        return e;
    }
    case TOK_FILESIZE: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_LITERAL, t);
        if (e) e->u.literal = value_filesize(t->int_val);
        return e;
    }
    case TOK_STRING: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_LITERAL, t);
        if (e) {
            e->u.literal = value_string_n(t->str, t->str_len);
            if (e->u.literal.type != VAL_STRING && t->str_len) {
                perr(P, t, "out of memory");
                expr_free(e);
                return NULL;
            }
        }
        return e;
    }
    case TOK_TRUE: case TOK_FALSE: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_LITERAL, t);
        if (e) e->u.literal = value_bool(t->type == TOK_TRUE);
        return e;
    }
    case TOK_IDENT: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_COLUMN, t);
        if (!e) return NULL;
        e->u.var.name = copy_slice(t->lexeme, t->lexeme_len);
        if (!e->u.var.name) { perr(P, t, "out of memory"); free(e); return NULL; }
        return e;
    }
    case TOK_DOLLAR_IDENT: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_VAR, t);
        if (!e) return NULL;
        e->u.var.name = copy_slice(t->lexeme, t->lexeme_len);
        if (!e->u.var.name) { perr(P, t, "out of memory"); free(e); return NULL; }
        return e;
    }
    case TOK_NOT: case TOK_MINUS: {
        adv(P);
        struct expr *operand = parse_expr(P, BP_UNARY);
        if (!operand) return NULL;
        struct expr *e = expr_new(P, EXPR_UNARY, t);
        if (!e) { expr_free(operand); return NULL; }
        e->u.unary.op = (int)t->type;
        e->u.unary.operand = operand;
        return e;
    }
    case TOK_LPAREN: {
        adv(P);
        struct expr *inner = parse_expr(P, BP_NONE);
        if (!inner) return NULL;
        if (pk(P)->type != TOK_RPAREN) {
            perr(P, pk(P), "expected ')'");
            expr_free(inner);
            return NULL;
        }
        adv(P);
        return inner;
    }
    case TOK_SLASH: {
        /* A '/' in PREFIX position is the bare word "/" -- the root path
         * (`ls /`). In INFIX position (the Pratt loop) it stays division;
         * the grammar position disambiguates what the lexer can't. */
        adv(P);
        struct expr *e = expr_new(P, EXPR_COLUMN, t);
        if (!e) return NULL;
        e->u.var.name = copy_slice("/", 1);
        if (!e->u.var.name) { perr(P, t, "out of memory"); free(e); return NULL; }
        return e;
    }
    case TOK_ERROR:
        perr(P, t, t->lexeme ? t->lexeme : "lex error");
        return NULL;
    default:
        perr(P, t, "expected an expression");
        return NULL;
    }
}

/* --- the Pratt loop --- */
static struct expr *parse_expr(struct parser *P, int min_bp) {
    struct expr *lhs = parse_prefix(P);
    if (!lhs) return NULL;

    bool saw_comparison = false;   /* non-chaining enforcement, per level */

    for (;;) {
        const struct token *t = pk(P);
        int bp = infix_bp(t->type);
        if (bp == BP_NONE || bp < min_bp) break;

        /* postfix field access binds tightest: expr '.' IDENT */
        if (t->type == TOK_DOT) {
            adv(P);
            const struct token *name = pk(P);
            if (name->type != TOK_IDENT) {
                perr(P, name, "expected a field name after '.'");
                expr_free(lhs);
                return NULL;
            }
            adv(P);
            struct expr *f = expr_new(P, EXPR_FIELD, t);
            if (!f) { expr_free(lhs); return NULL; }
            f->u.field.obj = lhs;
            f->u.field.field = copy_slice(name->lexeme, name->lexeme_len);
            if (!f->u.field.field) { perr(P, name, "out of memory"); expr_free(f); return NULL; }
            lhs = f;
            continue;
        }

        if (is_comparison(t->type)) {
            if (saw_comparison) {
                perr(P, t, "comparisons don't chain (use 'and': a < b and b < c)");
                expr_free(lhs);
                return NULL;
            }
            saw_comparison = true;
        }

        adv(P);
        struct expr *rhs = parse_expr(P, bp + 1);   /* +1: left-assoc */
        if (!rhs) { expr_free(lhs); return NULL; }
        struct expr *b = expr_new(P, EXPR_BINARY, t);
        if (!b) { expr_free(lhs); expr_free(rhs); return NULL; }
        b->u.binary.op  = (int)t->type;
        b->u.binary.lhs = lhs;
        b->u.binary.rhs = rhs;
        lhs = b;
    }
    return lhs;
}

/* -------------------------------------------------------------------------
 * command := IDENT expr*    (args stop at '|', ')' or EOF)
 * ------------------------------------------------------------------------- */
static struct command *parse_command(struct parser *P) {
    const struct token *name = pk(P);
    if (name->type != TOK_IDENT) {
        perr(P, name, "expected a command name");
        return NULL;
    }
    adv(P);

    struct command *cmd = (struct command *)calloc(1, sizeof(*cmd));
    if (!cmd) { perr(P, name, "out of memory"); return NULL; }
    cmd->name = copy_slice(name->lexeme, name->lexeme_len);
    cmd->line = name->line;
    cmd->col  = name->col;
    if (!cmd->name) { perr(P, name, "out of memory"); free(cmd); return NULL; }

    size_t cap = 0;
    while (pk(P)->type != TOK_PIPE && pk(P)->type != TOK_EOF &&
           pk(P)->type != TOK_RPAREN) {
        struct expr *arg = parse_expr(P, BP_NONE);
        if (!arg) { command_free(cmd); return NULL; }
        if (cmd->nargs == cap) {
            size_t nc = cap ? cap * 2 : 4;
            struct expr **na = (struct expr **)realloc(cmd->args, nc * sizeof(*na));
            if (!na) { perr(P, pk(P), "out of memory"); expr_free(arg); command_free(cmd); return NULL; }
            cmd->args = na;
            cap = nc;
        }
        cmd->args[cmd->nargs++] = arg;
    }
    return cmd;
}

/* -------------------------------------------------------------------------
 * pipeline := command ('|' command)*
 * ------------------------------------------------------------------------- */
static struct pipeline *parse_pipeline(struct parser *P) {
    struct pipeline *pl = (struct pipeline *)calloc(1, sizeof(*pl));
    if (!pl) { perr(P, pk(P), "out of memory"); return NULL; }

    size_t cap = 0;
    for (;;) {
        struct command *cmd = parse_command(P);
        if (!cmd) { pipeline_free(pl); return NULL; }
        if (pl->nstages == cap) {
            size_t nc = cap ? cap * 2 : 4;
            struct command **ns = (struct command **)realloc(pl->stages, nc * sizeof(*ns));
            if (!ns) { perr(P, pk(P), "out of memory"); command_free(cmd); pipeline_free(pl); return NULL; }
            pl->stages = ns;
            cap = nc;
        }
        pl->stages[pl->nstages++] = cmd;

        if (pk(P)->type != TOK_PIPE) break;
        adv(P);                                   /* consume '|' */
        if (pk(P)->type == TOK_EOF) {
            perr(P, pk(P), "expected a command after '|'");
            pipeline_free(pl);
            return NULL;
        }
    }
    return pl;
}

/* -------------------------------------------------------------------------
 * The entry point: let-statement or pipeline, then require EOF (trailing
 * tokens are an error, not silently ignored).
 * ------------------------------------------------------------------------- */
struct stmt *parse(const struct token *toks, size_t ntoks, char *err, size_t errcap) {
    struct parser P = { toks, ntoks, 0, err, errcap, false };
    if (errcap) err[0] = '\0';

    /* surface a lexer error immediately, wherever it sits */
    for (size_t i = 0; i < ntoks; i++) {
        if (toks[i].type == TOK_ERROR) {
            perr(&P, &toks[i], toks[i].lexeme ? toks[i].lexeme : "lex error");
            return NULL;
        }
    }

    struct stmt *s = (struct stmt *)calloc(1, sizeof(*s));
    if (!s) { perr(&P, pk(&P), "out of memory"); return NULL; }

    if (pk(&P)->type == TOK_LET) {
        adv(&P);
        const struct token *name = pk(&P);
        if (name->type != TOK_IDENT) {
            perr(&P, name, "expected a variable name after 'let'");
            free(s);
            return NULL;
        }
        adv(&P);
        if (pk(&P)->type != TOK_ASSIGN) {
            perr(&P, pk(&P), "expected '=' in let binding");
            free(s);
            return NULL;
        }
        adv(&P);
        struct expr *e = parse_expr(&P, BP_NONE);
        if (!e) { free(s); return NULL; }
        s->kind = STMT_LET;
        s->u.let.name = copy_slice(name->lexeme, name->lexeme_len);
        s->u.let.expr = e;
        if (!s->u.let.name) { perr(&P, name, "out of memory"); stmt_free(s); return NULL; }
    } else {
        struct pipeline *pl = parse_pipeline(&P);
        if (!pl) { free(s); return NULL; }
        s->kind = STMT_PIPELINE;
        s->u.pipe = pl;
    }

    if (pk(&P)->type != TOK_EOF) {
        perr(&P, pk(&P), "unexpected trailing input");
        stmt_free(s);
        return NULL;
    }
    return s;
}
