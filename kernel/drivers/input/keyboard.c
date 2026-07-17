#include "drivers/input/keyboard.h"
#include "arch/x86_64/irq/irq.h"
#include "include/io.h"
#include "drivers/char/serial.h"
#include "process/process.h"
#include "include/errno.h"


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

/* SHIFTED layer, US QWERTY. Existed as a gap until the pipeline shell made
 * it absurd: a shell whose core operator is '|' on an OS whose keyboard
 * could not TYPE '|' (shift produced the unshifted char -- no uppercase, no
 * !@#$, no quotes-vs-apostrophe either). Same indexing as scan_to_ascii. */
static const char scan_to_ascii_shift[128] = {
    0,   0x1B, '!', '@', '#', '$', '%', '^', '&', '*',   // 0x00-0x09
    '(', ')', '_', '+', '\b','\t','Q', 'W', 'E', 'R',    // 0x0A-0x13
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,     // 0x14-0x1D
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',    // 0x1E-0x27
    '"', '~',  0,  '|', 'Z', 'X', 'C', 'V', 'B', 'N',    // 0x28-0x31
    'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,      // 0x32-0x3B
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x3C-0x45
    0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,      // 0x46-0x4F (keypad)
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x50-0x59
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x5A-0x63
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x64-0x6D
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x6E-0x77
    0,   0,   0,   0,   0,   0,   0,   0,                // 0x78-0x7F
};


/* DVORAK, and why this layout and not AZERTY.
 *
 * A layout is just these two tables -- scancode->key is hardware, key->char is
 * policy -- so this is what "support keymaps" actually costs. Dvorak was chosen
 * to PROVE that, because it is pure ASCII.
 *
 * ⚠️ AZERTY IS NOT SHIPPABLE YET, and not for lack of a table: French needs
 * é è ç à ù, which are NOT ASCII. The char stream is `char` and carries 7-bit
 * ASCII, so an "AZERTY" here could only be QWERTY-with-letters-moved and silent
 * holes where the accents belong. That is a worse lie than saying no. Real
 * AZERTY needs the char stream to grow a wider encoding first -- a genuine,
 * stated gap (see docs/TODO.md), not an oversight. */
static const char scan_to_ascii_dvorak[128] = {
    0,   0x1B, '1', '2', '3', '4', '5', '6', '7', '8',   // 0x00-0x09
    '9', '0', '[', ']', '\b','\t','\'',',', '.', 'p',    // 0x0A-0x13
    'y', 'f', 'g', 'c', 'r', 'l', '/', '=', '\n', 0,     // 0x14-0x1D
    'a', 'o', 'e', 'u', 'i', 'd', 'h', 't', 'n', 's',    // 0x1E-0x27
    '-', '`',  0,  '\\',';', 'q', 'j', 'k', 'x', 'b',    // 0x28-0x31
    'm', 'w', 'v', 'z', 0,   '*', 0,   ' ', 0,   0,      // 0x32-0x3B
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x3C-0x45
    0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,      // 0x46-0x4F
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x50-0x59
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x5A-0x63
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x64-0x6D
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x6E-0x77
    0,   0,   0,   0,   0,   0,   0,   0,                // 0x78-0x7F
};
static const char scan_to_ascii_dvorak_shift[128] = {
    0,   0x1B, '!', '@', '#', '$', '%', '^', '&', '*',   // 0x00-0x09
    '(', ')', '{', '}', '\b','\t','"', '<', '>', 'P',    // 0x0A-0x13
    'Y', 'F', 'G', 'C', 'R', 'L', '?', '+', '\n', 0,     // 0x14-0x1D
    'A', 'O', 'E', 'U', 'I', 'D', 'H', 'T', 'N', 'S',    // 0x1E-0x27
    '_', '~',  0,  '|', ':', 'Q', 'J', 'K', 'X', 'B',    // 0x28-0x31
    'M', 'W', 'V', 'Z', 0,   '*', 0,   ' ', 0,   0,      // 0x32-0x3B
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x3C-0x45
    0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,      // 0x46-0x4F
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x50-0x59
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x5A-0x63
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x64-0x6D
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x6E-0x77
    0,   0,   0,   0,   0,   0,   0,   0,                // 0x78-0x7F
};

