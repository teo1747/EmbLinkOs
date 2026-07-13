/* user/bin/wmdemo.c -- the EmbLink window-compositor demo.
 *
 * Piece 2 of the UI stack. Where uidemo.c presents ONE surface centred on the
 * framebuffer, this creates TWO kernel-composited WINDOWS from a single ring-3
 * process:
 *   - Window 1 hosts the real EmUI toolkit (a live card with an animated
 *     bar + chart), rendered with the CPU backend + TrueType rasteriser and
 *     PRESENTED into the window's content buffer.
 *   - Window 2 is drawn directly (a small monitor panel with a sweeping
 *     accent), proving a window needn't use the toolkit at all.
 * The kernel compositor draws both over a desktop, each with a title bar, in
 * z-order. Window 2 then sweeps horizontally across Window 1 -- because a move
 * raises a window to the front, you see it occlude Window 1 and the desktop
 * recompose live behind it. ESC quits (destroys both windows). */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include "embk.h"
#include "kit.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "scene_render.h"
#include "font.h"

/* window 1: the toolkit card. window 2: the direct-drawn panel. */
#define W1_W 340
#define W1_H 320
#define W2_W 260
#define W2_H 180

#define SCREEN_W 1024
#define SCREEN_H 768

static float g_load = 0.5f;

static uint8_t *read_file(const char *path, size_t *len) {
    int fd = (int)embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) return 0;
    size_t cap = 1u << 20, n = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (n + 65536 > cap) { cap *= 2; buf = realloc(buf, cap); }
        int64_t got = embk_read(fd, buf + n, 65536);
        if (got <= 0) break;
        n += (size_t)got;
    }
    embk_close(fd);
    *len = n;
    return buf;
}

/* ---- window 1: the EmUI toolkit view ------------------------------------ */
static void win1_app(void) {
    static const float thru[7] = { 0.35f, 0.55f, 0.42f, 0.78f, 0.60f, 0.88f, 0.72f };
    Screen(.justify = Center, .align = Center) {
        Card(.width = 300, .spacing = 12) {
            NavBar("System") {
                Badge("live").accent();
            }
            HStack(.spacing = 10) {
                Avatar("EM");
                VStack(.spacing = 2, .align = Leading) {
                    Text("EmbLink OS").bold();
                    Text("compositor demo").caption().secondary();
                }
            }
            Divider();
            Text("CPU LOAD").caption().tertiary();
            ProgressBar(em_animate("load", g_load, 1.8f));
            Text("THROUGHPUT").caption().tertiary();
            Chart(thru, 7, .height = 64);
        }
    }
}

/* ---- window 2: a directly-drawn monitor panel --------------------------- */
static inline uint32_t px_argb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static void fillrect(uint32_t *buf, int w, int h, int x, int y, int rw, int rh, uint32_t c) {
    for (int yy = y; yy < y + rh; yy++) {
        if (yy < 0 || yy >= h) continue;
        for (int xx = x; xx < x + rw; xx++) {
            if (xx < 0 || xx >= w) continue;
            buf[yy * w + xx] = c;
        }
    }
}
static void win2_gradient_rows(uint32_t *buf, int w, int h, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        int t = (y * 255) / (h - 1);
        uint8_t r = 0x14 + (0x20 - 0x14) * t / 255;
        uint8_t g = 0x16 + (0x24 - 0x16) * t / 255;
        uint8_t b = 0x20 + (0x38 - 0x20) * t / 255;
        uint32_t c = px_argb(r, g, b);
        for (int x = 0; x < w; x++) buf[y * w + x] = c;
    }
}
/* one-time: the static gradient + stat pips (never change after this) */
static void draw_win2_bg(uint32_t *buf, int w, int h) {
    win2_gradient_rows(buf, w, h, 0, h);
    fillrect(buf, w, h, 16, 16, 10, 10, px_argb(0x5e, 0xd6, 0x8a));
    fillrect(buf, w, h, 34, 16, 10, 10, px_argb(0xff, 0xd9, 0x5e));
    fillrect(buf, w, h, 52, 16, 10, 10, px_argb(0xe0, 0x6b, 0x6b));
}
/* per-frame: redraw ONLY the animated band (sweep bar + meters) over a fresh
 * slice of the gradient, so the static background isn't rewritten every frame. */
#define W2_BAND_Y0 (W2_H/2 - 12)
static void draw_win2_band(uint32_t *buf, int w, int h, float phase) {
    win2_gradient_rows(buf, w, h, W2_BAND_Y0, h);
    int track_x = 20, track_w = w - 40, bar_w = 64;
    int bx = track_x + (int)((track_w - bar_w) * (0.5f + 0.5f * sinf(phase)));
    fillrect(buf, w, h, track_x, h / 2 - 4, track_w, 8, px_argb(0x2a, 0x2e, 0x40));
    fillrect(buf, w, h, bx, h / 2 - 8, bar_w, 16, px_argb(0x5b, 0x63, 0xd6));
    for (int i = 0; i < 8; i++) {
        int bh = 8 + (int)(28 * (0.5f + 0.5f * sinf(phase * 1.7f + i * 0.6f)));
        fillrect(buf, w, h, 22 + i * 28, h - 20 - bh, 18, bh, px_argb(0x3d, 0x8b, 0xff));
    }
}

