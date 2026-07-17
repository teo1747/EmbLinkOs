/* ==========================================================================
 * builtins.c -- the PURE builtins (no OS dependency; host-testable):
 * echo, where, select, sort-by, first, count, plus the registry.
 * ls (OS-backed) lives in builtins_os.c.
 * ========================================================================== */
#include "builtins/builtins.h"
#include "hist/hist.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Shared helpers
 * ------------------------------------------------------------------------- */
static struct value err_args(const struct command *cmd, const char *what) {
    char msg[96];
    snprintf(msg, sizeof msg, "%s: %s", cmd->name, what);
    return value_error(msg);
}

/* Most row-transforms want "the input as rows": a TABLE. (A LIST of records
 * would be honest too, but v1 keeps it to tables -- ls and every transform
 * emit tables, so a list here means the user piped something odd.) */
static struct table *input_table(const struct command *cmd, struct value *input,
                                 struct value *out_err) {
    if (input->type != VAL_TABLE) {
        char msg[96];
        snprintf(msg, sizeof msg, "%s: expected a table from the previous stage (got %s)",
                 cmd->name, input->type == VAL_NULL ? "nothing" : "a non-table value");
        *out_err = value_error(msg);
        return NULL;
    }
    return input->u.table;
}

/* -------------------------------------------------------------------------
 * echo expr* -- evaluate args in the driver scope. The identity/constructor
 * builtin: `echo 1mb + 512kb` prints a filesize, `echo $x` prints a binding.
 * ------------------------------------------------------------------------- */
static struct value bi_echo(const struct command *cmd, struct value input,
                            struct scope *env) {
    value_free(&input);   /* echo ignores pipeline input */
    if (cmd->nargs == 0) return value_null();
    if (cmd->nargs == 1) return expr_eval(cmd->args[0], env);
    struct value list = value_list();
    for (size_t i = 0; i < cmd->nargs; i++) {
        struct value v = expr_eval(cmd->args[i], env);
        if (v.type == VAL_ERROR) { value_free(&list); return v; }
        value_list_push(&list, v);
    }
    return list;
}

/* -------------------------------------------------------------------------
 * where <expr> -- keep rows where the expr is TRUE (strict truthiness: a
 * non-boolean predicate result is an error naming the offending type).
 * ------------------------------------------------------------------------- */
static struct value bi_where(const struct command *cmd, struct value input,
                             struct scope *env) {
    if (cmd->nargs != 1) {
        value_free(&input);
        return err_args(cmd, "takes exactly one condition (e.g. where size > 1mb)");
    }
    struct value err;
    struct table *t = input_table(cmd, &input, &err);
    if (!t) { value_free(&input); return err; }

    struct value out = value_table();
    for (size_t i = 0; i < t->count; i++) {
        struct scope rowscope;
        scope_init(&rowscope, env);
        rowscope.row = &t->rows[i];
        struct value cond = expr_eval(cmd->args[0], &rowscope);
        scope_free(&rowscope);

        if (cond.type == VAL_ERROR) { value_free(&out); value_free(&input); return cond; }
        int truth = value_truthy(&cond);
        value_free(&cond);
        if (truth < 0) {
            value_free(&out);
            value_free(&input);
            return err_args(cmd, "condition must be a boolean (use ==, >, =~ ...)");
        }
        if (truth)
            value_table_push_row(&out, record_clone_value(&t->rows[i]));
    }
    value_free(&input);
    return out;
}

/* -------------------------------------------------------------------------
 * select col* -- project columns, in the order named. A row missing a named
 * field gets a null cell (heterogeneous rows are legal).
 * ------------------------------------------------------------------------- */
static struct value bi_select(const struct command *cmd, struct value input,
                              struct scope *env) {
    (void)env;
    if (cmd->nargs == 0) {
        value_free(&input);
        return err_args(cmd, "needs column names (e.g. select name size)");
    }
    struct value err;
    struct table *t = input_table(cmd, &input, &err);
    if (!t) { value_free(&input); return err; }

    /* column names come from the exprs as WORDS (bare idents / strings) */
    const char *cols[16];
    if (cmd->nargs > 16) { value_free(&input); return err_args(cmd, "too many columns (max 16)"); }
    for (size_t i = 0; i < cmd->nargs; i++) {
        cols[i] = expr_as_word(cmd->args[i]);
        if (!cols[i]) { value_free(&input); return err_args(cmd, "column names must be plain words"); }
    }

    struct value out = value_table();
    for (size_t r = 0; r < t->count; r++) {
        struct value row = value_record();
        for (size_t c = 0; c < cmd->nargs; c++) {
            const struct value *f = record_field(&t->rows[r], cols[c]);
            value_record_set(&row, cols[c], f ? value_clone(f) : value_null());
        }
        value_table_push_row(&out, row);
    }
    value_free(&input);
    return out;
}

