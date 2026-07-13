/* ui/dsl/em_app.c -- the EmApplication runtime (EmUI V4.1).
 *
 * TARGET-ONLY translation unit (it speaks the EmbLink SDK); linked into
 * libembk.so but never into the host test/showcase builds. One EM_APPLICATION
 * declaration replaces an app's whole main():
 *
 *   - resources: installs the embk file loader and loads the app font
 *   - toolkit bring-up: arenas, theme, ui_init, animation clock
 *   - the window: kernel-chrome or CHROMELESS (then WindowBar dragging is
 *     wired automatically via the registered mover + origin binding)
 *   - the loop with RETAINED updates: a frame is built ONLY on an input edge,
 *     a ui-epoch bump, a nav transition, or when a live animation asked for
 *     it (em_request_frame). Idle frames cost one input poll + sleep.
 *   - presents: dirty-rect normally; full clear+repaint on structural frames
 *   - teardown: the view's own CloseButton (em_window_closed) or ESC. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "scene_render.h"
#include "font.h"

/* --- the embk resource loader (whole file -> malloc'd buffer) ------------ */
static uint8_t *emapp_load(const char *path, size_t *out_len) {
    int fd = (int)embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) return 0;
    size_t cap = 1u << 20, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { embk_close(fd); return 0; }
    for (;;) {
        if (n + 65536 > cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) { embk_close(fd); return 0; } }
        int64_t got = embk_read(fd, buf + n, 65536);
        if (got <= 0) break;
        n += (size_t)got;
    }
    embk_close(fd);
    *out_len = n;
    return buf;
}

static void emapp_mover(int win, int32_t x, int32_t y) { embk_win_move(win, x, y); }

