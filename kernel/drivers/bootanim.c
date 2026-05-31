#include "bootanim.h"
#include "framebuffer.h"
#include "font_8x16.h"
#include "pit.h"
#include "keyboard.h"
#include "../include/types.h"
#include <stdint.h>


// === Color palette (RGB; fb_put_pixel handles BGR swap) ===
#define BG_R  0x3A
#define BG_G  0x7B
#define BG_C  0xC4   // sky blue

#define TRACE_R 0xD9
#define TRACE_G 0xA0
#define TRACE_B 0x40   // copper/gold

#define PULSE_R 0xFF
#define PULSE_G 0xF0
#define PULSE_B 0xC0   // bright pulse head

#define TEXT_R 0xFF
#define TEXT_G 0xC8
#define TEXT_B 0x50    // HELIOS copper


// === Animation tuning — change these to adjust speed/smoothness ===
#define STEP_STRIDE    3     // pixels per frame (smaller = smoother + slower)
#define STEP_DELAY_MS  8     // ms per frame (larger = slower)
#define TRACE_HALF     2     // trace half-thickness


static uint32_t SCRW, SCRH, CX, CY;


static bool check_skip(void) {
    if (keyboard_has_char()) {
        char c = keyboard_getchar();
        if (c == '\n' || c == '\r') return true;
    }
    return false;
}


static void draw_dot(uint32_t cx, uint32_t cy, int half,
                     uint8_t r, uint8_t g, uint8_t b) {
    for (int dy = -half; dy <= half; dy++) {
        for (int dx = -half; dx <= half; dx++) {
            int x = (int)cx + dx;
            int y = (int)cy + dy;
            if (x >= 0 && y >= 0) {
                fb_put_pixel((uint32_t)x, (uint32_t)y, r, g, b);
            }
        }
    }
}


static void draw_via(uint32_t cx, uint32_t cy) {
    int radius = 8;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= radius * radius) {
                int x = (int)cx + dx, y = (int)cy + dy;
                if (x >= 0 && y >= 0) {
                    if (dx * dx + dy * dy >= (radius - 3) * (radius - 3)) {
                        fb_put_pixel((uint32_t)x, (uint32_t)y, TRACE_R, TRACE_G, TRACE_B);
                    } else {
                        fb_put_pixel((uint32_t)x, (uint32_t)y, 0x20, 0x20, 0x20);
                    }
                }
            }
        }
    }
}


static void draw_helios(uint32_t scale) {
    const char *text = "HELIOS";
    int len = 6;
    uint32_t glyph_w = FONT_WIDTH * scale;
    uint32_t glyph_h = FONT_HEIGHT * scale;
    uint32_t total_w = glyph_w * len;
    uint32_t start_x = CX - total_w / 2;
    uint32_t start_y = CY - glyph_h / 2;

    for (int ci = 0; ci < len; ci++) {
        const uint8_t *glyph = &font_8x16[(uint8_t)text[ci] * FONT_HEIGHT];
        uint32_t gx = start_x + ci * glyph_w;
        for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
            uint8_t bits = glyph[row];
            for (uint32_t col = 0; col < FONT_WIDTH; col++) {
                if ((bits >> (7 - col)) & 1) {
                    for (uint32_t sy = 0; sy < scale; sy++) {
                        for (uint32_t sx = 0; sx < scale; sx++) {
                            fb_put_pixel(gx + col * scale + sx,
                                         start_y + row * scale + sy,
                                         TEXT_R, TEXT_G, TEXT_B);
                        }
                    }
                }
            }
        }
    }
}


static bool animate_trace_in(uint32_t y_start, int dir,
                             uint32_t horiz_len, uint32_t target_x) {
    int half = TRACE_HALF;

    uint32_t bend_x = horiz_len;
    uint32_t bend_y = y_start;
    for (uint32_t x = 0; x <= bend_x; x += STEP_STRIDE) {
        draw_dot(x, y_start, half, TRACE_R, TRACE_G, TRACE_B);
        draw_dot(x, y_start, half + 1, PULSE_R, PULSE_G, PULSE_B);
        pit_delay_ms(STEP_DELAY_MS);
        draw_dot(x, y_start, half + 1, TRACE_R, TRACE_G, TRACE_B);
        draw_dot(x, y_start, half, TRACE_R, TRACE_G, TRACE_B);
        if (check_skip()) return true;
    }

    draw_via(bend_x, bend_y);

    uint32_t steps = target_x - bend_x;
    for (uint32_t i = 0; i <= steps; i += STEP_STRIDE) {
        uint32_t cx = bend_x + i;
        int cy = (int)bend_y + dir * (int)i;
        if (cy < 0) cy = 0;
        draw_dot(cx, (uint32_t)cy, half, TRACE_R, TRACE_G, TRACE_B);
        draw_dot(cx, (uint32_t)cy, half + 1, PULSE_R, PULSE_G, PULSE_B);
        pit_delay_ms(STEP_DELAY_MS);
        draw_dot(cx, (uint32_t)cy, half + 1, TRACE_R, TRACE_G, TRACE_B);
        draw_dot(cx, (uint32_t)cy, half, TRACE_R, TRACE_G, TRACE_B);
        if (check_skip()) return true;
    }
    return false;
}