/* -------------------------------------------------------------------------
 * sort-by <expr> -- ascending by the key expr, evaluated once per row.
 * Insertion sort: stable, no qsort_r dependency, fine at shell scale.
 * Unorderable key pairs (mixed types) are an error, not a silent shuffle.
 * ------------------------------------------------------------------------- */
static struct value bi_sort_by(const struct command *cmd, struct value input,
                               struct scope *env) {
    if (cmd->nargs != 1) {
        value_free(&input);
        return err_args(cmd, "takes exactly one key (e.g. sort-by size)");
    }
    struct value err;
    struct table *t = input_table(cmd, &input, &err);
    if (!t) { value_free(&input); return err; }

    size_t n = t->count;
    struct value *keys = n ? (struct value *)calloc(n, sizeof(*keys)) : NULL;
    if (n && !keys) { value_free(&input); return value_error("out of memory"); }

    for (size_t i = 0; i < n; i++) {
        struct scope rowscope;
        scope_init(&rowscope, env);
        rowscope.row = &t->rows[i];
        keys[i] = expr_eval(cmd->args[0], &rowscope);
        scope_free(&rowscope);
        if (keys[i].type == VAL_ERROR) {
            struct value e = keys[i];
            keys[i] = value_null();          /* don't double-free below */
            for (size_t k = 0; k < n; k++) value_free(&keys[k]);
            free(keys);
            value_free(&input);
            return e;
        }
    }

    /* insertion sort rows + keys in lockstep (rows are struct record by
     * value -- moving one is a struct copy, cheap: three pointers) */
    for (size_t i = 1; i < n; i++) {
        struct record row = t->rows[i];
        struct value  key = keys[i];
        size_t j = i;
        while (j > 0) {
            int c;
            if (eval_compare_values(&keys[j - 1], &key, &c) != 0) {
                for (size_t k = 0; k < n; k++) value_free(&keys[k]);
                free(keys);
                value_free(&input);
                return err_args(cmd, "rows have mixed key types; can't order them");
            }
            if (c <= 0) break;
            t->rows[j] = t->rows[j - 1];
            keys[j] = keys[j - 1];
            j--;
        }
        t->rows[j] = row;
        keys[j] = key;
    }

    for (size_t k = 0; k < n; k++) value_free(&keys[k]);
    free(keys);
    return input;   /* sorted in place; ownership passes through */
}

/* -------------------------------------------------------------------------
 * first [n] -- the first n rows of a table (or items of a list). Default 1.
 * ------------------------------------------------------------------------- */
static struct value bi_first(const struct command *cmd, struct value input,
                             struct scope *env) {
    int64_t n = 1;
    if (cmd->nargs >= 1) {
        struct value nv = expr_eval(cmd->args[0], env);
        if (nv.type == VAL_ERROR) { value_free(&input); return nv; }
        if (nv.type != VAL_INT || nv.u.i < 0) {
            value_free(&nv);
            value_free(&input);
            return err_args(cmd, "takes a non-negative count");
        }
        n = nv.u.i;
    }

    if (input.type == VAL_TABLE) {
        struct value out = value_table();
        struct table *t = input.u.table;
        for (size_t i = 0; i < t->count && (int64_t)i < n; i++)
            value_table_push_row(&out, record_clone_value(&t->rows[i]));
        value_free(&input);
        return out;
    }
    if (input.type == VAL_LIST) {
        struct value out = value_list();
        for (size_t i = 0; i < input.u.list.count && (int64_t)i < n; i++)
            value_list_push(&out, value_clone(&input.u.list.items[i]));
        value_free(&input);
        return out;
    }
    value_free(&input);
    return err_args(cmd, "expected a table or list from the previous stage");
}

/* -------------------------------------------------------------------------
 * count -- how many rows / items; a scalar counts as 1, null as 0.
 * ------------------------------------------------------------------------- */
static struct value bi_count(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)cmd; (void)env;
    int64_t n;
    switch (input.type) {
    case VAL_TABLE: n = (int64_t)input.u.table->count; break;
    case VAL_LIST:  n = (int64_t)input.u.list.count;   break;
    case VAL_NULL:  n = 0; break;
    default:        n = 1; break;
    }
    value_free(&input);
    return value_int(n);
}

