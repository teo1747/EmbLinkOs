/* kernel/gfx/compositor.c -- the window compositor (see compositor.h). */

#include "gfx/compositor.h"
#include "drivers/video/framebuffer.h"
#include "drivers/input/mouse.h"
#include "arch/x86_64/cpu/spinlock.h"
#include "process/process.h"   /* struct process (shared_next_va, pml4_phys) for zero-copy */
#include "mm/pmm.h"            /* pmm_alloc_page / pmm_free_page / PAGE_SIZE */
#include "mm/vmm.h"            /* vmm_kmap_pages / vmm_map_in for shared windows */
#include "include/kmalloc.h"
#include "include/kprintf.h"
#include "include/errno.h"

/* ---- window registry ---------------------------------------------------- */

struct comp_window {
    int       used;
    uint32_t  id;
    int       pid;
    int32_t   x, y;         /* window-frame top-left on screen (chrome incl.) */
    uint32_t  cw, ch;       /* content dimensions in px                       */
    int       z;            /* z-order: larger is nearer the front            */
    int       visible;
    int       desktop;      /* 1 = chromeless full-screen back layer (the home) */
    int       chromeless;   /* 1 = floating window with NO kernel chrome: the app
                             * draws its own bar/close (EmUI V4 Window/WindowBar)
                             * and moves itself via win_move. Unlike `desktop` it
                             * keeps normal z-order (raisable, focusable). */
    int       widget;       /* 1 = DESKTOP WIDGET (EmUI V5): chromeless AND kept
                             * in a z-band above the desktop but BELOW every app
                             * window; clicks raise it only within that band. */
    /* Latched click replay: a press EDGE on this window's content is remembered
     * here so an app that was busy (e.g. mid first-frame render, seconds long
     * under TCG) still receives the click on its next win_input polls instead
     * of the click being silently eaten (win_input is a state poll, not an
     * event queue). 0 = none, 1 = replay press, 2 = replay release. */
    int       pend_click;
    int       pend_lx, pend_ly;   /* content-local coords of the latched press */
    char      title[COMP_TITLE_MAX + 1];
    uint32_t *content;      /* cw*ch, 0xAARRGGBB premultiplied. For a copy      */
                            /* window this is a kmalloc'd kernel buffer the      */
                            /* client presents INTO; for a shared window it's a  */
                            /* flat kernel view of pages the client renders into */
                            /* directly (zero-copy).                             */
    /* ---- zero-copy (shared) windows only ---- */
    int       shared;       /* 1 = client & compositor share the pixel pages     */
    uint64_t *phys;         /* the npages physical pages (compositor-owned)      */
    uint32_t  npages;
    uint64_t  kview;        /* kernel VA base of the flat view (== content)      */
    uint64_t  client_va;    /* base VA the pages are mapped at in the client     */
    uint64_t  client_pml4;  /* client address space, to unmap on teardown        */
};

static struct comp_window g_wins[COMP_MAX_WINDOWS];
static spinlock_t g_comp_lock = SPINLOCK_INIT;
static uint32_t   g_next_id = 1;
/* z bands: desktop = 0; widgets in [1, WIDGET_Z_TOP); app windows above it. */
#define WIDGET_Z_TOP 1000000
static int        g_next_z  = WIDGET_Z_TOP + 1;
static int        g_widget_z = 1;
static int        g_active  = 0;   /* desktop has been painted at least once  */
static uint32_t   g_top_id  = 0;   /* id of the currently-focused (front) window */

/* ---- pointer state (cursor + click-to-focus + title-bar drag) ----------- */
#define CURSOR_W 11
#define CURSOR_H 18
static int      g_cursor_x, g_cursor_y, g_cursor_valid;
static uint32_t g_prev_buttons;
static int      g_dragging;        /* a title-bar drag is in progress          */
static uint32_t g_drag_id;         /* which window is being dragged             */
static int      g_drag_dx, g_drag_dy;  /* pointer offset within the window frame */

/* Content-local pointer routing: each tick records the topmost window under the
 * cursor and the pointer in ITS content space, so sys_win_input can deliver it
 * to that window's app (this is what makes an in-window UI clickable -- see
 * compositor_win_input). g_ptr_pid == 0 means the pointer isn't over any
 * window's content (e.g. over a title bar). */
static int      g_ptr_pid;         /* owner pid of the hovered window (0 = none) */
static uint32_t g_ptr_win;         /* its window id                              */
/* POINTER CAPTURE (V5): while the left button stays down, keep routing to the
 * window where the press LANDED even if the cursor leaves it -- this is what
 * keeps a fast WindowBar drag (or an outward resize-grip drag) alive; local
 * coords may go negative/past the content during capture, which is fine for
 * delta-based drag math. 0 = no capture. */
static int      g_cap_pid;
static uint32_t g_cap_win;
static int      g_ptr_lx, g_ptr_ly;    /* pointer in that window's content coords */
static uint32_t g_ptr_buttons;     /* button state to deliver                    */
static int32_t  g_ptr_wheel;       /* scroll delta accrued for the hovered window */

/* ---- palette ------------------------------------------------------------ */

#define DESK_TOP_R 0x1b
#define DESK_TOP_G 0x1e
#define DESK_TOP_B 0x2b
#define DESK_BOT_R 0x0e
#define DESK_BOT_G 0x10
#define DESK_BOT_B 0x17

