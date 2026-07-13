/* kernel/drivers/input/mouse.c -- PS/2 mouse driver (see mouse.h).
 *
 * The 8042 controller delivers mouse data on IRQ12. Each movement is a 3-byte
 * packet: [flags, dx, dy]. We accumulate a clamped absolute cursor position and
 * a button bitmask that ring-3 polls via sys_ui_input. */

#include "drivers/input/mouse.h"
#include "arch/x86_64/irq/irq.h"
#include "include/io.h"
#include "drivers/char/serial.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

/* status register bits */
#define PS2_ST_OUTPUT_FULL 0x01   /* byte available to read from 0x60 */
#define PS2_ST_INPUT_FULL  0x02   /* controller busy; wait before writing */
#define PS2_ST_FROM_MOUSE  0x20   /* the pending byte is from the aux (mouse) port */

/* packet byte-0 flags */
#define PKT_LEFT     0x01
#define PKT_RIGHT    0x02
#define PKT_MIDDLE   0x04
#define PKT_ALWAYS1  0x08   /* validity/sync bit -- always set on a real byte 0 */
#define PKT_X_SIGN   0x10
#define PKT_Y_SIGN   0x20

static volatile int32_t  g_x, g_y;
static volatile uint32_t g_buttons;
static volatile int32_t  g_wheel;        /* accumulated scroll notches (IntelliMouse Z) */
static int32_t  g_w = 1024, g_h = 768;

static uint8_t  g_pkt[4];
static int      g_cycle;
static int      g_pkt_len = 3;           /* 4 once IntelliMouse (wheel) is negotiated */

void mouse_get_state(int32_t *x, int32_t *y, uint32_t *buttons) {
    if (x) *x = g_x;
    if (y) *y = g_y;
    if (buttons) *buttons = g_buttons;
}

/* Return and clear the scroll delta accumulated since the last call (notches;
 * +up / -down). ring-3 polls this each frame via sys_ui_input. */
int32_t mouse_take_wheel(void) {
    int32_t w = g_wheel; g_wheel = 0; return w;
}

/* controller handshake helpers (bounded spins so a wedged 8042 can't hang) */
static void ps2_wait_write(void) { for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & PS2_ST_INPUT_FULL)) return; }
static void ps2_wait_read(void)  { for (int i = 0; i < 100000; i++) if (  inb(PS2_STATUS) & PS2_ST_OUTPUT_FULL) return; }

/* send a command byte to the MOUSE (aux) device (0xD4 prefix), read its ACK. */
static void mouse_command(uint8_t cmd) {
    ps2_wait_write(); outb(PS2_CMD, 0xD4);
    ps2_wait_write(); outb(PS2_DATA, cmd);
    ps2_wait_read();  (void)inb(PS2_DATA);   /* consume the 0xFA ACK */
}

/* Apply the accumulated packet's motion + buttons (bytes 0..2, common to both
 * the 3- and 4-byte formats). */
static void apply_motion(void) {
    int32_t dx = g_pkt[1];
    int32_t dy = g_pkt[2];
    if (g_pkt[0] & PKT_X_SIGN) dx |= ~0xFF;   /* sign-extend 8->32 */
    if (g_pkt[0] & PKT_Y_SIGN) dy |= ~0xFF;
    int32_t nx = g_x + dx;
    int32_t ny = g_y - dy;                     /* PS/2 y is up-positive; screen is down */
    if (nx < 0) nx = 0; if (nx >= g_w) nx = g_w - 1;
    if (ny < 0) ny = 0; if (ny >= g_h) ny = g_h - 1;
    g_x = nx; g_y = ny;
    g_buttons = g_pkt[0] & (PKT_LEFT | PKT_RIGHT | PKT_MIDDLE);
}

