#include "drivers/input/keyboard.h"
#include "arch/x86_64/irq/irq.h"
#include "include/io.h"
#include "drivers/char/serial.h"
#include "process/process.h"


#include <stdint.h>


#define KBD_DATA_PORT 0x60
#define KBD_STATUS_PORT 0x64

/* Readers blocked on an empty buffer. Zero-init is sufficient: struct
 * wait_queue is just { struct thread *head; }, and head == NULL IS the
 * empty state -- no sentinel to construct. */
static struct wait_queue kbd_wait_queue;   /* for keyboard_deliver() to wait on */

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

/* Every push site (PS/2 IRQ, USB HID) MUST go through here so the push and the
 * wake are one atomic step -- a bare buffer_push() lands the char but never
 * wakes a reader blocked in keyboard_getchar_blocking(), which is a lost
 * wakeup (the read hangs forever). Defined below; forward-declared so the IRQ
 * handler can reach it. */
static void keyboard_deliver(char c);


/* Extended (0xE0-prefixed) navigation keys, delivered to userspace as private
 * single-byte control codes the UI toolkit's text editor interprets (see
 * EMBK_KEY_* in user/lib/embk.h -- kept in lockstep). Arrows/Home/End/Delete
 * so a text field can move its cursor without an escape-sequence protocol. */
#define EK_LEFT  0x11
#define EK_RIGHT 0x12
#define EK_UP    0x13
#define EK_DOWN  0x14
#define EK_HOME  0x02
#define EK_END   0x05
#define EK_DEL   0x7F

static void keyboard_handler(void) {
    uint8_t scancode = inb(KBD_DATA_PORT);
    static int extended = 0;   /* the previous byte was the 0xE0 prefix */

    if (scancode == 0xE0) { extended = 1; return; }   /* prefix: the next byte is an extended key */

    if (scancode & 0x80) { extended = 0; return; }    /* key release: ignore (and clear prefix) */

    if (extended) {
        extended = 0;
        char c = 0;
        switch (scancode) {
            case 0x4B: c = EK_LEFT;  break;
            case 0x4D: c = EK_RIGHT; break;
            case 0x48: c = EK_UP;    break;
            case 0x50: c = EK_DOWN;  break;
            case 0x47: c = EK_HOME;  break;
            case 0x4F: c = EK_END;   break;
            case 0x53: c = EK_DEL;   break;
            default: break;
        }
        if (c) keyboard_deliver(c);
        return;
    }

    char ascii = scan_to_ascii[scancode];
    if (ascii) keyboard_deliver(ascii);
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

/* Every push site goes through here. The lock makes {push -> wake} atomic
 * against a reader's {check-empty -> block}, closing the lost-wakeup window.
 * Safe from IRQ context because of the kernel's standing invariant:
 * g_sched_lock is only ever held with interrupts OFF, so an IRQ physically
 * cannot land while a thread holds it -- there is no self-deadlock path.
 * The critical section is deliberately tiny (one ring push + one state
 * flip); the wake only marks the thread READY, it does not switch to it. */
static void keyboard_deliver(char c) {
    sched_lock();
    buffer_push(c);   /* the kernel shell will read it */
    wait_queue_wake_one(&kbd_wait_queue);
    sched_unlock();
}

/* --- keyboard grab -------------------------------------------------------
 * A ring-3 UI app (e.g. uidemo) grabs the keyboard so the kernel shell stops
 * draining it; both otherwise poll the same buffer and split keystrokes. The
 * grab is auto-released when the grabbing process is reaped (see process.c). */
static volatile int      g_kbd_grabbed;
static volatile uint32_t g_kbd_grabber_pid;

void keyboard_set_grab(int grab, uint32_t pid) {
    g_kbd_grabbed = grab ? 1 : 0;
    g_kbd_grabber_pid = grab ? pid : 0;
}
int keyboard_is_grabbed(void) { return g_kbd_grabbed; }
void keyboard_release_grab_pid(uint32_t pid) {
    if (g_kbd_grabbed && g_kbd_grabber_pid == pid) { g_kbd_grabbed = 0; g_kbd_grabber_pid = 0; }
}

/* Blocking read for FD_BACKING_CONSOLE (fd 0). Deliberately NOT
 * keyboard_getchar() -- that one hlt-spins the CPU without ever yielding,
 * which is correct for the kernel shell (pre-scheduler, nothing else to run)
 * but would wedge us here: a shell blocked on read(0) would halt the core,
 * so the child it just spawned would never get scheduled to run. */
char keyboard_getchar_blocking(void) {
    char c;
    sched_lock();
    while (buf_head == buf_tail) {                             /* re-check UNDER the lock */
        sched_block_current_locked(&kbd_wait_queue);           /* block until a key is pushed */
        sched_lock();                                          /* re-acquire the lock to re-check the condition */
    }
    buffer_pop(&c);                                          /* still locked -> atomic with the check */
    sched_unlock();
    
    return c;
}

// Inject a character as if it came from a keyboard press.
// Called by the USB HID driver for keys received over xHCI.
void keyboard_inject_char(char c) {
    keyboard_deliver(c);
}