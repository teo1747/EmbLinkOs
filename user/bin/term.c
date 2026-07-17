/* user/bin/term.c -- the EmbLink Terminal: the structured shell, on screen.
 *
 * A deliberately DUMB byte terminal. It spawns /shell.elf as a child with
 * both stdio ends piped (Piece-0 plumbing: INSTALL_OBJ into the child,
 * fd_install_obj for its own ends), forwards keystrokes into the shell's
 * stdin, and renders whatever the shell prints -- the shell already does
 * echo, backspace erase and the prompt, so the terminal implements no line
 * editing at all. Output is polled with embk_fd_avail() from the runtime's
 * idle hook (a render loop must never block in read()).
 *
 * Closing the window closes the shell FOR FREE: the terminal's exit drops
 * its pipe fds (exit-time reap loop), the shell's read(0) returns EOF, and
 * its REPL exits -- nobody sends signals, the plumbing itself hangs up. */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

#define T_COLS 96          /* chars per line before a hard wrap */
#define T_ROWS 24          /* visible lines */
#define SB_ROWS 200        /* scrollback depth (a ring of whole lines) */
#define FD_SHELL_IN  10    /* our WRITE end of the shell's stdin  */
#define FD_SHELL_OUT 11    /* our READ end of the shell's stdout  */

/* Scrollback: a ring of SB_ROWS lines. g_head is the ring slot of the
 * CURRENT (partial) line; g_count how many slots hold real lines (grows to
 * SB_ROWS then saturates -- oldest lines fall off). g_view is how many
 * lines the user has paged back from live (0 = live view; typing snaps it
 * back to 0). */
static char g_sb[SB_ROWS][T_COLS + 1];
static int  g_head = 0, g_count = 1, g_col = 0;
static int  g_view = 0;
static int  g_shell = -1;      /* spawn handle */
static bool g_dead = false;

/* ring slot of logical line `i` (0 = oldest live line, g_count-1 = current) */
static int sb_slot(int i) {
    return (g_head - (g_count - 1) + i + 2 * SB_ROWS) % SB_ROWS;
}

static void term_newline(void) {
    g_head = (g_head + 1) % SB_ROWS;
    if (g_count < SB_ROWS) g_count++;
    memset(g_sb[g_head], 0, sizeof g_sb[0]);
    g_col = 0;
    /* keep a paged-back view anchored on the same CONTENT while new lines
     * arrive underneath -- until the ring saturates and eats it */
    if (g_view > 0 && g_view < g_count - T_ROWS) g_view++;
    if (g_view > g_count - T_ROWS) g_view = g_count > T_ROWS ? g_count - T_ROWS : 0;
}

static void term_putc(char c) {
    if (c == '\n') { term_newline(); return; }
    if (c == '\r') { g_col = 0; return; }
    if (c == '\f') {                       /* form feed: the shell's `clear` */
        memset(g_sb, 0, sizeof g_sb);
        g_head = 0;
        g_count = 1;
        g_col = 0;
        g_view = 0;
        return;
    }
    if (c == '\b') {
        if (g_col > 0) { g_col--; g_sb[g_head][g_col] = '\0'; }
        return;
    }
    if ((unsigned char)c < 0x20) return;   /* other control bytes: drop */
    if (g_col >= T_COLS) term_newline();   /* hard wrap */
    g_sb[g_head][g_col++] = c;
    g_sb[g_head][g_col] = '\0';
}

static void term_say(const char *s) { while (*s) term_putc(*s++); }

/* --- runtime hooks -------------------------------------------------------- */

/* idle: drain the shell's output pipe without blocking; wake the renderer
 * only when something actually arrived. Also notice the shell dying. */
static void term_idle(void) {
    bool got = false;
    for (;;) {
        int64_t n = embk_fd_avail(FD_SHELL_OUT);
        if (n <= 0) break;
        char buf[256];
        int r = (int)embk_read(FD_SHELL_OUT, buf, n < (int64_t)sizeof buf ? (size_t)n : sizeof buf);
        if (r <= 0) break;
        for (int i = 0; i < r; i++) term_putc(buf[i]);
        got = true;
    }
    if (!g_dead && g_shell >= 0 && !embk_proc_alive(g_shell)) {
        g_dead = true;
        term_say("\n[shell exited -- press ESC to close]\n");
        got = true;
    }
    if (got) em_request_frame();
}

/* keys: PgUp/PgDn page the scrollback; anything typed snaps back to live
 * and is forwarded to the shell; nav keys the shell has no story for are
 * swallowed; ESC falls through to the runtime (close). */
