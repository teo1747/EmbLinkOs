/* ==========================================================================
 * lex.c -- see lex.h. A hand-written scanner. One pass, growable token array.
 * ========================================================================== */
#include "lex.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Scanner state
 * ------------------------------------------------------------------------- */
struct lexer {
    const char *src;
    size_t      pos;
    size_t      line, col;

    struct token *toks;
    size_t        count, cap;
    bool          oom;
};

static char peek(struct lexer *L)      { return L->src[L->pos]; }
static char peek2(struct lexer *L)     { return L->src[L->pos] ? L->src[L->pos + 1] : '\0'; }
static char advance(struct lexer *L) {
    char c = L->src[L->pos++];
    if (c == '\n') { L->line++; L->col = 1; } else { L->col++; }
    return c;
}

/* -------------------------------------------------------------------------
 * Character classes. A "word char" is what can appear INSIDE a bare word.
 * Note `-` and `.` are NOT word chars by this predicate -- they only join a
 * word CONTEXTUALLY (see scan_word), which is the whole point of the rule.
 * ------------------------------------------------------------------------- */
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_word(char c)  { return is_alpha(c) || is_digit(c) || c == '/'; }
       /* '/' is always a word char (paths); '-' and '.' are contextual. */
static bool is_word_or_join(char c) { return is_word(c) || c == '-' || c == '.'; }

/* -------------------------------------------------------------------------
 * Token array growth
 * ------------------------------------------------------------------------- */
static struct token *emit(struct lexer *L, enum tok_type type) {
    if (L->count == L->cap) {
        size_t nc = L->cap ? L->cap * 2 : 16;
        struct token *nt = (struct token *)realloc(L->toks, nc * sizeof(struct token));
        if (!nt) { L->oom = true; return NULL; }
        L->toks = nt; L->cap = nc;
    }
    struct token *t = &L->toks[L->count++];
    memset(t, 0, sizeof *t);
    t->type = type;
    t->line = L->line;
    return t;
}

static void emit_error(struct lexer *L, const char *msg) {
    struct token *t = emit(L, TOK_ERROR);
    if (t) { t->lexeme = msg; t->lexeme_len = strlen(msg); t->col = L->col; }
}

/* -------------------------------------------------------------------------
 * Number + filesize. Entered when peek() is a digit. Forms:
 *   123         -> INT
 *   3.14        -> FLOAT
 *   1mb 512kb   -> FILESIZE (case-insensitive kb/mb/gb/tb suffix, 1024-based)
 *   .5 is NOT a number here (leading digit required) -- keeps the dot rule clean.
 * ------------------------------------------------------------------------- */
static int64_t unit_mult(const char *s, size_t n) {
    /* s/n is the lowercased 2-char suffix already matched */
    if (n == 2 && s[1] == 'b') {
        switch (s[0]) {
            case 'k': return 1024LL;
            case 'm': return 1024LL * 1024;
            case 'g': return 1024LL * 1024 * 1024;
            case 't': return 1024LL * 1024 * 1024 * 1024;
        }
    }
    return 0;
}

static void scan_number(struct lexer *L) {
    size_t start = L->pos, start_col = L->col;
    while (is_digit(peek(L))) advance(L);

    bool is_float = false;
    /* a '.' is a decimal point ONLY if followed by a digit -- otherwise it's
     * a DOT operator / bare-word join, handled elsewhere. So "1.5" is float,
     * "1.foo" is INT(1) then DOT (well: 1 . foo). */
    if (peek(L) == '.' && is_digit(peek2(L))) {
        is_float = true;
        advance(L);                    /* the dot */
        while (is_digit(peek(L))) advance(L);
    }

    /* filesize suffix? lowercase 2 letters forming a known unit, and NOT
     * followed by more word chars (so "1mbx" is not a filesize -- it's a lex
     * error-ish bare mixing; we treat trailing word chars as "not a unit"). */
    if (!is_float) {
        char c0 = peek(L), c1 = peek2(L);
        char lo0 = (c0 >= 'A' && c0 <= 'Z') ? c0 + 32 : c0;
        char lo1 = (c1 >= 'A' && c1 <= 'Z') ? c1 + 32 : c1;
        char suf[2] = { lo0, lo1 };
        int64_t mult = unit_mult(suf, 2);
        /* the char AFTER the 2-letter suffix must not continue a word */
        char after = L->src[L->pos + 2] ? L->src[L->pos + 2] : '\0';
        if (mult && !is_word_or_join(after)) {
            /* parse the integer part, apply the multiplier */
            int64_t base = 0;
            for (size_t i = start; i < L->pos; i++) base = base * 10 + (L->src[i] - '0');
            advance(L); advance(L);    /* consume the two suffix chars */
            struct token *t = emit(L, TOK_FILESIZE);
            if (t) { t->int_val = base * mult; t->lexeme = L->src + start;
                     t->lexeme_len = L->pos - start; t->col = start_col; }
            return;
        }
    }

    size_t len = L->pos - start;
    struct token *t = emit(L, is_float ? TOK_FLOAT : TOK_INT);
    if (!t) return;
    t->lexeme = L->src + start; t->lexeme_len = len; t->col = start_col;
    /* parse the payload */
    if (is_float) {
        t->float_val = strtod(L->src + start, NULL);
    } else {
        int64_t v = 0;
        for (size_t i = start; i < len + start; i++) v = v * 10 + (L->src[i] - '0');
        t->int_val = v;
    }
}

