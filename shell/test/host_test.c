/* ==========================================================================
 * host_test.c -- host-side tests for the shell's pure pipeline:
 * lexer -> parser -> evaluator -> builtins, plus the wire round-trip.
 * Build: make shell-test. Exit code = number of failures.
 * ========================================================================== */
#include "lex/lex.h"
#include "parse/parse.h"
#include "eval/eval.h"
#include "builtins/builtins.h"
#include "wire/wire.h"
#include "sval/sval.h"
#include "hist/hist.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("  ok   %s\n", name); \
    else      { printf("  FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* --- helpers ------------------------------------------------------------- */

/* Evaluate one full line's pipeline against an input value (NULL-type for
 * none) and a fresh scope; returns the resulting value. */
static struct value run(const char *line, struct value input, struct scope *top) {
    size_t n = 0;
    struct token *toks = lex(line, &n);
    if (!toks) return value_error("lex OOM");
    char err[160];
    struct stmt *st = parse(toks, n, err, sizeof err);
    lex_free_tokens(toks, n);
    if (!st) return value_error(err);
    if (st->kind != STMT_PIPELINE) { stmt_free(st); return value_error("expected pipeline"); }

    /* thread `input` into the FIRST stage by hand (pipeline_run starts from
     * null): run stage 0 with input, then the rest. */
    struct value cur = input;
    for (size_t i = 0; i < st->u.pipe->nstages; i++) {
        const struct command *cmd = st->u.pipe->stages[i];
        builtin_fn fn = builtin_lookup(cmd->name);
        cur = fn ? fn(cmd, cur, top) : extern_stage_run(cmd, cur, top);
        if (cur.type == VAL_ERROR) break;
    }
    stmt_free(st);
    return cur;
}

/* A fabricated ls-shaped table: {name, size} rows. */
static struct value sample_table(void) {
    struct value t = value_table();
    const char *names[] = { "kernel.bin", "font.ttf", "readme.txt" };
    int64_t sizes[]     = { 3 * 1024 * 1024, 800 * 1024, 512 };
    for (int i = 0; i < 3; i++) {
        struct value r = value_record();
        value_record_set(&r, "name", value_string(names[i]));
        value_record_set(&r, "size", value_filesize(sizes[i]));
        value_table_push_row(&t, r);
    }
    return t;
}

/* --- lexer ---------------------------------------------------------------- */
static void test_lexer(void) {
    printf("lexer:\n");
    size_t n = 0;
    struct token *t = lex("ls | where size > 1mb  # trailing comment", &n);
    CHECK(t && n == 7, "token count (6 + EOF)");
    CHECK(t[0].type == TOK_IDENT && t[1].type == TOK_PIPE && t[2].type == TOK_IDENT &&
          t[3].type == TOK_IDENT && t[4].type == TOK_GT && t[5].type == TOK_FILESIZE,
          "token kinds");
    CHECK(t[5].int_val == 1024 * 1024, "1mb pre-parsed to bytes");
    lex_free_tokens(t, n);

    t = lex("sort-by file.txt \"a\\nb\" 'raw\\n' 3.5", &n);
    CHECK(t && t[0].type == TOK_IDENT && t[0].lexeme_len == 7, "dash joins sort-by");
    CHECK(t[1].type == TOK_IDENT && t[1].lexeme_len == 8, "dot joins file.txt");
    CHECK(t[2].type == TOK_STRING && t[2].str_len == 3 && t[2].str[1] == '\n',
          "double-quote escapes decode");
    CHECK(t[3].type == TOK_STRING && t[3].str_len == 5 && t[3].str[3] == '\\',
          "single-quote stays raw");
    CHECK(t[4].type == TOK_FLOAT && t[4].float_val > 3.4 && t[4].float_val < 3.6,
          "float literal");
    lex_free_tokens(t, n);

    t = lex("\"unterminated", &n);
    bool has_err = false;
    for (size_t i = 0; i < n; i++) if (t[i].type == TOK_ERROR) has_err = true;
    CHECK(has_err, "unterminated string -> TOK_ERROR");
    lex_free_tokens(t, n);
}

/* --- wire round-trip ------------------------------------------------------- */
static void test_wire(void) {
    printf("wire:\n");
    struct value t = sample_table();
    struct value extras = value_list();
    value_list_push(&extras, value_int(-42));
    value_list_push(&extras, value_float(2.5));
    value_list_push(&extras, value_bool(true));
    value_list_push(&extras, value_date(1752537600));
    value_list_push(&extras, value_path("/boot/kernel"));
    value_list_push(&extras, value_null());
    struct value rec = value_record();
    value_record_set(&rec, "table", t);          /* MOVES t */
    value_record_set(&rec, "list", extras);      /* MOVES extras */

    struct wire_buf frame;
    wire_buf_init(&frame);
    CHECK(wire_serialize(&rec, &frame) == 0, "serialize");

    /* feed the frame byte-by-byte through the incremental reader -- the
     * pipe-boundary case */
    struct wire_reader rd;
    wire_reader_init(&rd);
    struct value back = value_null();
    int got = 0;
    for (size_t i = 0; i < frame.len && !got; i++) {
        if (wire_reader_feed(&rd, frame.data + i, 1) != 0) break;
        int rc = wire_reader_next(&rd, &back);
        if (rc == 1) got = 1;
        else if (rc < 0) break;
    }
    CHECK(got == 1, "incremental decode completes");
    CHECK(value_equal(&rec, &back), "round-trip equality");

    value_free(&rec);
    value_free(&back);
    wire_buf_free(&frame);
    wire_reader_free(&rd);
}

/* --- parser ---------------------------------------------------------------- */
static void test_parser(void) {
    printf("parser:\n");
    size_t n = 0;
    char err[160];

    struct token *t = lex("ls /data | where size > 1mb and name =~ \"txt\" | sort-by size", &n);
    struct stmt *s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(s && s->kind == STMT_PIPELINE && s->u.pipe->nstages == 3, "3-stage pipeline");
    CHECK(s && s->u.pipe->stages[1]->nargs == 1 &&
          s->u.pipe->stages[1]->args[0]->kind == EXPR_BINARY &&
          s->u.pipe->stages[1]->args[0]->u.binary.op == TOK_AND,
          "where's arg is one AND expr (precedence)");
    stmt_free(s);

    t = lex("echo 1 + 2 * 3", &n);
    s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(s && s->u.pipe->stages[0]->nargs == 1 &&
          s->u.pipe->stages[0]->args[0]->u.binary.op == TOK_PLUS,
          "* binds tighter than +");
    stmt_free(s);

    t = lex("where a < b < c", &n);
    s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(!s && strstr(err, "chain"), "comparison chaining rejected");

    t = lex("let x = 1mb + 512kb", &n);
    s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(s && s->kind == STMT_LET && strcmp(s->u.let.name, "x") == 0, "let statement");
    stmt_free(s);

    t = lex("echo $row.size", &n);
    s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(s && s->u.pipe->stages[0]->args[0]->kind == EXPR_FIELD, "field access parses");
    stmt_free(s);

    t = lex("ls |", &n);
    s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(!s && err[0] != '\0', "trailing pipe is an error with a message");
}

/* --- evaluator + builtins --------------------------------------------------- */
static void test_eval(void) {
    printf("eval/builtins:\n");
    struct scope top;
    scope_init(&top, NULL);

    struct value v = run("echo 1 + 2 * 3", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 7, "arithmetic precedence evaluates");
    value_free(&v);

    v = run("echo 1mb + 512kb", value_null(), &top);
    CHECK(v.type == VAL_FILESIZE && v.u.i == 1536 * 1024, "filesize arithmetic keeps the tag");
    value_free(&v);

    v = run("echo 2mb > 1000000", value_null(), &top);
    CHECK(v.type == VAL_BOOL && v.u.b, "filesize/int cross-compare coerces");
    value_free(&v);

    v = run("echo 1 / 0", value_null(), &top);
    CHECK(v.type == VAL_ERROR, "division by zero is an error value");
    value_free(&v);

    v = run("echo 6 / 2", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 3, "infix / divides");
    value_free(&v);

    v = run("echo /", value_null(), &top);
    CHECK(v.type == VAL_STRING && strcmp(v.u.s.bytes, "/") == 0,
          "prefix / is the bare word (root path)");
    value_free(&v);

    /* let + $var through a real scope */
    {
        size_t n = 0;
        struct token *t = lex("let cutoff = 600kb", &n);
        char err[160];
        struct stmt *s = parse(t, n, err, sizeof err);
        lex_free_tokens(t, n);
        struct value bound = expr_eval(s->u.let.expr, &top);
        scope_bind(&top, s->u.let.name, bound);
        stmt_free(s);

        v = run("echo $cutoff + 1", value_null(), &top);
        CHECK(v.type == VAL_FILESIZE && v.u.i == 600 * 1024 + 1, "let binding resolves via $");
        value_free(&v);

        v = run("echo $nope", value_null(), &top);
        CHECK(v.type == VAL_ERROR, "unresolved $var errors");
        value_free(&v);

        v = run("echo hi", value_null(), &top);
        CHECK(v.type == VAL_STRING && strcmp(v.u.s.bytes, "hi") == 0,
              "bare word falls back to string");
        value_free(&v);
    }

    /* where / sort-by / select / first / count on the sample table */
    v = run("where size > 600kb", sample_table(), &top);
    CHECK(v.type == VAL_TABLE && v.u.table->count == 2, "where filters rows");
    value_free(&v);

    v = run("where size", sample_table(), &top);
    CHECK(v.type == VAL_ERROR, "non-boolean condition is an error (strict truthiness)");
    value_free(&v);

    v = run("sort-by size", sample_table(), &top);
    CHECK(v.type == VAL_TABLE && v.u.table->count == 3 &&
          record_field(&v.u.table->rows[0], "size")->u.i == 512 &&
          record_field(&v.u.table->rows[2], "size")->u.i == 3 * 1024 * 1024,
          "sort-by orders ascending");
    value_free(&v);

    v = run("where name =~ \"txt\" | count", sample_table(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 1, "=~ substring + count");
    value_free(&v);

    v = run("select name | first 2 | count", sample_table(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 2, "select | first | count chain");
    value_free(&v);

    v = run("sort-by size | first | select name", sample_table(), &top);
    CHECK(v.type == VAL_TABLE && v.u.table->count == 1 &&
          strcmp(record_field(&v.u.table->rows[0], "name")->u.s.bytes, "readme.txt") == 0,
          "smallest file wins the full chain");
    value_free(&v);

    v = run("frobnicate", value_null(), &top);
    CHECK(v.type == VAL_ERROR, "unknown command surfaces an error (host stub)");
    value_free(&v);

    /* the standard-name pure batch: head/tail/last, reverse, get, wc, help */
    v = run("head 2 | count", sample_table(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 2, "head is first");
    value_free(&v);

    v = run("last 2 | first | get name", sample_table(), &top);
    CHECK(v.type == VAL_LIST && v.u.list.count == 1 &&
          strcmp(v.u.list.items[0].u.s.bytes, "font.ttf") == 0,
          "last 2 | first picks the middle row; get pulls the column");
    value_free(&v);

    v = run("reverse | first | get name", sample_table(), &top);
    CHECK(v.type == VAL_LIST && v.u.list.count == 1 &&
          strcmp(v.u.list.items[0].u.s.bytes, "readme.txt") == 0,
          "reverse flips row order");
    value_free(&v);

    v = run("get size | count", sample_table(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 3, "get column -> list of 3");
    value_free(&v);

    v = run("echo \"one two\\nthree\" | wc", value_null(), &top);
    CHECK(v.type == VAL_RECORD &&
          value_record_get(&v, "lines") && value_record_get(&v, "lines")->u.i == 2 &&
          value_record_get(&v, "words") && value_record_get(&v, "words")->u.i == 3,
          "wc counts lines and words of text");
    value_free(&v);

    v = run("help | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i >= 20, "help lists the command set");
    value_free(&v);

    /* null cells (a dir's size): comparisons are FALSE, sorts put nulls first */
    {
        struct value nt = value_table();
        struct value r1 = value_record();
        value_record_set(&r1, "name", value_string("bin"));
        value_record_set(&r1, "size", value_null());
        value_table_push_row(&nt, r1);
        struct value r2 = value_record();
        value_record_set(&r2, "name", value_string("kernel.bin"));
        value_record_set(&r2, "size", value_filesize(3 * 1024 * 1024));
        value_table_push_row(&nt, r2);

        struct value c1 = value_clone(&nt);
        v = run("where size > 1mb | count", c1, &top);
        CHECK(v.type == VAL_INT && v.u.i == 1, "null > x is false (dir rows don't match)");
        value_free(&v);

        struct value c2 = value_clone(&nt);
        v = run("where size < 1mb | count", c2, &top);
        CHECK(v.type == VAL_INT && v.u.i == 0, "null < x is ALSO false (SQL semantics)");
        value_free(&v);

        v = run("sort-by size", nt, &top);
        CHECK(v.type == VAL_TABLE && v.u.table->count == 2 &&
              strcmp(record_field(&v.u.table->rows[0], "name")->u.s.bytes, "bin") == 0,
              "sort-by puts null keys first");
        value_free(&v);
    }

    scope_free(&top);
}

/* --- history ring ---------------------------------------------------------- */
static void test_hist(void) {
    printf("history:\n");
    CHECK(hist_count() == 0, "starts empty");

    hist_push("ls");
    hist_push("ps");
    CHECK(hist_count() == 2 && strcmp(hist_get(0), "ls") == 0 &&
          strcmp(hist_get(1), "ps") == 0, "stores in order (0 = oldest)");

    hist_push("ps");
    CHECK(hist_count() == 2, "ignoredups: repeating the last line is collapsed");
    hist_push("ls");
    CHECK(hist_count() == 3, "a NON-adjacent repeat is still stored");

    hist_push("");
    CHECK(hist_count() == 3, "empty lines are never stored");

    /* Up-arrow recall walks back from the newest: the shell asks for
     * hist_get(count - browse), so browse=1 must be the last command. */
    CHECK(strcmp(hist_get(hist_count() - 1), "ls") == 0,
          "browse=1 recalls the LAST command (the core ask)");

    /* overfill: oldest falls off, newest survives, count saturates */
    for (int i = 0; i < HIST_MAX + 10; i++) {
        char line[32];
        snprintf(line, sizeof line, "cmd%d", i);
        hist_push(line);
    }
    CHECK(hist_count() == HIST_MAX, "count saturates at HIST_MAX");
    {
        char want[32];
        snprintf(want, sizeof want, "cmd%d", HIST_MAX + 10 - 1);
        CHECK(strcmp(hist_get(hist_count() - 1), want) == 0,
              "newest survives the ring wrap");
        CHECK(strcmp(hist_get(0), "ls") != 0, "oldest fell off the ring");
    }
    CHECK(hist_get(hist_count()) == NULL, "out-of-range index is NULL");
}

int main(void) {
    printf("=== shell host tests ===\n");
    test_lexer();
    test_wire();
    test_parser();
    test_eval();
    test_hist();
    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nall green\n", g_fail);
    return g_fail;
}