/* Live modifier state (EKM_*). File-scope, not a handler static, because it is
 * now a QUERY (keyboard_mods) and it stamps every key event. */
static volatile uint8_t g_mods;

/* Defined below (they need the tables above and the 8042 helpers below); the
 * IRQ handler sits between the two and has to reach both. */
static void kbd_set_leds(void);
static const struct keymap *g_layout;

/* The key-event ring. Separate from the char ring on purpose -- see keyboard.h.
 * Overflow DROPS THE NEWEST rather than overwriting the oldest: losing the tail
 * of a burst is recoverable, but silently eating a key-UP would strand a
 * modifier "held" forever, and a stuck Ctrl is far worse than a dropped key. */
#define KBD_EVENT_RING 64
static struct key_event ev_ring[KBD_EVENT_RING];
static volatile uint32_t ev_head, ev_tail;

static void kbd_event_push(uint16_t code, int pressed) {
    uint32_t nh = (ev_head + 1) % KBD_EVENT_RING;
    if (nh == ev_tail) return;             /* full: drop the newest (see above) */
    ev_ring[ev_head].code    = code;
    ev_ring[ev_head].mods    = g_mods;
    ev_ring[ev_head].pressed = (uint8_t)(pressed ? 1 : 0);
    ev_head = nh;
}

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


/* The EK_* navigation codes now live in keyboard.h -- they are a contract
 * shared with the USB HID driver (usb/xhci.c) and userland (EMBK_KEY_*), not
 * a PS/2 private. Keeping them here is what left the USB path with no
 * navigation keys at all. */

/* Set a modifier bit from a make/break, and emit the event for the modifier KEY
 * itself. `bit` is the EKM_* to hold while down. Shift/Ctrl/Alt/GUI are LEVELS:
 * each has two physical keys, so we cannot just clear the bit on a break -- we
 * track the two sides and clear only when BOTH are up, or releasing one Shift
 * while the other is held would wrongly un-shift. */
static uint8_t g_side;                       /* which physical modifier keys are down */
#define SIDE_LSHIFT 0x01
#define SIDE_RSHIFT 0x02
#define SIDE_LCTRL  0x04
#define SIDE_RCTRL  0x08
#define SIDE_LALT   0x10
#define SIDE_RALT   0x20
#define SIDE_LGUI   0x40
#define SIDE_RGUI   0x80

static void mod_update(uint8_t side_bit, uint8_t pair_mask, uint8_t ekm,
                       uint16_t code, int pressed)
{
    if (pressed) g_side |= side_bit; else g_side &= (uint8_t)~side_bit;
    if (g_side & pair_mask) g_mods |= ekm; else g_mods &= (uint8_t)~ekm;
    kbd_event_push(code, pressed);
}

/* The locks. A latch, not a level: it flips on the MAKE and ignores the break
 * entirely (holding Caps Lock down does not "hold" anything), which is exactly
 * how it differs from Shift. The LED is the only feedback the user gets. */
static void lock_toggle(uint8_t ekm, uint16_t code, int pressed)
{
    if (pressed) { g_mods ^= ekm; kbd_set_leds(); }
    kbd_event_push(code, pressed);
}

