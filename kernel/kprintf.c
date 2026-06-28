#include "include/kprintf.h"
#include "drivers/serial.h"
#include <stdint.h>
#include "include/types.h"   // for size_t


// GCC built-in variadic types
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)


// ============================================================
//  Output sink: an abstraction over "where do characters go"
// ============================================================
// The core formatter calls sink->put(sink, c) for every character.
// kprintf uses a sink that writes to serial; snprintf uses one that
// writes to a bounded buffer. Format logic is written ONCE.

struct out_sink {
    void (*put)(struct out_sink *s, char c);
    // buffer-specific fields (unused by the serial sink)
    char  *buf;
    size_t size;     // total buffer size
    size_t pos;      // next write index
    size_t written;  // total chars the format produced (for return value)
};

// Serial sink: write straight to COM1
static void sink_serial_put(struct out_sink *s, char c) {
    serial_write_char(c);
    s->written++;
}

// Buffer sink: write to buf if room remains (always leave space for '\0')
static void sink_buffer_put(struct out_sink *s, char c) {
    if (s->pos + 1 < s->size) {   // +1 keeps room for the null terminator
        s->buf[s->pos++] = c;
    }
    s->written++;   // count even when truncated (standard snprintf semantics)
}


// ============================================================
//  Number formatting helpers (write through the sink)
// ============================================================

static void emit_unsigned(struct out_sink *s, uint64_t value, uint8_t base,
                          uint8_t width, uint8_t pad_zero, uint8_t uppercase) {
    char buffer[32];
    int i = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (value == 0) {
        buffer[i++] = '0';
    } else {
        while (value > 0) {
            buffer[i++] = digits[value % base];
            value /= base;
        }
    }

    // padding to width
    while (width > i) {
        s->put(s, pad_zero ? '0' : ' ');
        width--;
    }

    // digits in reverse
    while (i > 0) {
        s->put(s, buffer[--i]);
    }
}

static void emit_padding(struct out_sink *s, uint8_t n, char ch) {
    while (n--) s->put(s, ch);
}

static uint8_t strnlen_u8(const char *p) {
    uint8_t n = 0;
    while (p[n]) n++;
    return n;
}

static void emit_string(struct out_sink *s, const char *string,
                        uint8_t width, uint8_t left_justify) {
    if (!string) string = "(null)";
    uint8_t len = strnlen_u8(string);
    uint8_t pad = (width > len) ? (uint8_t)(width - len) : 0;

    if (!left_justify) emit_padding(s, pad, ' ');
    for (uint8_t i = 0; i < len; i++) s->put(s, string[i]);
    if (left_justify) emit_padding(s, pad, ' ');
}

static void emit_signed(struct out_sink *s, int64_t value,
                        uint8_t width, uint8_t pad_zero) {
    if (value < 0) {
        s->put(s, '-');
        value = -value;
        if (width > 0) width--;
    }
    emit_unsigned(s, (uint64_t)value, 10, width, pad_zero, 0);
}


// ============================================================
//  The ONE core formatter — used by both kprintf and snprintf
// ============================================================

