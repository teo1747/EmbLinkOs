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

/* ---- the KEY EVENT stream -----------------------------------------------
 * The char stream above answers "what did the user TYPE". It cannot answer
 * "what key is DOWN", and it never will -- not from lack of effort, but
 * because it is out of room and inherently ambiguous:
 *
 *   - C0 is already spoken for. Ctrl+letter occupies 0x01-0x1A, so Up (0x13)
 *     and Ctrl+S are THE SAME BYTE. That ambiguity is real today.
 *   - F1-F12, Alt, the GUI/Menu keys and every key RELEASE have no byte to be.
 *
 * So key events are a SECOND, parallel stream rather than an escape encoding
 * bolted onto the first. A text reader keeps doing byte compares and is
 * completely unaffected; anything that needs real key state (a game, a
 * hold-to-repeat UI, a chord like Alt+F4) polls events instead. Neither stream
 * is a lossy re-encoding of the other, which is the whole point.
 *
 * Keycodes are NOT ASCII: printable keys carry their UNSHIFTED ASCII (so 'a'
 * is 'a' whether or not Shift is down -- the char stream already told you it
 * was 'A'), and everything else lives at >= 0x100 where nothing collides. */
#define EKC_F1     0x101
#define EKC_F2     0x102
#define EKC_F3     0x103
#define EKC_F4     0x104
#define EKC_F5     0x105
#define EKC_F6     0x106
#define EKC_F7     0x107
#define EKC_F8     0x108
#define EKC_F9     0x109
#define EKC_F10    0x10A
#define EKC_F11    0x10B
#define EKC_F12    0x10C
#define EKC_INS    0x110
#define EKC_LWIN   0x111
#define EKC_RWIN   0x112
#define EKC_MENU   0x113
#define EKC_LEFT   0x120
#define EKC_RIGHT  0x121
#define EKC_UP     0x122
#define EKC_DOWN   0x123
#define EKC_HOME   0x124
#define EKC_END    0x125
#define EKC_PGUP   0x126
#define EKC_PGDN   0x127
#define EKC_DEL    0x128
#define EKC_LSHIFT 0x130
#define EKC_RSHIFT 0x131
#define EKC_LCTRL  0x132
#define EKC_RCTRL  0x133
#define EKC_LALT   0x134
#define EKC_RALT   0x135
#define EKC_CAPS   0x140
#define EKC_NUM    0x141
#define EKC_SCROLL 0x142

/* Modifier bits, as seen AT THE MOMENT the event was generated. */
#define EKM_SHIFT  0x01
#define EKM_CTRL   0x02
#define EKM_ALT    0x04
#define EKM_GUI    0x08   /* either Windows/Command key */
#define EKM_CAPS   0x10   /* the LATCH, not the key */
#define EKM_NUM    0x20
#define EKM_SCROLL 0x40

struct key_event {
    uint16_t code;      /* unshifted ASCII, or an EKC_* */
    uint8_t  mods;      /* EKM_* bitmap at event time */
    uint8_t  pressed;   /* 1 = make, 0 = break */
};

/* Pop one event. Returns 1 and fills *ev, or 0 when the queue is empty.
 * Non-blocking by design: an event consumer is a UI frame loop or a game, and
 * both already have a clock -- neither wants to block a thread on a keypress. */
int keyboard_event_pop(struct key_event *ev);

/* The live modifier bitmap (EKM_*). Answers "is Shift down RIGHT NOW", which
 * the event stream cannot if you missed the make. */
uint8_t keyboard_mods(void);

/* ---- keyboard layouts ---------------------------------------------------
 * A layout is the two ASCII tables, nothing more: the scancode->key mapping is
 * hardware, and the key->character mapping is policy. Swapping tables is the
 * whole of "support AZERTY" for a set-1 PC keyboard.
 * keyboard_set_layout() returns 0 on success, -1 for an unknown name (it does
 * NOT fall back to US silently -- a typo'd layout name that quietly keeps
 * QWERTY is worse than a refusal). */
struct keymap {
    const char *name;
    const char *normal;   /* [128], indexed by set-1 make code */
    const char *shift;    /* [128] */
};
int         keyboard_set_layout(const char *name);
const char *keyboard_layout(void);

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