/* Desktop background: a vertical indigo->charcoal gradient. */
static fb_color_t desktop_color(int y, int screen_h) {
    if (screen_h <= 1) screen_h = 2;
    int t = (y * 255) / (screen_h - 1);
    if (t < 0) t = 0; if (t > 255) t = 255;
    int r = DESK_TOP_R + ((DESK_BOT_R - DESK_TOP_R) * t) / 255;
    int g = DESK_TOP_G + ((DESK_BOT_G - DESK_TOP_G) * t) / 255;
    int b = DESK_TOP_B + ((DESK_BOT_B - DESK_TOP_B) * t) / 255;
    return FB_RGB(r, g, b);
}

/* ---- geometry ----------------------------------------------------------- */

/* full window footprint (frame): title bar + content, plus the border ring. */
/* A desktop (home) window is chromeless -- no title bar above its content. */
static int win_titlebar_h(const struct comp_window *w) {
    return (w->desktop || w->chromeless) ? 0 : COMP_TITLEBAR_H;
}

static void win_frame_rect(const struct comp_window *w,
                           int *x0, int *y0, int *x1, int *y1) {
    *x0 = w->x;
    *y0 = w->y;
    *x1 = w->x + (int)w->cw;
    *y1 = w->y + win_titlebar_h(w) + (int)w->ch;
}

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

/* ---- lookup ------------------------------------------------------------- */

static struct comp_window *win_find(int pid, uint32_t id) {
    for (int i = 0; i < COMP_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].id == id && g_wins[i].pid == pid)
            return &g_wins[i];
    return 0;
}

static struct comp_window *win_by_id(uint32_t id) {
    for (int i = 0; i < COMP_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].id == id) return &g_wins[i];
    return 0;
}

/* front-most window's z (for the focus highlight). 0 if none. */
static int top_z(void) {
    int z = 0;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].visible && g_wins[i].z > z) z = g_wins[i].z;
    return z;
}

static struct comp_window *front_window(void) {
    struct comp_window *top = 0;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (!g_wins[i].used || !g_wins[i].visible) continue;
        if (!top || g_wins[i].z > top->z) top = &g_wins[i];
    }
    return top;
}

/* front-most window whose frame contains screen (x,y), or NULL. */
static struct comp_window *topmost_at(int x, int y) {
    struct comp_window *best = 0;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (!g_wins[i].used || !g_wins[i].visible) continue;
        int fx0, fy0, fx1, fy1; win_frame_rect(&g_wins[i], &fx0, &fy0, &fx1, &fy1);
        if (x >= fx0 && x < fx1 && y >= fy0 && y < fy1)
            if (!best || g_wins[i].z > best->z) best = &g_wins[i];
    }
    return best;
}

/* Screen rect of a window's title-bar CLOSE button (right side). The chromeless
 * desktop window has no title bar, so no close button (returns 0). */
#define COMP_CLOSE_SZ 16
static int win_close_rect(const struct comp_window *w, int *x0, int *y0, int *x1, int *y1) {
    if (w->desktop || w->chromeless) return 0;
    *x0 = w->x + (int)w->cw - COMP_CLOSE_SZ - 6;
    *y0 = w->y + (COMP_TITLEBAR_H - COMP_CLOSE_SZ) / 2;
    *x1 = *x0 + COMP_CLOSE_SZ;
    *y1 = *y0 + COMP_CLOSE_SZ;
    return 1;
}

/* ---- compositing -------------------------------------------------------- */

/* Draw one window's chrome + content clipped to region [rx0,ry0)-(rx1,ry1). */
static void paint_window(struct comp_window *w, int focused,
                         int rx0, int ry0, int rx1, int ry1) {
    int fx0, fy0, fx1, fy1;
    win_frame_rect(w, &fx0, &fy0, &fx1, &fy1);
    /* early out if the window doesn't touch the region at all */
    if (fx1 <= rx0 || fx0 >= rx1 || fy1 <= ry0 || fy0 >= ry1) return;

    int bar_h = win_titlebar_h(w);   /* 0 for the chromeless desktop window */

    /* --- title bar (repaint whole bar if the region touches it; text is
     *     cheap and this avoids clipped-glyph artefacts). Skipped entirely for
     *     the chromeless desktop layer. --- */
    if (!w->desktop && !w->chromeless) {
        int tbx0 = w->x, tby0 = w->y, tbx1 = w->x + (int)w->cw, tby1 = w->y + COMP_TITLEBAR_H;
        if (!(tbx1 <= rx0 || tbx0 >= rx1 || tby1 <= ry0 || tby0 >= ry1)) {
            fb_color_t bar = focused ? FB_RGB(0x5b, 0x63, 0xd6) : FB_RGB(0x2a, 0x2d, 0x3d);
            fb_fill_rect(tbx0, tby0, (int)w->cw, COMP_TITLEBAR_H, bar);
            /* a small "traffic light" dot, then the title text */
            fb_fill_circle(tbx0 + 13, tby0 + COMP_TITLEBAR_H / 2, 4,
                           focused ? FB_RGB(0xff, 0xd9, 0x5e) : FB_RGB(0x55, 0x58, 0x6b));
            int tr = focused ? 0xff : 0xb8, tg = focused ? 0xff : 0xbc, tb = focused ? 0xff : 0xc8;
            int br = focused ? 0x5b : 0x2a, bg = focused ? 0x63 : 0x2d, bb = focused ? 0xd6 : 0x3d;
            fb_draw_string(w->title, tbx0 + 28, tby0 + (COMP_TITLEBAR_H - 16) / 2,
                           tr, tg, tb, br, bg, bb);
            /* close button: a red dot with an X -- click it to close the window */
            int cbx0, cby0, cbx1, cby1;
            win_close_rect(w, &cbx0, &cby0, &cbx1, &cby1);
            fb_fill_circle((cbx0 + cbx1) / 2, (cby0 + cby1) / 2, COMP_CLOSE_SZ / 2,
                           focused ? FB_RGB(0xe0, 0x5b, 0x5b) : FB_RGB(0x50, 0x3a, 0x40));
            fb_color_t xc = focused ? FB_RGB(0x2a, 0x10, 0x10) : FB_RGB(0x9a, 0x82, 0x88);
            fb_draw_line(cbx0 + 5, cby0 + 5, cbx1 - 5, cby1 - 5, xc);
            fb_draw_line(cbx1 - 5, cby0 + 5, cbx0 + 5, cby1 - 5, xc);
        }
    }

    /* --- content: blit only the sub-rectangle overlapping the region --- */
    int cx0 = w->x, cy0 = w->y + bar_h;
    int cx1 = cx0 + (int)w->cw, cy1 = cy0 + (int)w->ch;
    int sx0 = imax(cx0, rx0), sy0 = imax(cy0, ry0);
    int sx1 = imin(cx1, rx1), sy1 = imin(cy1, ry1);
    if (sx1 > sx0 && sy1 > sy0 && w->content) {
        int src_x = sx0 - cx0, src_y = sy0 - cy0;
        const uint32_t *src = w->content + (size_t)src_y * w->cw + src_x;
        fb_blit(sx0, sy0, sx1 - sx0, sy1 - sy0, src, w->cw);
    }

    /* --- 1px border ring (not on the desktop, nor app-chromed windows) --- */
    if (!w->desktop && !w->chromeless)
        fb_draw_rect(fx0, fy0, fx1 - fx0, fy1 - fy0,
                     focused ? FB_RGB(0x7c, 0x84, 0xf0) : FB_RGB(0x3a, 0x3f, 0x55));
}