static bool animate_trace_out(uint32_t start_x, uint32_t start_y, int dir,
                              uint32_t diag_len) {
    int half = TRACE_HALF;

    for (uint32_t i = 0; i <= diag_len; i += STEP_STRIDE) {
        uint32_t cx = start_x + i;
        int cy = (int)start_y + dir * (int)i;
        if (cy < 0) cy = 0;
        draw_dot(cx, (uint32_t)cy, half, TRACE_R, TRACE_G, TRACE_B);
        draw_dot(cx, (uint32_t)cy, half + 1, PULSE_R, PULSE_G, PULSE_B);
        pit_delay_ms(STEP_DELAY_MS);
        draw_dot(cx, (uint32_t)cy, half + 1, TRACE_R, TRACE_G, TRACE_B);
        draw_dot(cx, (uint32_t)cy, half, TRACE_R, TRACE_G, TRACE_B);
        if (check_skip()) return true;
    }

    uint32_t bend_x = start_x + diag_len;
    int bend_y = (int)start_y + dir * (int)diag_len;
    if (bend_y < 0) bend_y = 0;
    draw_via(bend_x, (uint32_t)bend_y);

    for (uint32_t x = bend_x; x < SCRW; x += STEP_STRIDE) {
        draw_dot(x, (uint32_t)bend_y, half, TRACE_R, TRACE_G, TRACE_B);
        draw_dot(x, (uint32_t)bend_y, half + 1, PULSE_R, PULSE_G, PULSE_B);
        pit_delay_ms(STEP_DELAY_MS);
        draw_dot(x, (uint32_t)bend_y, half + 1, TRACE_R, TRACE_G, TRACE_B);
        draw_dot(x, (uint32_t)bend_y, half, TRACE_R, TRACE_G, TRACE_B);
        if (check_skip()) return true;
    }
    return false;
}


void boot_animation(void) {
    const fb_info_t *info = fb_get_info();
    SCRW = info->width;
    SCRH = info->height;
    CX = SCRW / 2;
    CY = SCRH / 2;

    fb_clear(BG_R, BG_G, BG_C);

    uint32_t scale = 6;
    uint32_t text_w = FONT_WIDTH * scale * 6;
    uint32_t text_left  = CX - text_w / 2;
    uint32_t text_right = CX + text_w / 2;

    uint32_t spread = 120;
    uint32_t y_top = CY - spread;
    uint32_t y_mid = CY;
    uint32_t y_bot = CY + spread;

    uint32_t converge_x = text_left - 40;
    uint32_t horiz_top = converge_x - spread;
    uint32_t horiz_bot = converge_x - spread;

    bool skipped = false;

    if (!skipped) skipped = animate_trace_in(y_top, +1, horiz_top, converge_x);
    if (!skipped) skipped = animate_trace_in(y_mid,  0, converge_x, converge_x);
    if (!skipped) skipped = animate_trace_in(y_bot, -1, horiz_bot, converge_x);

    if (!skipped) {
        draw_helios(scale);
        pit_delay_ms(400);
        if (check_skip()) skipped = true;
    }

    uint32_t out_x = text_right + 40;
    uint32_t out_diag = spread;
    if (!skipped) skipped = animate_trace_out(out_x, CY, -1, out_diag);
    if (!skipped) skipped = animate_trace_out(out_x, CY,  0, out_diag);
    if (!skipped) skipped = animate_trace_out(out_x, CY, +1, out_diag);

    if (!skipped) pit_delay_ms(600);

    if (skipped) {
        fb_clear(BG_R, BG_G, BG_C);
        draw_helios(scale);
        pit_delay_ms(150);
    }
}