int em_app_run(const EmApp *app) {
    if (!app || !app->view) return 1;
    const char *title = app->title ? app->title : "EmApp";
    uint64_t t0 = embk_uptime_ms();

    /* resources + toolkit */
    em_res_set_loader(emapp_load);
    uint32_t fr = em_font(app->font ? app->font : "/font.ttf");
    { char b[96]; snprintf(b, sizeof b, "%s: font loaded (+%lums)\n", title,
                           (unsigned long)(embk_uptime_ms() - t0)); embk_puts(1, b); }

    struct scene_arena sa; scene_arena_init(&sa);
    struct layout_arena la; layout_arena_init(&la);
    ui_theme_set_fonts(fr, fr);
    em_theme_use(app->theme);
    ui_init(&sa, &la);
    em_set_clock(embk_uptime_ms);

    /* the window: requested content size, clamped + centred on the screen */
    uint32_t sw = 0, sh = 0;
    embk_screen_size(&sw, &sh);
    int winw = app->size.w > 0 ? app->size.w : 640;
    int winh = app->size.h > 0 ? app->size.h : 480;
    int chromeless = (app->chrome == Chromeless);
    int bar = chromeless ? 0 : 26;
    if (sw && winw > (int)sw - 8)        winw = (int)sw - 8;
    if (sh && winh > (int)sh - bar - 16) winh = (int)sh - bar - 16;
    if (winw < 200) winw = 200;
    if (winh < 160) winh = 160;
    int wx = ((int)sw - winw) / 2;        if (wx < 0) wx = 0;
    int wy = ((int)sh - winh - bar) / 2;  if (wy < 0) wy = 0;

    uint32_t *px = 0;
    int win = embk_win_create_shared_ex((uint32_t)winw, (uint32_t)winh, wx, wy, title,
                                        chromeless ? EMBK_WINF_CHROMELESS : 0, (void **)&px);
    if (win < 0 || !px) {
        char b[64]; snprintf(b, sizeof b, "%s: win_create FAILED\n", title); embk_puts(1, b);
        return 1;
    }
    if (chromeless) {                    /* the view's WindowBar drags us */
        em_window_set_mover(emapp_mover);
        em_window_bind(win, wx, wy);
    }
    em_window_set_resizable(app->resize == Resizable);
    embk_key_grab(1);

    struct render_target rt = { px, (uint32_t)winw, (uint32_t)winh, (uint32_t)winw * 4, EMBK_PIXFMT_BGRA8888_PRE };
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    { char b[64]; snprintf(b, sizeof b, "%s: interactive loop running\n", title); embk_puts(1, b); }

    int pace = app->pace_ms > 0 ? app->pace_ms : 10;
    int prev_epoch = em_ui_epoch(), first = 1;
    struct embk_win_input prev_in; memset(&prev_in, 0, sizeof prev_in);

    for (;;) {
        /* --- inputs (always polled; they are the retained-update triggers) --- */
        struct embk_win_input in;
        embk_win_input(&in);
        int had_key = 0;
        for (int c; (c = embk_key_poll()) != 0; ) {
            if (c == 27) {
                embk_win_destroy(win); embk_key_grab(0);
                char b[64]; snprintf(b, sizeof b, "%s: exit\n", title); embk_puts(1, b);
                return 0;
            }
            ui_input_char(c);
            had_key = 1;
        }
        int input_edge = had_key ||
                         in.focused != prev_in.focused ||
                         (in.focused && (in.x != prev_in.x || in.y != prev_in.y ||
                                         in.buttons != prev_in.buttons || in.wheel));
        prev_in = in;

        /* --- retained updates: skip ALL ui work on untouched frames --------- */
        int build = first || input_edge || em_take_frame_request() ||
                    em_ui_epoch() != prev_epoch || em_nav_transitioning();
        if (!build) { embk_sleep_ms(pace); continue; }

        if (in.focused) {
            ui_pointer((float)in.x, (float)in.y, (in.buttons & EMBK_MOUSE_LEFT) != 0);
            if (in.wheel) ui_wheel((float)in.wheel);
        } else {
            ui_pointer(-100.0f, -100.0f, false);
        }

        /* full clear+repaint on structural frames (epoch bump / first) */
        bool force_full = false;
        if (first || em_ui_epoch() != prev_epoch) {
            const struct ui_theme *t = ui_theme();
            uint32_t bg = (255u << 24) | ((uint32_t)(t->bg.r * 255) << 16)
                        | ((uint32_t)(t->bg.g * 255) << 8) | (uint32_t)(t->bg.b * 255);
            for (int i = 0; i < winw * winh; i++) px[i] = bg;
            scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
            prev_epoch = em_ui_epoch();
            force_full = true;
        }

        ui_frame_begin(); em_new_frame(); app->view(); em_flush(); ui_frame_end();
        ui_run_layout((float)winw, (float)winh);
        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);

        if (em_window_closed()) {        /* the view's OWN close control fired */
            embk_win_destroy(win); embk_key_grab(0);
            char b[64]; snprintf(b, sizeof b, "%s: closed by its own button\n", title); embk_puts(1, b);
            return 0;
        }

        if (force_full || r.full || r.n_dirty == 0) {
            embk_win_present(win, px, (uint32_t)winw, (uint32_t)winh);
        } else {
            int x0 = 1 << 29, y0 = 1 << 29, x1 = -(1 << 29), y1 = -(1 << 29);
            for (int i = 0; i < r.n_dirty; i++) {
                int a = (int)r.dirty[i].x, b = (int)r.dirty[i].y;
                int c = (int)(r.dirty[i].x + r.dirty[i].w) + 1, d = (int)(r.dirty[i].y + r.dirty[i].h) + 1;
                if (a < x0) x0 = a;
                if (b < y0) y0 = b;
                if (c > x1) x1 = c;
                if (d > y1) y1 = d;
            }
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > winw) x1 = winw;
            if (y1 > winh) y1 = winh;
            if (x1 > x0 && y1 > y0)
                embk_win_present_rect(win, px, (uint32_t)winw, (uint32_t)winh, x0, y0, x1 - x0, y1 - y0);
        }

        if (first) {
            first = 0;
            char b[96]; snprintf(b, sizeof b, "%s: first frame presented (+%lums)\n", title,
                                 (unsigned long)(embk_uptime_ms() - t0)); embk_puts(1, b);
        }

        /* grip released with a real delta -> re-back the window at the new
         * size; the old pixel pointer dies inside embk_win_resize. */
        int dw = 0, dh = 0;
        if (em_window_take_resize(&dw, &dh)) {
            int nw = winw + dw, nh = winh + dh;
            if (nw < 200) nw = 200;
            if (nh < 160) nh = 160;
            if (sw && nw > (int)sw) nw = (int)sw;
            if (sh && nh > (int)sh) nh = (int)sh;
            uint32_t *npx = 0;
            if (embk_win_resize(win, (uint32_t)nw, (uint32_t)nh, (void **)&npx) >= 0 && npx) {
                px = npx; winw = nw; winh = nh;
                rt.pixels = px; rt.width = (uint32_t)winw; rt.height = (uint32_t)winh;
                rt.stride = (uint32_t)winw * 4;
                const struct ui_theme *t = ui_theme();
                uint32_t bg = (255u << 24) | ((uint32_t)(t->bg.r * 255) << 16)
                            | ((uint32_t)(t->bg.g * 255) << 8) | (uint32_t)(t->bg.b * 255);
                for (int i = 0; i < winw * winh; i++) px[i] = bg;
                scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
                ui_frame_begin(); em_new_frame(); app->view(); em_flush(); ui_frame_end();
                ui_run_layout((float)winw, (float)winh);
                scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);
                embk_win_present(win, px, (uint32_t)winw, (uint32_t)winh);
                char b[64]; snprintf(b, sizeof b, "%s: resized to %dx%d\n", title, winw, winh); embk_puts(1, b);
            }
        }
        embk_sleep_ms(pace);
    }
}