/* A small arrow cursor drawn straight into the framebuffer (clipped to screen
 * by the fb primitives). White fill with a dark 1px edge for contrast. */
static void draw_cursor(int cx, int cy) {
    for (int dy = 0; dy < CURSOR_H; dy++) {
        int span = dy < 9 ? dy : (CURSOR_H - 1 - dy) + 2;   /* arrow silhouette */
        if (span > CURSOR_W - 1) span = CURSOR_W - 1;
        for (int dx = 0; dx <= span; dx++) {
            int edge = (dx == 0) || (dx == span) || (dy == 0);
            fb_put_pixel_c((uint32_t)(cx + dx), (uint32_t)(cy + dy),
                           edge ? FB_RGB(0x10, 0x10, 0x14) : FB_RGB(0xff, 0xff, 0xff));
        }
    }
}

/* Repaint screen region [rx0,ry0)-(rx1,ry1): desktop, then all windows that
 * intersect it, bottom-to-top. Caller holds g_comp_lock. Presents the region. */
static void paint_region(int rx0, int ry0, int rx1, int ry1) {
    const fb_info_t *fi = fb_get_info();
    if (!fi) return;
    int W = (int)fi->width, H = (int)fi->height;
    if (rx0 < 0) rx0 = 0; if (ry0 < 0) ry0 = 0;
    if (rx1 > W) rx1 = W; if (ry1 > H) ry1 = H;
    if (rx1 <= rx0 || ry1 <= ry0) return;

    /* 1) desktop background (gradient, row by row) */
    for (int y = ry0; y < ry1; y++)
        fb_fill_rect(rx0, y, rx1 - rx0, 1, desktop_color(y, H));

    /* 2) windows, ascending z: repeatedly find the lowest not-yet-drawn z.
     *    O(N^2) over <=8 windows -- trivially cheap and keeps the code obvious. */
    int tz = top_z();
    int drawn = 0;
    int last_z = -1;
    while (drawn < COMP_MAX_WINDOWS) {
        int best = -1;
        for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
            if (!g_wins[i].used || !g_wins[i].visible) continue;
            if (g_wins[i].z <= last_z) continue;
            if (best < 0 || g_wins[i].z < g_wins[best].z) best = i;
        }
        if (best < 0) break;
        paint_window(&g_wins[best], g_wins[best].z == tz, rx0, ry0, rx1, ry1);
        last_z = g_wins[best].z;
        drawn++;
    }

    /* 3) the cursor, on top of everything, if it falls in this region */
    if (g_cursor_valid) {
        int cx0 = g_cursor_x, cy0 = g_cursor_y;
        int cx1 = cx0 + CURSOR_W, cy1 = cy0 + CURSOR_H;
        if (!(cx1 <= rx0 || cx0 >= rx1 || cy1 <= ry0 || cy0 >= ry1))
            draw_cursor(g_cursor_x, g_cursor_y);
    }

    fb_present();
}

/* Repaint just a window's title-bar strip (so its focus tint updates without
 * touching its content). */
static void paint_titlebar(struct comp_window *w) {
    if (w->desktop || w->chromeless) return;   /* no kernel bar to repaint */
    paint_region(w->x, w->y, w->x + (int)w->cw, w->y + COMP_TITLEBAR_H);
}

/* After any z-order change, keep the focus highlight honest: the front window
 * gets the accent title bar, everyone else the muted one. Only the two windows
 * whose focus actually flipped need repainting. Caller holds g_comp_lock. */
