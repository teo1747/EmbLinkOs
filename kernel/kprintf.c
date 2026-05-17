#include "include/kprintf.h"
#include "drivers/console.h"
#include <stdint.h>


// GCC built-in variadic types

typedef __builtin_va_list va_list ;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)


// Number to string conversion

static void print_unsigned(uint64_t value, uint8_t base, uint8_t width, uint8_t pad_zero, uint8_t uppercase)
{
    char buffer[32];
    int i = 0;
    const char *digits_lower =  "0123456789abcdef";
    const char *digits_upper =  "0123456789ABCDEF";
    const char *digits = (uppercase)? digits_upper : digits_lower;
    
    if (value == 0) {
        buffer[i++] = '0';
    } else {
        while (value > 0) {
            buffer[i++] = digits[value % base];
            value /= base;
        }
    }
    
    // padding
    while (width > i) {
      buffer[i++] = (pad_zero)? '0' :' ';
    }

    // Output in reverse order
    while (i > 0) {
        console_putchar(buffer[--i]);
    }
}

static void print_signed(int64_t value, uint8_t width, uint8_t pad_zero) {
    if (value < 0) {
        console_putchar('-');
        value = -value;
        if (width > 0) width--;
    }
    print_unsigned((uint64_t)value, 10, width, pad_zero, 0);
}

// Printing functions

void kprintf(const char *fmt,...)
{
    va_list arg;
    va_start(arg, fmt);
    while (*fmt) {
        if (*fmt != '%') {
        console_putchar(*fmt++);   // print normal char
        continue;
    }
    
        
        fmt++; // skip %

        //parse widht and padding
        uint8_t width = 0;
        uint8_t pad_zero = 0;
        if (*fmt == '0') {
            pad_zero = 1;
            fmt++;
        }

        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        // parse length modifier
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') {
                fmt++;  // accept 'll' as well in 64-bit mode
            }
        }

        // parse type
        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t value = (is_long) ? va_arg(arg, int64_t) : va_arg(arg, int);
                print_signed(value, width, pad_zero);
                break;
            }
            case 'u': {
                uint64_t value = (is_long) ? va_arg(arg, uint64_t) : va_arg(arg, unsigned int);
                print_unsigned(value, 10, width, pad_zero, 0);
                break;
            }
            case 'x': {
                uint64_t value = (is_long) ? va_arg(arg, uint64_t) : va_arg(arg, unsigned int);
                print_unsigned(value, 16, width, pad_zero, 0);
                break;
            }
            case 'X': {
                uint64_t value = (is_long) ? va_arg(arg, uint64_t) : va_arg(arg, unsigned int);
                print_unsigned(value, 16, width, pad_zero, 1);
                break;
            }
            case 'c': {
                char value = (char)va_arg(arg, int);
                console_putchar(value);
                break;
            }
            case 'p': {
                uint64_t value = (uint64_t)va_arg(arg, void *);
                console_write("0x");
                print_unsigned(value, 16, 16, 1, 0);   // pad with zeros to 16 chars
                break;
            }
            case 's': {
                const char *string = va_arg(arg,const char*);
                if (!string) {
                    string = "(null)";
                }
                while (*string) {
                    console_putchar(*string++);
                }
                break;
            }
            case '%':
                console_putchar('%');
                break;
            default: {
                console_putchar('?');
                break;
            }
        }
        fmt++;
    }
    va_end(arg);
}