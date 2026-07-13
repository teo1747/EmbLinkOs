/* user/bin/uidemo.c -- a live ring-3 EmbLink UI app.
 *
 * The bridge: builds a real settings card with the themed widget kit, renders
 * it into a Piece-1 SURFACE with the CPU backend + TrueType rasterizer, and
 * PRESENTS that surface to the framebuffer (sys_ui_present) -- so the toolkit
 * that was host-tested to PNG now draws live on screen in the OS.
 *
 * newlib-linked (libc + libm + malloc, SSE ok). Fonts are read from the FAT
 * volume mounted at /fat32. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "embk.h"
#include "kit.h"
#include "ui.h"
#include "em.h"          /* EmUI V2 -- the SwiftUI-flavored DSL */
#include "theme.h"
#include "scene_render.h"
#include "font.h"

#define W 560
#define H 760

/* Read a whole file via raw VFS syscalls. embk_read advances the fd position,
 * and (since the embkfs O(n^2) read fix) chunked sequential reads are served
 * from the fs's whole-object cache, so this is fast. */
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

/* live app state -- SwiftUI's @State, here plain C variables the views bind to */
static bool  g_dark = true, g_saved = false;
static char  g_name[32] = "";
static int   g_tab = 0;
static float g_vol = 0.6f;
static float g_scroll = 0.0f;
static float g_sync = 0.0f;    /* animation target for the Sync progress */
static bool  g_alert = false;  /* the confirm-discard modal is open */

/* A second page, reached by Push() -- the fade-in transition (em_nav) plays
 * when navigating to/from it. */
static void details_page(void);

/* THE ENTIRE UI, written in the EmUI V2 DSL -- chainable modifiers + bindings. */
static void app(void) {
    static const char *const tabs[] = { "Display", "Sound" };

    Screen(.justify = Center, .align = Center) {
        Card(.width = 380, .spacing = 14) {

            NavBar("Settings") {
                if (IconButton(IconGear).id("gear").clicked()) Push(details_page);
            }

            HStack(.spacing = 12) {
                Avatar("EM");
                VStack(.spacing = 2, .align = Leading) {
                    Text("EmbLink OS").bold();
                    Text("Interactive demo").caption().secondary();
                }
                Spacer();
                if (g_saved) Badge("Saved").success();
                else         Badge("Live").accent();
            }

            Divider();

            Text("Name").caption().secondary();
            if (TextField(g_name, sizeof g_name, "Enter your name...").focused()) g_saved = false;

            Segmented(tabs, 2, &g_tab);
            Toggle("Dark mode", &g_dark);

            Row() { Text("Volume"); Spacer(); }
            Slider(&g_vol);

            /* animated: the bar eases toward its target every frame */
            Row() {
                Text("Sync");
                Spacer();
                if (Button(g_sync > 0.5f ? "Reset" : "Start").ghost().clicked())
                    g_sync = g_sync > 0.5f ? 0.0f : 1.0f;
            }
            ProgressBar(em_animate("sync", g_sync, 2.5f));

            Text("RECENT").caption().tertiary();
            ScrollView(&g_scroll, 108) {
                for (int i = 0; i < 9; i++) {
                    char nm[24]; snprintf(nm, sizeof nm, "Document %d", i + 1);
                    Row() {
                        Label(IconDot, nm);
                        Spacer();
                        Text("2 KB").caption().secondary();
                    }
                }
            }

            Divider();

            HStack(.spacing = 10) {
                Spacer();
                if (Button("Cancel").secondary().clicked()) g_alert = true;   /* confirm first */
                if (Button("Save changes").primary().clicked()) g_saved = true;
            }
        }
    }
}

/* The modal confirm dialog. Declared at the ROOT (a sibling of the navigation
 * stack) so it overlays whatever page shows. */
static void modal(void) {
    if (!g_alert) return;
    Overlay() {
        Dialog(.width = 300, .spacing = 14) {
            Text("Discard changes?").title();
            Text("Your edits will be lost.").secondary();
            HStack(.spacing = 10) {
                Spacer();
                if (Button("Keep").secondary().clicked()) g_alert = false;
                if (Button("Discard").destructive().clicked()) {
                    g_name[0] = 0; g_vol = 0.6f; g_saved = false; g_alert = false;
                }
            }
        }
    }
    if (OverlayDismissed()) g_alert = false;
}

/* the pushed page -- fades in over the settings card via em_nav's transition. */
static void details_page(void) {
    Screen(.justify = Center, .align = Center) {
        Card(.width = 380, .spacing = 14) {
            NavBar("About") {
                if (Button("Back").ghost().clicked()) Pop();
            }
            HStack(.spacing = 12) {
                Avatar("EM");
                VStack(.spacing = 2, .align = Leading) {
                    Text("EmbLink OS").bold();
                    Text("build 2026.07").caption().secondary();
                }
            }
            Divider();
            Text("This page slid in with an eased opacity transition -- em_nav "
                 "cross-fades the incoming page over the one below.").caption().secondary();
            Row() { Text("Kernel"); Spacer(); Text("x86-64").caption().secondary(); }
            Row() { Text("Compositor"); Spacer(); Text("Piece 2").caption().secondary(); }
            Row() { Text("Windows"); Spacer(); Text("live").caption().success(); }
        }
    }
}