static void enforce_focus(void) {
    struct comp_window *top = front_window();
    uint32_t nt = top ? top->id : 0;
    if (nt == g_top_id) return;
    struct comp_window *old = win_by_id(g_top_id);   /* previously focused */
    g_top_id = nt;
    if (old && old->visible) paint_titlebar(old);    /* -> muted */
    if (top) paint_titlebar(top);                    /* -> accent */
}

/* Release a window's pixel backing. For a shared (zero-copy) window this MUST
 * run while the client address space is still alive (before its teardown), so
 * the pages -- which the compositor owns, not the client -- are unmapped from
 * the client first and freed exactly once here (not again by the client's
 * address-space teardown). */
static void win_free_backing(struct comp_window *w) {
    if (w->shared) {
        if (w->client_pml4 && w->client_va) {
            for (uint32_t i = 0; i < w->npages; i++)
                vmm_unmap_in(w->client_pml4, w->client_va + (uint64_t)i * PAGE_SIZE);
        }
        if (w->kview) vmm_kunmap_pages(w->kview, w->npages);
        if (w->phys) {
            for (uint32_t i = 0; i < w->npages; i++) pmm_free_page(w->phys[i]);
            kfree(w->phys);
        }
        w->phys = 0; w->kview = 0; w->client_va = 0; w->client_pml4 = 0; w->shared = 0;
    } else if (w->content) {
        kfree(w->content);
    }
    w->content = 0;
}

/* ---- public API --------------------------------------------------------- */

int64_t compositor_win_create(int pid, uint32_t cw, uint32_t ch,
                              int32_t x, int32_t y, const char *title) {
    const fb_info_t *fi = fb_get_info();
    if (!fi) return -EMBK_ENODEV;
    if (cw == 0 || ch == 0 || cw > fi->width || ch + COMP_TITLEBAR_H > fi->height)
        return -EMBK_EINVAL;

    spin_lock(&g_comp_lock);
    int slot = -1;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++)
        if (!g_wins[i].used) { slot = i; break; }
    if (slot < 0) { spin_unlock(&g_comp_lock); return -EMBK_ENOMEM; }

    uint32_t *buf = (uint32_t *)kmalloc((size_t)cw * ch * 4);
    if (!buf) { spin_unlock(&g_comp_lock); return -EMBK_ENOMEM; }
    /* start opaque black so an un-presented window isn't garbage */
    for (size_t i = 0; i < (size_t)cw * ch; i++) buf[i] = 0xFF000000u;

    struct comp_window *w = &g_wins[slot];
    w->used = 1;
    w->id = g_next_id++;
    w->pid = pid;
    w->x = x; w->y = y;
    w->cw = cw; w->ch = ch;
    w->z = g_next_z++;
    w->visible = 1;
    w->desktop = 0;
    w->chromeless = 0;
    w->widget = 0;
    w->pend_click = 0;
    w->content = buf;
    int n = 0;
    if (title) { while (n < COMP_TITLE_MAX && title[n]) { w->title[n] = title[n]; n++; } }
    w->title[n] = 0;

    uint32_t id = w->id;

    if (!g_active) {
        /* first window: take over the whole screen with the desktop */
        g_active = 1;
        paint_region(0, 0, (int)fi->width, (int)fi->height);
    } else {
        int fx0, fy0, fx1, fy1; win_frame_rect(w, &fx0, &fy0, &fx1, &fy1);
        paint_region(fx0, fy0, fx1, fy1);
    }
    enforce_focus();   /* the new window is now front; demote the old front */
    spin_unlock(&g_comp_lock);
    kprintf("compositor: window %u created (%ux%u) for pid %d\n",
            (unsigned)id, (unsigned)cw, (unsigned)ch, pid);
    return (int64_t)id;
}

/* Zero-copy variant: the compositor allocates page-aligned pixel pages, maps
 * them into a flat cached kernel view (for compositing) AND into the CLIENT
 * (so it renders directly into shared memory). No per-present pixel copy --
 * win_present becomes damage-only. `out_client_va` receives the client mapping. */
