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

#endif /* __KEYBOARD__H__ */