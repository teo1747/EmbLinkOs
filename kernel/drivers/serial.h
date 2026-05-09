#ifndef _SERIAL_H
#define _SERIAL_H



void serial_init(void);
void serial_write_char(char c);
void serial_write_string(const char *str);

#endif // _SERIAL_H