/* -------------------------------------------------------------------------
 * Quoted string. `quote` is '"' (escapes) or '\'' (raw). Produces TOK_STRING
 * with an OWNED decoded `str`. On unterminated quote, a TOK_ERROR.
 * ------------------------------------------------------------------------- */
static void scan_string(struct lexer *L, char quote) {
    size_t start_col = L->col;
    const char *raw_start = L->src + L->pos - 1;   /* include the opening quote;
                                                    * the quote itself was already
                                                    * consumed by the caller */

    /* decode into a growable buffer */
    char  *out = NULL; size_t olen = 0, ocap = 0;
    #define PUSH(ch) do { \
        if (olen == ocap) { size_t nc = ocap ? ocap*2 : 16; \
            char *nb = (char*)realloc(out, nc); if (!nb) { free(out); L->oom = true; return; } \
            out = nb; ocap = nc; } \
        out[olen++] = (ch); } while (0)

    for (;;) {
        char c = peek(L);
        if (c == '\0') { free(out); emit_error(L, "unterminated string"); return; }
        if (c == quote) { advance(L); break; }     /* closing quote */
        if (quote == '"' && c == '\\') {
            advance(L);                             /* backslash */
            char e = advance(L);
            switch (e) {
                case 'n': PUSH('\n'); break;
                case 't': PUSH('\t'); break;
                case 'r': PUSH('\r'); break;
                case '0': PUSH('\0'); break;
                case '"': PUSH('"');  break;
                case '\'':PUSH('\''); break;
                case '\\':PUSH('\\'); break;
                case '\0': free(out); emit_error(L, "trailing backslash in string"); return;
                default:  PUSH(e);    break;        /* unknown escape -> literal char */
            }
        } else {
            PUSH(advance(L));                        /* raw char (single-quote: even backslash) */
        }
    }
    PUSH('\0');   /* NUL-terminate the decoded buffer for C-string convenience;
                   * str_len below excludes it */

    struct token *t = emit(L, TOK_STRING);
    if (!t) { free(out); return; }
    t->str = out; t->str_len = olen - 1;   /* -1: exclude the NUL */
    t->lexeme = raw_start; t->lexeme_len = (size_t)(L->src + L->pos - raw_start);
    t->col = start_col;
    #undef PUSH
    (void)raw_start;
    goto done; done: ;
}

/* -------------------------------------------------------------------------
 * Bare word (and keyword recognition). Entered when peek() is is_word() or a
 * '$'. Consumes word chars, and CONTEXTUALLY absorbs a '-' or '.' iff it is
 * flanked by word chars on both sides (the whole dash/dot rule).
 * ------------------------------------------------------------------------- */
static enum tok_type keyword_of(const char *s, size_t n) {
    #define KW(str, ty) if (n == sizeof(str)-1 && memcmp(s, str, n) == 0) return ty;
    KW("and", TOK_AND) KW("or", TOK_OR) KW("not", TOK_NOT)
    KW("true", TOK_TRUE) KW("false", TOK_FALSE) KW("let", TOK_LET)
    #undef KW
    return TOK_IDENT;
}

static void scan_word(struct lexer *L, bool dollar) {
    size_t start = L->pos, start_col = L->col;
    if (dollar) advance(L);            /* consume '$'; the name follows */
    size_t name_start = L->pos;

    while (true) {
        char c = peek(L);
        if (is_word(c)) { advance(L); continue; }
        /* contextual join: '-' or '.' continues the word ONLY if the char
         * AFTER it is also a word char. (The char BEFORE is a word char by
         * construction -- we only reach here having consumed at least one.)
         * EXCEPTION: a $var never dot-joins -- `$row.size` must lex as
         * $row DOT size (field access), per lex.h's contract. */
        if (c == '.' && dollar) break;
        if ((c == '-' || c == '.') && is_word(peek2(L))) { advance(L); continue; }
        break;
    }

    size_t len = L->pos - start;
    if (dollar) {
        struct token *t = emit(L, TOK_DOLLAR_IDENT);
        if (t) { t->lexeme = L->src + name_start;
                 t->lexeme_len = L->pos - name_start; t->col = start_col; }
        return;
    }

    enum tok_type kw = keyword_of(L->src + start, len);
    struct token *t = emit(L, kw);
    if (t) { t->lexeme = L->src + start; t->lexeme_len = len; t->col = start_col; }
}

/* -------------------------------------------------------------------------
 * Operators + punctuation (the single/double-char symbols).
 * ------------------------------------------------------------------------- */