static int term_key(int c) {
    if (c == 27) return 0;                       /* ESC -> runtime closes us */
    if (c == EMBK_KEY_PGUP || c == EMBK_KEY_PGDN) {
        int max_back = g_count > T_ROWS ? g_count - T_ROWS : 0;
        if (c == EMBK_KEY_PGUP) g_view += T_ROWS / 2;
        else                    g_view -= T_ROWS / 2;
        if (g_view > max_back) g_view = max_back;
        if (g_view < 0) g_view = 0;
        em_request_frame();
        return 1;
    }
    if (g_dead) return 1;
    char ch;
    if (c == EMBK_KEY_DEL) ch = '\b';
    /* Up/Down go THROUGH to the shell: history recall is the shell's job
     * (it owns the line buffer and the echo), exactly as readline belongs
     * to bash and not to xterm. The terminal keeps PgUp/PgDn for its own
     * scrollback -- that's the whole key split. */
    else if (c == EMBK_KEY_UP || c == EMBK_KEY_DOWN) ch = (char)c;
    else if (c == '\n' || c == '\b' || c == '\t' ||
             (c >= 0x20 && c <= 0x7E)) ch = (char)c;
    else return 1;                               /* other nav keys: swallowed */
    if (g_view != 0) { g_view = 0; em_request_frame(); }   /* typing = live */
    write(FD_SHELL_IN, &ch, 1);
    return 1;                                    /* never reaches the toolkit */
}

/* --- spawn the shell with both stdio ends piped --------------------------- */
static bool term_spawn_shell(void) {
    int pin[2], pout[2];                         /* {read_h, write_h} */
    if (embk_pipe(pin) != 0) return false;       /* shell stdin */
    if (embk_pipe(pout) != 0) {
        embk_close_handle(pin[0]); embk_close_handle(pin[1]);
        return false;
    }

    struct embk_spawn_file_action acts[3];
    memset(acts, 0, sizeof acts);
    acts[0].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ;   /* stdin  <- pin read end */
    acts[0].target_fd = 0;
    acts[0].src_obj_handle = pin[0];
    acts[1].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ;   /* stdout -> pout write end */
    acts[1].target_fd = 1;
    acts[1].src_obj_handle = pout[1];
    acts[2].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ;   /* stderr -> same pipe */
    acts[2].target_fd = 2;
    acts[2].src_obj_handle = pout[1];

    char *argv[] = { "/shell.elf", NULL };
    int64_t h = embk_spawn("/shell.elf", argv, acts, 3);

    /* our copies of the CHILD's ends must go, whatever happened -- EOF
     * accounting depends on it */
    embk_close_handle(pin[0]);
    embk_close_handle(pout[1]);
    if (h < 0) {
        embk_close_handle(pin[1]); embk_close_handle(pout[0]);
        return false;
    }
    g_shell = (int)h;

    /* our ends become plain fds; the handles are then redundant */
    embk_fd_install_obj(pin[1],  FD_SHELL_IN);
    embk_fd_install_obj(pout[0], FD_SHELL_OUT);
    embk_close_handle(pin[1]);
    embk_close_handle(pout[0]);
    return true;
}

/* --- the view -------------------------------------------------------------- */
static void term_view(void) {
    Window("Terminal") {
        WindowBar("Terminal") {
            CloseGrip();
        }
        VStack(.spacing = 1, .padding = 12, .align = Leading) {
            int start = g_count - T_ROWS - g_view;
            if (start < 0) start = 0;
            for (int r = 0; r < T_ROWS; r++) {
                /* NO `continue`/`break` inside a container's brace scope: the
                 * EmUI containers are for-loop MACROS, so a bare continue
                 * targets the MACRO's hidden loop and silently skips the rest
                 * of the body -- it rendered an entirely blank terminal until
                 * this became an if/else. */
                if (r == 0 && g_view > 0) {
                    /* paged back: the top row becomes the position marker */
                    static char mark[64];
                    snprintf(mark, sizeof mark,
                             "-- scrollback: %d line(s) back (PgDn -> live) --", g_view);
                    Text(mark).caption().secondary();
                } else {
                    int logical = start + r;
                    const char *ln = " ";   /* " ": empty text collapses (V7 lesson) */
                    if (logical >= 0 && logical < g_count) {
                        const char *s = g_sb[sb_slot(logical)];
                        if (s[0]) ln = s;
                    }
                    Text(ln).caption();
                }
            }
        }
    }
}

int main(void) {
    if (!term_spawn_shell()) {
        term_say("can't start /shell.elf\n");
        g_dead = true;
    }
    em_set_key_hook(term_key);
    em_set_idle_hook(term_idle);

    static EmApp app = {
        .title  = "Terminal",
        .size   = { 740, 480 },
        .theme  = Dark,
        .chrome = Chromeless,
        .resize = FixedSize,
        .view   = term_view,
        .font   = "/mono.ttf",   /* DejaVu Sans Mono -- aligned table columns */
    };
    int rc = em_app_run(&app);
    /* window closed: our pipe fds die with us (reap loop) -> the shell's
     * read(0) EOFs -> its REPL exits -> reap. Collect it if still around. */
    if (g_shell >= 0) embk_wait(g_shell);
    return rc;
}