static int64_t win_create_shared_impl(struct process *client, uint32_t cw, uint32_t ch,
                                      int32_t x, int32_t y, const char *title,
                                      int desktop, int chromeless, int widget,
                                      uint64_t *out_client_va) {
    const fb_info_t *fi = fb_get_info();
    if (!fi || !client) return -EMBK_EINVAL;
    if (widget) chromeless = 1;                  /* widgets are always chromeless */
    uint32_t bar = (desktop || chromeless) ? 0 : COMP_TITLEBAR_H;
    if (cw == 0 || ch == 0 || cw > fi->width || ch + bar > fi->height)
        return -EMBK_EINVAL;

    uint64_t bytes = (uint64_t)cw * ch * 4;
    uint32_t npages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t *phys = (uint64_t *)kmalloc((size_t)npages * sizeof(uint64_t));
    if (!phys) return -EMBK_ENOMEM;
    uint32_t got = 0;
    for (; got < npages; got++) { uint64_t p = pmm_alloc_page(); if (!p) break; phys[got] = p; }
    if (got < npages) {
        for (uint32_t i = 0; i < got; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }

    uint64_t kview = vmm_kmap_pages(phys, npages);   /* compositor's flat view */
    if (!kview) {
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }

    /* map the SAME pages into the client so it renders directly (zero-copy) */
    uint64_t cva = client->shared_next_va, va = cva;
    int ok = 1;
    for (uint32_t i = 0; i < npages; i++) {
        if (vmm_map_in(client->pml4_phys, va, phys[i], VMM_USER | VMM_WRITABLE | VMM_NX) < 0) { ok = 0; break; }
        va += PAGE_SIZE;
    }
    if (!ok) {
        for (uint64_t u = cva; u < va; u += PAGE_SIZE) vmm_unmap_in(client->pml4_phys, u);
        vmm_kunmap_pages(kview, npages);
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }
    client->shared_next_va = cva + (uint64_t)npages * PAGE_SIZE;

    spin_lock(&g_comp_lock);
    int slot = -1;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) if (!g_wins[i].used) { slot = i; break; }
    if (slot < 0) {
        spin_unlock(&g_comp_lock);
        for (uint32_t i = 0; i < npages; i++) vmm_unmap_in(client->pml4_phys, cva + (uint64_t)i * PAGE_SIZE);
        vmm_kunmap_pages(kview, npages);
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }
    struct comp_window *w = &g_wins[slot];
    w->used = 1; w->id = g_next_id++; w->pid = (int)client->pid;
    w->x = x; w->y = y; w->cw = cw; w->ch = ch; w->visible = 1;
    w->desktop = desktop;
    w->chromeless = chromeless;
    w->widget = widget;
    w->pend_click = 0;
    w->z = desktop ? 0 : widget ? g_widget_z++ : g_next_z++;   /* z band per kind */
    w->content = (uint32_t *)kview;
    w->shared = 1; w->phys = phys; w->npages = npages;
    w->kview = kview; w->client_va = cva; w->client_pml4 = client->pml4_phys;
    for (size_t i = 0; i < (size_t)cw * ch; i++) w->content[i] = 0xFF000000u;
    int n = 0;
    if (title) { while (n < COMP_TITLE_MAX && title[n]) { w->title[n] = title[n]; n++; } }
    w->title[n] = 0;
    uint32_t id = w->id;
    if (!g_active) { g_active = 1; paint_region(0, 0, (int)fi->width, (int)fi->height); }
    else { int fx0, fy0, fx1, fy1; win_frame_rect(w, &fx0, &fy0, &fx1, &fy1); paint_region(fx0, fy0, fx1, fy1); }
    enforce_focus();
    spin_unlock(&g_comp_lock);
    if (out_client_va) *out_client_va = cva;
    kprintf("compositor: %s window %u created (%ux%u, %u pages) for pid %d\n",
            desktop ? "desktop" : "shared",
            (unsigned)id, (unsigned)cw, (unsigned)ch, (unsigned)npages, (int)client->pid);
    return (int64_t)id;
}

/* Zero-copy floating window (public). */
int64_t compositor_win_create_shared(struct process *client, uint32_t cw, uint32_t ch,
                                     int32_t x, int32_t y, const char *title,
                                     uint64_t *out_client_va) {
    return win_create_shared_impl(client, cw, ch, x, y, title, 0, 0, 0, out_client_va);
}

/* Zero-copy floating window with NO kernel chrome: the app draws its own bar
 * and close control (EmUI V4 Window/WindowBar) and moves itself via win_move.
 * Normal z-order otherwise (raisable, focusable, killable). */
int64_t compositor_win_create_chromeless(struct process *client, uint32_t cw, uint32_t ch,
                                         int32_t x, int32_t y, const char *title,
                                         uint64_t *out_client_va) {
    return win_create_shared_impl(client, cw, ch, x, y, title, 0, 1, 0, out_client_va);
}

/* Resize a SHARED window's content to (nw,nh): allocate a fresh page backing,
 * map it into the client at a new VA (returned via out_client_va), swap it in,
 * and repaint the union of the old+new footprints. The OLD backing is unmapped
 * and freed after the swap; the client must stop using its old pointer as soon
 * as this returns (the EmApp runtime does the swap synchronously). */
