#include "bootanim.h"
#include "framebuffer.h"
#include "font_8x16.h"
#include "pit.h"
#include "keyboard.h"
#include "../include/types.h"
#include <stdint.h>


// === PCB-inspired palette (RGB; fb_put_pixel handles BGR swap) ===
#define BG_R      0x0E
#define BG_G      0x3A
#define BG_B      0x2F
#define GRID_R    0x1A
#define GRID_G    0x55
#define GRID_B    0x45
#define TOP_COPPER_R   0xC9
#define TOP_COPPER_G   0x8A
#define TOP_COPPER_B   0x33
#define TOP_PULSE_R    0xFF
#define TOP_PULSE_G    0xE8
#define TOP_PULSE_B    0xAE
#define BOT_COPPER_R   0x59
#define BOT_COPPER_G   0xA6
#define BOT_COPPER_B   0xD9
#define BOT_PULSE_R    0xB8
#define BOT_PULSE_G    0xE9
#define BOT_PULSE_B    0xFF
#define PAD_IN_R  0x24
#define PAD_IN_G  0x24
#define PAD_IN_B  0x24
#define TEXT_R    0xFF
#define TEXT_G    0xCD
#define TEXT_B    0x6A

// === Animation tuning ===
#define STEP_STRIDE    4
#define STEP_DELAY_MS  6
#define TRACE_HALF     2
#define GRID_STEP      24

struct route {
    uint8_t n;
    int x[8];
    int y[8];
    bool on_bottom;
};

struct via_point {
    int x;
    int y;
};

static uint32_t SCRW, SCRH, CX, CY;

static bool check_skip(void)
{
    if (keyboard_has_char()) {
        char c = keyboard_getchar();
        if (c == '\n' || c == '\r') return true;
    }
    return false;
}

static int iabs(int v) { return (v < 0) ? -v : v; }
static int imax(int a, int b) { return (a > b) ? a : b; }

static void route_colors(const struct route *r,
                         uint8_t *cr, uint8_t *cg, uint8_t *cb,
                         uint8_t *pr, uint8_t *pg, uint8_t *pb)
{
    if (r->on_bottom) {
        *cr = BOT_COPPER_R; *cg = BOT_COPPER_G; *cb = BOT_COPPER_B;
        *pr = BOT_PULSE_R;  *pg = BOT_PULSE_G;  *pb = BOT_PULSE_B;
    } else {
        *cr = TOP_COPPER_R; *cg = TOP_COPPER_G; *cb = TOP_COPPER_B;
        *pr = TOP_PULSE_R;  *pg = TOP_PULSE_G;  *pb = TOP_PULSE_B;
    }
}

static void draw_brush(int cx, int cy, int half, uint8_t r, uint8_t g, uint8_t b)
{
    for (int dy = -half; dy <= half; dy++) {
        for (int dx = -half; dx <= half; dx++) {
            int x = cx + dx;
            int y = cy + dy;
            if (x >= 0 && y >= 0 && x < (int)SCRW && y < (int)SCRH)
                fb_put_pixel((uint32_t)x, (uint32_t)y, r, g, b);
        }
    }
}

static void draw_line(int x0, int y0, int x1, int y1, int half,
                      uint8_t r, uint8_t g, uint8_t b)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = imax(iabs(dx), iabs(dy));
    if (steps == 0) {
        draw_brush(x0, y0, half, r, g, b);
        return;
    }

    for (int i = 0; i <= steps; i++) {
        int x = x0 + (dx * i) / steps;
        int y = y0 + (dy * i) / steps;
        draw_brush(x, y, half, r, g, b);
    }
}

static void draw_pad(int cx, int cy, int radius, bool on_bottom)
{
    uint8_t copper_r = on_bottom ? BOT_COPPER_R : TOP_COPPER_R;
    uint8_t copper_g = on_bottom ? BOT_COPPER_G : TOP_COPPER_G;
    uint8_t copper_b = on_bottom ? BOT_COPPER_B : TOP_COPPER_B;

    int r2 = radius * radius;
    int inner = radius - 3;
    int inner2 = inner * inner;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;

            int x = cx + dx;
            int y = cy + dy;
            if (x < 0 || y < 0 || x >= (int)SCRW || y >= (int)SCRH) continue;

            if (d2 >= inner2) fb_put_pixel((uint32_t)x, (uint32_t)y, copper_r, copper_g, copper_b);
            else              fb_put_pixel((uint32_t)x, (uint32_t)y, PAD_IN_R, PAD_IN_G, PAD_IN_B);
        }
    }
}