static void keyboard_handler(void) {
    uint8_t sc = inb(KBD_DATA_PORT);
    static int extended = 0;     /* the previous byte was the 0xE0 prefix */

    if (sc == 0xE0) { extended = 1; return; }   /* prefix: next byte is extended */
    if (sc == 0xE1) { return; }                 /* Pause/Break's 6-byte burst: not decoded */

    int     pressed = !(sc & 0x80);             /* break codes are make | 0x80 */
    uint8_t make    = sc & 0x7F;

    if (extended) {
        extended = 0;
        /* An 0xE0-prefixed 0x2A/0xAA is a FAKE shift: set 1 pads it around the
         * nav keys so a shift-unaware BIOS still sees sane codes. It must never
         * touch real shift state, and it is not a key -- drop it silently. */
        if (make == 0x2A || make == 0x36) return;

        switch (make) {
            case 0x1D: mod_update(SIDE_RCTRL, SIDE_LCTRL|SIDE_RCTRL, EKM_CTRL, EKC_RCTRL, pressed); return;
            case 0x38: mod_update(SIDE_RALT,  SIDE_LALT|SIDE_RALT,   EKM_ALT,  EKC_RALT,  pressed); return;
            case 0x5B: mod_update(SIDE_LGUI,  SIDE_LGUI|SIDE_RGUI,   EKM_GUI,  EKC_LWIN,  pressed); return;
            case 0x5C: mod_update(SIDE_RGUI,  SIDE_LGUI|SIDE_RGUI,   EKM_GUI,  EKC_RWIN,  pressed); return;
            case 0x5D: kbd_event_push(EKC_MENU, pressed); return;
            default: break;
        }

        /* The real nav cluster (the keypad's twins are handled below). */
        uint16_t kc = 0; char ch = 0;
        switch (make) {
            case 0x4B: kc = EKC_LEFT;  ch = EK_LEFT;  break;
            case 0x4D: kc = EKC_RIGHT; ch = EK_RIGHT; break;
            case 0x48: kc = EKC_UP;    ch = EK_UP;    break;
            case 0x50: kc = EKC_DOWN;  ch = EK_DOWN;  break;
            case 0x47: kc = EKC_HOME;  ch = EK_HOME;  break;
            case 0x4F: kc = EKC_END;   ch = EK_END;   break;
            case 0x53: kc = EKC_DEL;   ch = EK_DEL;   break;
            case 0x49: kc = EKC_PGUP;  ch = EK_PGUP;  break;
            case 0x51: kc = EKC_PGDN;  ch = EK_PGDN;  break;
            case 0x52: kc = EKC_INS;   ch = 0;        break;  /* no char: C0 is full */
            case 0x1C: kc = '\n';      ch = '\n';     break;  /* keypad Enter */
            case 0x35: kc = '/';       ch = '/';      break;  /* keypad / */
            default: return;
        }
        kbd_event_push(kc, pressed);
        if (pressed && ch) keyboard_deliver(ch);
        return;
    }

    /* --- modifiers and locks (the breaks we must NOT ignore) --- */
    switch (make) {
        case 0x2A: mod_update(SIDE_LSHIFT, SIDE_LSHIFT|SIDE_RSHIFT, EKM_SHIFT, EKC_LSHIFT, pressed); return;
        case 0x36: mod_update(SIDE_RSHIFT, SIDE_LSHIFT|SIDE_RSHIFT, EKM_SHIFT, EKC_RSHIFT, pressed); return;
        case 0x1D: mod_update(SIDE_LCTRL,  SIDE_LCTRL|SIDE_RCTRL,   EKM_CTRL,  EKC_LCTRL,  pressed); return;
        case 0x38: mod_update(SIDE_LALT,   SIDE_LALT|SIDE_RALT,     EKM_ALT,   EKC_LALT,   pressed); return;
        case 0x3A: lock_toggle(EKM_CAPS,   EKC_CAPS,   pressed); return;
        case 0x45: lock_toggle(EKM_NUM,    EKC_NUM,    pressed); return;
        case 0x46: lock_toggle(EKM_SCROLL, EKC_SCROLL, pressed); return;
        default: break;
    }

    /* --- F1-F12: event-only. There is no byte for them (C0 is Ctrl+letter's),
     * and inventing an escape sequence would force every reader to become a
     * state machine. They are exactly what the event stream is for. --- */
    if (make >= 0x3B && make <= 0x44) { kbd_event_push(EKC_F1 + (make - 0x3B), pressed); return; }
    if (make == 0x57) { kbd_event_push(EKC_F11, pressed); return; }
    if (make == 0x58) { kbd_event_push(EKC_F12, pressed); return; }

    /* --- the keypad, whose codes COLLIDE with the nav cluster's ---
     * A non-extended 0x47 is keypad-7-or-Home; the real Home arrived above with
     * an 0xE0. Num Lock picks which, and that is the whole of what Num Lock is. */
    if (make >= 0x47 && make <= 0x53) {
        static const char pad_num[] = "789-456+1230.";        /* 0x47..0x53 */
        static const uint16_t pad_nav[] = {
            EKC_HOME, EKC_UP, EKC_PGUP, 0, EKC_LEFT, 0, EKC_RIGHT,
            0, EKC_END, EKC_DOWN, EKC_PGDN, EKC_INS, EKC_DEL };
        static const char pad_nav_ch[] = {
            EK_HOME, EK_UP, EK_PGUP, '-', EK_LEFT, 0, EK_RIGHT,
            '+', EK_END, EK_DOWN, EK_PGDN, 0, EK_DEL };
        int i = make - 0x47;
        if (g_mods & EKM_NUM) {
            char c = pad_num[i];
            if (!c) return;
            kbd_event_push((uint16_t)c, pressed);
            if (pressed) keyboard_deliver(c);
        } else {
            if (!pad_nav[i]) return;
            kbd_event_push(pad_nav[i], pressed);
            if (pressed && pad_nav_ch[i]) keyboard_deliver(pad_nav_ch[i]);
        }
        return;
    }

    /* --- ordinary keys --- */
    char base = g_layout->normal[make];
    if (!base) { if (pressed) { } return; }   /* unmapped: no event, no char */

    /* The event carries the UNSHIFTED key -- "which key", not "what character".
     * The char stream below answers the character question, and duplicating it
     * here would just be a second, worse answer. */
    kbd_event_push((uint16_t)base, pressed);
    if (!pressed) return;                     /* releases produce no text */

    char ascii = (g_mods & EKM_SHIFT) ? g_layout->shift[make] : base;
    if (!ascii) return;

    /* Caps Lock: LETTERS ONLY, and it XORs with Shift rather than adding to it
     * (Caps+Shift+a is 'a', not 'A'). Applying it to the whole shift table --
     * the obvious implementation -- would make Caps Lock type '!' for '1',
     * which no keyboard on earth does. */
    if (g_mods & EKM_CAPS) {
        if      (ascii >= 'a' && ascii <= 'z') ascii = (char)(ascii - 'a' + 'A');
        else if (ascii >= 'A' && ascii <= 'Z') ascii = (char)(ascii - 'A' + 'a');
    }

    /* Ctrl + letter -> the C0 control code (Ctrl-C = 0x03, Ctrl-D = 0x04, ...).
     * This is what actually MAKES ^C possible: without it the driver produced a
     * plain 'c' and keyboard_deliver()'s 0x03 branch could never fire. Gate on a
     * real letter so Ctrl+digit / Ctrl+symbol pass through unchanged rather than
     * becoming stray control bytes. */
    if ((g_mods & EKM_CTRL) && ascii >= 'a' && ascii <= 'z') ascii = ascii & 0x1f;
    else if ((g_mods & EKM_CTRL) && ascii >= 'A' && ascii <= 'Z') ascii = ascii & 0x1f;

    keyboard_deliver(ascii);
}



