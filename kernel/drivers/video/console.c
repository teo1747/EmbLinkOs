#include "drivers/video/console.h"
#include "drivers/video/framebuffer.h"
#include "drivers/video/font_8x16.h"
#include "drivers/char/serial.h"
#include <stdint.h>


// Console state
static uint32_t cursor_col = 0;
static uint32_t cursor_row = 0;
static uint32_t cols = 0;   // total number of columns
static uint32_t rows = 0;   // total number of rows

// Current color
static uint8_t fg_r = 255, fg_g = 255, fg_b = 255;  // white
static uint8_t bg_r = 0, bg_g = 0, bg_b = 0;        // black
static bool g_console_ready = false;

// Cached Framebuffer info pointer
static const fb_info_t *fb_info = 0;


void console_init(void) {
    serial_write_string("\n=== Console init ===\n");
    fb_info = fb_get_info();

    cols = fb_info->width / FONT_WIDTH;  // 128
    rows = fb_info->height / FONT_HEIGHT;  // 48

    cursor_col = 0;
    cursor_row = 0;

    fb_clear(bg_r, bg_g, bg_b);
    fb_present();
    g_console_ready = true;
}

bool console_is_ready(void)
{
    return g_console_ready;
}

void console_set_color(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2) {

    fg_r = r1; fg_g = g1; fg_b = b1;
    bg_r = r2; bg_g = g2; bg_b = b2;
}


void console_clear(void){

    fb_clear(bg_r, bg_g, bg_b);
    fb_present();
    cursor_col = 0;
    cursor_row = 0;
}



static void scroll_up(void) {
    // Fast memmove-based scroll in the framebuffer layer, then push the whole
    // screen (the scroll dirties everything anyway).
    fb_scroll_up(FONT_HEIGHT, FB_RGB(bg_r, bg_g, bg_b));
}

void console_putchar(char c) {

    // Always write to serial first - keeps debug log intact
    serial_write_char(c);

    // Handle special characters
    switch (c) {
        case '\n':
            cursor_col = 0;
            cursor_row++;
            if (cursor_row >= rows) {
                scroll_up();
                cursor_row = rows - 1;
            }
            fb_present();
            return;
        case '\r':
            cursor_col = 0;
            return;
        case '\b':
            if (cursor_col > 0) {
                cursor_col--;
            }else if (cursor_row > 0) {
                cursor_col = cols - 1;
                cursor_row--;
            }

            // Blanck the cell at the new cursor position
            fb_draw_char(' ', cursor_col * FONT_WIDTH, cursor_row * FONT_HEIGHT, fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);
            fb_present();

            return;

        case '\t':
            // Advance to NEXT 8-column boundary
            cursor_col = (cursor_col + 8) & ~(7);
            if (cursor_col >= cols) {
                cursor_col = 0;
                cursor_row++;
                if (cursor_row >= rows) {
                    scroll_up();
                    cursor_row = rows - 1;
                }
                fb_present();
            }
            return;
    }

    // Normal Printable Character
    uint32_t px = cursor_col * FONT_WIDTH;
    uint32_t py = cursor_row * FONT_HEIGHT;
    fb_draw_char(c, px, py, fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);

    cursor_col++;
    if (cursor_col >= cols) {
        cursor_col = 0;
        cursor_row++;
        if (cursor_row >= rows) {
            scroll_up();
            cursor_row = rows - 1;
        }
    }
    fb_present();

}


void console_write(const char *str)

{
    while (*str) {
        console_putchar(*str++);
    }
}

