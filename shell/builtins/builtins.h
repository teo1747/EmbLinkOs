/* ==========================================================================
 * builtins.h -- the builtin command registry. A builtin is one pipeline
 * stage: it takes the previous stage's materialized Value (OWNED -- the
 * builtin must free or incorporate it), the command node (for its args),
 * and the driver's scope; it returns an OWNED Value (VAL_ERROR aborts the
 * pipeline).
 *
 * v1 set (each documented in docs/SHELL.md):
 *   ls [path]          -> Table {name, type, size}         (OS-backed)
 *   echo expr*         -> the value(s); 0 args=null, 1=itself, n=List
 *   where <expr>       -> filter rows where expr is true (strict booleans)
 *   select col*        -> project columns (missing field -> null cell)
 *   sort-by <expr>     -> sort rows ascending by the key expr
 *   first [n]          -> first n rows/items (default 1)
 *   count              -> Int: rows of a table / items of a list / 1 scalar
 *
 * Pure builtins live in builtins.c (host-testable); ls lives in
 * builtins_os.c (needs embk_readdir/embk_stat -- the host test build links
 * a stub instead).
 * ========================================================================== */
#ifndef __EMBK_BUILTINS_H__
#define __EMBK_BUILTINS_H__

#include "parse/parse.h"
#include "eval/eval.h"
#include "value/value.h"

typedef struct value (*builtin_fn)(const struct command *cmd,
                                   struct value input, struct scope *env);

/* NULL if `name` isn't a builtin (the evaluator then spawns /name.elf).
 * Tries the PURE table first, then the OS table below. */
builtin_fn builtin_lookup(const char *name);

/* The OS-backed half of the registry (ls, cat, rm, mkdir, cp, mv, cd, pwd,
 * touch, save, ps, kill, uptime, date, clear) -- implemented in
 * builtins_os.c on target; the host test build's stub returns NULL for
 * everything (those names then fail as unknown commands, which is fine:
 * the host tests only exercise the pure set). */
builtin_fn builtin_lookup_os(const char *name);

#endif /* __EMBK_BUILTINS_H__ */