int main(void) {
    /* toolkit font + context (only window 1 uses the toolkit) */
    size_t rl = 0;
    uint8_t *reg = read_file("/font.ttf", &rl);
    uint32_t fr = reg ? font_load(reg, rl) : 0;
    if (fr) font_install_backend();
    embk_puts(1, fr ? "wmdemo: font loaded\n" : "wmdemo: FONT MISSING\n");

    struct scene_arena sa; scene_arena_init(&sa);
    struct layout_arena la; layout_arena_init(&la);
    ui_theme_set_fonts(fr, fr);
    ui_theme_use_dark(true);
    ui_init(&sa, &la);

    /* BOTH windows are ZERO-COPY: the kernel maps each window's pixel pages into
     * us, so window 1's toolkit renderer draws its card STRAIGHT into shared
     * memory (render target = the mapped buffer) and window 2 is drawn directly
     * too -- present is a damage-only call for both, no pixel copies. They
     * overlap and are static; the kernel compositor routes the pointer
     * (click-to-focus, title-bar drag). */
    uint32_t *w1buf = 0, *w2buf = 0;
    int win1 = embk_win_create_shared(W1_W, W1_H, 250, 160, "System Monitor", (void **)&w1buf);
    int win2 = embk_win_create_shared(W2_W, W2_H, 430, 300, "Activity", (void **)&w2buf);
    if (win1 < 0 || win2 < 0 || !w1buf || !w2buf) { embk_puts(1, "wmdemo: win_create FAILED\n"); return 1; }

    struct render_target rt1 = { w1buf, W1_W, W1_H, (uint32_t)W1_W * 4, EMBK_PIXFMT_BGRA8888_PRE };
    struct scene_renderer r1; scene_render_init(&r1, cpu_backend_get());

    draw_win2_bg(w2buf, W2_W, W2_H);          /* static background painted once ... */
    embk_win_present(win2, w2buf, W2_W, W2_H); /* ... and composited once (loop presents only the band) */

    embk_key_grab(1);
    em_set_clock(embk_uptime_ms);
    embk_puts(1, "wmdemo: two windows composited; click to focus, drag title bars; ESC quits\n");

    for (;;) {
        for (int c; (c = embk_key_poll()) != 0; )
            if (c == 27) {
                embk_win_destroy(win2); embk_win_destroy(win1);
                embk_key_grab(0); embk_puts(1, "wmdemo: exit\n"); return 0;
            }

        uint64_t t = embk_uptime_ms();
        float ph = (float)t / 1000.0f;

        /* window 1 target load oscillates so the toolkit bar keeps easing */
        g_load = 0.55f + 0.35f * sinf(ph * 0.8f);

        /* render window 1 (toolkit); PRESENT ONLY THE DIRTY RECT so the kernel
         * compositor recomposites just the changed sub-region (a few hundred px
         * for an animating bar), not the whole 340x320 window every frame. */
        ui_pointer(-100.0f, -100.0f, false);
        ui_frame_begin(); em_new_frame(); win1_app(); em_flush(); ui_frame_end();
        ui_run_layout((float)W1_W, (float)W1_H);
        scene_render_frame(&r1, &sa, ui_scene_of(ui_root()), &rt1);
        if (r1.full || r1.n_dirty == 0) {
            embk_win_present(win1, w1buf, W1_W, W1_H);
        } else {
            int x0 = 1<<29, y0 = 1<<29, x1 = -(1<<29), y1 = -(1<<29);
            for (int i = 0; i < r1.n_dirty; i++) {
                int a = (int)r1.dirty[i].x, b = (int)r1.dirty[i].y;
                int c = (int)(r1.dirty[i].x + r1.dirty[i].w) + 1, d = (int)(r1.dirty[i].y + r1.dirty[i].h) + 1;
                if (a < x0) x0 = a;
                if (b < y0) y0 = b;
                if (c > x1) x1 = c;
                if (d > y1) y1 = d;
            }
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > W1_W) x1 = W1_W;
            if (y1 > W1_H) y1 = W1_H;
            if (x1 > x0 && y1 > y0)
                embk_win_present_rect(win1, w1buf, W1_W, W1_H, x0, y0, x1 - x0, y1 - y0);
        }

        /* window 2: redraw + present ONLY the animated band (the static gradient
         * background was painted once, before the loop). */
        draw_win2_band(w2buf, W2_W, W2_H, ph);
        embk_win_present_rect(win2, w2buf, W2_W, W2_H, 0, W2_BAND_Y0, W2_W, W2_H - W2_BAND_Y0);

        embk_sleep_ms(10);   /* pace while YIELDING (present-bound anyway) */
    }
    return 0;
}
