#ifndef _SERIAL_H
#define _SERIAL_H

#include <stdint.h>



void serial_init(void);
void serial_write_char(char c);
void serial_write_string(const char *str);
void serial_write_hex(uint64_t value);

/* Polled input (COM1). serial_has_char() is a non-blocking data-ready check;
 * serial_read_char() reads one byte and must only be called after it. */
int  serial_has_char(void);
char serial_read_char(void);

#endif // _SERIAL_H
