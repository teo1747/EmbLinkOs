#ifndef __KEYBOARD__H__
#define __KEYBOARD__H__

#include <stdint.h>

/* ---- the non-ASCII key codes this OS delivers ---------------------------
 * Navigation keys reach userspace as PRIVATE single-byte control codes, not
 * ANSI escape sequences -- so a reader is a byte compare, never an escape
 * state machine (the shell's Up/Down history recall and the terminal's
 * PgUp/PgDn scrollback both rely on that).
 *
 * A THREE-WAY CONTRACT, kept here so all of it shares ONE definition:
 *   - the PS/2 driver   (keyboard.c, extended 0xE0 scancodes)
 *   - the USB HID driver (usb/xhci.c, HID usages 0x4A-0x52)
 *   - userland          (EMBK_KEY_* in user/lib/embk.h -- mirror any change)
 * These were file-local to keyboard.c, which is exactly why the USB path
 * silently had NO navigation keys at all: xhci.c couldn't name them. */
#define EK_HOME  0x02
#define EK_END   0x05
#define EK_PGUP  0x0E   /* NOT 0x03/0x04: those stay free for a future Ctrl-C/D */
#define EK_PGDN  0x0F
#define EK_LEFT  0x11
#define EK_RIGHT 0x12
#define EK_UP    0x13
#define EK_DOWN  0x14
#define EK_DEL   0x7F

// Initialise the keyboard driver, register IRQ1 handler and unmask IRQ1 at PIC
void keyboard_init(void);

// Blocking read: returns the next ASCII character available
// Returns 0 if no character is available (e.g. non-ASCII key pressed)
char keyboard_getchar(void);

// Non-blocking read: returns 1 if the next ASCII character is available, otherwise returns 0
int keyboard_has_char(void);

/* Ctrl-C routing — docs/INTERRUPTION.md.
 *
 * `pid` receives console interrupts: a ^C cancels it (process_cancel) instead of
 * being delivered as a byte. 0 clears the route, and then ^C is just input.
 *
 * There is ONE slot because there is ONE console. This is a DELEGATION, not an
 * inferred "foreground process" — EmbLink has no session or process group, and
 * a process that never routes is simply never interrupted. Set via
 * sys_console_interrupt_route(), which only accepts a HANDLE the caller holds,
 * so nobody can route interrupts at a process they were never given. */
void keyboard_set_interrupt_target(uint32_t pid);
uint32_t keyboard_get_interrupt_target(void);

/* Cancellation-aware console read (docs/INTERRUPTION.md). EMBK_OK + *out, or
 * -EMBK_ECANCELED if the calling process was cancelled while waiting. */
int keyboard_getchar_blocking_cancelable(char *out);

// Inject a character into the keyboard buffer (used by USB HID driver).
void keyboard_inject_char(char c);

// Keyboard grab: while grabbed, the kernel shell stops draining the buffer so a
// ring-3 UI app has exclusive keystrokes. Auto-released when the grabber exits.
void keyboard_set_grab(int grab, uint32_t pid);
int  keyboard_is_grabbed(void);
void keyboard_release_grab_pid(uint32_t pid);

/* Blocking read that YIELDS (wait-queue backed) rather than halting the CPU.
 * Use this from any context where a scheduler exists; keyboard_getchar()'s
 * hlt-spin is only correct pre-scheduler. */
char keyboard_getchar_blocking(void);

#endif /* __KEYBOARD__H__ */