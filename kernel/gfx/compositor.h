#ifndef __GFX_COMPOSITOR_H__
#define __GFX_COMPOSITOR_H__

/* kernel/gfx/compositor.h -- EmbLink UI Piece 2: the window compositor.
 *
 * Piece 1 (surface.c) shares a single client surface and blits it, centred,
 * over whatever is on the framebuffer. Piece 2 turns the framebuffer into a
 * managed DESKTOP: many client "windows", each a positioned rectangle of
 * client-rendered pixels, drawn in z-order over a desktop background with
 * kernel-drawn chrome (a title bar) and a focus highlight.
 *
 * Model (deliberately simple for a first cut, documented as such):
 *   - Each window owns a kernel-side content buffer (kmalloc'd, virtually
 *     contiguous). The client PRESENTS pixels into it via copy_from_user --
 *     the same copy-on-present path sys_ui_present already uses, not the
 *     zero-copy surface-commit path (that's a future optimisation: the
 *     compositor could instead sample the client's mapped surface pages).
 *   - Compositing is REGION-based: presenting/moving a window repaints only
 *     the affected screen rectangle, redrawing every window that intersects
 *     it bottom-to-top so overlap and z-order stay correct. This mirrors the
 *     dirty-rect discipline the ring-3 toolkit already uses.
 *   - Windows are named by a small integer id, scoped to the owning pid.
 *     process teardown reclaims a crashed client's windows (memory only --
 *     the clean path is an explicit win_destroy, which also repaints). */

#include <stdint.h>
#include "include/types.h"

struct process;   /* fwd decl -- compositor.h must not include process.h */

#define COMP_MAX_WINDOWS   8
#define COMP_TITLEBAR_H    26     /* px; kernel-drawn chrome above the content */
#define COMP_BORDER        1      /* px; 1px frame around the whole window     */
#define COMP_TITLE_MAX     31

/* Create a window owned by `pid`, content `cw` x `ch`, top-left of the window
 * frame (chrome included) at screen (x,y). `title` is a kernel-side string
 * (the syscall layer copies it in). Returns a window id (>0) or -EMBK_*. */
int64_t compositor_win_create(int pid, uint32_t cw, uint32_t ch,
                              int32_t x, int32_t y, const char *title);

/* Zero-copy window: the compositor allocates the pixel pages, maps them into a
 * flat kernel view for compositing AND into `client` (which renders directly
 * into the shared memory). `out_client_va` receives the client's mapping base.
 * win_present then only DAMAGES (no pixel copy). Returns a window id or -EMBK_*. */
int64_t compositor_win_create_shared(struct process *client, uint32_t cw, uint32_t ch,
                                     int32_t x, int32_t y, const char *title,
                                     uint64_t *out_client_va);

/* Full-screen chromeless HOME/desktop window for `client`: sized to the
 * framebuffer, pinned at the back (z=0), no title bar or border, zero-copy. The
 * screen dimensions are returned via *out_w/*out_h so the app needn't hardcode
 * them. Returns a window id (>0) or -EMBK_*. */
int64_t compositor_win_create_desktop(struct process *client, uint64_t *out_client_va,
                                      uint32_t *out_w, uint32_t *out_h);

/* Zero-copy floating window with NO kernel chrome (no bar/close/border): the
 * app draws its own chrome and moves itself via win_move. Normal z-order. */
int64_t compositor_win_create_chromeless(struct process *client, uint32_t cw, uint32_t ch,
                                         int32_t x, int32_t y, const char *title,
                                         uint64_t *out_client_va);

/* DESKTOP WIDGET (EmUI V5): chromeless, z-banded above the desktop but below
 * every app window. */
int64_t compositor_win_create_widget(struct process *client, uint32_t cw, uint32_t ch,
                                     int32_t x, int32_t y, const char *title,
                                     int glass, uint64_t *out_client_va);

/* GLASS window (EmUI V8): chromeless, but the compositor blurs the backdrop
 * behind it and composites its translucent pixels over the frost (acrylic). */
int64_t compositor_win_create_glass(struct process *client, uint32_t cw, uint32_t ch,
                                    int32_t x, int32_t y, const char *title,
                                    uint64_t *out_client_va);

/* Resize a shared window's content: fresh page backing, new client VA out.
 * The caller must switch to the new pointer immediately. */
int64_t compositor_win_resize(struct process *client, uint32_t id,
                              uint32_t nw, uint32_t nh, uint64_t *out_client_va);

/* Non-zero if (pid,id) is a shared (zero-copy) window -- present skips the copy. */
int compositor_win_is_shared(int pid, uint32_t id);

/* Deliver the content-local pointer to `pid` iff it owns the window under the
 * cursor: returns 1 (focused) with content-local x/y, buttons, win id and the
 * accrued scroll wheel filled (wheel is consumed), or 0. Lets an app inside a
 * window read its own mouse. */
int compositor_win_input(int pid, int32_t *lx, int32_t *ly,
                         uint32_t *buttons, uint32_t *win, int32_t *wheel);

/* Hand back the kernel content buffer for (pid,id) so the syscall layer can
 * copy_from_user client pixels straight into it, plus its dims. NULL if no
 * such window or it isn't owned by `pid`. */
uint32_t *compositor_win_content(int pid, uint32_t id, uint32_t *cw, uint32_t *ch);

/* After the client has written new pixels into the content buffer, recomposite
 * the screen region covered by content sub-rect (rx,ry,rw,rh); rw==0 (or ==cw
 * & rh==ch) means the whole window. */
int64_t compositor_win_damage(int pid, uint32_t id,
                              uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh);

/* Move the window's frame top-left to screen (x,y) and raise it to the front.
 * Repaints the union of the old and new footprints. */
int64_t compositor_win_move(int pid, uint32_t id, int32_t x, int32_t y);

/* Destroy a window: free its buffer, erase it (repaint the exposed region). */
int64_t compositor_win_destroy(int pid, uint32_t id);

/* Reclaim every window owned by `pid` -- memory only, no repaint (safe to call
 * from process_reap_slot under the scheduler lock). */
void    compositor_reap_pid(int pid);

/* Pump pointer input: read the mouse, draw the cursor, and route clicks --
 * a press raises the window under the pointer to the front (click-to-focus),
 * and a press on a title bar begins a drag that moves the window. Called each
 * iteration of the kernel's main shell loop (thread context, so the plain
 * compositor spinlock is safe -- never call this from an IRQ handler). */
void    compositor_pointer_tick(void);

#endif /* __GFX_COMPOSITOR_H__ */