int main(void) {
    /* Load the font from the EMBKFS root (fast now that the fs read is O(n)). */
    uint64_t t_start = embk_uptime_ms();   /* launch-latency log */
    size_t rl = 0;
    uint8_t *reg = read_file("/font.ttf", &rl);
    uint32_t fr = reg ? font_load(reg, rl) : 0;
    uint32_t fb = fr;
    if (fr) font_install_backend();
    { char b[80]; snprintf(b, sizeof b, "uidemo: font loaded (+%lums)\n",
                           (unsigned long)(embk_uptime_ms() - t_start)); embk_puts(1, b); }

    struct scene_arena sa; scene_arena_init(&sa);
    struct layout_arena la; layout_arena_init(&la);
    ui_theme_set_fonts(fr, fb);
    ui_init(&sa, &la);

    /* Fit the window to the screen: the desired card is WxH (tall), but a short
     * display (e.g. 640x480 or 1024x768) can't hold 786px -- clamp so the window
     * isn't rejected, and centre it. The toolkit lays out into the actual size. */
    uint32_t sw = 0, sh = 0;
    embk_screen_size(&sw, &sh);
    int winw = W, winh = H;
    if (sw && winw > (int)sw - 8)  winw = (int)sw - 8;
    if (sh && winh > (int)sh - 40) winh = (int)sh - 40;   /* room for title bar + margin */
    if (winw < 240) winw = 240;
    if (winh < 240) winh = 240;
    int wx = ((int)sw - winw) / 2;        if (wx < 0) wx = 0;
    int wy = ((int)sh - winh - 26) / 2;   if (wy < 8) wy = 8;

    /* A ZERO-COPY compositor window: the kernel maps the window's pixel pages
     * into us, so the toolkit renders its card STRAIGHT into shared memory and
     * present is a damage-only call. The compositor draws the window chrome, the
     * cursor, and routes our content-local pointer (embk_win_input). */
    uint32_t *px = 0;
    int win = embk_win_create_shared((uint32_t)winw, (uint32_t)winh, wx, wy, "UI Demo", (void **)&px);
    if (win < 0 || !px) { embk_puts(1, "uidemo: win_create FAILED\n"); return 1; }

    struct render_target rt = { px, (uint32_t)winw, (uint32_t)winh, (uint32_t)winw*4, EMBK_PIXFMT_BGRA8888_PRE };
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());   /* persistent! */
    embk_key_grab(1);   /* take the keyboard from the shell so typing reaches our fields */
    em_set_clock(embk_uptime_ms);   /* drive the animator off the monotonic ms clock */
    embk_puts(1, "uidemo: interactive loop running\n");

    /* THE EVENT LOOP: poll pointer -> declare -> layout -> paint(dirty) -> present. */
    bool prev_dark = !g_dark;   /* force the first frame to clear + full-repaint */
    bool prev_ov = false;       /* modal-overlay active last frame (edge-detect force_full) */
    for (;;) {
        /* the compositor routes this window's content-local pointer to us */
        struct embk_win_input in;
        embk_win_input(&in);
        if (in.focused) {
            ui_pointer((float)in.x, (float)in.y, (in.buttons & EMBK_MOUSE_LEFT) != 0);
            if (in.wheel) ui_wheel((float)in.wheel);   /* wheel -> hovered scroll view */
        } else {
            ui_pointer(-100.0f, -100.0f, false);       /* pointer isn't over us */
        }

        /* drain pending keystrokes into the UI (routed to the focused field).
         * ESC quits: release the keyboard grab and destroy the window. */
        for (int c; (c = embk_key_poll()) != 0; ) {
            if (c == 27) {
                embk_win_destroy(win); embk_key_grab(0);
                embk_puts(1, "uidemo: exit\n"); return 0;
            }
            ui_input_char(c);
        }

        ui_theme_use_dark(g_dark);          /* the Dark-mode toggle restyles live */

        /* Force a full repaint only on the frames that structurally change the
         * whole window: a theme flip, an active page transition, or a modal
         * open/close EDGE (a removed node's vacated pixels aren't erased by the
         * dirty-rect renderer). A modal merely *staying* open no longer forces a
         * repaint every frame -- the incremental dirty-rect path handles it. */
        bool ov = em_overlay_active();
        bool force_full = false;
        if (g_dark != prev_dark || em_nav_transitioning() || (ov != prev_ov)) {
            const struct ui_theme *t = ui_theme();
            uint32_t bg = (255u<<24) | ((uint32_t)(t->bg.r*255)<<16)
                        | ((uint32_t)(t->bg.g*255)<<8) | (uint32_t)(t->bg.b*255);
            for (int i = 0; i < winw*winh; i++) px[i] = bg;
            scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
            prev_dark = g_dark;
            force_full = true;
        }
        prev_ov = ov;

        ui_frame_begin(); em_new_frame(); em_nav(app); modal(); em_flush(); ui_frame_end();
        ui_run_layout((float)winw, (float)winh);
        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);   /* renders into the shared buffer */


        /* present only what changed (damage-only for a shared window) */
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

        { static int first = 1;                          /* one-time launch-latency log */
          if (first) { first = 0; char b[80];
              snprintf(b, sizeof b, "uidemo: first frame presented (+%lums)\n",
                       (unsigned long)(embk_uptime_ms() - t_start)); embk_puts(1, b); } }

        /* pace ~60-100Hz while YIELDING the CPU -- a spin loop here starved the
         * home + every other app of their slices (and vice versa). */
        embk_sleep_ms(10);
    }
    scene_render_destroy(&r);
    return 0;
}
