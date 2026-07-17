/* ==========================================================================
 * eval.h -- the two evaluators (the sketch's structural split):
 *
 *   - PIPELINE evaluator: runs a sequence of commands, threading a
 *     materialized Value from each stage into the next (v1 = materialize,
 *     not stream: a stage is `Value transform(Value)`; fine at shell scale).
 *   - EXPRESSION evaluator: evaluates one expr AST to a single Value in a
 *     scope. Called BY commands (where/sort-by) once per row with the row's
 *     fields in scope.
 *
 * SETTLED decisions (were open in the sketch; rationale in docs/SHELL.md):
 *   - TRUTHINESS IS STRICT: Bool is itself, Null is false, anything else in
 *     a boolean context is an ERROR ("expected a boolean"), not a guess.
 *     A structured shell earns its keep by being explicit.
 *   - ERROR MODEL: errors are VAL_ERROR values threaded through the pipeline;
 *     the first one aborts the remaining stages and the driver prints it and
 *     keeps the prompt alive. No separate channel, no early-abort longjmp.
 *   - Bare words (EXPR_COLUMN) resolve scope-first, then FALL BACK to their
 *     own name as a STRING -- so `echo hi` and `ls /data` read naturally
 *     without quoting, while `where size > 1mb` still resolves `size` from
 *     the row. $vars never fall back: an unresolved $x is an error.
 *
 * COERCION lives HERE (the value layer deliberately refused it):
 *   - the numeric family (INT / FILESIZE / DATE / FLOAT) is mutually
 *     comparable; FLOAT contaminates to double, otherwise int64 compare.
 *   - STRING and PATH are mutually comparable text.
 *   - arithmetic keeps the "richer" tag: INT+FILESIZE = FILESIZE,
 *     INT+DATE = DATE, FLOAT anywhere = FLOAT; FILESIZE+DATE is an error.
 *   - `=~` is substring-match over text (real regex is future work).
 * ========================================================================== */
#ifndef __EMBK_EVAL_H__
#define __EMBK_EVAL_H__

#include "parse/parse.h"
#include "value/value.h"

/* -------------------------------------------------------------------------
 * Scope: lexical chain + the current row. `let` bindings first, then the
 * row's fields, then the parent -- one lookup path for columns and $vars
 * (the parser's EXPR_COLUMN/EXPR_VAR split only sharpens error messages).
 * ------------------------------------------------------------------------- */
struct scope {
    struct scope        *parent;   /* NULL at the top */
    const struct record *row;      /* borrowed; NULL when no row context */
    char                **names;   /* owned `let` binding names  */
    struct value         *values;  /* owned `let` binding values */
    size_t                count, cap;
};

void scope_init(struct scope *s, struct scope *parent);
void scope_free(struct scope *s);                    /* frees bindings, not parent */
int  scope_bind(struct scope *s, const char *name, struct value v);
     /* MOVES v in; replaces an existing binding of the same name. 0 / -1 OOM. */
const struct value *scope_lookup(const struct scope *s, const char *name);
     /* bindings -> row field -> parent; NULL if nowhere. BORROWED. */

/* -------------------------------------------------------------------------
 * Expression evaluation -> an OWNED value (possibly VAL_ERROR).
 * ------------------------------------------------------------------------- */
struct value expr_eval(const struct expr *e, struct scope *env);

/* Strict truthiness (see header comment). Returns 0/1, or -1 if the value
 * isn't boolean-shaped -- callers turn that into a VAL_ERROR. */
int value_truthy(const struct value *v);

/* Ordering WITH the evaluator's coercion (numeric family cross-compares,
 * text family cross-compares). 0 -> *out is -1/0/1; -1 -> no meaningful
 * order (caller builds the error). What sort-by keys on. */
int eval_compare_values(const struct value *a, const struct value *b, int *out);

/* Convenience for builtins that take a WORD argument (a path, a column
 * name): an EXPR_COLUMN's name or a literal STRING/PATH, WITHOUT evaluating
 * it against the scope. Returns a BORROWED char* or NULL if the expr isn't
 * word-shaped. */
const char *expr_as_word(const struct expr *e);

/* -------------------------------------------------------------------------
 * Pipeline evaluation. `top` is the driver's persistent scope (lets live
 * there across REPL lines). Returns the final OWNED value -- VAL_ERROR if
 * any stage failed (remaining stages are skipped).
 * ------------------------------------------------------------------------- */
struct value pipeline_run(const struct pipeline *pl, struct scope *top);

/* One external stage: spawn /name.elf with the pipe plumbing (fd 3 out,
 * fd 0 structured in when there's input), collect its frames, reap it.
 * Implemented in eval_extern.c ON TARGET; the host test build links a stub
 * that returns an error ("external programs need the OS"). Takes ownership
 * of `input`. */
struct value extern_stage_run(const struct command *cmd, struct value input,
                              struct scope *env);

#endif /* __EMBK_EVAL_H__ */