/* Absolute pointer update (USB tablet): map [0,range] -> screen and set the
 * cursor directly. Same clamped g_x/g_y the compositor reads; whichever device
 * (PS/2 or tablet) produces events wins. Aligned 32-bit stores are atomic on
 * x86, so this races harmlessly with the IRQ12 PS/2 writer. */
void mouse_set_absolute(int32_t x, int32_t y, int32_t range,
                        uint32_t buttons, int32_t wheel) {
    if (range <= 0) range = 1;
    int32_t nx = (int32_t)(((int64_t)x * (g_w - 1)) / range);
    int32_t ny = (int32_t)(((int64_t)y * (g_h - 1)) / range);
    if (nx < 0) nx = 0; if (nx >= g_w) nx = g_w - 1;
    if (ny < 0) ny = 0; if (ny >= g_h) ny = g_h - 1;
    g_x = nx; g_y = ny;
    g_buttons = buttons & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE);
    if (wheel) g_wheel += wheel;
}

static void mouse_handler(void) {
    /* On IRQ12 the pending byte is mouse data; read it directly. (Some 8042
     * emulations don't set the AUX status bit reliably, so we don't gate on it.) */
    uint8_t b = inb(PS2_DATA);

    switch (g_cycle) {
        case 0:
            if (!(b & PKT_ALWAYS1)) return;  /* out of sync -- wait for a real byte 0 */
            g_pkt[0] = b; g_cycle = 1;
            break;
        case 1:
            g_pkt[1] = b; g_cycle = 2;
            break;
        case 2:
            g_pkt[2] = b;
            if (g_pkt_len == 3) { apply_motion(); g_cycle = 0; }
            else g_cycle = 3;               /* IntelliMouse: one more byte (Z/wheel) */
            break;
        case 3: {
            g_pkt[3] = b; g_cycle = 0;
            apply_motion();
            int32_t z = g_pkt[3] & 0x0F;    /* 4-bit two's-complement scroll delta */
            if (z & 0x08) z |= ~0x0F;
            g_wheel += z;
            break;
        }
    }
}

void mouse_init(uint32_t screen_w, uint32_t screen_h) {
    serial_write_string("\n=== Mouse init ===\n");
    g_w = (int32_t)screen_w; g_h = (int32_t)screen_h;
    g_x = g_w / 2; g_y = g_h / 2;
    g_cycle = 0; g_buttons = 0;

    /* enable the auxiliary (mouse) device */
    ps2_wait_write(); outb(PS2_CMD, 0xA8);

    /* turn on IRQ12 (bit 1) and the mouse clock (clear bit 5) in the config byte */
    ps2_wait_write(); outb(PS2_CMD, 0x20);
    ps2_wait_read();  uint8_t cfg = inb(PS2_DATA);
    cfg |=  0x02;
    cfg &= ~0x20;
    ps2_wait_write(); outb(PS2_CMD, 0x60);
    ps2_wait_write(); outb(PS2_DATA, cfg);

    mouse_command(0xF6);   /* set defaults */

    /* IntelliMouse magic knock: sample rates 200,100,80 then GET_DEVICE_ID; a
     * reply of 0x03 means the mouse now emits a 4th byte carrying the wheel. */
    mouse_command(0xF3); mouse_command(200);
    mouse_command(0xF3); mouse_command(100);
    mouse_command(0xF3); mouse_command(80);
    ps2_wait_write(); outb(PS2_CMD, 0xD4);
    ps2_wait_write(); outb(PS2_DATA, 0xF2);          /* GET_DEVICE_ID */
    ps2_wait_read();  (void)inb(PS2_DATA);           /* ACK */
    ps2_wait_read();  uint8_t id = inb(PS2_DATA);
    if (id == 0x03) { g_pkt_len = 4; serial_write_string("Mouse: IntelliMouse wheel enabled\n"); }

    mouse_command(0xF4);   /* enable data reporting */

    irq_register(12, mouse_handler);
    serial_write_string("Mouse registered on IRQ 12\n");
}