/* --- 8042 controller ------------------------------------------------------
 * We used to rely on whatever the BIOS left behind. That worked, but it is a
 * bet: it assumes the device is in scancode set 2 AND that controller
 * translation is on (which is what makes set 2 arrive here looking like the
 * set 1 our tables index). Nothing verified either.
 *
 * ⚠️ THIS MUST COMPOSE WITH mouse.c, WHICH TOUCHES THE SAME CONFIG BYTE.
 * mouse_init() does its own read-modify-write to set bit 1 (IRQ12) and clear
 * bit 5 (mouse clock). So:
 *   - we ONLY touch keyboard bits (0 = IRQ1, 4 = kbd clock, 6 = translation),
 *     read-modify-write, so the two inits compose in EITHER order;
 *   - we do NOT issue a controller self-test (0xAA) and do NOT disable ports
 *     (0xAD/0xA7). Every "proper 8042 init" writeup does; here they would reset
 *     state the mouse depends on. The mouse works today -- do not break it to
 *     be textbook-correct.
 *
 * 🪤 THE PAIRING TRAP: scancode set and translation are ONE decision, not two.
 * Set the device to set 1 while translation is on and the controller
 * translates set-1 bytes as if they were set-2 -- garbage, and a dead keyboard.
 * We pick the standard PC pairing (device in SET 2 + translation ON) and state
 * it, because it is what our set-1 tables require. */