static void scan_symbol(struct lexer *L) {
    size_t col = L->col;
    char c = advance(L);
    enum tok_type ty;
    switch (c) {
        case '|': ty = TOK_PIPE;   break;
        case '(': ty = TOK_LPAREN; break;
        case ')': ty = TOK_RPAREN; break;
        case ',': ty = TOK_COMMA;  break;
        case '+': ty = TOK_PLUS;   break;
        case '*': ty = TOK_STAR;   break;
        case '-': ty = TOK_MINUS;  break;   /* standalone -- (a word-joining '-'
                                             * was consumed by scan_word) */
        case '.': ty = TOK_DOT;    break;   /* standalone -- word/number dots
                                             * handled in scan_word/scan_number */
        case '=':
            if (peek(L) == '=') { advance(L); ty = TOK_EQ; }
            else if (peek(L) == '~') { advance(L); ty = TOK_MATCH; }
            else ty = TOK_ASSIGN;
            break;
        case '!':
            if (peek(L) == '=') { advance(L); ty = TOK_NE; }
            else { emit_error(L, "unexpected '!' (did you mean '!=' or 'not'?)"); return; }
            break;
        case '<': if (peek(L) == '=') { advance(L); ty = TOK_LE; } else ty = TOK_LT; break;
        case '>': if (peek(L) == '=') { advance(L); ty = TOK_GE; } else ty = TOK_GT; break;
        case '/':
            /* a leading '/' with a following word char is a PATH bare word
             * (/foo/bar); handled by scan_word being entered on is_word('/').
             * Reaching here means '/' standing alone -> division. */
            ty = TOK_SLASH; break;
        default:
            emit_error(L, "unexpected character");
            return;
    }
    struct token *t = emit(L, ty);
    if (t) { t->lexeme = L->src + L->pos - 1; t->lexeme_len = 1; t->col = col; }
}

/* -------------------------------------------------------------------------
 * Main loop
 * ------------------------------------------------------------------------- */
struct token *lex(const char *src, size_t *out_n) {
    struct lexer L;
    memset(&L, 0, sizeof L);
    L.src = src; L.line = 1; L.col = 1;

    for (;;) {
        char c = peek(&L);
        if (c == '\0') break;

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(&L); continue; }
        if (c == '#') { while (peek(&L) && peek(&L) != '\n') advance(&L); continue; }

        if (is_digit(c))            { scan_number(&L); }
        else if (c == '"' || c == '\'') { advance(&L); scan_string(&L, c); }
        else if (c == '$')          { scan_word(&L, true); }
        /* '/' is a word char (paths: /foo/bar) -- but a '/' NOT followed by
         * a word char stands alone: DIVISION. Without this check scan_word
         * swallowed every '/' and TOK_SLASH was unreachable. Like the dash
         * rule, division therefore needs spaces: `a / b`, not `a/b` (which
         * is one path-ish word -- the documented gotcha). */
        else if (c == '/' && !is_word(peek2(&L))) { scan_symbol(&L); }
        else if (is_word(c))        { scan_word(&L, false); }
        else                        { scan_symbol(&L); }

        if (L.oom) { lex_free_tokens(L.toks, L.count); return NULL; }
        /* a TOK_ERROR was emitted: stop lexing, let the caller report it.
         * (We still append EOF below.) */
        if (L.count && L.toks[L.count - 1].type == TOK_ERROR) break;
    }

    struct token *eof = emit(&L, TOK_EOF);
    if (!eof) { lex_free_tokens(L.toks, L.count); return NULL; }

    *out_n = L.count;
    return L.toks;
}

void lex_free_tokens(struct token *toks, size_t n) {
    if (!toks) return;
    for (size_t i = 0; i < n; i++)
        if (toks[i].type == TOK_STRING) free(toks[i].str);
    free(toks);
}

const char *tok_type_name(enum tok_type t) {
    switch (t) {
        case TOK_EOF: return "EOF"; case TOK_INT: return "INT"; case TOK_FLOAT: return "FLOAT";
        case TOK_STRING: return "STRING"; case TOK_FILESIZE: return "FILESIZE";
        case TOK_TRUE: return "true"; case TOK_FALSE: return "false";
        case TOK_IDENT: return "IDENT"; case TOK_DOLLAR_IDENT: return "$IDENT"; case TOK_LET: return "let";
        case TOK_PIPE: return "|"; case TOK_LPAREN: return "("; case TOK_RPAREN: return ")";
        case TOK_DOT: return "."; case TOK_COMMA: return ",";
        case TOK_OR: return "or"; case TOK_AND: return "and"; case TOK_NOT: return "not";
        case TOK_EQ: return "=="; case TOK_NE: return "!="; case TOK_LT: return "<";
        case TOK_GT: return ">"; case TOK_LE: return "<="; case TOK_GE: return ">=";
        case TOK_MATCH: return "=~"; case TOK_PLUS: return "+"; case TOK_MINUS: return "-";
        case TOK_STAR: return "*"; case TOK_SLASH: return "/"; case TOK_ASSIGN: return "=";
        case TOK_ERROR: return "ERROR";
    }
    return "?";
}