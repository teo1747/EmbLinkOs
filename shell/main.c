/* ==========================================================================
 * main.c -- the EmbLink shell driver (shell.elf). Ties the stages together:
 * read a line -> lex -> parse -> evaluate -> sink.
 *
 * Two modes:
 *   shell.elf              interactive REPL on fds 0/1 (the console)
 *   shell.elf -c "line"    run ONE line, exit 0 on success / 1 on error
 *                          (what `test shell` and scripts drive)
 *
 * The SINK (the sketch's last box): if fd 3 is a structured pipe
 * (sval_structured_out -- the shell itself was composed into a bigger
 * pipeline), EMIT the result as a frame; otherwise pretty-print to fd 1.
 *
 * Line editing is deliberately minimal: the kernel console delivers raw
 * keystrokes with NO echo, so the shell echoes what it reads and handles
 * backspace itself. History/cursor keys are the future terminal app's job.
 * ========================================================================== */
#include "lex/lex.h"
#include "parse/parse.h"
#include "eval/eval.h"
#include "sval/sval.h"
#include "hist/hist.h"
#include "embk.h"        /* EMBK_KEY_UP/DOWN -- the kernel's private arrow codes */
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>


#define LINE_MAX_LEN 512

/* True when THIS shell's stdin is the machine console and we've put it in raw mode
 * -- i.e. we own the console session. False under the Terminal app or '-c'
 * (stdin is a pipe, not the console): then the global console mode is someone
 * else's property and we must never touch it. same fstat-shape probe as sval.c's
 * fd_is_fifo, for the same reason: ride the standard stat shape the newlib stub 
 *already fills. */
static int g_console_session = 0;

static bool stdin_is_console(void) {
    struct stat st;
    if (fstat(0, &st) != 0) return false;
    return S_ISCHR(st.st_mode);                         /* the console fstats as a character device */
}

/* The termions dance, EmbLink edition. Suspend = hand the line discipline back to
 * the tty (cooked) for the lifetime of a foreground child, so a child that reads
 * stdin gets echo/editing/^D-EOF from the kernel, knowing nothing about this shell.
 * Resume = take it back for our own editor. Both are no-ops unless we own the session,
 * so eval_extern can call them unconditionally. */
void shell_tty_suspend(void) {
    if (g_console_session) embk_tty_mode(EMBK_TTY_COOKED);
}

void shell_tty_resume(void) {
    if (g_console_session) embk_tty_mode(EMBK_TTY_RAW);
}

static void puts1(const char *s) { write(1, s, strlen(s)); }

/* -------------------------------------------------------------------------
 * Run one line through the stages. Returns 0 on success, 1 on any error
 * (which has already been printed).
 * ------------------------------------------------------------------------- */
static int run_line(const char *line, struct scope *top) {
    size_t ntoks = 0;
    struct token *toks = lex(line, &ntoks);
    if (!toks) { puts1("error: out of memory\n"); return 1; }

    /* an empty line lexes to just EOF -- nothing to do */
    if (ntoks == 1 && toks[0].type == TOK_EOF) {
        lex_free_tokens(toks, ntoks);
        return 0;
    }

    char err[160];
    struct stmt *st = parse(toks, ntoks, err, sizeof err);
    lex_free_tokens(toks, ntoks);
    if (!st) {
        puts1("error: ");
        puts1(err);
        puts1("\n");
        return 1;
    }

    int rc = 0;
    if (st->kind == STMT_LET) {
        struct value v = expr_eval(st->u.let.expr, top);
        if (v.type == VAL_ERROR) {
            puts1("error: ");
            puts1(value_error_msg(&v));
            puts1("\n");
            value_free(&v);
            rc = 1;
        } else if (scope_bind(top, st->u.let.name, v) != 0) {
            puts1("error: out of memory\n");
            rc = 1;
        }
    } else {
        struct value out = pipeline_run(st->u.pipe, top);
        if (out.type == VAL_ERROR) {
            puts1("error: ");
            puts1(value_error_msg(&out));
            puts1("\n");
            rc = 1;
        } else if (out.type != VAL_NULL) {
            if (sval_structured_out())
                (void)sval_emit(&out);       /* composed into a larger pipeline */
            else
                (void)sval_print(&out, 1);   /* a human is watching */
        }
        value_free(&out);
    }
    stmt_free(st);
    return rc;
}

/* Wipe the on-screen line and replace it with `text` -- what an Up/Down
 * recall does. "\b \b" per char is the portable erase (back up, overwrite
 * with a space, back up again); the terminal and the kernel console both
 * understand it, so no cursor-addressing escapes are needed. */
static void line_replace(char *buf, size_t cap, size_t *len, const char *text) {
    while (*len > 0) { puts1("\b \b"); (*len)--; }
    if (!text) { buf[0] = '\0'; return; }
    size_t n = strlen(text);
    if (n >= cap) n = cap - 1;
    memcpy(buf, text, n);
    buf[n] = '\0';
    *len = n;
    if (n) write(1, buf, n);                    /* echo the recalled line */
}