/* -------------------------------------------------------------------------
 * last [n] / tail [n] -- the LAST n rows/items (default 1). first's mirror.
 * ------------------------------------------------------------------------- */
static struct value bi_last(const struct command *cmd, struct value input,
                            struct scope *env) {
    int64_t n = 1;
    if (cmd->nargs >= 1) {
        struct value nv = expr_eval(cmd->args[0], env);
        if (nv.type == VAL_ERROR) { value_free(&input); return nv; }
        if (nv.type != VAL_INT || nv.u.i < 0) {
            value_free(&nv);
            value_free(&input);
            return err_args(cmd, "takes a non-negative count");
        }
        n = nv.u.i;
    }

    if (input.type == VAL_TABLE) {
        struct table *t = input.u.table;
        size_t start = (int64_t)t->count > n ? t->count - (size_t)n : 0;
        struct value out = value_table();
        for (size_t i = start; i < t->count; i++)
            value_table_push_row(&out, record_clone_value(&t->rows[i]));
        value_free(&input);
        return out;
    }
    if (input.type == VAL_LIST) {
        size_t start = (int64_t)input.u.list.count > n ? input.u.list.count - (size_t)n : 0;
        struct value out = value_list();
        for (size_t i = start; i < input.u.list.count; i++)
            value_list_push(&out, value_clone(&input.u.list.items[i]));
        value_free(&input);
        return out;
    }
    value_free(&input);
    return err_args(cmd, "expected a table or list from the previous stage");
}

/* -------------------------------------------------------------------------
 * reverse -- rows/items in reverse order.
 * ------------------------------------------------------------------------- */
static struct value bi_reverse(const struct command *cmd, struct value input,
                               struct scope *env) {
    (void)env;
    if (input.type == VAL_TABLE) {
        struct table *t = input.u.table;
        for (size_t i = 0, j = t->count; i + 1 < j; i++, j--) {
            struct record tmp = t->rows[i];
            t->rows[i] = t->rows[j - 1];
            t->rows[j - 1] = tmp;
        }
        return input;   /* reversed in place */
    }
    if (input.type == VAL_LIST) {
        for (size_t i = 0, j = input.u.list.count; i + 1 < j; i++, j--) {
            struct value tmp = input.u.list.items[i];
            input.u.list.items[i] = input.u.list.items[j - 1];
            input.u.list.items[j - 1] = tmp;
        }
        return input;
    }
    value_free(&input);
    return err_args(cmd, "expected a table or list from the previous stage");
}

/* -------------------------------------------------------------------------
 * get <field> -- a record's field, or a table column as a LIST. The bridge
 * from "rows" to "plain values" (`ls | get name`).
 * ------------------------------------------------------------------------- */
static struct value bi_get(const struct command *cmd, struct value input,
                           struct scope *env) {
    (void)env;
    if (cmd->nargs != 1) {
        value_free(&input);
        return err_args(cmd, "takes one field name (e.g. get name)");
    }
    const char *field = expr_as_word(cmd->args[0]);
    if (!field) { value_free(&input); return err_args(cmd, "field must be a plain word"); }

    if (input.type == VAL_RECORD) {
        const struct value *f = value_record_get(&input, field);
        struct value out = f ? value_clone(f) : value_null();
        if (!f) {
            value_free(&input);
            return err_args(cmd, "record has no such field");
        }
        value_free(&input);
        return out;
    }
    if (input.type == VAL_TABLE) {
        struct value out = value_list();
        struct table *t = input.u.table;
        for (size_t i = 0; i < t->count; i++) {
            const struct value *f = record_field(&t->rows[i], field);
            value_list_push(&out, f ? value_clone(f) : value_null());
        }
        value_free(&input);
        return out;
    }
    value_free(&input);
    return err_args(cmd, "expected a record or table from the previous stage");
}

/* -------------------------------------------------------------------------
 * wc -- counts. A string counts lines/words/bytes (cat file | wc); a
 * table/list reports its row/item count as a record for consistency.
 * ------------------------------------------------------------------------- */