#define PS2_CFG_IRQ1        0x01   /* bit 0: keyboard IRQ1 enable       */
#define PS2_CFG_KBD_CLKOFF  0x10   /* bit 4: 1 = keyboard clock DISABLED */
#define PS2_CFG_XLATE       0x40   /* bit 6: set2 -> set1 translation    */

static void ps2_wait_write(void) {          /* input buffer clear before we write */
    for (int i = 0; i < 100000; i++) if (!(inb(KBD_STATUS_PORT) & 0x02)) return;
}
static void ps2_wait_read(void) {           /* output buffer full before we read */
    for (int i = 0; i < 100000; i++) if (inb(KBD_STATUS_PORT) & 0x01) return;
}

/* Send a byte to the KEYBOARD (not the controller) and eat its ACK (0xFA).
 * Bounded: a device that never ACKs must not hang the boot. */
static int kbd_cmd(uint8_t byte) {
    for (int try = 0; try < 3; try++) {
        ps2_wait_write(); outb(KBD_DATA_PORT, byte);
        ps2_wait_read();
        uint8_t r = inb(KBD_DATA_PORT);
        if (r == 0xFA) return 1;            /* ACK */
        if (r != 0xFE) return 0;            /* not a RESEND: give up, report it */
    }
    return 0;
}

/* The three lock LEDs. `0xED` then a bitmap: bit0 Scroll, bit1 Num, bit2 Caps.
 * Caps Lock without its LED is a latch the user cannot see -- the light IS the
 * feature's only feedback. */
static void kbd_set_leds(void) {
    uint8_t bits = 0;
    if (g_mods & EKM_SCROLL) bits |= 0x01;
    if (g_mods & EKM_NUM)    bits |= 0x02;
    if (g_mods & EKM_CAPS)   bits |= 0x04;
    if (kbd_cmd(0xED)) kbd_cmd(bits);
}

void keyboard_init(void) {
    serial_write_string("\n=== Keyboard init ===\n");

    irq_register(1, keyboard_handler);

    /* Config byte: read-modify-write, keyboard bits only (see the warning above). */
    ps2_wait_write(); outb(KBD_STATUS_PORT, 0x20);
    ps2_wait_read();  uint8_t cfg = inb(KBD_DATA_PORT);
    cfg |=  PS2_CFG_IRQ1;                    /* deliver IRQ1                     */
    cfg &= ~PS2_CFG_KBD_CLKOFF;              /* clock ON (bit CLEAR enables it)  */
    cfg |=  PS2_CFG_XLATE;                   /* set2 -> set1, what our tables want */
    ps2_wait_write(); outb(KBD_STATUS_PORT, 0x60);
    ps2_wait_write(); outb(KBD_DATA_PORT, cfg);

    /* Scancode set 2 EXPLICITLY -- the other half of the translation pairing.
     * Best-effort: QEMU and most real controllers ACK; one that does not is
     * almost certainly already in set 2 (that being the power-on default), and
     * refusing to boot over it would be worse than proceeding. Say so either
     * way rather than pretending we configured something we did not. */
    if (kbd_cmd(0xF0) && kbd_cmd(0x02))
        serial_write_string("Keyboard: scancode set 2 + translation (set 1 seen)\n");
    else
        serial_write_string("Keyboard: set-2 select not ACKed; assuming power-on default\n");

    kbd_cmd(0xF4);                           /* enable scanning */

    g_mods = 0;                              /* locks start OFF, and the LEDs agree */
    kbd_set_leds();

    serial_write_string("Keyboard registered on IRQ 1\n");
}

