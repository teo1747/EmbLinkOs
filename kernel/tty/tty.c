#include "tty.h"
#include "drivers/input/keyboard.h"
#include "drivers/video/console.h"
#include "include/errno.h"
#include "include/kprintf.h"

#include <stdbool.h>

/* Longest line the discipline assembles. Past this, printables stop being 
 * accepted (erase keys still work) until you delete or pass Enter -- bounded,
 * never an overrun. The shell reads in chunks, so this is not its line limit. */
#define TTY_LINE_MAX 1024

/* Internal token from the assembler: ^D on an EMPTY line. tty_read turns this
 * into the 0-byte EOF return. Its own value so it can't be confused with the
 * EMBK_* space. */
#define TTY_EOF 1

static struct tty {
    enum tty_mode mode;          // TTY_COOKED or TTY_RAW
    char linebuf[TTY_LINE_MAX];  // The line being assembled in cooked mode
    size_t fill;                 // How many bytes are in the line buffer
    size_t pos;                  // Where the next read will start from in the line buffer
    bool complete;               // True if ^D was seen on an empty line
} g_tty;

void tty_set_mode(enum tty_mode mode) {
    g_tty.mode = mode;
}

enum tty_mode tty_get_mode(void) {
    return g_tty.mode;
}

/* Visually rub out the last echoed cell: cursor-left, overwrite with space,
 * cursor-right again. ASSUMES console_putchar('\b') moves the cursor left
 * without drawing (verify in drivers/video/console.c). */
static void tty_echo_erase(void) {
    console_putchar('\b');
    console_putchar(' ');
    console_putchar('\b');
}