/* -------------------------------------------------------------------------
 * Interactive line read: echo (the console doesn't), backspace, enter, and
 * Up/Down history recall.
 *
 * The kernel delivers arrows as its own private single-byte codes
 * (EMBK_KEY_UP/DOWN = 0x13/0x14, see the keyboard driver) -- NOT ANSI escape
 * sequences -- so recall is a plain byte check, no escape-state machine.
 * That's true whether the shell is read by the Terminal (which forwards the
 * codes) or the kernel console (whose fd-0 read yields them directly).
 *
 * `browse` = how many steps back from the live line: 0 = the line being
 * typed, 1 = newest history entry, ... count = oldest. Down past the newest
 * returns to an EMPTY line (bash returns to the partially-typed line; we
 * don't stash it -- a deliberate simplification, documented).
 * Returns the line length, or -1 on read error / EOF.
 * ------------------------------------------------------------------------- */
static int read_line(char *buf, size_t cap) {
    size_t len = 0;
    size_t browse = 0;
    for (;;) {
        char c;
        int n = read(0, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n' || c == '\r') {
            puts1("\n");
            buf[len] = '\0';
            hist_push(buf);                     /* empty/dup lines self-filter */
            return (int)len;
        }
        if (c == '\b' || c == 0x7f) {          /* backspace / DEL */
            if (len > 0) {
                len--;
                puts1("\b \b");                 /* erase on screen */
            }
            continue;
        }
        if (c == (char)EMBK_KEY_UP) {
            if (browse < hist_count()) {
                browse++;
                line_replace(buf, cap, &len, hist_get(hist_count() - browse));
            }
            continue;
        }
        if (c == (char)EMBK_KEY_DOWN) {
            if (browse > 0) {
                browse--;
                line_replace(buf, cap, &len,
                             browse ? hist_get(hist_count() - browse) : NULL);
            }
            continue;
        }
        if (c == 0x04) {                            /* ^D = EOF */
            if (len == 0) return -1;             /* empty line = leave */
            continue;                            /* non-empty line = ignore */
        }
        if (c == 0x03) {                            /* ^C = abandon this line, re-prompt.
                                                     * The line is NOT hist_push'd (that is
                                                     * the Enter path only), so a killed
                                                     * line never enters history -- Up then
                                                     * recalls the previous REAL command. */
            puts1("^C\n");
            buf[0] = '\0';
            return 0;                            /* empty line: main loop re-prompts */
        }
        if ((unsigned char)c < 0x20) continue;  /* ignore other control chars */
        if (len + 1 < cap) {
            buf[len++] = c;
            write(1, &c, 1);                    /* echo */
            browse = 0;                          /* typing leaves history */
        }
    }
}

/* Defaults this SHELL chooses for the commands it runs.
 *
 * Deliberately shell POLICY, not kernel behaviour. The kernel fabricates
 * nothing: a process gets an environment only because its spawner named one
 * (spawn.h), and a shell naming sensible defaults for its children is exactly
 * what a shell is for. Everything here is visible in `env` and overridable with
 * `env set`, so it is a stated default, not hidden magic.
 *
 * rewrite==0 IS THE POINT: if whoever spawned this shell already named HOME, we
 * must not clobber their choice with ours. Defaults lose to explicit decisions.
 *
 * HOME exists because tools look there for their config (git wants
 * ~/.gitconfig). PATH is "/" because that is where the flat EMBKFS root puts
 * every executable today. */
extern void shell_cwd_publish(void);   /* builtins_os.c */

static void seed_default_env(void) {
    setenv("HOME", "/", 0);
    setenv("PATH", "/", 0);
    /* Publish the directory we STARTED in (crt0 already seeded our cwd from an
     * inherited PWD, if our own spawner named one), so the first external
     * command runs where the user thinks it does -- not at "/". */
    shell_cwd_publish();
}

int main(int argc, char **argv) {
    struct scope top;
    scope_init(&top, NULL);
    seed_default_env();


    if (stdin_is_console()) {
        embk_tty_mode(EMBK_TTY_RAW);
        /* At OUR prompt, ^C is a LINE-EDIT key (kill the input line), NOT a
         * cancellation -- so route it to NOBODY, which makes it arrive as the
         * byte 0x03 that read_line handles. Routing ^C to SELF was wrong: it
         * cancels this process, and the cancel flag is STICKY with no way to
         * clear it, so the shell could never read again -- one ^C exited it.
         * eval_extern hands the route to a CHILD for its lifetime (there ^C IS a
         * real cancellation) and reclaims it -- back to this NOBODY state -- after. */
        (void)embk_console_interrupt_route(-1);
        g_console_session = 1;
    }

    /* one-shot mode: shell.elf -c "ls | where size > 1mb" */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        int rc = run_line(argv[2], &top);
        scope_free(&top);
        return rc;
    }

    puts1("EmbLink shell -- structured pipelines. `exit` leaves.\n");
    char line[LINE_MAX_LEN];
    for (;;) {
        puts1("embk> ");
        int n = read_line(line, sizeof line);
        if (n < 0) break;                      /* console EOF/error: leave */
        if (strcmp(line, "exit") == 0) break;
        (void)run_line(line, &top);
    }
    if (g_console_session) embk_tty_mode(EMBK_TTY_COOKED);
    scope_free(&top);
    return 0;
}