static struct value bi_wc(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    struct value out = value_record();
    if (input.type == VAL_STRING || input.type == VAL_PATH) {
        int64_t lines = 0, words = 0, bytes = (int64_t)input.u.s.len;
        bool in_word = false;
        for (size_t i = 0; i < input.u.s.len; i++) {
            char c = input.u.s.bytes[i];
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') in_word = false;
            else if (!in_word) { in_word = true; words++; }
        }
        if (bytes && input.u.s.bytes[input.u.s.len - 1] != '\n') lines++;
        value_record_set(&out, "lines", value_int(lines));
        value_record_set(&out, "words", value_int(words));
        value_record_set(&out, "bytes", value_int(bytes));
    } else if (input.type == VAL_TABLE) {
        value_record_set(&out, "rows", value_int((int64_t)input.u.table->count));
    } else if (input.type == VAL_LIST) {
        value_record_set(&out, "items", value_int((int64_t)input.u.list.count));
    } else {
        value_free(&out);
        value_free(&input);
        return err_args(cmd, "expected text, a table or a list");
    }
    value_free(&input);
    return out;
}

/* -------------------------------------------------------------------------
 * history -- past commands, as a Table {n, command}. A table, of course:
 * `history | where command =~ "ls"` is the payoff of not printing text.
 * ------------------------------------------------------------------------- */
static struct value bi_history(const struct command *cmd, struct value input,
                               struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    struct value out = value_table();
    size_t n = hist_count();
    for (size_t i = 0; i < n; i++) {
        const char *line = hist_get(i);
        if (!line) continue;
        struct value r = value_record();
        value_record_set(&r, "n", value_int((int64_t)i + 1));   /* 1-based, oldest first */
        value_record_set(&r, "command", value_string(line));
        value_table_push_row(&out, r);
    }
    return out;
}

/* -------------------------------------------------------------------------
 * help -- the command reference, as a table (of course it's a table).
 * ------------------------------------------------------------------------- */
static struct value bi_help(const struct command *cmd, struct value input,
                            struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    static const struct { const char *name, *usage; } rows[] = {
        { "ls [path]",        "list a directory as a table {name,type,size}" },
        { "cat <file>",       "a file's contents, as a string" },
        { "cd [dir] / pwd",   "change / show the shell's working directory" },
        { "mkdir <dir>",      "create a directory" },
        { "rmdir <dir>",      "remove an EMPTY directory" },
        { "touch <file>",     "create an empty file" },
        { "rm <file>",        "remove a file" },
        { "cp <src> <dst>",   "copy a file" },
        { "mv <src> <dst>",   "move (copy + remove) a file" },
        { "save <path>",      "write the piped value to a file as text" },
        { "ps",               "processes, as a table {pid,ppid,state,pri,kind}" },
        { "kill <pid>",       "terminate a process by pid" },
        { "env",              "the environment passed to spawned commands, as a table" },
        { "env set K V",      "set a variable (children get it; nothing is inherited)" },
        { "env unset K",      "remove a variable" },
        { "echo <expr>...",   "evaluate expressions (echo 1mb + 512kb)" },
        { "where <cond>",     "keep rows where the condition is true" },
        { "select <col>...",  "project columns" },
        { "sort-by <expr>",   "sort rows ascending by a key" },
        { "first/head [n]",   "the first n rows (default 1)" },
        { "last/tail [n]",    "the last n rows (default 1)" },
        { "reverse",          "rows in reverse order" },
        { "get <field>",      "a column as a list / a record's field" },
        { "count",            "how many rows/items" },
        { "wc",               "line/word/byte counts of text" },
        { "history",          "past commands as a table (Up/Down recalls them)" },
        { "uptime / date",    "system uptime / wall-clock date" },
        { "clear",            "clear the terminal" },
        { "let x = <expr>",   "bind a variable ($x)" },
        { "exit",             "leave the shell" },
    };
    struct value out = value_table();
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        struct value r = value_record();
        value_record_set(&r, "command", value_string(rows[i].name));
        value_record_set(&r, "what",    value_string(rows[i].usage));
        value_table_push_row(&out, r);
    }
    return out;
}

/* -------------------------------------------------------------------------
 * Registry: the PURE half; builtin_lookup_os() (builtins_os.c / host stub)
 * carries everything that needs a syscall.
 * ------------------------------------------------------------------------- */
builtin_fn builtin_lookup(const char *name) {
    static const struct { const char *name; builtin_fn fn; } tab[] = {
        { "echo",    bi_echo    },
        { "where",   bi_where   },
        { "select",  bi_select  },
        { "sort-by", bi_sort_by },
        { "first",   bi_first   },
        { "head",    bi_first   },   /* the standard name, same stage */
        { "last",    bi_last    },
        { "tail",    bi_last    },
        { "reverse", bi_reverse },
        { "get",     bi_get     },
        { "count",   bi_count   },
        { "wc",      bi_wc      },
        { "history", bi_history },
        { "help",    bi_help    },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(tab[i].name, name) == 0) return tab[i].fn;
    return builtin_lookup_os(name);
}