static void format_string(struct out_sink *s, const char *fmt, va_list arg) {
    while (*fmt) {
        if (*fmt != '%') {
            s->put(s, *fmt++);
            continue;
        }

        fmt++; // skip '%'

        // parse flags ('-' left justify, '0' zero-pad)
        uint8_t left_justify = 0;
        uint8_t pad_zero = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_justify = 1;
            else pad_zero = 1;
            fmt++;
        }
        if (left_justify) pad_zero = 0; // '-' overrides '0'

        // parse width
        uint8_t width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        // parse length modifier (l, ll)
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') {
                fmt++;   // accept 'll' too (same as 'l' in 64-bit)
            }
        }

        // parse conversion
        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t value = is_long ? va_arg(arg, int64_t) : va_arg(arg, int);
                if (!left_justify) {
                    emit_signed(s, value, width, pad_zero);
                } else {
                    // For left-justify we render once without width, then pad right.
                    char tmp[32];
                    int n = 0;
                    uint64_t u = (uint64_t)(value < 0 ? -value : value);
                    if (value < 0) tmp[n++] = '-';
                    char rev[32];
                    int r = 0;
                    if (u == 0) rev[r++] = '0';
                    while (u > 0) { rev[r++] = (char)('0' + (u % 10)); u /= 10; }
                    while (r > 0) tmp[n++] = rev[--r];
                    for (int i = 0; i < n; i++) s->put(s, tmp[i]);
                    if (width > (uint8_t)n) emit_padding(s, (uint8_t)(width - (uint8_t)n), ' ');
                }
                break;
            }
            case 'u': {
                uint64_t value = is_long ? va_arg(arg, uint64_t) : va_arg(arg, unsigned int);
                if (!left_justify) {
                    emit_unsigned(s, value, 10, width, pad_zero, 0);
                } else {
                    char rev[32]; int r = 0;
                    if (value == 0) rev[r++] = '0';
                    while (value > 0) { rev[r++] = (char)('0' + (value % 10)); value /= 10; }
                    int n = r;
                    while (r > 0) s->put(s, rev[--r]);
                    if (width > (uint8_t)n) emit_padding(s, (uint8_t)(width - (uint8_t)n), ' ');
                }
                break;
            }
            case 'x': {
                uint64_t value = is_long ? va_arg(arg, uint64_t) : va_arg(arg, unsigned int);
                if (!left_justify) {
                    emit_unsigned(s, value, 16, width, pad_zero, 0);
                } else {
                    const char *digits = "0123456789abcdef";
                    char rev[32]; int r = 0;
                    if (value == 0) rev[r++] = '0';
                    while (value > 0) { rev[r++] = digits[value % 16]; value /= 16; }
                    int n = r;
                    while (r > 0) s->put(s, rev[--r]);
                    if (width > (uint8_t)n) emit_padding(s, (uint8_t)(width - (uint8_t)n), ' ');
                }
                break;
            }
            case 'X': {
                uint64_t value = is_long ? va_arg(arg, uint64_t) : va_arg(arg, unsigned int);
                if (!left_justify) {
                    emit_unsigned(s, value, 16, width, pad_zero, 1);
                } else {
                    const char *digits = "0123456789ABCDEF";
                    char rev[32]; int r = 0;
                    if (value == 0) rev[r++] = '0';
                    while (value > 0) { rev[r++] = digits[value % 16]; value /= 16; }
                    int n = r;
                    while (r > 0) s->put(s, rev[--r]);
                    if (width > (uint8_t)n) emit_padding(s, (uint8_t)(width - (uint8_t)n), ' ');
                }
                break;
            }
            case 'c': {
                char value = (char)va_arg(arg, int);
                if (!left_justify && width > 1) emit_padding(s, (uint8_t)(width - 1), ' ');
                s->put(s, value);
                if (left_justify && width > 1) emit_padding(s, (uint8_t)(width - 1), ' ');
                break;
            }
            case 'p': {
                uint64_t value = (uint64_t)va_arg(arg, void *);
                uint8_t plen = 18; // 0x + 16 hex digits
                if (!left_justify && width > plen) emit_padding(s, (uint8_t)(width - plen), ' ');
                s->put(s, '0');
                s->put(s, 'x');
                emit_unsigned(s, value, 16, 16, 1, 0);   // 16 hex digits, zero-padded
                if (left_justify && width > plen) emit_padding(s, (uint8_t)(width - plen), ' ');
                break;
            }
            case 's': {
                const char *string = va_arg(arg, const char *);
                emit_string(s, string, width, left_justify);
                break;
            }
            case '%':
                s->put(s, '%');
                break;
            default:
                s->put(s, '?');
                break;
        }
        fmt++;
    }
}


// ============================================================
//  Public API — thin wrappers over the core
// ============================================================

void kprintf(const char *fmt, ...) {
    struct out_sink s = { .put = sink_serial_put, .written = 0 };
    va_list arg;
    va_start(arg, fmt);
    format_string(&s, fmt, arg);
    va_end(arg);
}

int snprintf(char *buffer, size_t size, const char *fmt, ...) {
    if (size == 0) {
        return 0;   // can't even write a null terminator; report nothing written
    }

    struct out_sink s = {
        .put     = sink_buffer_put,
        .buf     = buffer,
        .size    = size,
        .pos     = 0,
        .written = 0,
    };

    va_list arg;
    va_start(arg, fmt);
    format_string(&s, fmt, arg);
    va_end(arg);

    buffer[s.pos] = '\0';   // always null-terminate (pos < size guaranteed)

    return (int)s.written;   // chars that WOULD have been written (standard)
}