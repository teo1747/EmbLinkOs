#include "drivers/input/keyboard.h"
#include "arch/x86_64/irq/irq.h"
#include "include/io.h"
#include "drivers/char/serial.h"

#include <stdint.h>


#define KBD_DATA_PORT 0x60
#define KBD_STATUS_PORT 0x64

// Scancode to ASCII translaation - US QWERTY, Set 1
// Indexed by scancode (0-0x7F for "pressed" code)
// 0 = unmapped / special key

static const char scan_to_ascii[128] = {
    0,   0x1B, '1', '2', '3', '4', '5', '6', '7', '8',   // 0x00-0x09
    '9', '0', '-', '=', '\b','\t','q', 'w', 'e', 'r',    // 0x0A-0x13
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,     // 0x14-0x1D (1D=LeftCtrl)
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',    // 0x1E-0x27
    '\'','`',  0,  '\\','z', 'x', 'c', 'v', 'b', 'n',    // 0x28-0x31 (2A=LeftShift)
    'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,      // 0x32-0x3B (36=RightShift, 38=LeftAlt)
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x3C-0x45 (F-keys, NumLock, ScrollLock)
    0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,      // 0x46-0x4F (keypad)
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x50-0x59
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x5A-0x63
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x64-0x6D
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x6E-0x77
    0,   0,   0,   0,   0,   0,   0,   0,                // 0x78-0x7F
};


// Circular buffer for storing incoming characters from the keyboard
#define KBD_BUFFER_SIZE 128
static char kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t buf_head = 0;   // write index (IRQ writes here)
static volatile uint32_t buf_tail = 0;   // read index (kernel reads here)

// Push a character into the circular buffer (called from IRQ handler or USB HID)
static void buffer_push(char c) {
    uint32_t next_head = (buf_head + 1) % KBD_BUFFER_SIZE;
    if (next_head == buf_tail) {
        return;
    
    }
    kbd_buffer[buf_head] = c;
    buf_head = next_head;
}


// pop a character from the circular buffer, return 0 if buffer is empty (called from keyboard_getchar)
static int buffer_pop(char *c) {
    if (buf_head == buf_tail) {
        return 0;
    }
    *c = kbd_buffer[buf_tail];
    buf_tail = (buf_tail + 1) % KBD_BUFFER_SIZE;
    return 1;
}


static void keyboard_handler(void) {
    // Read scancode from keyboard data port
    uint8_t scancode = inb(KBD_DATA_PORT);
    

    // Check if key release (high bit set) - ignore releases
    if (scancode & 0x80) {
        return;
    }

    // Translate scancode to ASCII character
    char ascii = scan_to_ascii[scancode];
    if (ascii) {
        buffer_push(ascii);
       
    }
}



// Inject a character as if it came from a keyboard press.
// Called by the USB HID driver for keys received over xHCI.
void keyboard_inject_char(char c) {
    if (c) { buffer_push(c); }
}

void keyboard_init(void) {
    serial_write_string("\n=== Keyboard init ===\n");
    // Register IRQ1 handler
    irq_register(1, keyboard_handler);

    serial_write_string("Keyboard registered on IRQ 1\n");
}


char keyboard_getchar(void) {
    char c;
    // spin until a character is available in the buffer
    while (!buffer_pop(&c)) {
        __asm__ volatile ("hlt"); // No character available, halt until next interrupt
    }
    
    return c;
}


int keyboard_has_char(void) {
    return buf_head != buf_tail;
}