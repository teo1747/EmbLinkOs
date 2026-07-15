#ifndef __KEYBOARD__H__
#define __KEYBOARD__H__

#include <stdint.h>

// Initialise the keyboard driver, register IRQ1 handler and unmask IRQ1 at PIC
void keyboard_init(void);

// Blocking read: returns the next ASCII character available
// Returns 0 if no character is available (e.g. non-ASCII key pressed)
char keyboard_getchar(void);

// Non-blocking read: returns 1 if the next ASCII character is available, otherwise returns 0
int keyboard_has_char(void);

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