/* COOKED assembly: block per keystroke, echo/edit, until a line is complete.
 * Returns EMBK_OK (line ready: complete=true, pos=0), TTY_EOF (^D on empty line)
 * or EMBK_ECANCELED (^C -- propagated straight from the keyboard layer, which 
 * already turned 0x03 into a cancel, so we never even see that byte). */
 static int tty_assemble_line(void) {
    for (;;) {
        char c;
        int rc = keyboard_getchar_blocking_cancelable(&c);
        if (rc != EMBK_OK) return rc;  // ^C or other error

        switch ((unsigned char)c) {
            case '\n':
                console_putchar('\n');
                g_tty.linebuf[g_tty.fill++] = '\n';
                g_tty.complete = true;
                g_tty.pos = 0;
                return EMBK_OK;
            
            case 0x04:  // ^D
                if (g_tty.fill == 0) return TTY_EOF;  // EOF on empty line
                g_tty.complete = true;  // ^D on non-empty line is ignored in cooked mode
                g_tty.pos = 0;
                return EMBK_OK;

            case '\b':  // Backspace
                if (g_tty.fill > 0) {
                    g_tty.fill--;
                    tty_echo_erase();
                }
                break;                 // at column 0, backspace is ignored, never back over the prompt
            
            
            case 0x15:  // ^U
                while (g_tty.fill > 0) {
                    g_tty.fill--;             // kill the whole line, but don't clear the buffer -- the shell may still read it if it wants to
                    tty_echo_erase();
                }
                break;
            
            case 0x17:  // ^W                 // erase the last word, but not the whitespace before it
                while (g_tty.fill > 0 && g_tty.linebuf[g_tty.fill - 1] == ' ') {
                    g_tty.fill--;             // kill trailing whitespace first
                    tty_echo_erase();
                }
                while (g_tty.fill > 0 && g_tty.linebuf[g_tty.fill - 1] != ' ') {
                    g_tty.fill--;             // then kill the last word
                    tty_echo_erase();
                }
                break;
            
            default:
                /* Printable: append + echo, always keeping one slot for a future 
                 * '\n'. Everything else -- other C0 controls, EmbLink's nav codes
                 * (EK_UP=0x13 etc.), and forward-Delete (0x7F) -- is ignored in
                 * cooked mode; those belong to a raw mode editor. */
                 if ((unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E) {
                    if (g_tty.fill < TTY_LINE_MAX - 1) {
                        g_tty.linebuf[g_tty.fill++] = c;
                        console_putchar(c);
                    }
                    // line full: drop the character; erase keys still work, but no more printables are accepted until Enter or ^D
                }
                break;
        }
    }
}

/* Hand bytes from a COMPLETED line to the caller, up to len; reset when the
 * line is exhausted so the next call assembles a fresh one. 
 */
static size_t tty_drain_completed(char *buf, size_t len) {
    size_t to_copy = g_tty.fill - g_tty.pos;
    size_t copied = (to_copy < len) ? to_copy : len;
    for (size_t i = 0; i < copied; i++) {
        buf[i] = g_tty.linebuf[g_tty.pos++];
    }
    
    if (g_tty.pos >= g_tty.fill) {
        g_tty.fill = 0;  // reset for the next line
        g_tty.pos = 0;
        g_tty.complete = false;
    }
    return copied;
}

/* RAW: block for the first byte, then sweep up whatever else is already
 * buffered (won't block again), delivering bytes untouched. This is exactly
 * the old console_fd_read behavior -- now correctly named "raw mode". */
static int tty_read_raw(char *buf, size_t len, size_t *out_read) {
    size_t total_read = 1;  // We will block for the first byte, so we know at least one byte will be read

    char c;
    int rc = keyboard_getchar_blocking_cancelable(&c);
    if (rc != EMBK_OK) return rc;  // ^C or other error

    buf[0] = c;

    // Now sweep up any additional bytes that are already buffered
    while (total_read < len && keyboard_has_char()) {
       
        buf[total_read++] = keyboard_getchar();
    }

    *out_read = total_read;
    return EMBK_OK;
}

int tty_read(char *buf, size_t len, size_t *out_read) {
    if (len == 0) {
        *out_read = 0;
        return EMBK_OK;  // nothing to read, but not an error
    }

    char *buf_ptr = (char *)buf;
    if (g_tty.mode == TTY_RAW) {
        return tty_read_raw(buf_ptr, len, out_read);
    } 

    if (!g_tty.complete) {              // no pending line, so assemble one
        int rc = tty_assemble_line();
        if (rc == TTY_EOF) { 
            *out_read = 0;  // EOF on empty line
            return EMBK_OK;
        }
            
        if (rc != EMBK_OK) return rc;  // ^C -ECANCELED 
    }
    *out_read = tty_drain_completed(buf_ptr, len);
    return EMBK_OK;
}




/* Drive the discipline with injected keys, assert the delivered line. A large
 * got[] means one tty_read delivers a whole short line; case 7 tests the
 * multi-read split separately. */
static bool tty_expect(const char *keys, size_t nkeys,
                       const char *want, size_t nwant, const char *name) {
    tty_set_mode(TTY_COOKED);
    g_tty.fill = 0; g_tty.pos = 0; g_tty.complete = false;
    while (keyboard_has_char()) (void)keyboard_getchar();

    for (size_t i = 0; i < nkeys; i++) keyboard_inject_char(keys[i]);

    char got[TTY_LINE_MAX]; size_t got_n = 0;
    int rc = tty_read(got, sizeof(got), &got_n);
    bool ok = (rc == EMBK_OK && got_n == nwant);
    for (size_t i = 0; ok && i < nwant; i++) if (got[i] != want[i]) ok = false;
    kprintf("[tty] %-16s %s (rc=%d got=%lu want=%lu)\n",
            name, ok ? "PASS" : "FAIL", rc, (unsigned long)got_n, (unsigned long)nwant);
    return ok;
}

int tty_run_selftests(void) {
    int fails = 0;
    enum tty_mode saved = tty_get_mode();

    fails += !tty_expect("ls\n",         3, "ls\n",  3, "plain-line");
    fails += !tty_expect("ls\bx\n",      5, "lx\n",  3, "backspace");    /* the walkthrough case */
    fails += !tty_expect("abc\x15xy\n",  8, "xy\n",  3, "kill-line");    /* ^U */
    fails += !tty_expect("foo bar\x17\n",9, "foo \n",5, "word-erase");   /* ^W */
    fails += !tty_expect("abc\x04",      4, "abc",   3, "eot-flush");    /* ^D non-empty: no \n */
    fails += !tty_expect("\x04",         1, "",      0, "eot-eof");      /* ^D empty: 0 bytes */

    /* case 7: a completed line spans two small reads */
    tty_set_mode(TTY_COOKED);
    g_tty.fill = 0; g_tty.pos = 0; g_tty.complete = false;
    while (keyboard_has_char()) (void)keyboard_getchar();
    keyboard_inject_char('h'); keyboard_inject_char('i'); keyboard_inject_char('\n');
    char a[2], b[2]; size_t na = 0, nb = 0;
    tty_read(a, 2, &na); tty_read(b, 2, &nb);
    bool split = (na == 2 && a[0]=='h' && a[1]=='i' && nb == 1 && b[0]=='\n');
    kprintf("[tty] %-16s %s\n", "split-read", split ? "PASS" : "FAIL");
    fails += !split;

    /* case 8: RAW passes bytes through -- backspace is NOT processed */
    tty_set_mode(TTY_RAW);
    while (keyboard_has_char()) (void)keyboard_getchar();
    keyboard_inject_char('a'); keyboard_inject_char('\b'); keyboard_inject_char('c');
    char r[8]; size_t nr = 0; tty_read(r, sizeof(r), &nr);
    bool raw = (nr == 3 && r[0]=='a' && r[1]=='\b' && r[2]=='c');
    kprintf("[tty] %-16s %s (got=%lu)\n", "raw-passthru", raw ? "PASS" : "FAIL", (unsigned long)nr);
    fails += !raw;

    tty_set_mode(saved);
    g_tty.fill = 0; g_tty.pos = 0; g_tty.complete = false;
    while (keyboard_has_char()) (void)keyboard_getchar();
    kprintf("[tty] selftests: %s\n", fails ? "FAILURES" : "all pass");
    return fails ? -EMBK_EIO : EMBK_OK;
}