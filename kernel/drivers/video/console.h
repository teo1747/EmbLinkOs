#ifndef _CONSOLE_H_
#define _CONSOLE_H_

#include "include/types.h"
#include <stdint.h>


// Initialise the console. Must be called after fb_init()
// Computes dimensions from the framebuffer info, clears the screen
// reset console cursor position to (0, 0)
void console_init(void);

// Write a character to the console at the current cursor position and move the cursor
// Handles \n, \r, \b, \t, \e and \a escape sequences
void console_putchar(char c);

// Write a string to the console at the current cursor position and move the cursor
// write a null-terminated string to the console
void console_write(const char *str);

// Set Foreground and Background colors for subsequent writes to the console
void console_set_color(uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                      uint8_t bg_r, uint8_t bg_g, uint8_t bg_b);

// Clear the console screen and reset cursor position to (0, 0)
void console_clear(void);

// True once console_init completed and framebuffer-backed writes are safe.
bool console_is_ready(void);

// Enable/disable the ON-SCREEN half of the console (serial output is unaffected).
// Turned off once the desktop owns the framebuffer so kernel text / the serial
// debug console never paint over the userspace UI.
void console_set_fb_enabled(bool on);



#endif /* _CONSOLE_H_ */