int64_t compositor_win_resize(struct process *client, uint32_t id,
                              uint32_t nw, uint32_t nh, uint64_t *out_client_va) {
    const fb_info_t *fi = fb_get_info();
    if (!fi || !client) return -EMBK_EINVAL;
    if (nw < 120 || nh < 90 || nw > fi->width || nh > fi->height) return -EMBK_EINVAL;

    /* new backing first (outside the lock: pmm/vmm work) */
    uint64_t bytes = (uint64_t)nw * nh * 4;
    uint32_t npages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t *phys = (uint64_t *)kmalloc((size_t)npages * sizeof(uint64_t));
    if (!phys) return -EMBK_ENOMEM;
    uint32_t got = 0;
    for (; got < npages; got++) { uint64_t p = pmm_alloc_page(); if (!p) break; phys[got] = p; }
    if (got < npages) {
        for (uint32_t i = 0; i < got; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }
    uint64_t kview = vmm_kmap_pages(phys, npages);
    if (!kview) {
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }
    uint64_t cva = client->shared_next_va, va = cva;
    int ok = 1;
    for (uint32_t i = 0; i < npages; i++) {
        if (vmm_map_in(client->pml4_phys, va, phys[i], VMM_USER | VMM_WRITABLE | VMM_NX) < 0) { ok = 0; break; }
        va += PAGE_SIZE;
    }
    if (!ok) {
        for (uint64_t u = cva; u < va; u += PAGE_SIZE) vmm_unmap_in(client->pml4_phys, u);
        vmm_kunmap_pages(kview, npages);
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }
    client->shared_next_va = cva + (uint64_t)npages * PAGE_SIZE;
    for (size_t i = 0; i < (size_t)nw * nh; i++) ((uint32_t *)kview)[i] = 0xFF000000u;

    /* swap under the lock; remember the old backing to free after */
    uint64_t *ophys = 0; uint32_t onpages = 0; uint64_t okview = 0, ocva = 0, opml4 = 0;
    int ox0, oy0, ox1, oy1, nx0, ny0, nx1, ny1;
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find((int)client->pid, id);
    if (!w || !w->shared) {
        spin_unlock(&g_comp_lock);
        for (uint32_t i = 0; i < npages; i++) vmm_unmap_in(client->pml4_phys, cva + (uint64_t)i * PAGE_SIZE);
        vmm_kunmap_pages(kview, npages);
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_EINVAL;
    }
    win_frame_rect(w, &ox0, &oy0, &ox1, &oy1);   /* old footprint */
    ophys = w->phys; onpages = w->npages; okview = w->kview; ocva = w->client_va; opml4 = w->client_pml4;
    w->cw = nw; w->ch = nh;
    w->content = (uint32_t *)kview;
    w->phys = phys; w->npages = npages; w->kview = kview;
    w->client_va = cva; w->client_pml4 = client->pml4_phys;
    win_frame_rect(w, &nx0, &ny0, &nx1, &ny1);   /* new footprint */
    paint_region(imin(ox0, nx0), imin(oy0, ny0), imax(ox1, nx1), imax(oy1, ny1));
    spin_unlock(&g_comp_lock);

    /* free the OLD backing (nobody references it any more) */
    if (ophys) {
        for (uint32_t i = 0; i < onpages; i++) vmm_unmap_in(opml4, ocva + (uint64_t)i * PAGE_SIZE);
        vmm_kunmap_pages(okview, onpages);
        for (uint32_t i = 0; i < onpages; i++) pmm_free_page(ophys[i]);
        kfree(ophys);
    }
    if (out_client_va) *out_client_va = cva;
    kprintf("compositor: win %u resized to %ux%u for pid %d\n",
            (unsigned)id, (unsigned)nw, (unsigned)nh, (int)client->pid);
    return (int64_t)id;
}

/* DESKTOP WIDGET (EmUI V5): chromeless, and z-banded ABOVE the desktop but
 * BELOW every app window -- an always-visible tile apps float over. */
int64_t compositor_win_create_widget(struct process *client, uint32_t cw, uint32_t ch,
                                     int32_t x, int32_t y, const char *title,
                                     uint64_t *out_client_va) {
    return win_create_shared_impl(client, cw, ch, x, y, title, 0, 1, 1, out_client_va);
}

/* Full-screen chromeless HOME/desktop window: sized to the framebuffer, pinned
 * at the back (z=0), no title bar. Returns the screen dims via *out_w/*out_h so
 * the client needn't hardcode them. Zero-copy like the floating variant. */
int64_t compositor_win_create_desktop(struct process *client, uint64_t *out_client_va,
                                      uint32_t *out_w, uint32_t *out_h) {
    const fb_info_t *fi = fb_get_info();
    if (!fi) return -EMBK_ENODEV;
    int64_t id = win_create_shared_impl(client, fi->width, fi->height, 0, 0,
                                        "", 1, 0, 0, out_client_va);
    if (id > 0) { if (out_w) *out_w = fi->width; if (out_h) *out_h = fi->height; }
    return id;
}

int compositor_win_is_shared(int pid, uint32_t id) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    int s = w ? w->shared : 0;
    spin_unlock(&g_comp_lock);
    return s;
}

uint32_t *compositor_win_content(int pid, uint32_t id, uint32_t *cw, uint32_t *ch) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    uint32_t *buf = 0;
    if (w) { buf = w->content; if (cw) *cw = w->cw; if (ch) *ch = w->ch; }
    spin_unlock(&g_comp_lock);
    return buf;
}

int64_t compositor_win_damage(int pid, uint32_t id,
                              uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    if (!w) { spin_unlock(&g_comp_lock); return -EMBK_ENOENT; }
    if (rw == 0 || rh == 0) { rx = 0; ry = 0; rw = w->cw; rh = w->ch; }
    if (rx > w->cw) rx = w->cw; if (ry > w->ch) ry = w->ch;
    if (rx + rw > w->cw) rw = w->cw - rx;
    if (ry + rh > w->ch) rh = w->ch - ry;
    /* content coords -> screen coords (content sits below the title bar, or at
     * the frame origin for the chromeless desktop) */
    int sx0 = w->x + (int)rx;
    int sy0 = w->y + win_titlebar_h(w) + (int)ry;
    paint_region(sx0, sy0, sx0 + (int)rw, sy0 + (int)rh);
    spin_unlock(&g_comp_lock);
    return 0;
}

int64_t compositor_win_move(int pid, uint32_t id, int32_t x, int32_t y) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    if (!w) { spin_unlock(&g_comp_lock); return -EMBK_ENOENT; }
    int ox0, oy0, ox1, oy1; win_frame_rect(w, &ox0, &oy0, &ox1, &oy1);
    w->x = x; w->y = y;
    w->z = w->widget ? g_widget_z++ : g_next_z++;   /* raise within its own band */
    int nx0, ny0, nx1, ny1; win_frame_rect(w, &nx0, &ny0, &nx1, &ny1);
    /* repaint the union of old and new footprints in one region */
    paint_region(imin(ox0, nx0), imin(oy0, ny0), imax(ox1, nx1), imax(oy1, ny1));
    enforce_focus();   /* moving raises to front; demote whoever was front */
    spin_unlock(&g_comp_lock);
    return 0;
}