static void draw_cross_layer_via(int cx, int cy)
{
    int radius = 9;
    int r2 = radius * radius;
    int mid = (radius - 3) * (radius - 3);
    int inner = (radius - 6) * (radius - 6);

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;

            int x = cx + dx;
            int y = cy + dy;
            if (x < 0 || y < 0 || x >= (int)SCRW || y >= (int)SCRH) continue;

            if (d2 >= mid) {
                fb_put_pixel((uint32_t)x, (uint32_t)y, TOP_COPPER_R, TOP_COPPER_G, TOP_COPPER_B);
            } else if (d2 >= inner) {
                fb_put_pixel((uint32_t)x, (uint32_t)y, BOT_COPPER_R, BOT_COPPER_G, BOT_COPPER_B);
            } else {
                fb_put_pixel((uint32_t)x, (uint32_t)y, PAD_IN_R, PAD_IN_G, PAD_IN_B);
            }
        }
    }
}

static void draw_cross_layer_via_glow(int cx, int cy, bool bottom_phase)
{
    uint8_t r = bottom_phase ? BOT_PULSE_R : TOP_PULSE_R;
    uint8_t g = bottom_phase ? BOT_PULSE_G : TOP_PULSE_G;
    uint8_t b = bottom_phase ? BOT_PULSE_B : TOP_PULSE_B;

    /* A soft halo makes the active layer switch obvious without overpowering traces. */
    draw_brush(cx, cy, 2, r, g, b);
    draw_brush(cx, cy, 1, r, g, b);
    draw_cross_layer_via(cx, cy);
}

static void draw_via_glow_set(const struct via_point *vias, uint8_t nvias, bool bottom_phase)
{
    for (uint8_t i = 0; i < nvias; i++)
        draw_cross_layer_via_glow(vias[i].x, vias[i].y, bottom_phase);
}

static void draw_board_background(void)
{
    for (uint32_t y = 0; y < SCRH; y++) {
        for (uint32_t x = 0; x < SCRW; x++) {
            uint8_t r = BG_R;
            uint8_t g = BG_G;
            uint8_t b = BG_B;
            if (((x + y) & 31u) == 0) {
                r += 2; g += 3; b += 2;
            }
            fb_put_pixel(x, y, r, g, b);
        }
    }

    for (uint32_t x = 0; x < SCRW; x += GRID_STEP)
        draw_line((int)x, 0, (int)x, (int)SCRH - 1, 0, GRID_R, GRID_G, GRID_B);
    for (uint32_t y = 0; y < SCRH; y += GRID_STEP)
        draw_line(0, (int)y, (int)SCRW - 1, (int)y, 0, GRID_R, GRID_G, GRID_B);

    for (uint32_t y = GRID_STEP / 2; y < SCRH; y += GRID_STEP * 2) {
        for (uint32_t x = GRID_STEP / 2; x < SCRW; x += GRID_STEP * 2) {
            draw_pad((int)x, (int)y, 3, false);
        }
    }
}

