#ifndef _SERIAL_H
#define _SERIAL_H

#include <stdint.h>



void serial_init(void);
void serial_write_char(char c);
void serial_write_string(const char *str);
void serial_write_hex(uint64_t value);

#endif // _SERIAL_H