int64_t compositor_win_destroy(int pid, uint32_t id) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    if (!w) { spin_unlock(&g_comp_lock); return -EMBK_ENOENT; }
    int fx0, fy0, fx1, fy1; win_frame_rect(w, &fx0, &fy0, &fx1, &fy1);
    win_free_backing(w);                       /* frees copy buf OR unmaps+frees shared pages */
    w->used = 0; w->visible = 0;
    if (id == g_top_id) g_top_id = 0;          /* force focus recompute below */
    paint_region(fx0, fy0, fx1, fy1);          /* expose whatever was behind */
    enforce_focus();                           /* promote the new front window */
    spin_unlock(&g_comp_lock);
    return 0;
}

void compositor_reap_pid(int pid) {
    /* Reclaim a dead client's windows. MUST run BEFORE its address space is torn
     * down (process_reap_slot calls us pre-vmm_destroy): a shared window's pages
     * are unmapped from the client here so the address-space teardown doesn't
     * double-free them. No framebuffer work (safe under the scheduler lock); a
     * crashed client's pixels linger until the next repaint. */
    spin_lock(&g_comp_lock);
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (g_wins[i].used && g_wins[i].pid == pid) {
            win_free_backing(&g_wins[i]);
            g_wins[i].used = 0; g_wins[i].visible = 0;
            if (g_wins[i].id == g_top_id) g_top_id = 0;
        }
    }
    spin_unlock(&g_comp_lock);
}

void compositor_pointer_tick(void) {
    if (!g_active) return;               /* no desktop yet -> nothing to route */
    int32_t x, y; uint32_t b;
    mouse_get_state(&x, &y, &b);

    spin_lock(&g_comp_lock);
    int left = (b & MOUSE_BTN_LEFT) != 0, prev = (g_prev_buttons & MOUSE_BTN_LEFT) != 0;

    /* one accumulated dirty bbox for the whole tick -> a single paint_region */
    int have = 0, bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    #define DIRTY(x0, y0, x1, y1) do {                                       \
        if (!have) { bx0=(x0); by0=(y0); bx1=(x1); by1=(y1); have=1; }       \
        else { if ((x0)<bx0)bx0=(x0); if ((y0)<by0)by0=(y0);                 \
               if ((x1)>bx1)bx1=(x1); if ((y1)>by1)by1=(y1); }               \
    } while (0)

    if (!g_cursor_valid) { g_cursor_x = x; g_cursor_y = y; g_cursor_valid = 1; }

    int raised = 0;
    int close_pid = 0;   /* set when a close button is clicked */
    /* press edge: raise the window under the pointer; title-bar press = drag,
     * close-button press = close. The desktop (home) layer is never raised or
     * dragged -- it stays pinned at the back; its clicks flow through to the app
     * as content input below. */
    if (left && !prev) {
        struct comp_window *w = topmost_at(x, y);
        /* latch a content-area press for the owner (desktop included): if the
         * app is busy rendering when this press+release happens, win_input
         * replays it (press then release) so the click is never eaten. */
        if (w) {
            int clx = x - w->x, cly = y - (w->y + win_titlebar_h(w));
            if (clx >= 0 && clx < (int)w->cw && cly >= 0 && cly < (int)w->ch) {
                w->pend_click = 1; w->pend_lx = clx; w->pend_ly = cly;
            }
        }
        if (w && !w->desktop) {
            int cbx0, cby0, cbx1, cby1;
            if (win_close_rect(w, &cbx0, &cby0, &cbx1, &cby1) &&
                x >= cbx0 && x < cbx1 && y >= cby0 && y < cby1) {
                /* close: HIDE the window now so it disappears immediately (the
                 * killed process is reaped asynchronously; don't wait on it),
                 * repaint what's behind, then kill its process below. Its shared
                 * pixel backing is freed later by compositor_reap_pid. */
                close_pid = w->pid;
                kprintf("compositor: close btn -> hide win %u, kill pid %d\n", (unsigned)w->id, w->pid);   /* DIAG */
                w->visible = 0;
                if (w->id == g_top_id) g_top_id = 0;
                int fx0, fy0, fx1, fy1; win_frame_rect(w, &fx0, &fy0, &fx1, &fy1);
                DIRTY(fx0, fy0, fx1, fy1);
                raised = 1;   /* re-run enforce_focus() to promote the new front */
            } else {
                w->z = w->widget ? g_widget_z++ : g_next_z++;   /* raise within band */
                raised = 1;
                int fx0, fy0, fx1, fy1; win_frame_rect(w, &fx0, &fy0, &fx1, &fy1);
                DIRTY(fx0, fy0, fx1, fy1);
                if (y < w->y + win_titlebar_h(w)) {   /* grabbed the title bar */
                    g_dragging = 1; g_drag_id = w->id;
                    g_drag_dx = x - w->x; g_drag_dy = y - w->y;
                }
            }
        }
    }
    if (!left && prev) g_dragging = 0;   /* release ends any drag */

    /* drag: move the grabbed window to follow the pointer */
    if (g_dragging && left) {
        struct comp_window *w = win_by_id(g_drag_id);
        if (w) {
            int nx = x - g_drag_dx, ny = y - g_drag_dy;
            if (nx != w->x || ny != w->y) {
                int ox0, oy0, ox1, oy1; win_frame_rect(w, &ox0, &oy0, &ox1, &oy1);
                w->x = nx; w->y = ny;
                int nx0, ny0, nx1, ny1; win_frame_rect(w, &nx0, &ny0, &nx1, &ny1);
                DIRTY(ox0, oy0, ox1, oy1);
                DIRTY(nx0, ny0, nx1, ny1);
            }
        }
    }

    /* cursor motion: fold the old + new cursor rects into the dirty bbox so
     * the old cursor is erased and the new one drawn (paint_region stamps the
     * cursor at g_cursor_* last). */
    if (x != g_cursor_x || y != g_cursor_y) {
        DIRTY(g_cursor_x, g_cursor_y, g_cursor_x + CURSOR_W, g_cursor_y + CURSOR_H);
        g_cursor_x = x; g_cursor_y = y;
        DIRTY(x, y, x + CURSOR_W, y + CURSOR_H);
    }

    if (have) paint_region(bx0, by0, bx1, by1);
    if (raised) enforce_focus();   /* demote whoever used to be front */

    /* Record the content-local pointer for the topmost window under the cursor
     * (skipping its title bar) so its owning app can read it via sys_win_input.
     * A drag in progress keeps routing to no app (the compositor owns it). */
    g_ptr_pid = 0;
    if (!g_dragging) {
        if (!(b & MOUSE_BTN_LEFT)) g_cap_pid = 0;      /* release ends capture */
        struct comp_window *cap = g_cap_pid ? win_find(g_cap_pid, g_cap_win) : 0;
        if (cap && cap->visible) {
            /* captured: route to the press-owner regardless of containment */
            g_ptr_pid = cap->pid; g_ptr_win = cap->id;
            g_ptr_lx = x - cap->x;
            g_ptr_ly = y - (cap->y + win_titlebar_h(cap));
            g_ptr_buttons = b;
        } else {
            g_cap_pid = 0;
            struct comp_window *h = topmost_at(x, y);
            if (h) {
                int lx = x - h->x;
                int ly = y - (h->y + win_titlebar_h(h));
                if (lx >= 0 && lx < (int)h->cw && ly >= 0 && ly < (int)h->ch) {
                    g_ptr_pid = h->pid; g_ptr_win = h->id;
                    g_ptr_lx = lx; g_ptr_ly = ly; g_ptr_buttons = b;
                    if ((b & MOUSE_BTN_LEFT) && !(g_prev_buttons & MOUSE_BTN_LEFT)) {
                        g_cap_pid = h->pid; g_cap_win = h->id;   /* press starts capture */
                    }
                }
            }
        }
    }
    /* drain the scroll wheel every tick; route it to the hovered window's app */
    int32_t wheel = mouse_take_wheel();
    if (wheel && g_ptr_pid) g_ptr_wheel += wheel;

    g_prev_buttons = b;
    #undef DIRTY
    spin_unlock(&g_comp_lock);

    /* A close-button click terminates the owning process OUTSIDE g_comp_lock:
     * process_kill -> process_reap_slot -> compositor_reap_pid re-takes
     * g_comp_lock, so holding it here would deadlock. The window was already
     * hidden + repainted above, so it's gone regardless of when the reap runs. */
    if (close_pid) process_kill((uint32_t)close_pid);
}

