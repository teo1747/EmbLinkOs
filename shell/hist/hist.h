/* ==========================================================================
 * hist.h -- the shell's command history ring.
 *
 * Lives in the SHELL, not the terminal -- the same split Linux has (readline
 * is bash's, not xterm's): the shell owns the line buffer and does its own
 * echo, so it's the only thing that can erase a line and re-draw a recalled
 * one. The terminal stays a dumb byte pipe and simply forwards the arrow
 * keys through (it keeps PgUp/PgDn for its own scrollback).
 *
 * A fixed ring of fixed-width slots: no allocation, no ownership questions,
 * and an oldest-falls-off policy that can't grow without bound on a shell
 * that may run for weeks. Sized for a person's working set, not a log.
 * ========================================================================== */
#ifndef __EMBK_HIST_H__
#define __EMBK_HIST_H__

#include <stddef.h>

#define HIST_MAX      32    /* entries kept; oldest falls off */
#define HIST_LINE_MAX 512   /* must match the REPL's line buffer */

/* Record a submitted line. Empty lines are never stored, and a line
 * identical to the most recent one is collapsed (bash's `ignoredups`) --
 * hammering Enter shouldn't flood the ring. */
void hist_push(const char *line);

/* How many entries are live (saturates at HIST_MAX). */
size_t hist_count(void);

/* Entry `i`, 0 = OLDEST .. count-1 = newest. NULL if out of range.
 * Borrowed: valid until that slot is overwritten. */
const char *hist_get(size_t i);

#endif /* __EMBK_HIST_H__ */
