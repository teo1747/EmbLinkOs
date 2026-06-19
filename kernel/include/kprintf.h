#ifndef _KPRINTF_H_
#define _KPRINTF_H_


#include "types.h"
#include <stdint.h>


void kprintf(const char *fmt, ...); // variadic function
void snprintf(char *buffer, size_t size, const char *fmt, ...);  // variadic function


#endif /* _KPRINTF_H_ */