/* Deliver the content-local pointer to the calling process IF it owns the window
 * currently under the cursor. Returns 1 (focused) with the out-params filled, or
 * 0 (the pointer isn't over this process's window content). This is how an app
 * inside a compositor window reads its mouse -- the home/desktop app gets every
 * click that falls through the floating windows onto it. */
int compositor_win_input(int pid, int32_t *lx, int32_t *ly,
                         uint32_t *buttons, uint32_t *win, int32_t *wheel) {
    spin_lock(&g_comp_lock);
    int focused = (g_ptr_pid != 0 && g_ptr_pid == pid);
    if (focused) {
        if (lx) *lx = g_ptr_lx;
        if (ly) *ly = g_ptr_ly;
        if (buttons) *buttons = g_ptr_buttons;
        if (win) *win = g_ptr_win;
        if (wheel) { *wheel = g_ptr_wheel; g_ptr_wheel = 0; }
    }

    /* Latched-click replay: if one of this pid's windows recorded a press the
     * app hasn't seen (it was busy rendering when press+release happened),
     * synthesize the click across two polls -- press first, release next --
     * at the latched content coords. If the app is polling LIVE while the
     * button is still physically down on that window, drop the latch instead
     * (the real-time path is already delivering it; replaying would double-
     * fire), which also keeps slider drags on real button state. */
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        struct comp_window *w = &g_wins[i];
        if (!w->used || w->pid != pid || !w->pend_click) continue;
        if (w->pend_click == 1) {
            if (focused && w->id == g_ptr_win && (g_ptr_buttons & MOUSE_BTN_LEFT)) {
                w->pend_click = 0;              /* live path saw the press */
            } else {
                if (lx) *lx = w->pend_lx;
                if (ly) *ly = w->pend_ly;
                if (buttons) *buttons = MOUSE_BTN_LEFT;
                if (win) *win = w->id;
                focused = 1;
                w->pend_click = 2;              /* release replays next poll */
            }
        } else {                                 /* == 2 */
            if (lx) *lx = w->pend_lx;
            if (ly) *ly = w->pend_ly;
            if (buttons) *buttons = 0;
            if (win) *win = w->id;
            focused = 1;
            w->pend_click = 0;
        }
        break;
    }
    spin_unlock(&g_comp_lock);
    return focused;
}