static void draw_emblinkos(uint32_t scale)
{
    const char *text = "EmbLinkOs";
    int len = 9;
    uint32_t glyph_w = FONT_WIDTH * scale;
    uint32_t glyph_h = FONT_HEIGHT * scale;
    uint32_t total_w = glyph_w * (uint32_t)len;
    uint32_t start_x = CX - total_w / 2;
    uint32_t start_y = CY - glyph_h / 2;

    for (int ci = 0; ci < len; ci++) {
        const uint8_t *glyph = &font_8x16[(uint8_t)text[ci] * FONT_HEIGHT];
        uint32_t gx = start_x + (uint32_t)ci * glyph_w;
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

static void draw_route_base(const struct route *r)
{
    uint8_t copper_r, copper_g, copper_b;
    uint8_t pulse_r, pulse_g, pulse_b;
    route_colors(r, &copper_r, &copper_g, &copper_b, &pulse_r, &pulse_g, &pulse_b);

    for (uint8_t i = 0; i + 1 < r->n; i++)
        draw_line(r->x[i], r->y[i], r->x[i + 1], r->y[i + 1], TRACE_HALF, copper_r, copper_g, copper_b);

    for (uint8_t i = 0; i < r->n; i++)
        draw_pad(r->x[i], r->y[i], 7, r->on_bottom);
}

static bool animate_route_signal(const struct route *r,
                                 const struct via_point *vias, uint8_t nvias)
{
    uint8_t copper_r, copper_g, copper_b;
    uint8_t pulse_r, pulse_g, pulse_b;
    route_colors(r, &copper_r, &copper_g, &copper_b, &pulse_r, &pulse_g, &pulse_b);

    for (uint8_t seg = 0; seg + 1 < r->n; seg++) {
        int x0 = r->x[seg];
        int y0 = r->y[seg];
        int x1 = r->x[seg + 1];
        int y1 = r->y[seg + 1];
        int dx = x1 - x0;
        int dy = y1 - y0;
        int steps = imax(iabs(dx), iabs(dy));

        if (steps == 0) continue;
        for (int i = 0; i <= steps; i += STEP_STRIDE) {
            int x = x0 + (dx * i) / steps;
            int y = y0 + (dy * i) / steps;
            draw_brush(x, y, TRACE_HALF + 1, pulse_r, pulse_g, pulse_b);
            draw_via_glow_set(vias, nvias, ((i / STEP_STRIDE) & 1) != 0);
            fb_present();
            pit_delay_ms(STEP_DELAY_MS);
            draw_brush(x, y, TRACE_HALF + 1, copper_r, copper_g, copper_b);
            if (check_skip()) return true;
        }
    }
    return false;
}

void boot_animation(void)
{
    const fb_info_t *info = fb_get_info();
    SCRW = info->width;
    SCRH = info->height;
    CX = SCRW / 2;
    CY = SCRH / 2;

    draw_board_background();

    uint32_t scale = (SCRW >= 1024) ? 5 : 4;
    uint32_t text_w = FONT_WIDTH * scale * 9;
    int text_left = (int)(CX - text_w / 2);
    int text_right = (int)(CX + text_w / 2);

    struct route in_top = {
        .n = 4,
        .x = { 24, 170, text_left - 88, text_left - 22 },
        .y = { (int)CY - 170, (int)CY - 170, (int)CY - 85, (int)CY - 32 },
        .on_bottom = false,
    };
    struct route in_mid = {
        .n = 3,
        .x = { 18, text_left - 70, text_left - 18 },
        .y = { (int)CY, (int)CY, (int)CY },
        .on_bottom = true,
    };
    struct route in_bot = {
        .n = 4,
        .x = { 26, 175, text_left - 86, text_left - 20 },
        .y = { (int)CY + 170, (int)CY + 170, (int)CY + 88, (int)CY + 34 },
        .on_bottom = false,
    };
    struct route out_top = {
        .n = 4,
        .x = { text_right + 22, text_right + 88, (int)SCRW - 190, (int)SCRW - 20 },
        .y = { (int)CY - 32, (int)CY - 88, (int)CY - 170, (int)CY - 170 },
        .on_bottom = true,
    };
    struct route out_mid = {
        .n = 3,
        .x = { text_right + 18, text_right + 70, (int)SCRW - 20 },
        .y = { (int)CY, (int)CY, (int)CY },
        .on_bottom = false,
    };
    struct route out_bot = {
        .n = 4,
        .x = { text_right + 20, text_right + 86, (int)SCRW - 192, (int)SCRW - 22 },
        .y = { (int)CY + 34, (int)CY + 90, (int)CY + 170, (int)CY + 170 },
        .on_bottom = true,
    };

    struct via_point layer_vias[] = {
        { text_left - 18,  (int)CY },
        { text_left - 22,  (int)CY - 32 },
        { text_left - 20,  (int)CY + 34 },
        { text_right + 18, (int)CY },
        { text_right + 22, (int)CY - 32 },
        { text_right + 20, (int)CY + 34 },
    };
    uint8_t via_count = (uint8_t)(sizeof layer_vias / sizeof layer_vias[0]);

    draw_route_base(&in_top);
    draw_route_base(&in_mid);
    draw_route_base(&in_bot);
    draw_route_base(&out_top);
    draw_route_base(&out_mid);
    draw_route_base(&out_bot);

    /* Explicit layer-change markers around the logo routing fan-in/fan-out. */
    for (uint8_t i = 0; i < via_count; i++)
        draw_cross_layer_via(layer_vias[i].x, layer_vias[i].y);

    draw_emblinkos(scale);
    fb_present();

    bool skipped = false;
    if (!skipped) skipped = animate_route_signal(&in_top, layer_vias, via_count);
    if (!skipped) skipped = animate_route_signal(&in_mid, layer_vias, via_count);
    if (!skipped) skipped = animate_route_signal(&in_bot, layer_vias, via_count);
    if (!skipped) skipped = animate_route_signal(&out_top, layer_vias, via_count);
    if (!skipped) skipped = animate_route_signal(&out_mid, layer_vias, via_count);
    if (!skipped) skipped = animate_route_signal(&out_bot, layer_vias, via_count);

    if (!skipped) pit_delay_ms(350);

    if (skipped) {
        draw_board_background();
        draw_route_base(&in_top);
        draw_route_base(&in_mid);
        draw_route_base(&in_bot);
        draw_route_base(&out_top);
        draw_route_base(&out_mid);
        draw_route_base(&out_bot);
        for (uint8_t i = 0; i < via_count; i++)
            draw_cross_layer_via(layer_vias[i].x, layer_vias[i].y);
        draw_emblinkos(scale);
        fb_present();
        pit_delay_ms(120);
    }
}