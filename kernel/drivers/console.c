#include "console.h"
#include "framebuffer.h"
#include "font_8x16.h"
#include "serial.h"
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
    cursor_col = 0;
    cursor_row = 0;
}



static void scroll_up(void) {
    uint8_t *buffer = fb_get_buffer();
    const fb_info_t *info = fb_get_info();

    uint32_t row_bytes = info->pitch * FONT_HEIGHT;
    uint32_t total_bytes = info->pitch * info->height;
    uint32_t move_bytes = total_bytes - row_bytes;

    // Move everything up by one character row (FONT_HEIGHT pixel rows)
    // src = pixel row FONT_HEIGHT, dst = pixel row 0
    uint8_t *dst = buffer; 
    uint8_t *src = buffer + row_bytes;
    for (uint32_t i = 0; i < move_bytes; i++) {
        dst[i] = src[i];
    }

    // Clear the last character row
    uint8_t *bottom = buffer + (total_bytes - row_bytes);
    uint32_t bpp_bytes = info->bpp / 8;
    for (uint32_t y = 0; y < FONT_HEIGHT; y ++) {
        for (uint32_t x = 0; x < info->width; x++) {
            uint8_t *pixel = bottom + (y * info->pitch) + (x * bpp_bytes);
            if (info->format == FB_FORMAT_RGB) {
                pixel[0] = bg_r;
                pixel[1] = bg_g;
                pixel[2] = bg_b;
            }else if (info->format == FB_FORMAT_BGR) {
                pixel[2] = bg_r;
                pixel[1] = bg_g;
                pixel[0] = bg_b;
            }   
        }

    }
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

}


void console_write(const char *str)

{
    while (*str) {
        console_putchar(*str++);
    }
}