/* ======================================================================= */
/* em_widget_run -- the DESKTOP WIDGET runtime (V5).                       */
/* A trimmed em_app_run: EMBK_WINF_WIDGET window at a fixed position, no    */
/* keyboard grab, no close control, and an optional refresh_ms timer that   */
/* re-runs the view (a clock ticks with refresh_ms=1000; everything else    */
/* stays fully retained/idle).                                             */
/* ======================================================================= */

int em_widget_run(const EmWidget *wg) {
    if (!wg || !wg->view) return 1;
    const char *title = wg->title ? wg->title : "Widget";

    em_res_set_loader(emapp_load);
    uint32_t fr = em_font(wg->font ? wg->font : "/font.ttf");

    struct scene_arena sa; scene_arena_init(&sa);
    struct layout_arena la; layout_arena_init(&la);
    ui_theme_set_fonts(fr, fr);
    em_theme_use(wg->theme);
    ui_init(&sa, &la);
    em_set_clock(embk_uptime_ms);

    int winw = wg->size.w > 0 ? wg->size.w : 190;
    int winh = wg->size.h > 0 ? wg->size.h : 96;
    uint32_t *px = 0;
    int win = embk_win_create_shared_ex((uint32_t)winw, (uint32_t)winh,
                                        wg->pos.x, wg->pos.y, title,
                                        EMBK_WINF_WIDGET, (void **)&px);
    if (win < 0 || !px) {
        char b[64]; snprintf(b, sizeof b, "%s: widget create FAILED\n", title); embk_puts(1, b);
        return 1;
    }

    struct render_target rt = { px, (uint32_t)winw, (uint32_t)winh, (uint32_t)winw * 4, EMBK_PIXFMT_BGRA8888_PRE };
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    { char b[64]; snprintf(b, sizeof b, "%s: widget up\n", title); embk_puts(1, b); }

    int pace = wg->pace_ms > 0 ? wg->pace_ms : 50;   /* widgets idle harder */
    int prev_epoch = em_ui_epoch(), first = 1;
    uint64_t last_tick = 0;
    struct embk_win_input prev_in; memset(&prev_in, 0, sizeof prev_in);

    for (;;) {
        struct embk_win_input in;
        embk_win_input(&in);
        int input_edge = in.focused != prev_in.focused ||
                         (in.focused && (in.x != prev_in.x || in.y != prev_in.y ||
                                         in.buttons != prev_in.buttons));
        prev_in = in;

        uint64_t now = embk_uptime_ms();
        int tick = wg->refresh_ms > 0 && (now - last_tick) >= (uint64_t)wg->refresh_ms;

        int build = first || input_edge || tick || em_take_frame_request() ||
                    em_ui_epoch() != prev_epoch;
        if (!build) { embk_sleep_ms(pace); continue; }
        if (tick) last_tick = now;

        if (in.focused) ui_pointer((float)in.x, (float)in.y, (in.buttons & EMBK_MOUSE_LEFT) != 0);
        else            ui_pointer(-100.0f, -100.0f, false);

        if (first || em_ui_epoch() != prev_epoch) {
            const struct ui_theme *t = ui_theme();
            uint32_t bg = (255u << 24) | ((uint32_t)(t->bg.r * 255) << 16)
                        | ((uint32_t)(t->bg.g * 255) << 8) | (uint32_t)(t->bg.b * 255);
            for (int i = 0; i < winw * winh; i++) px[i] = bg;
            scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
            prev_epoch = em_ui_epoch();
        }

        ui_frame_begin(); em_new_frame(); wg->view(); em_flush(); ui_frame_end();
        ui_run_layout((float)winw, (float)winh);
        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);
        embk_win_present(win, px, (uint32_t)winw, (uint32_t)winh);

        if (first) { first = 0; char b[64]; snprintf(b, sizeof b, "%s: widget first frame\n", title); embk_puts(1, b); }
        embk_sleep_ms(pace);
    }
}