/* --- layouts -------------------------------------------------------------- */
static const struct keymap g_layouts[] = {
    { "us",     scan_to_ascii,        scan_to_ascii_shift        },
    { "dvorak", scan_to_ascii_dvorak, scan_to_ascii_dvorak_shift },
};
static const struct keymap *g_layout = &g_layouts[0];

int keyboard_set_layout(const char *name) {
    for (unsigned i = 0; i < sizeof g_layouts / sizeof g_layouts[0]; i++) {
        const char *a = g_layouts[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) { g_layout = &g_layouts[i]; return 0; }
    }
    return -1;      /* unknown: keep the current layout, and SAY so (see the header) */
}
const char *keyboard_layout(void) { return g_layout->name; }

uint8_t keyboard_mods(void) { return g_mods; }

int keyboard_event_pop(struct key_event *ev) {
    if (ev_head == ev_tail) return 0;
    *ev = ev_ring[ev_tail];
    ev_tail = (ev_tail + 1) % KBD_EVENT_RING;
    return 1;
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
/* Where ^C goes. See docs/INTERRUPTION.md.
 *
 * ONE slot, because there is ONE console. This is NOT an inferred "foreground
 * process": no session, no process group, no walking a tree. A process that
 * holds the console DELEGATES its interrupts to a child it holds a handle for
 * (sys_console_interrupt_route), exactly as it delegates fds via file-actions
 * and the environment via envp. 0 = nobody routed anything, and then ^C is just
 * a byte -- a true and predictable statement about a system where no one asked
 * to be interrupted.
 *
 * Single-user concession, stated plainly: the slot is global and last-writer-
 * wins, the same class of concession as embk_proc_kill's ambient authority. */
static volatile uint32_t g_console_int_target;   /* pid, or 0 for none */

void keyboard_set_interrupt_target(uint32_t pid) { g_console_int_target = pid; }
uint32_t keyboard_get_interrupt_target(void) { return g_console_int_target; }

static void keyboard_deliver(char c) {
    /* ^C: cancel the routed target instead of delivering a byte.
     *
     * DELIBERATELY BEFORE sched_lock(): process_cancel() takes g_sched_lock
     * itself, and this runs in IRQ context -- taking it twice would self-
     * deadlock. Safe to take it here at all only because of the standing
     * invariant (see below): g_sched_lock is never held with interrupts on, so
     * an IRQ cannot land while some thread holds it, and it is free right now. */
    if (c == 0x03) {
        uint32_t target = g_console_int_target;
        if (target) {
            process_cancel(target);
            return;         /* consumed: ^C is an interruption, not input */
        }
        /* Nobody routed: fall through and hand ^C over as an ordinary byte
         * rather than silently swallowing a keystroke. */
    }

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

/* Like keyboard_getchar_blocking(), but honours cancellation (docs/INTERRUPTION.md).
 * Returns EMBK_OK with *out set, or -EMBK_ECANCELED if this process was cancelled
 * while (or before) waiting. The plain version stays for callers with no process
 * context -- the kernel shell -- where cancellation has no meaning.
 *
 * Ordering matches the pipe path: a buffered key is a real result and is
 * returned even if the process is also cancelled; only BLOCKING is refused. */
int keyboard_getchar_blocking_cancelable(char *out) {
    sched_lock();
    while (buf_head == buf_tail) {
        if (current_process && current_process->cancelled) {
            sched_unlock();
            return -EMBK_ECANCELED;
        }
        sched_block_current_locked(&kbd_wait_queue);
        sched_lock();
    }
    buffer_pop(out);
    sched_unlock();
    return EMBK_OK;
}

// Inject a character as if it came from a keyboard press.
// Called by the USB HID driver for keys received over xHCI.
void keyboard_inject_char(char c) {
    keyboard_deliver(c);
}