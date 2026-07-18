#ifndef __TTY_H__
#define __TTY_H__

#include "include/types.h"
#include <stddef.h>
#include <stdint.h>


/* The console line discipline. One global tty backs the ONE console -- same
 * stance as the single CTRL-C interrupt slot in keyboard.c: EmbLink has one
 * console, not a pty multiverse, so per-fd tty state would be structure with
 * no secondary instances exist. The CONSOLE fd's read op (fs/fd.c) is a thin 
 * layer shim over tty_read(); everything a terminal does that a raw keyboard
 * does not -- line buffering, echo, EOF-on-^D -- lives here. */

enum tty_mode {
    TTY_COOKED = 0,      // The tty is in cooked mode (line buffering, echo, etc.)
    TTY_RAW    = 1,      // The tty is in raw mode (no line buffering, no echo, etc.)
};

/* Blocking console read. Cooked: blocks until a full line is available (ENTER) or
 * ^D (EOF) is encountered/flushes the input buffer. then hands over the line across
 * as many calls as the caller's buffer needs. RAW: returns immediately with
 * whatever input is available, no line buffering, no echo. EMBK_OK with *out_read
 * set, or -EMBK_ECANCELED if the caller was ^C-canceled while waiting. *out_read==0
 * with EMBK_OK is EOF (^D on an empty line) -- never "nothing type yet". */
int tty_read(char *buf, size_t len, size_t *out_read);

/* Global mode (see above). Default is TTY_COOKED == 0, so the zero-initialized
 * global is already cooked before anyone sets it. */
void tty_set_mode(enum tty_mode mode);
enum tty_mode tty_get_mode(void);

int tty_run_selftests(void);     // 'test tty' -- injection-driven, no real keys


#endif /* __TTY_H__ */